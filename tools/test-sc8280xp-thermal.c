/* SPDX-License-Identifier: MIT
 * Host test for compat/sc8280xp-thermal.c on a synthetic tsens bus: the
 * policy's trips, the faulty readings it must refuse, its stop callback and
 * its MMIO allowlist. No hardware; the version word is a synthetic pin, since
 * the real one has not been measured (docs/GAOKUN3-PHASE1-GUARDS.md).
 * Built and run by tools/test-sc8280xp-thermal.py. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "sc8280xp-thermal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define HZ UINT64_C(19200000)       /* CNTFRQ_EL0 the gaokun3 reported (el2qual stage 3) */
#define PIN UINT32_C(0x5a5a0001)    /* synthetic: not a value any tsens is known to report */
#define VALID (1u << 21)
#define CRIT (1u << 19)
#define CTRL_OK(count) ((1u << 0) | (1u << 21) | ((((1u << (count)) - 1)) << 3))
#define CEIL (SC8280XP_THERMAL_LIMIT_MC / 100)

/* A second transcription of the DT table, so the allowlist is checked against
 * something other than itself; the .py driver checks both against the DTB. */
static const struct { uint64_t tm, srot; unsigned sensors; } dt[4] = {
    {UINT64_C(0x0c263000), UINT64_C(0x0c222000), 14u},
    {UINT64_C(0x0c265000), UINT64_C(0x0c223000), 16u},
    {UINT64_C(0x0c251000), UINT64_C(0x0c224000), 11u},
    {UINT64_C(0x0c252000), UINT64_C(0x0c225000), 5u},
};
#define SENSOR(b, n) (dt[b].tm + 0xa0 + 4 * (n))
#define SROT(b) (dt[b].srot)
#define PSHOLD UINT64_C(0x0c264000)  /* restart@c264000 sits between tsens0 and tsens1 */

struct bus {
    uint32_t version[4], control[4], trdy[4], status[4][16];
    uint64_t now, slow, step, reads, fail;
    uint64_t flick[2], flick_n[2], hot_addr, hot_poll;
    unsigned stops, stop_reason;
    uint64_t stop_where, stop_now;
    uint32_t pin;
};
static struct bus baseline;
static struct sc8280xp_thermal_state s;

/* Brief VALID clears for flick_n reads of flick[i]; a reading at the limit
 * from poll hot_poll on. */
static void mutate(struct bus *b, uint64_t a, uint32_t *v) {
    for (unsigned i = 0; i < 2; i++)
        if (a == b->flick[i] && b->flick_n[i]) { b->flick_n[i]--; *v &= ~VALID; }
    if (a == b->hot_addr && s.polls >= b->hot_poll) *v = VALID | CEIL;
}

static int read_word(void *ctx, uint64_t a, uint32_t *v) {
    struct bus *b = ctx;
    CHECK(!b->stops);                        /* no MMIO once a stop was requested */
    CHECK(sc8280xp_thermal_address_allowed(a));
    b->reads++;
    if (a == b->fail) return -1;
    for (unsigned i = 0; i < 4; i++) {
        if (a == dt[i].srot) { *v = b->version[i]; return 0; }
        if (a == dt[i].srot + 4) { *v = b->control[i]; return 0; }
        if (a == dt[i].tm + 0xe4) { *v = b->trdy[i]; return 0; }
        if (a >= SENSOR(i, 0) && a < SENSOR(i, dt[i].sensors) && !(a & 3)) {
            *v = b->status[i][(a - SENSOR(i, 0)) / 4];
            mutate(b, a, v);
            return 0;
        }
    }
    CHECK(0);                                /* outside the DT's declared words */
    return -1;
}

static uint64_t counter(void *ctx) {
    struct bus *b = ctx;
    uint64_t t = b->now + b->slow;
    b->now += b->step;
    return t;
}

static void on_stop(void *ctx, unsigned reason, uint64_t where, uint64_t now) {
    struct bus *b = ctx;
    b->stops++; b->stop_reason = reason; b->stop_where = where; b->stop_now = now;
}

