/* SPDX-License-Identifier: MIT
 * Huawei MateBook E Go (gaokun3) battery/AC admission policy, decided from one
 * snapshot of the embedded controller's battery registers. Provenance,
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
