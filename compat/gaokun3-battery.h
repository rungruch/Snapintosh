/* SPDX-License-Identifier: MIT
 * Huawei MateBook E Go (gaokun3) battery/AC admission policy, decided from one
 * snapshot of the embedded controller's battery registers, and a trend check
 * over consecutive snapshots for AC the battery does not receive. Provenance,
 * evidence classes and what is still unmeasured: docs/GAOKUN3-PHASE1-GUARDS.md.
 *
 * Host-only and I/O-free: no monitor build links this file. The caller owns
 * the EC transport (I2C, EC at 0x38 on the gaokun3's i2c15) and hands over
 * the raw bytes it read; this file only decodes and decides. The record has
 * the four fields tools/cycle_lib/common.py's battery_ok() reads (status,
 * plausible, ac, percent), and gaokun3_battery_admit() is that same rule;
 * tools/test-gaokun3-battery.py checks the two agree on every test case. */
#pragma once
#include <stdint.h>

#define GAOKUN3_BATTERY_ABI 1u

/* EC byte addresses and bits: Linux drivers/power/supply/huawei-gaokun-battery.c,
 * used as a hardware reference only. OBSERVED in that driver, not measured here. */
#define GAOKUN3_EC_ADP 0x81u              /* adapter/presence byte */
#define GAOKUN3_EC_BST 0x82u              /* charge state byte (ACPI _BST-like) */
#define GAOKUN3_EC_BAT_STATUS 0x90u       /* seven little-endian words, below */
#define GAOKUN3_EC_BAT_STATUS_BYTES 14u
#define GAOKUN3_ADP_AC (1u << 0)
#define GAOKUN3_ADP_PRESENT (1u << 1)
#define GAOKUN3_BST_DISCHARGING (1u << 0)
#define GAOKUN3_BST_CHARGING (1u << 1)
#define GAOKUN3_BST_CRITICAL (1u << 2)    /* the charge level that sets it is unmeasured */
#define GAOKUN3_BST_FULL (1u << 3)

/* Which of the three EC reads completed; all three are required. */
#define GAOKUN3_HAVE_ADP (1u << 0)
#define GAOKUN3_HAVE_BST (1u << 1)
#define GAOKUN3_HAVE_STATUS (1u << 2)
#define GAOKUN3_HAVE_ALL (GAOKUN3_HAVE_ADP | GAOKUN3_HAVE_BST | GAOKUN3_HAVE_STATUS)

/* The lowest charge-without-AC minimum the policy accepts: AGENTS.md rule 5 and
 * tools/cycle_lib/pins.example.json battery.floor. */
#define GAOKUN3_BATTERY_FLOOR_PERCENT 50u
/* Policy, not a hardware fact: a snapshot older than this no longer describes
 * the battery an admission is about to rely on. */
#define GAOKUN3_BATTERY_MAX_AGE_MS 4000u

enum gaokun3_battery_reason {
    GAOKUN3_BATTERY_OK,
    GAOKUN3_BATTERY_LOW,           /* admit only: no AC and under the minimum */
    GAOKUN3_BATTERY_CONFIG,        /* NULL argument, unknown have bits, bad minimum, bad record */
    GAOKUN3_BATTERY_READ,          /* an EC read did not complete */
    GAOKUN3_BATTERY_STALE,         /* too old, or timestamped after now */
    GAOKUN3_BATTERY_BITS,          /* a bit set that the policy does not know */
    GAOKUN3_BATTERY_ABSENT,        /* the EC reports no battery */
    GAOKUN3_BATTERY_CONTRADICTION, /* charging and discharging, or charging without AC */
    GAOKUN3_BATTERY_CRITICAL,      /* the EC's critical bit, whatever the AC state */
    GAOKUN3_BATTERY_RANGE,         /* percent, voltage or full capacity out of bounds */
    GAOKUN3_BATTERY_INCONSISTENT,  /* percent disagrees with capacity / full */
    /* The trend check's own (gaokun3_battery_trend_feed, _admit_watched): */
    GAOKUN3_BATTERY_DRAIN,         /* the charge fell while the EC claimed AC (BATT-1) */
    GAOKUN3_BATTERY_UNWATCHED,     /* AC excuses a low charge only after a watched window */
};

/* What the transport read. Bytes of a read whose have bit is clear are ignored. */
struct gaokun3_battery_snapshot {
    uint32_t have;                 /* GAOKUN3_HAVE_* */
    uint8_t adp;                   /* EC 0x81 */
    uint8_t bst;                   /* EC 0x82 */
    uint8_t status[GAOKUN3_EC_BAT_STATUS_BYTES]; /* EC 0x90..0x9d */
    uint64_t read_ms;              /* monotonic ms at which the first of the reads began */
};

