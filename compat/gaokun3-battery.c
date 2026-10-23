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
