/* SPDX-License-Identifier: MIT
 * Host test for compat/gaokun3-battery.c: EC snapshots, including the faulty
 * readings the policy must refuse, decoded and admitted at every minimum.
 * Each case prints one JSON line; tools/test-gaokun3-battery.py replays the
 * records through tools/cycle_lib/common.py's battery_ok() and requires the
 * same verdict. Values are synthetic except where a comment says OBSERVED. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "gaokun3-battery.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define T0 UINT64_C(1000000)            /* ms */
#define NOW (T0 + 500)
#define FULL 4483u                      /* OBSERVED: gaokun-android healthd fc=4483000 uAh */
#define AC_PRESENT (GAOKUN3_ADP_AC | GAOKUN3_ADP_PRESENT)
#define BATTERY_ONLY GAOKUN3_ADP_PRESENT
#define DIS GAOKUN3_BST_DISCHARGING
#define CHG GAOKUN3_BST_CHARGING
#define CRT GAOKUN3_BST_CRITICAL
#define FUL GAOKUN3_BST_FULL

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static struct gaokun3_battery_snapshot snap(unsigned adp, unsigned bst, unsigned percent, unsigned mv,
                                            unsigned cap, unsigned full, int rate) {
    struct gaokun3_battery_snapshot s = {.have = GAOKUN3_HAVE_ALL, .adp = (uint8_t)adp, .bst = (uint8_t)bst, .read_ms = T0};
    put16(s.status + 0, percent);
    put16(s.status + 2, mv);
    put16(s.status + 4, cap);
    put16(s.status + 6, full);
    put16(s.status + 10, (uint32_t)rate & 0xffffu);
    return s;
}
/* Capacity for a gauge reading of percent, on the observed full capacity. */
static unsigned cap_at(unsigned percent) { return (percent * FULL + 50) / 100; }
static struct gaokun3_battery_snapshot at(unsigned adp, unsigned bst, unsigned percent) {
    return snap(adp, bst, percent, 6000 + 28 * percent, cap_at(percent), FULL, 0);
}

static unsigned cases;

/* Decode one snapshot, check its reason and AC reading, and check admission
 * at every minimum against the rule written out longhand. */
static struct gaokun3_battery_record run(const char *name, struct gaokun3_battery_snapshot s, uint64_t now,
                                         unsigned want, int want_ac) {
    struct gaokun3_battery_record r;
    unsigned why = gaokun3_battery_decode(&s, now, &r);
    if (why != want) fprintf(stderr, "%s: reason %u, want %u\n", name, why, want);
    CHECK(why == want && r.reason == why && r.abi == GAOKUN3_BATTERY_ABI);
    CHECK(r.plausible == (why == GAOKUN3_BATTERY_OK));
    CHECK(r.status == (s.have == GAOKUN3_HAVE_ALL ? 0u : 1u));
    if (want_ac >= 0) CHECK(r.ac == (uint32_t)want_ac);
    printf("{\"case\":\"%s\",\"reason\":%u,\"status\":%u,\"plausible\":%u,\"ac\":%u,\"percent\":%u,\"verdicts\":[",
           name, why, r.status, r.plausible, r.ac, r.percent);
    for (unsigned m = GAOKUN3_BATTERY_FLOOR_PERCENT; m <= 100; m++) {
        unsigned v = gaokun3_battery_admit(&r, m);
        unsigned expect = why != GAOKUN3_BATTERY_OK ? why
                        : r.ac || r.percent >= m ? GAOKUN3_BATTERY_OK : GAOKUN3_BATTERY_LOW;
        CHECK(v == expect);
        printf("%s%u", m == GAOKUN3_BATTERY_FLOOR_PERCENT ? "" : ",", v);
    }
    puts("]}");
    CHECK(gaokun3_battery_admit(&r, GAOKUN3_BATTERY_FLOOR_PERCENT - 1) == GAOKUN3_BATTERY_CONFIG);
    CHECK(gaokun3_battery_admit(&r, 101) == GAOKUN3_BATTERY_CONFIG);
    CHECK(gaokun3_battery_admit(&r, 0) == GAOKUN3_BATTERY_CONFIG);
    cases++;
    return r;
}

