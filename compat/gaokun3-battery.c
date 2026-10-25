/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "gaokun3-battery.h"

#define ADP_KNOWN (GAOKUN3_ADP_AC | GAOKUN3_ADP_PRESENT)
#define BST_KNOWN (GAOKUN3_BST_DISCHARGING | GAOKUN3_BST_CHARGING | GAOKUN3_BST_CRITICAL | GAOKUN3_BST_FULL)

/* Plausibility bounds. All INFERRED, none measured on a gaokun3:
 * - voltage: the pack is two Li-ion cells in series (gaokun-android's power
 *   profile works from 7.7 V, 2S), so roughly 6.0 V empty to 8.8 V full; the
 *   bounds leave margin either side and only catch a misread word.
 * - full capacity: gaokun-android's healthd reported 4483 mAh full; a word
 *   outside 1000..10000 is a misread, not a worn or upgraded pack.
 * - consistency: percent is the EC's own gauge, capacity/full is its raw
 *   charge. They should agree within a few points; ten is a guess at the EC's
 *   reserve and rounding, to be replaced by measured pairs.
 * Refusing on a bound fails closed: a real battery outside one stops runs
 * until the bound is corrected from a recorded reading. */
#define VOLTAGE_MIN_MV 5000u
#define VOLTAGE_MAX_MV 9000u
#define FULL_MIN_MAH 1000u
#define FULL_MAX_MAH 10000u
#define PERCENT_TOLERANCE 10u

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

unsigned gaokun3_battery_decode(const struct gaokun3_battery_snapshot *snap, uint64_t now_ms,
                                struct gaokun3_battery_record *r) {
    if (!r) return GAOKUN3_BATTERY_CONFIG;
    *r = (struct gaokun3_battery_record){.abi = GAOKUN3_BATTERY_ABI, .status = 1};
    unsigned why = GAOKUN3_BATTERY_OK;
    if (!snap || (snap->have & ~GAOKUN3_HAVE_ALL)) {
        why = GAOKUN3_BATTERY_CONFIG;
        goto out;
    }
    if (snap->have & GAOKUN3_HAVE_ADP) r->adp = snap->adp;
    if (snap->have & GAOKUN3_HAVE_BST) r->bst = snap->bst;
    if (snap->have & GAOKUN3_HAVE_STATUS) {
        /* 0x90 percent, 0x92 mV, 0x94 mAh now, 0x96 mAh full, 0x98 ?, 0x9a mA, 0x9c ? */
        r->percent = le16(snap->status + 0);
        r->voltage_mv = le16(snap->status + 2);
        r->capacity_mah = le16(snap->status + 4);
        r->full_mah = le16(snap->status + 6);
        uint32_t rate = le16(snap->status + 10);
        r->rate_ma = rate & 0x8000u ? (int32_t)rate - 0x10000 : (int32_t)rate;
    }
    r->ac = (r->adp & GAOKUN3_ADP_AC) && !(r->bst & GAOKUN3_BST_DISCHARGING);
    if (snap->have != GAOKUN3_HAVE_ALL) {
        why = GAOKUN3_BATTERY_READ;
        goto out;
    }
    r->status = 0;
    if (snap->read_ms > now_ms) {
        why = GAOKUN3_BATTERY_STALE;           /* a clock that ran backwards proves nothing */
        goto out;
    }
    r->age_ms = now_ms - snap->read_ms;
    if (r->age_ms > GAOKUN3_BATTERY_MAX_AGE_MS) why = GAOKUN3_BATTERY_STALE;
    /* Unknown bits first: a byte with them set (0xff from a bus that NAKed,
     * say) cannot be trusted to mean what its known bits say. */
    else if ((r->adp & ~ADP_KNOWN) || (r->bst & ~BST_KNOWN)) why = GAOKUN3_BATTERY_BITS;
    else if (!(r->adp & GAOKUN3_ADP_PRESENT)) why = GAOKUN3_BATTERY_ABSENT;
    else if (((r->bst & GAOKUN3_BST_CHARGING) && (r->bst & GAOKUN3_BST_DISCHARGING)) ||
             ((r->bst & GAOKUN3_BST_CHARGING) && !(r->adp & GAOKUN3_ADP_AC)))
        why = GAOKUN3_BATTERY_CONTRADICTION;
    /* The level at which the EC sets this is unmeasured, and on AC it may mean
     * the source cannot keep up: refuse rather than let AC excuse it. */
    else if (r->bst & GAOKUN3_BST_CRITICAL) why = GAOKUN3_BATTERY_CRITICAL;
    else if (r->percent > 100 || r->voltage_mv < VOLTAGE_MIN_MV || r->voltage_mv > VOLTAGE_MAX_MV ||
             r->full_mah < FULL_MIN_MAH || r->full_mah > FULL_MAX_MAH)
        why = GAOKUN3_BATTERY_RANGE;
    else if (r->capacity_mah > r->full_mah) why = GAOKUN3_BATTERY_INCONSISTENT;
    else {
        uint32_t gauge = (r->capacity_mah * 100u + r->full_mah / 2) / r->full_mah;
        uint32_t diff = gauge > r->percent ? gauge - r->percent : r->percent - gauge;
        if (diff > PERCENT_TOLERANCE) why = GAOKUN3_BATTERY_INCONSISTENT;
    }
out:
    r->reason = why;
    r->plausible = why == GAOKUN3_BATTERY_OK;
    return why;
}

