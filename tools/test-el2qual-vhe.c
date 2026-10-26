/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/vhe.c, the VHE hand-off plan, and the
 * report lines that carry it (report.c's el2q_vhe_line). Linked into
 * tools/test-el2qual.c's binary by tools/test-el2qual.py; test_vhe() returns
 * its check count. The order of the switch itself (el2q_exit_vhe) is checked
 * by tools/test-el2qual.py, not here. */
#include "report.h"
#include "vhe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

unsigned test_vhe(void);

static struct el2q_vhe_regs gaokun3_entry(uint64_t nonvhe_tcr)
{
    /* HCR_EL2 and SCTLR_EL2 as slbounce leaves them on the gaokun3 (VERIFIED,
     * runs 8-10). TCR, TTBR0, CPTR and CNTHCTL are unmeasured there: these
     * are what the QEMU emulation produces from EDK2's own values. */
    return (struct el2q_vhe_regs){
        .hcr = 0x488000000ull, .sctlr = 0x30c5183dull, .tcr = el2q_vhe_tcr(nonvhe_tcr),
        .ttbr0 = 0xbffff000ull, .cptr = 0x33ff, .cnthctl = 3,
    };
}

unsigned test_vhe(void)
{
    const struct el2q_vhe_regs sentinel = {1, 2, 3, 4, 5, 6};
    struct el2q_vhe_regs in, out;

    /* EDK2's TCR_EL2 on QEMU: 48-bit, 4K, inner shareable write-back, PS 5. */
    const uint64_t edk2_tcr = 0x80853510ull;
    in = gaokun3_entry(edk2_tcr);
    CHECK(in.tcr == 0x580903510ull);                 /* EPD1 set, TG1 4K, IPS 5 */
    CHECK(el2q_vhe_exit_plan(&in, &out) == EL2Q_VHE_EXIT_DONE);
    CHECK(out.hcr == EL2Q_HCR_RW);                   /* E2H and TGE gone, AArch64 EL1 */
    CHECK(out.tcr == edk2_tcr);                      /* the same walk, non-VHE layout */
    /* The gaokun3's SCTLR_EL2 keeps M, C, SA and I; its VHE-only EL0 bits
     * happen to sit exactly on the non-VHE RES1 bits, so it comes out equal. */
    CHECK(out.sctlr == 0x30c5183dull);
    CHECK(out.ttbr0 == 0xbffff000ull);
    CHECK(out.cptr == EL2Q_VHE_EXIT_CPTR && out.cnthctl == EL2Q_VHE_EXIT_CNTHCTL);

    /* slbounce's whole entry state as the gaokun3 reported it (VERIFIED,
     * runs 11 and 12, 2026-10-26: VHEIN/VHEIN2), and the registers it read
     * back after the switch (VHEOUT/VHEOUT2). The plan must reproduce what
     * the hardware ended up holding. TCR: 40-bit VA (T0SZ 24), 4K, inner
     * shareable write-back walks, IPS 40-bit, TTBR1 walks off. */
    in = (struct el2q_vhe_regs){
        .hcr = 0x488000000ull, .sctlr = 0x30c5183dull, .tcr = 0x280803518ull,
        .ttbr0 = 0x9f519000ull, .cptr = 0x300000ull, .cnthctl = 3,
    };
    CHECK(el2q_vhe_exit_plan(&in, &out) == EL2Q_VHE_EXIT_DONE);
    CHECK(out.hcr == 0x80000000ull && out.sctlr == 0x30c5183dull && out.tcr == 0x80823518ull);
    CHECK(out.ttbr0 == 0x9f519000ull && out.cptr == 0x33ffull && out.cnthctl == 3);

    /* Round trips through both conversions: granules, PS, TBI, HA/HD, HPD. */
    static const uint64_t walks[] = {
        0x80853510ull,                               /* 4K, PS 5 */
        0x80823510ull | (2ull << 14),                /* 16K, PS 2, T0SZ 16 */
        0x80843520ull | (1ull << 14),                /* 64K, PS 4, T0SZ 32 */
        0x80853510ull | (1ull << 20) | (3ull << 21) | (1ull << 24),   /* TBI, HA, HD, HPD */
        0x80803019ull,                               /* T0SZ 25, non-cacheable walks */
    };
    for (unsigned i = 0; i < sizeof walks / sizeof walks[0]; i++) {
        in = gaokun3_entry(walks[i]);
        CHECK(el2q_vhe_exit_plan(&in, &out) == EL2Q_VHE_EXIT_DONE);
        CHECK(out.tcr == walks[i]);
    }

    /* SCTLR_EL2: the EL2 controls carry, the VHE-only EL0 controls drop, the
     * non-VHE RES1 bits fill in, and DSSBS (bit 44) is allowed through. */
    in = gaokun3_entry(edk2_tcr);
    in.sctlr = 0x1ull | (1ull << 31) | (1ull << 26) | (1ull << 15) | (1ull << 44);  /* M EnIA UCI UCT DSSBS */
    CHECK(el2q_vhe_exit_plan(&in, &out) == EL2Q_VHE_EXIT_DONE);
    CHECK(out.sctlr == (0x30c50830ull | 0x1ull | (1ull << 31) | (1ull << 44)));

    /* The ASID field has no meaning without E2H. */
    in = gaokun3_entry(edk2_tcr);
    in.ttbr0 = 0x0042000080001000ull;
    CHECK(el2q_vhe_exit_plan(&in, &out) == EL2Q_VHE_EXIT_DONE);
    CHECK(out.ttbr0 == 0x80001000ull);

    /* RW alone or nothing besides E2H|TGE: both are slbounce-shaped. */
    in = gaokun3_entry(edk2_tcr);
    in.hcr = EL2Q_HCR_E2H | EL2Q_HCR_TGE;            /* what emulate-vhe writes */
    CHECK(el2q_vhe_exit_plan(&in, &out) == EL2Q_VHE_EXIT_DONE);

    /* Every refusal leaves *out alone. */
    static const struct { uint64_t hcr, tcr_set, tcr_clear, sctlr_set; unsigned want; } bad[] = {
        {EL2Q_HCR_RW, 0, 0, 0, EL2Q_VHE_EXIT_NOT_VHE},                    /* already non-VHE */
        {EL2Q_HCR_E2H | EL2Q_HCR_RW, 0, 0, 0, EL2Q_VHE_EXIT_HCR},         /* TGE clear */
        {0x488000000ull | (1ull << 4), 0, 0, 0, EL2Q_VHE_EXIT_HCR},       /* IMO: someone routed IRQs */
        {0x488000000ull | (1ull << 41), 0, 0, 0, EL2Q_VHE_EXIT_HCR},      /* API */
        {0x488000000ull, 1ull << 7, 0, 0, EL2Q_VHE_EXIT_TTBR0_OFF},       /* EPD0 */
        {0x488000000ull, 0, 1ull << 23, 0, EL2Q_VHE_EXIT_TTBR1},          /* EPD1 clear */
        {0x488000000ull, 1ull << 59, 0, 0, EL2Q_VHE_EXIT_TCR},            /* DS: 52-bit */
        {0x488000000ull, 1ull << 6, 0, 0, EL2Q_VHE_EXIT_TCR},             /* RES0 */
        {0x488000000ull, 1ull << 35, 0, 0, EL2Q_VHE_EXIT_TCR},            /* RES0 */
        {0x488000000ull, 1ull << 43, 0, 0, EL2Q_VHE_EXIT_TCR},            /* HWU059 */
        {0x488000000ull, 1ull << 51, 0, 0, EL2Q_VHE_EXIT_TCR},            /* TBID0 */
        {0x488000000ull, 1ull << 57, 0, 0, EL2Q_VHE_EXIT_TCR},            /* TCMA0 */
        {0x488000000ull, 0, 0, 1ull << 36, EL2Q_VHE_EXIT_SCTLR},          /* BT1 */
        {0x488000000ull, 0, 0, 1ull << 40, EL2Q_VHE_EXIT_SCTLR},          /* TCF */
        {0x488000000ull, 0, 0, 1ull << 63, EL2Q_VHE_EXIT_SCTLR},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        in = gaokun3_entry(edk2_tcr);
        in.hcr = bad[i].hcr;
        in.tcr = (in.tcr | bad[i].tcr_set) & ~bad[i].tcr_clear;
        in.sctlr |= bad[i].sctlr_set;
        out = sentinel;
        CHECK(el2q_vhe_exit_plan(&in, &out) == bad[i].want);
        CHECK(!memcmp(&out, &sentinel, sizeof out));
    }

    /* Words: one per outcome, distinct, and UNKNOWN past the end. */
    for (unsigned a = 0; a < EL2Q_VHE_EXIT_COUNT; a++) {
        CHECK(strcmp(el2q_vhe_exit_word(a), "UNKNOWN"));
        for (unsigned b = a + 1; b < EL2Q_VHE_EXIT_COUNT; b++)
            CHECK(strcmp(el2q_vhe_exit_word(a), el2q_vhe_exit_word(b)));
    }
    CHECK(!strcmp(el2q_vhe_exit_word(EL2Q_VHE_EXIT_COUNT), "UNKNOWN"));

    /* The fixed report lines. */
    static struct el2q_run r;
    char line[EL2Q_TEXT_COLS + 1];
    memset(&r, 0, sizeof r);
    el2q_vhe_line(&r, 0, line);
    CHECK(!line[0]);                                 /* not asked: no lines */
    r.vhe_exit = EL2Q_VHE_EXIT_DONE;
    r.vhe_in = gaokun3_entry(edk2_tcr);
    CHECK(el2q_vhe_exit_plan(&r.vhe_in, &r.vhe_plan) == EL2Q_VHE_EXIT_DONE);
    r.vhe_out = r.vhe_plan;
    el2q_vhe_line(&r, 0, line);
    CHECK(!strcmp(line, "VHEX DONE PLAN=MATCH"));
    el2q_vhe_line(&r, 1, line);
    CHECK(!strcmp(line, "VHEIN HCR=488000000 SCTLR=30C5183D TCR=580903510"));
    el2q_vhe_line(&r, 2, line);
    CHECK(!strcmp(line, "VHEIN2 TTBR0=BFFFF000 CPTR=33FF CNTHCTL=3"));
    el2q_vhe_line(&r, 3, line);
    CHECK(!strcmp(line, "VHEOUT HCR=80000000 SCTLR=30C5183D TCR=80853510"));
    el2q_vhe_line(&r, 4, line);
    CHECK(!strcmp(line, "VHEOUT2 TTBR0=BFFFF000 CPTR=33FF CNTHCTL=3"));
    el2q_vhe_line(&r, 5, line);
    CHECK(!line[0]);
    r.vhe_out.tcr ^= 1;                              /* the CPU kept something else */
    el2q_vhe_line(&r, 0, line);
    CHECK(!strcmp(line, "VHEX DONE PLAN=DIFF"));
    /* On screen too: the gaokun3 has no serial port. */
    static struct el2q_text text;
    r.vhe_out = r.vhe_plan;
    el2q_report(&r, &text);
    unsigned found = 0;
    for (unsigned i = 0; i < text.count; i++) {
        found += !strcmp(text.line[i], "VHE EXIT .. DONE  HCR_EL2 0000000080000000");
        found += !strcmp(text.line[i], "VHEIN HCR=488000000 SCTLR=30C5183D TCR=580903510");
        found += !strcmp(text.line[i], "VHEIN2 TTBR0=BFFFF000 CPTR=33FF CNTHCTL=3");
        found += !strcmp(text.line[i], "VHEOUT HCR=80000000 SCTLR=30C5183D TCR=80853510");
        found += !strcmp(text.line[i], "VHEOUT2 TTBR0=BFFFF000 CPTR=33FF CNTHCTL=3");
    }
    CHECK(found == 5);
    r.vhe_exit = EL2Q_VHE_EXIT_NOT_ASKED;
    el2q_report(&r, &text);
    for (unsigned i = 0; i < text.count; i++) CHECK(strncmp(text.line[i], "VHE", 3));
    r.vhe_exit = EL2Q_VHE_EXIT_TTBR1;
    el2q_vhe_line(&r, 0, line);
    CHECK(!strcmp(line, "VHEX REFUSED-TTBR1 PLAN=-"));
    el2q_vhe_line(&r, 3, line);
    CHECK(!line[0]);                                 /* nothing read back: no switch */
    return checks;
}