struct gaokun3_battery_record {
    uint32_t abi, reason;
    uint32_t status;               /* 0 when every read completed: cycle_lib's status */
    uint32_t plausible;            /* 1 only when reason is OK */
    uint32_t ac;                   /* adapter bit set and the EC not discharging */
    uint32_t percent;
    uint32_t adp, bst;             /* raw bytes, for the report */
    uint32_t voltage_mv, capacity_mah, full_mah;
    int32_t rate_ma;               /* reported only: its sign convention is unverified */
    uint64_t age_ms;
};

/* Fills *record (when non-NULL) from *snapshot as of now_ms and returns its
 * reason: GAOKUN3_BATTERY_OK, or why the sample must not be relied on. Every
 * field the transport did read is decoded into the record, refused or not. */
unsigned gaokun3_battery_decode(const struct gaokun3_battery_snapshot *snapshot, uint64_t now_ms,
                                struct gaokun3_battery_record *record);
/* GAOKUN3_BATTERY_OK when a decoded record admits a run that needs minimum
 * percent without AC (GAOKUN3_BATTERY_FLOOR_PERCENT..100), LOW when only the
 * charge refuses it, otherwise the record's refusal or CONFIG. */
unsigned gaokun3_battery_admit(const struct gaokun3_battery_record *record, unsigned minimum);

/* ---- Trend check: AC the battery does not actually receive (BATT-1) ------
 * A weak USB source can leave the EC reporting the adapter and "charging"
 * while the battery drains: on 2026-10-25, a Mac's USB-C port under load
 * reported charging for 10 minutes while the charge fell about 62 mAh per
 * 5 minutes (OBSERVED through Windows, docs/GAOKUN3-PHASE1-GUARDS.md). One
 * snapshot cannot see that, so these functions watch consecutive records.
 *
 * While the records claim AC, the remaining charge (EC 0x94, mAh) must not
 * fall TREND_DROP_MAH below its latest high within TREND_WINDOW_MS; a high
 * older than the window is replaced by the current charge. That trips at a
 * net drain of about 5 mAh/min (about 2.5 W at 8.3 V). The same day's held
 * state on AC sagged about 10 mAh in 9.5 minutes, about 1 mAh/min, and the
 * weak source tripped it after about 2.5 minutes. Both limits are policy fitted
 * to those readings, through Windows' mWh at a fixed voltage, not to the EC's
 * own word; the EC's 1 mAh resolution is INFERRED from its driver. */
#define GAOKUN3_BATTERY_TREND_ABI 1u
#define GAOKUN3_BATTERY_TREND_WINDOW_MS 300000u
#define GAOKUN3_BATTERY_TREND_DROP_MAH 25u
/* The caller must feed a record at least this often; a longer gap leaves a
 * drain unwatched for that long, so it trips STALE rather than resume. */
#define GAOKUN3_BATTERY_TREND_MAX_GAP_MS 30000u

/* Owned by one caller, zeroed by _init; every field is the functions'. */
struct gaokun3_battery_trend {
    uint32_t abi;
    uint32_t reason;               /* OK, or the latched trip */
    uint32_t fed, watching;        /* any record yet; AC claimed continuously since watch_ms */
    uint64_t last_ms, watch_ms;
    uint64_t high_ms;              /* when the charge last reached high_mah */
    uint32_t high_mah, last_mah;
    uint32_t worst_drop_mah;       /* largest fall below a high seen, for the report */
};

void gaokun3_battery_trend_init(struct gaokun3_battery_trend *trend);
/* Feeds one record decoded at now_ms (monotonic; the caller's polling
 * clock) and returns OK or the latched trip. Trips, after which every call
 * returns the same reason and changes nothing:
 * - the record's own refusal, when it is not plausible;
 * - DRAIN, as above;
 * - STALE, a gap over TREND_MAX_GAP_MS since the previous record;
 * - CONFIG, a NULL argument, a bad ABI, or now_ms before the previous call.
 * A record without AC ends the watch without a trip: the charge rule, not
 * this check, covers a battery that is discharging openly. */
unsigned gaokun3_battery_trend_feed(struct gaokun3_battery_trend *trend,
                                    const struct gaokun3_battery_record *record, uint64_t now_ms);
/* gaokun3_battery_admit() with the trend applied: the trend must not have
 * tripped, and when only AC would admit (the charge is under minimum) the
 * AC claim must have been watched without a drain for a whole window, else
 * UNWATCHED. record must be the one last fed to trend. Stricter than
 * cycle_lib's battery_ok(), which has no history. */
unsigned gaokun3_battery_admit_watched(const struct gaokun3_battery_record *record,
                                       const struct gaokun3_battery_trend *trend, unsigned minimum);