static struct sc8280xp_thermal_io io_for(struct bus *b) {
    return (struct sc8280xp_thermal_io){b, read_word, counter, on_stop, b->pin};
}
static int step(struct bus *b) { struct sc8280xp_thermal_io io = io_for(b); return sc8280xp_thermal_poll(&s, b->now, HZ, &io); }
static int admit(struct bus *b) { struct sc8280xp_thermal_io io = io_for(b); return sc8280xp_thermal_admit(&s, HZ, &io); }

static void reset(struct bus *b, uint64_t now) {
    *b = baseline; b->now = now; memset(&s, 0, sizeof s);
}

/* The trip is the stop: exactly one callback with the recorded reason and
 * location, then no MMIO and no second callback however often it is polled. */
static void fired(struct bus *b, const char *name, unsigned reason, uint64_t where) {
    CHECK(s.reason == reason && s.location == where && !s.ready);
    CHECK(b->stops == 1 && b->stop_reason == reason && b->stop_where == where);
    uint64_t reads = b->reads;
    b->now += HZ / 4; CHECK(step(b));
    b->now += 2 * HZ; CHECK(step(b));
    CHECK(b->reads == reads && b->stops == 1 && sc8280xp_thermal_wait(&s, b->now, HZ, HZ) == 0);
    printf("{\"case\":\"%s\",\"reason\":%u,\"where\":\"0x%llx\",\"polls\":%llu,\"reads\":%llu}\n", name, reason,
           (unsigned long long)where, (unsigned long long)s.polls, (unsigned long long)s.reads);
}

static void admitted(const char *name, int result, uint64_t start, uint64_t end) {
    printf("{\"admit\":\"%s\",\"result\":%d,\"reason\":%llu,\"polls\":%llu,\"invalid_samples\":%llu,\"valid\":%llu,\"elapsed_ticks\":%llu}\n",
           name, result, (unsigned long long)s.reason, (unsigned long long)s.polls,
           (unsigned long long)s.invalid_samples, (unsigned long long)s.valid_count,
           (unsigned long long)(end - start));
}

static int expected_allowed(uint64_t a) {
    if (a & 3) return 0;
    for (unsigned i = 0; i < 4; i++)
        if (a == dt[i].srot || a == dt[i].srot + 4 || a == dt[i].tm + 0xe4 ||
            (a >= SENSOR(i, 0) && a < SENSOR(i, dt[i].sensors)))
            return 1;
    return 0;
}

static void check_allowlist(void) {
    unsigned allowed = 0;
    for (uint64_t a = UINT64_C(0x0c220000); a < UINT64_C(0x0c270000); a++) {
        int got = sc8280xp_thermal_address_allowed(a);
        CHECK(got == expected_allowed(a));
        allowed += (unsigned)got;
    }
    CHECK(allowed == 4 * 3 + SC8280XP_THERMAL_SENSORS);
    /* The enumeration is the allowlist exactly: every word allowed, none
     * repeated, as many as the scan found, and a short buffer never overrun. */
    uint64_t words[SC8280XP_THERMAL_WORDS + 1] = {0};
    CHECK(sc8280xp_thermal_words(words, SC8280XP_THERMAL_WORDS + 1) == SC8280XP_THERMAL_WORDS);
    CHECK(SC8280XP_THERMAL_WORDS == allowed && words[SC8280XP_THERMAL_WORDS] == 0);
    for (unsigned i = 0; i < SC8280XP_THERMAL_WORDS; i++) {
        CHECK(expected_allowed(words[i]));
        for (unsigned j = 0; j < i; j++) CHECK(words[j] != words[i]);
    }
    CHECK(words[0] == SROT(0) && words[1] == SROT(0) + 4 && words[2] == dt[0].tm + 0xe4 &&
          words[3] == SENSOR(0, 0) && words[SC8280XP_THERMAL_WORDS - 1] == SENSOR(3, 4));
    uint64_t three[3] = {0};
    CHECK(sc8280xp_thermal_words(three, 2) == SC8280XP_THERMAL_WORDS && three[2] == 0);
    CHECK(sc8280xp_thermal_words(NULL, 0) == SC8280XP_THERMAL_WORDS);
    CHECK(!sc8280xp_thermal_address_allowed(PSHOLD));
    for (unsigned i = 0; i < 4; i++) {
        CHECK(!sc8280xp_thermal_address_allowed(SENSOR(i, dt[i].sensors)));  /* one past the declared count */
        CHECK(!sc8280xp_thermal_address_allowed(SENSOR(i, 0) - 4));
        CHECK(!sc8280xp_thermal_address_allowed(dt[i].srot + 8));
        CHECK(!sc8280xp_thermal_address_allowed(dt[i].tm));                  /* TM+0: not read */
        CHECK(!sc8280xp_thermal_address_allowed(dt[i].tm + 0xe4 + 1));
        CHECK(!sc8280xp_thermal_address_allowed(dt[i].srot + (UINT64_C(1) << 32)));
    }
    puts("{\"case\":\"allowlist\",\"allowed_words\":58}");
}

