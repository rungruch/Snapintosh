/* SPDX-License-Identifier: MIT
 * SC8280XP (Huawei MateBook E Go, gaokun3) read-only thermal policy.
 * Provenance, evidence classes and what is still unmeasured:
 * docs/GAOKUN3-PHASE1-GUARDS.md.
 *
 * Host-only today: no monitor build links this file, so the A16 baseline is
 * unchanged. It follows compat/a16-thermal.c's policy (same reasons, limit,
 * grace and admission rules) on the SC8280XP's four tsens-v2 controllers. The
 * two stay separate until the A16 guard's retained-capture replay can prove a
 * shared tsens-v2 implementation byte-equivalent on the A16; that capture is
 * not in the tree, so folding them now could not be checked. */
#pragma once
#include <stdint.h>

#define SC8280XP_THERMAL_MAGIC UINT64_C(0x314d524548543853) /* S8THERM1 */
#define SC8280XP_THERMAL_ABI 1u
#define SC8280XP_THERMAL_BLOCKS 4u
#define SC8280XP_THERMAL_SLOTS 64u      /* 16 per controller, the tsens-v2 maximum */
#define SC8280XP_THERMAL_SENSORS 46u    /* 14 + 16 + 11 + 5, the DT's #qcom,sensors */
/* Carried over from the A16 rule (user, 2026-10-19: halt at 95 C). It is below
 * the 110 C critical trip of every tsens zone in the gaokun3 DT, but above the
 * GPU zone's 85 C passive trip and mem-thermal's 90 C hot trip (tsens1 sensor
 * 15). Under the monitor no OS throttles at those lower trips, so this limit is
 * the only action. The fanless tablet's owner may choose a lower one. */
#define SC8280XP_THERMAL_LIMIT_MC 95000
#define SC8280XP_THERMAL_ADMIT_SAMPLES 20u
/* The SROT version word all four controllers must report. Nothing has read it
 * on a gaokun3 yet, and the A16's value is a different revision, so it is 0:
 * not measured, and the guard refuses every poll (SC8280XP_THERMAL_UNPINNED).
 * Acceptance to change it: a recorded read-only measurement of SROT+0 on all
 * four controllers, written into docs/GAOKUN3-PHASE1-GUARDS.md. */
#define SC8280XP_THERMAL_VERSION_PIN 0u
#define SC8280XP_THERMAL_REPORT_WORDS 173u

/* The first eleven match enum a16_thermal_reason, so reports read the same. */
enum sc8280xp_thermal_reason {
    SC8280XP_THERMAL_OK, SC8280XP_THERMAL_HOT, SC8280XP_THERMAL_CONFIG,
    SC8280XP_THERMAL_INVALID, SC8280XP_THERMAL_LATE, SC8280XP_THERMAL_READ,
    SC8280XP_THERMAL_RANGE, SC8280XP_THERMAL_CRITICAL, SC8280XP_THERMAL_UNCHANGED,
    SC8280XP_THERMAL_SLOW, SC8280XP_THERMAL_CLOCK, SC8280XP_THERMAL_UNPINNED
};

/* Internal state is also the report: fixed-width words for a monitor-only
 * page. Signed temperatures are sign extended. Slot = controller * 16 + sensor. */
struct sc8280xp_thermal_state {
    uint64_t magic, abi, bytes, polls, reads, started, last_poll, next_poll;
    uint64_t last_change, trip_ticks, reason, location;
    int64_t current_mc, peak_mc;
    uint64_t valid_count, missing_count, max_gap_ticks, invalid_samples, read_failures;
    uint64_t poll_ticks, max_poll_ticks, ready, unchanged_ticks, read_inflight, pin;
    uint64_t version[4], control[4], trdy[4], valid_mask[4], seen_mask[4];
    uint64_t last_valid[64], raw[64];
};

struct sc8280xp_thermal_io {
    void *ctx;
    /* One 32-bit MMIO read. Nonzero stops the run; a failed read is never retried. */
    int (*read32)(void *ctx, uint64_t address, uint32_t *value);
    uint64_t (*counter)(void *ctx);
    /* Called exactly once, on the first trip. After it the guard does no MMIO. */
    void (*stop)(void *ctx, unsigned reason, uint64_t location, uint64_t now);
    uint32_t version_pin;           /* SC8280XP_THERMAL_VERSION_PIN in production */
};

/* Whether the guard may ever read this physical address: the four SROT version
 * and control words, the four TRDY words, and each controller's declared
 * sensor status words. Nothing else. */
int sc8280xp_thermal_address_allowed(uint64_t address);
/* Ticks until the next poll is due, at most limit; 0 when tripped or unset. */
uint64_t sc8280xp_thermal_wait(const struct sc8280xp_thermal_state *, uint64_t now,
                               uint64_t hz, uint64_t limit);
/* 0: polled or not yet due. 1: tripped (now or earlier). A zeroed state is
 * initialised on the first call. hz is the counter rate. */
int sc8280xp_thermal_poll(struct sc8280xp_thermal_state *, uint64_t now, uint64_t hz,
                          const struct sc8280xp_thermal_io *);
/* 0: one complete 46-sensor sample. 1: tripped. -1: no complete sample within
 * SC8280XP_THERMAL_ADMIT_SAMPLES attempts or half a second. */
int sc8280xp_thermal_admit(struct sc8280xp_thermal_state *, uint64_t hz,
                           const struct sc8280xp_thermal_io *);