/* cycle_lib.common.battery_ok(status, battery, minimum):
 *     status == 0 and plausible and (ac or percent >= minimum)
 * plus cycle_lib.common.battery_minimum()'s floor..100 bound on minimum. */
unsigned gaokun3_battery_admit(const struct gaokun3_battery_record *r, unsigned minimum) {
    if (!r || r->abi != GAOKUN3_BATTERY_ABI || minimum < GAOKUN3_BATTERY_FLOOR_PERCENT || minimum > 100)
        return GAOKUN3_BATTERY_CONFIG;
    if (r->status) return r->reason ? r->reason : GAOKUN3_BATTERY_READ;
    /* plausible and reason are set together by decode; a record where they
     * disagree was not produced by it. */
    if (r->plausible != (r->reason == GAOKUN3_BATTERY_OK)) return GAOKUN3_BATTERY_CONFIG;
    if (!r->plausible) return r->reason;
    return r->ac || r->percent >= minimum ? GAOKUN3_BATTERY_OK : GAOKUN3_BATTERY_LOW;
}

void gaokun3_battery_trend_init(struct gaokun3_battery_trend *t) {
    if (t) *t = (struct gaokun3_battery_trend){.abi = GAOKUN3_BATTERY_TREND_ABI};
}

/* Latched: the caller stops the run on the first trip, and nothing fed
 * afterwards may make the battery look trustworthy again. */
static unsigned trend_trip(struct gaokun3_battery_trend *t, unsigned why) {
    t->reason = why;
    t->watching = 0;
    return why;
}

unsigned gaokun3_battery_trend_feed(struct gaokun3_battery_trend *t,
                                    const struct gaokun3_battery_record *r, uint64_t now_ms) {
    if (!t || t->abi != GAOKUN3_BATTERY_TREND_ABI) return GAOKUN3_BATTERY_CONFIG;
    if (t->reason) return t->reason;
    if (!r || r->abi != GAOKUN3_BATTERY_ABI) return trend_trip(t, GAOKUN3_BATTERY_CONFIG);
    if (t->fed && now_ms < t->last_ms) return trend_trip(t, GAOKUN3_BATTERY_CONFIG);
    if (t->fed && now_ms - t->last_ms > GAOKUN3_BATTERY_TREND_MAX_GAP_MS)
        return trend_trip(t, GAOKUN3_BATTERY_STALE);
    t->fed = 1;
    t->last_ms = now_ms;
    /* A refused record says nothing about the charge, and the guard does
     * not rely on the battery past a refusal. */
    if (!r->plausible) return trend_trip(t, r->reason ? r->reason : GAOKUN3_BATTERY_CONFIG);
    if (r->status || r->reason != GAOKUN3_BATTERY_OK) return trend_trip(t, GAOKUN3_BATTERY_CONFIG);
    uint32_t mah = r->capacity_mah;
    t->last_mah = mah;
    if (!r->ac) {
        t->watching = 0;
        return GAOKUN3_BATTERY_OK;
    }
    if (!t->watching) {
        t->watching = 1;
        t->watch_ms = t->high_ms = now_ms;
        t->high_mah = mah;
        return GAOKUN3_BATTERY_OK;
    }
    /* Measure from the latest high: a source that charges, or holds the
     * charge, keeps moving it forward. A high older than the window restarts
     * the measurement, so a slow sag on a held charger (about 1 mAh/min
     * OBSERVED) never adds up to a trip, and a drain at the threshold rate
     * reaches TREND_DROP_MAH within one window. */
    if (now_ms - t->high_ms > GAOKUN3_BATTERY_TREND_WINDOW_MS || mah >= t->high_mah) {
        t->high_mah = mah;
        t->high_ms = now_ms;
        return GAOKUN3_BATTERY_OK;
    }
    uint32_t drop = t->high_mah - mah;
    if (drop > t->worst_drop_mah) t->worst_drop_mah = drop;
    return drop >= GAOKUN3_BATTERY_TREND_DROP_MAH ? trend_trip(t, GAOKUN3_BATTERY_DRAIN)
                                                   : GAOKUN3_BATTERY_OK;
}

unsigned gaokun3_battery_admit_watched(const struct gaokun3_battery_record *r,
                                       const struct gaokun3_battery_trend *t, unsigned minimum) {
    if (!t || t->abi != GAOKUN3_BATTERY_TREND_ABI) return GAOKUN3_BATTERY_CONFIG;
    unsigned verdict = gaokun3_battery_admit(r, minimum);
    if (verdict != GAOKUN3_BATTERY_OK && verdict != GAOKUN3_BATTERY_LOW) return verdict;
    if (t->reason) return t->reason;
    if (verdict == GAOKUN3_BATTERY_LOW || r->percent >= minimum) return verdict;
    /* Only the AC claim admits this run, and BATT-1 is exactly a false one:
     * it counts once a whole window of it has passed without a drain. */
    if (!t->watching || t->last_ms - t->watch_ms < GAOKUN3_BATTERY_TREND_WINDOW_MS)
        return GAOKUN3_BATTERY_UNWATCHED;
    return GAOKUN3_BATTERY_OK;
}