int main(void) {
    CHECK(sizeof(struct sc8280xp_thermal_state) == SC8280XP_THERMAL_REPORT_WORDS * 8);
    unsigned total = 0;
    for (unsigned i = 0; i < 4; i++) {
        baseline.version[i] = PIN;
        baseline.control[i] = CTRL_OK(dt[i].sensors);
        baseline.trdy[i] = 1;
        for (unsigned n = 0; n < dt[i].sensors; n++)
            baseline.status[i][n] = VALID | (300 + i * 16 + n);  /* 30.0 .. 35.2 C */
        total += dt[i].sensors;
    }
    CHECK(total == SC8280XP_THERMAL_SENSORS);
    baseline.pin = PIN;
    check_allowlist();

    struct bus b;
    uint64_t reads, t0;
    int r;

    /* The production pin is unmeasured (0): refuse before any MMIO. */
    reset(&b, 0); b.pin = 0; CHECK(step(&b) && b.reads == 0);
    fired(&b, "unpinned", SC8280XP_THERMAL_UNPINNED, 0);
    reset(&b, 0); b.pin = SC8280XP_THERMAL_VERSION_PIN;
    if (!SC8280XP_THERMAL_VERSION_PIN) {
        CHECK(admit(&b) == 1 && b.reads == 0);
        fired(&b, "production-pin", SC8280XP_THERMAL_UNPINNED, 0);
    }

    /* Baseline: all 46 sensors valid, ready, peak 35.2 C; a poll not yet due reads nothing. */
    reset(&b, 0); CHECK(!step(&b));
    CHECK(s.ready && s.valid_count == 46 && s.missing_count == 0 && s.peak_mc == 35200 && s.current_mc == 35200);
    CHECK(b.reads == 4 * 3 + 46 && s.reads == b.reads && s.pin == PIN && s.bytes == sizeof s);
    reads = b.reads; b.now = HZ / 8; CHECK(!step(&b) && b.reads == reads);
    CHECK(sc8280xp_thermal_wait(&s, b.now, HZ, HZ) == HZ / 8);
    CHECK(sc8280xp_thermal_wait(&s, b.now, HZ, 5) == 5);

    /* One step under the limit runs; the limit trips at that sensor. */
    b.now = HZ / 4; b.status[0][0] = VALID | (CEIL - 1); CHECK(!step(&b));
    CHECK(s.ready && s.current_mc == SC8280XP_THERMAL_LIMIT_MC - 100);
    b.now = HZ / 2; b.status[1][15] = VALID | CEIL; CHECK(step(&b));
    fired(&b, "hot-mem-thermal", SC8280XP_THERMAL_HOT, SENSOR(1, 15));
    CHECK(b.stop_now == HZ / 2);

    /* The last sensor read, across a counter wrap. */
    reset(&b, UINT64_MAX - HZ / 8); CHECK(!step(&b));
    b.now += HZ / 4; b.status[3][4] = VALID | CEIL; CHECK(step(&b));
    fired(&b, "hot-wrap", SC8280XP_THERMAL_HOT, SENSOR(3, 4));

    /* The sensor's own critical flag trips at any temperature. */
    reset(&b, 0); b.status[2][2] |= CRIT; CHECK(step(&b));
    fired(&b, "critical-gpu", SC8280XP_THERMAL_CRITICAL, SENSOR(2, 2));

    /* Signed decode: -1.0 C is in range; 120.1 C and -40.1 C are not. */
    reset(&b, 0); b.status[0][0] = VALID | 0xff6; CHECK(!step(&b));
    CHECK((s.raw[0] & 0xfff) == 0xff6 && s.ready && s.peak_mc == 35200);
    reset(&b, 0); b.status[0][5] = VALID | 1201; CHECK(step(&b));
    fired(&b, "range-high", SC8280XP_THERMAL_RANGE, SENSOR(0, 5));
    reset(&b, 0); b.status[3][0] = VALID | ((4096 - 401) & 0xfff); CHECK(step(&b));
    fired(&b, "range-low", SC8280XP_THERMAL_RANGE, SENSOR(3, 0));

    /* Version: one controller off the pin is a different block, not a tsens to trust. */
    reset(&b, 0); b.version[1] = PIN ^ 1; CHECK(step(&b));
    fired(&b, "version", SC8280XP_THERMAL_CONFIG, SROT(1));

    /* Control: disabled, in reset, ADC-code mode, or a declared sensor not enabled. */
    reset(&b, 0); b.control[3] &= ~1u; CHECK(step(&b));
    fired(&b, "ctrl-en", SC8280XP_THERMAL_CONFIG, SROT(3) + 4);
    reset(&b, 0); b.control[0] |= 1u << 1; CHECK(step(&b));
    fired(&b, "ctrl-sw-rst", SC8280XP_THERMAL_CONFIG, SROT(0) + 4);
    reset(&b, 0); b.control[2] &= ~(1u << 21); CHECK(step(&b));
    fired(&b, "ctrl-code", SC8280XP_THERMAL_CONFIG, SROT(2) + 4);
    reset(&b, 0); b.control[0] &= ~(1u << (3 + 13)); CHECK(step(&b));
    fired(&b, "ctrl-sensor-off", SC8280XP_THERMAL_CONFIG, SROT(0) + 4);
    reset(&b, 0); b.control[3] |= 0xffffu << 3; CHECK(!step(&b) && s.ready);  /* extra enables are allowed */

    /* A failed read is never retried and names its address. */
    reset(&b, 0); b.fail = SROT(0); CHECK(step(&b) && b.reads == 1);
    fired(&b, "read-first", SC8280XP_THERMAL_READ, SROT(0));
    reset(&b, 0); b.fail = SENSOR(2, 10); CHECK(step(&b) && s.read_failures == 1 && s.read_inflight);
    fired(&b, "read-sensor", SC8280XP_THERMAL_READ, SENSOR(2, 10));
    reset(&b, 0); b.fail = dt[1].tm + 0xe4; CHECK(step(&b));
    fired(&b, "read-trdy", SC8280XP_THERMAL_READ, dt[1].tm + 0xe4);

    /* VALID clear: tolerated under a second, a trip at one second. */
    reset(&b, 0); CHECK(!step(&b)); b.status[1][15] &= ~VALID;
    for (int i = 1; i < 4; i++) { b.now = i * HZ / 4; CHECK(!step(&b) && !s.ready && s.valid_count == 45); }
    b.now = HZ; CHECK(step(&b));
    fired(&b, "invalid", SC8280XP_THERMAL_INVALID, SENSOR(1, 15));
    reset(&b, 0); CHECK(!step(&b)); b.status[1][15] &= ~VALID; b.now = HZ / 4; CHECK(!step(&b) && !s.ready);
    b.status[1][15] |= VALID; b.now = HZ / 2; CHECK(!step(&b) && s.ready);

    /* Timing: a one-second gap, a slow poll, a frozen register file, the clock. */
    reset(&b, 0); CHECK(!step(&b)); b.now = HZ; CHECK(step(&b));
    fired(&b, "late", SC8280XP_THERMAL_LATE, 0);
    reset(&b, 0); b.slow = HZ / 4; CHECK(step(&b));
    fired(&b, "slow", SC8280XP_THERMAL_SLOW, 0);
    CHECK(b.stop_now == HZ / 4);
    reset(&b, 0); CHECK(!step(&b));
    for (int i = 1; i < 40; i++) { b.now = i * HZ / 4; CHECK(!step(&b)); }
    b.now = 10 * HZ; CHECK(step(&b));
    fired(&b, "unchanged", SC8280XP_THERMAL_UNCHANGED, 0);
    reset(&b, 0); CHECK(!step(&b));
    for (int i = 1; i <= 48; i++) { b.now = i * HZ / 4; b.status[3][2] ^= 1; CHECK(!step(&b)); }
    CHECK(s.ready && !s.reason);
    reset(&b, 0); { struct sc8280xp_thermal_io io = io_for(&b); CHECK(sc8280xp_thermal_poll(&s, 0, 0, &io)); }
    CHECK(b.reads == 0); fired(&b, "clock-zero", SC8280XP_THERMAL_CLOCK, 0);
    reset(&b, 0); { struct sc8280xp_thermal_io io = io_for(&b); CHECK(sc8280xp_thermal_poll(&s, 0, UINT64_C(2000000000), &io)); }
    CHECK(b.reads == 0 && s.reason == SC8280XP_THERMAL_CLOCK && b.stops == 1);

    /* Bad interfaces and a corrupted state fail closed. */
    reset(&b, 0); CHECK(sc8280xp_thermal_poll(&s, 0, HZ, NULL) && s.reason == SC8280XP_THERMAL_CONFIG && !b.stops);
    reset(&b, 0); { struct sc8280xp_thermal_io io = io_for(&b); io.stop = NULL; CHECK(sc8280xp_thermal_poll(&s, 0, HZ, &io)); }
    CHECK(s.reason == SC8280XP_THERMAL_CONFIG && b.reads == 0 && !b.stops);
    reset(&b, 0); { struct sc8280xp_thermal_io io = io_for(&b); io.read32 = NULL; CHECK(sc8280xp_thermal_poll(&s, 0, HZ, &io)); }
    CHECK(b.reads == 0); fired(&b, "no-read32", SC8280XP_THERMAL_CONFIG, 0);
    reset(&b, 0); { struct sc8280xp_thermal_io io = io_for(&b); io.counter = NULL; CHECK(sc8280xp_thermal_poll(&s, 0, HZ, &io)); }
    CHECK(b.reads == 0 && s.reason == SC8280XP_THERMAL_CONFIG && b.stops == 1);
    reset(&b, 0); CHECK(!step(&b)); s.abi = SC8280XP_THERMAL_ABI + 1; reads = b.reads; b.now = HZ / 4; CHECK(step(&b));
    CHECK(b.reads == reads); fired(&b, "abi", SC8280XP_THERMAL_CONFIG, 0);

    /* Admission resamples a brief clear and nothing else. */
    reset(&b, 0); b.flick[0] = SENSOR(1, 3); b.flick_n[0] = 1; CHECK(!step(&b) && !s.ready && s.valid_count == 45);
    reset(&b, 0); b.step = HZ / 1000; b.flick[0] = SENSOR(1, 3); b.flick_n[0] = 1; t0 = b.now; r = admit(&b);
    admitted("single-clear", r, t0, b.now);
    CHECK(r == 0 && s.ready && s.valid_count == 46 && s.polls == 2 && s.invalid_samples == 1 && !s.reason && !b.stops);
    CHECK(s.next_poll == s.last_poll + HZ / 4);
    reset(&b, 0); b.step = HZ / 1000; b.flick[0] = SENSOR(2, 10); b.flick_n[0] = 3; b.flick[1] = SENSOR(3, 4); b.flick_n[1] = 2;
    t0 = b.now; r = admit(&b); admitted("two-sensors", r, t0, b.now);
    CHECK(r == 0 && s.ready && s.polls == 4 && s.invalid_samples == 5);
    reset(&b, 0); b.step = HZ / 1000; b.flick[0] = SENSOR(0, 13); b.flick_n[0] = UINT64_MAX; t0 = b.now; r = admit(&b);
    admitted("never-valid", r, t0, b.now);
    CHECK(r == -1 && !s.ready && s.polls == SC8280XP_THERMAL_ADMIT_SAMPLES && !s.reason && s.last_poll - t0 < HZ / 2);
    b.step = 0; b.now = s.started + HZ; CHECK(step(&b));
    fired(&b, "never-valid-runtime", SC8280XP_THERMAL_INVALID, SENSOR(0, 13));
    reset(&b, 0); b.step = HZ / 20; b.flick[0] = SENSOR(0, 13); b.flick_n[0] = UINT64_MAX; t0 = b.now; r = admit(&b);
    admitted("time-bound", r, t0, b.now);
    CHECK(r == -1 && s.polls >= 2 && s.polls < SC8280XP_THERMAL_ADMIT_SAMPLES && s.last_poll - t0 < HZ / 2 && !s.reason);
    reset(&b, 0); b.flick[0] = SENSOR(0, 13); b.flick_n[0] = UINT64_MAX; r = admit(&b);
    admitted("stuck-counter", r, 0, b.now);
    CHECK(r == -1 && s.polls == SC8280XP_THERMAL_ADMIT_SAMPLES);
    reset(&b, 0); b.step = HZ / 1000; b.flick[0] = SENSOR(1, 3); b.flick_n[0] = 1; b.hot_addr = SENSOR(0, 0); b.hot_poll = 2;
    t0 = b.now; r = admit(&b); admitted("hot-retry", r, t0, b.now);
    CHECK(r == 1 && s.polls == 2);
    fired(&b, "hot-retry", SC8280XP_THERMAL_HOT, SENSOR(0, 0));
    reset(&b, 0); CHECK(!step(&b) && s.ready); reads = b.reads; b.now = HZ / 8; CHECK(!admit(&b) && b.reads == reads && s.polls == 1);
    reset(&b, 0); b.flick[0] = SENSOR(1, 3); b.flick_n[0] = 1; CHECK(!step(&b) && !s.ready); b.now = HZ / 8; b.step = HZ / 1000;
    CHECK(!admit(&b) && s.ready && s.polls == 2 && s.next_poll == s.last_poll + HZ / 4);
    reset(&b, 0); CHECK(!step(&b)); b.now = HZ; r = admit(&b); CHECK(r == 1);
    fired(&b, "admit-late", SC8280XP_THERMAL_LATE, 0);
    reset(&b, UINT64_MAX - HZ / 100); b.step = HZ / 1000; b.flick[0] = SENSOR(0, 0); b.flick_n[0] = 2; t0 = b.now; r = admit(&b);
    admitted("counter-wrap", r, t0, b.now);
    CHECK(r == 0 && s.ready && s.polls == 3);
    reset(&b, 0); { struct sc8280xp_thermal_io io = io_for(&b); CHECK(sc8280xp_thermal_admit(&s, 0, &io) == 1); }
    CHECK(b.reads == 0 && s.reason == SC8280XP_THERMAL_CLOCK);
    reset(&b, 0); CHECK(sc8280xp_thermal_admit(&s, HZ, NULL) == 1 && s.reason == SC8280XP_THERMAL_CONFIG && !b.reads);

    puts("PASS: sc8280xp thermal policy on a synthetic tsens bus");
    return 0;
}