int main(void) {
    struct gaokun3_battery_snapshot s;
    struct gaokun3_battery_record r;

    /* Admitted readings. 0x02 charging at 92% on AC is OBSERVED (gaokun-android ec_raw). */
    r = run("charging-92", snap(AC_PRESENT, CHG, 92, 8350, cap_at(92), FULL, 60), NOW, GAOKUN3_BATTERY_OK, 1);
    CHECK(r.voltage_mv == 8350 && r.capacity_mah == cap_at(92) && r.full_mah == FULL && r.rate_ma == 60 && r.age_ms == 500);
    run("ac-idle-80", at(AC_PRESENT, 0, 80), NOW, GAOKUN3_BATTERY_OK, 1);
    run("ac-full", at(AC_PRESENT, FUL, 100), NOW, GAOKUN3_BATTERY_OK, 1);
    run("battery-75", at(BATTERY_ONLY, DIS, 75), NOW, GAOKUN3_BATTERY_OK, 0);
    run("battery-50", at(BATTERY_ONLY, DIS, 50), NOW, GAOKUN3_BATTERY_OK, 0);
    run("battery-49", at(BATTERY_ONLY, DIS, 49), NOW, GAOKUN3_BATTERY_OK, 0);
    run("battery-0", at(BATTERY_ONLY, DIS, 0), NOW, GAOKUN3_BATTERY_OK, 0);
    run("battery-idle-60", at(BATTERY_ONLY, 0, 60), NOW, GAOKUN3_BATTERY_OK, 0);
    run("unplugged-still-full", at(BATTERY_ONLY, DIS | FUL, 100), NOW, GAOKUN3_BATTERY_OK, 0);
    /* An adapter that cannot carry the load: the EC discharges, so it is not AC. */
    run("ac-discharging-30", at(AC_PRESENT, DIS, 30), NOW, GAOKUN3_BATTERY_OK, 0);
    /* Known limitation (gaokun-android BATT-1): a weak USB source the EC reports
     * as charging while the battery drains. One snapshot cannot tell; it counts as AC. */
    run("weak-source-charging-10", at(AC_PRESENT, CHG, 10), NOW, GAOKUN3_BATTERY_OK, 1);
    s = at(AC_PRESENT, CHG, 92); put16(s.status + 8, 0xffff); put16(s.status + 12, 0xffff);
    run("unknown-words-ignored", s, NOW, GAOKUN3_BATTERY_OK, 1);
    s = at(BATTERY_ONLY, DIS, 75); put16(s.status + 10, 0xffc7);
    r = run("rate-negative", s, NOW, GAOKUN3_BATTERY_OK, 0);
    CHECK(r.rate_ma == -57);
    run("age-limit", at(BATTERY_ONLY, DIS, 75), T0 + GAOKUN3_BATTERY_MAX_AGE_MS, GAOKUN3_BATTERY_OK, 0);
    run("gauge-10-apart", snap(BATTERY_ONLY, DIS, 60, 7700, cap_at(70), FULL, 0), NOW, GAOKUN3_BATTERY_OK, 0);
    run("voltage-5000", snap(BATTERY_ONLY, DIS, 60, 5000, cap_at(60), FULL, 0), NOW, GAOKUN3_BATTERY_OK, 0);
    run("voltage-9000", snap(AC_PRESENT, FUL, 100, 9000, FULL, FULL, 0), NOW, GAOKUN3_BATTERY_OK, 1);
    run("full-1000", snap(BATTERY_ONLY, DIS, 60, 7700, 600, 1000, 0), NOW, GAOKUN3_BATTERY_OK, 0);
    run("full-10000", snap(BATTERY_ONLY, DIS, 60, 7700, 6000, 10000, 0), NOW, GAOKUN3_BATTERY_OK, 0);

    /* Incomplete reads: nothing from them is relied on. */
    s = at(AC_PRESENT, CHG, 92); s.have &= ~GAOKUN3_HAVE_ADP;
    run("read-adp", s, NOW, GAOKUN3_BATTERY_READ, 0);
    s = at(AC_PRESENT, CHG, 92); s.have &= ~GAOKUN3_HAVE_BST;
    run("read-bst", s, NOW, GAOKUN3_BATTERY_READ, 1);
    s = at(AC_PRESENT, CHG, 92); s.have &= ~GAOKUN3_HAVE_STATUS;
    r = run("read-status", s, NOW, GAOKUN3_BATTERY_READ, 1);
    CHECK(r.percent == 0 && r.voltage_mv == 0);
    s = at(AC_PRESENT, CHG, 92); s.have = 0;
    run("read-none", s, NOW, GAOKUN3_BATTERY_READ, 0);
    s = at(AC_PRESENT, CHG, 92); s.have |= 1u << 3;
    r = run("have-unknown", s, NOW, GAOKUN3_BATTERY_CONFIG, 0);
    CHECK(r.status == 1);

    /* Time. */
    run("stale", at(AC_PRESENT, CHG, 92), T0 + GAOKUN3_BATTERY_MAX_AGE_MS + 1, GAOKUN3_BATTERY_STALE, 1);
    r = run("future", at(AC_PRESENT, CHG, 92), T0 - 1, GAOKUN3_BATTERY_STALE, 1);
    CHECK(r.age_ms == 0);                               /* not a wrapped "age" */

    /* Bus garbage and bits the policy does not know. */
    s = snap(0, 0, 0, 0, 0, 0, 0);
    run("all-zero", s, NOW, GAOKUN3_BATTERY_ABSENT, 0);
    s.adp = s.bst = 0xff; memset(s.status, 0xff, sizeof s.status);
    run("all-ff", s, NOW, GAOKUN3_BATTERY_BITS, 0);
    run("absent", at(GAOKUN3_ADP_AC, 0, 92), NOW, GAOKUN3_BATTERY_ABSENT, 1);
    run("adp-bit2", at(AC_PRESENT | 0x04, CHG, 92), NOW, GAOKUN3_BATTERY_BITS, 1);
    run("adp-bit7", at(BATTERY_ONLY | 0x80, DIS, 92), NOW, GAOKUN3_BATTERY_BITS, 0);
    run("bst-bit4", at(AC_PRESENT, CHG | 0x10, 92), NOW, GAOKUN3_BATTERY_BITS, 1);
    run("bst-bit7", at(BATTERY_ONLY, DIS | 0x80, 92), NOW, GAOKUN3_BATTERY_BITS, 0);

    /* States that cannot both be true. */
    run("charge-and-discharge", at(AC_PRESENT, CHG | DIS, 92), NOW, GAOKUN3_BATTERY_CONTRADICTION, 0);
    run("charging-no-ac", at(BATTERY_ONLY, CHG, 92), NOW, GAOKUN3_BATTERY_CONTRADICTION, 0);

    /* The EC's critical bit refuses on battery and on AC alike. */
    r = run("critical-discharging", at(BATTERY_ONLY, DIS | CRT, 3), NOW, GAOKUN3_BATTERY_CRITICAL, 0);
    CHECK(r.percent == 3);
    run("critical-on-ac", at(AC_PRESENT, CHG | CRT, 4), NOW, GAOKUN3_BATTERY_CRITICAL, 1);
    run("critical-at-80", at(BATTERY_ONLY, DIS | CRT, 80), NOW, GAOKUN3_BATTERY_CRITICAL, 0);

    /* Words out of bounds. */
    run("percent-101", snap(AC_PRESENT, FUL, 101, 8700, FULL, FULL, 0), NOW, GAOKUN3_BATTERY_RANGE, 1);
    run("percent-ffff", snap(BATTERY_ONLY, DIS, 0xffff, 7700, cap_at(60), FULL, 0), NOW, GAOKUN3_BATTERY_RANGE, 0);
    run("voltage-4999", snap(BATTERY_ONLY, DIS, 60, 4999, cap_at(60), FULL, 0), NOW, GAOKUN3_BATTERY_RANGE, 0);
    run("voltage-9001", snap(AC_PRESENT, CHG, 60, 9001, cap_at(60), FULL, 0), NOW, GAOKUN3_BATTERY_RANGE, 1);
    run("voltage-zero", snap(BATTERY_ONLY, DIS, 60, 0, cap_at(60), FULL, 0), NOW, GAOKUN3_BATTERY_RANGE, 0);
    run("full-zero", snap(BATTERY_ONLY, DIS, 60, 7700, 0, 0, 0), NOW, GAOKUN3_BATTERY_RANGE, 0);
    run("full-999", snap(BATTERY_ONLY, DIS, 60, 7700, 599, 999, 0), NOW, GAOKUN3_BATTERY_RANGE, 0);
    run("full-10001", snap(BATTERY_ONLY, DIS, 60, 7700, 6001, 10001, 0), NOW, GAOKUN3_BATTERY_RANGE, 0);

    /* The gauge and the raw charge disagree. */
    run("capacity-over-full", snap(AC_PRESENT, FUL, 100, 8700, FULL + 1, FULL, 0), NOW, GAOKUN3_BATTERY_INCONSISTENT, 1);
    run("gauge-11-apart", snap(BATTERY_ONLY, DIS, 59, 7700, cap_at(70), FULL, 0), NOW, GAOKUN3_BATTERY_INCONSISTENT, 0);
    run("gauge-disagrees", snap(BATTERY_ONLY, DIS, 92, 7700, cap_at(40), FULL, 0), NOW, GAOKUN3_BATTERY_INCONSISTENT, 0);

    /* Interface misuse fails closed. */
    CHECK(gaokun3_battery_decode(NULL, NOW, &r) == GAOKUN3_BATTERY_CONFIG && r.status == 1 && !r.plausible);
    CHECK(gaokun3_battery_admit(&r, 50) == GAOKUN3_BATTERY_CONFIG);
    s = at(AC_PRESENT, CHG, 92);
    CHECK(gaokun3_battery_decode(&s, NOW, NULL) == GAOKUN3_BATTERY_CONFIG);
    CHECK(gaokun3_battery_admit(NULL, 50) == GAOKUN3_BATTERY_CONFIG);
    CHECK(gaokun3_battery_decode(&s, NOW, &r) == GAOKUN3_BATTERY_OK && gaokun3_battery_admit(&r, 50) == GAOKUN3_BATTERY_OK);
    struct gaokun3_battery_record bad = r; bad.abi++;
    CHECK(gaokun3_battery_admit(&bad, 50) == GAOKUN3_BATTERY_CONFIG);
    bad = r; bad.reason = GAOKUN3_BATTERY_CRITICAL;     /* plausible but with a refusal */
    CHECK(gaokun3_battery_admit(&bad, 50) == GAOKUN3_BATTERY_CONFIG);
    bad = r; bad.plausible = 0;                         /* refused without a reason */
    CHECK(gaokun3_battery_admit(&bad, 50) == GAOKUN3_BATTERY_CONFIG);
    bad = r; bad.status = 1; bad.reason = GAOKUN3_BATTERY_OK;
    CHECK(gaokun3_battery_admit(&bad, 50) == GAOKUN3_BATTERY_READ);

    /* The minimums admit accepts, for the driver to compare with battery_minimum(). */
    unsigned lo = 0, hi = 0, first = 1;
    for (unsigned m = 0; m <= 200; m++)
        if (gaokun3_battery_admit(&r, m) != GAOKUN3_BATTERY_CONFIG) { if (first) lo = m; hi = m; first = 0; }
    printf("{\"minimum_bounds\":[%u,%u]}\n", lo, hi);
    printf("PASS: gaokun3 battery policy, %u snapshot cases\n", cases);
    return 0;
}
