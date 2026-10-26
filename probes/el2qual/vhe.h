/* SPDX-License-Identifier: MIT */
/* The VHE hand-off: moving EL2 between slbounce's state (HCR_EL2.E2H and
 * TGE set, EL2 translating through the EL2&0 regime) and the non-VHE EL2
 * that q1n1 and the Snapintosh monitor assume (probes/xnuboot.c).
 *
 * E2H changes the layout of TCR_EL2, SCTLR_EL2, CPTR_EL2 and CNTHCTL_EL2,
 * so a value that is right on one side of the switch is wrong on the other.
 * This file only computes values; el2qual-asm.S performs the switch. It
 * touches no registers, which lets tools/test-el2qual.c check every
 * decision on the host, and it has no el2qual dependencies, so q1n1 can take
 * it as it stands.
 *
 * Layouts are from the Arm ARM's register descriptions (TCR_EL2, SCTLR_EL2,
 * CPTR_EL2 and CNTHCTL_EL2, each for HCR_EL2.E2H == 0 and == 1). The state
 * slbounce leaves on the gaokun3 (firmware 2.18) is measured: HCR_EL2
 * 0x488000000, SCTLR_EL2 0x30C5183D, TCR_EL2 0x280803518, CPTR_EL2 0x300000
 * and CNTHCTL_EL2 0x3 (VERIFIED, el2qual runs 11 and 12), and the plan
 * accepts it. Another firmware or slbounce build may differ, so the plan
 * still refuses anything it does not recognise rather than guessing. */
#ifndef EL2QUAL_VHE_H
#define EL2QUAL_VHE_H

#include <stdint.h>

#define EL2Q_HCR_RW  (1ull << 31)
#define EL2Q_HCR_TGE (1ull << 27)
#define EL2Q_HCR_E2H (1ull << 34)

/* The EL2 registers whose meaning E2H changes, plus the translation base. */
struct el2q_vhe_regs {
    uint64_t hcr, sctlr, tcr, ttbr0, cptr, cnthctl;
};

enum el2q_vhe_exit {
    EL2Q_VHE_EXIT_NOT_ASKED,  /* the "exit-vhe" option was not given */
    EL2Q_VHE_EXIT_DONE,       /* the switch ran; EL2 is non-VHE */
    EL2Q_VHE_EXIT_NOT_VHE,    /* E2H was already clear: nothing to do */
    EL2Q_VHE_EXIT_HCR,        /* HCR_EL2 is not E2H|TGE (with RW at most) */
    EL2Q_VHE_EXIT_TTBR0_OFF,  /* EPD0 set: the identity map is not in TTBR0 */
    EL2Q_VHE_EXIT_TTBR1,      /* EPD1 clear: non-VHE EL2 has no TTBR1 to keep it */
    EL2Q_VHE_EXIT_TCR,        /* a TCR_EL2 control with no non-VHE translation here */
    EL2Q_VHE_EXIT_SCTLR,      /* an SCTLR_EL2 control with no translation here */
    EL2Q_VHE_EXIT_COUNT
};

/* The fixed values the exit writes rather than converts. Their entry values
 * are unmeasured and partly EL0 controls with no non-VHE meaning, so a
 * conversion would carry unknowns forward.
 *  CPTR_EL2: RES1 bits [7:0], 9 and 13; TZ (8) and TSM (12) set, so SVE and
 *    SME trap (the gaokun3 has neither; Phase 0A); TFP (10), TTA, TAM and
 *    TCPAC clear, so FP/SIMD, trace, AMU and CPACR accesses do not trap.
 *  CNTHCTL_EL2: EL1PCTEN and EL1PCEN, the EL1 physical counter and timer
 *    usable from EL1, as the monitor expects before it sets its own; the
 *    event stream off. */
#define EL2Q_VHE_EXIT_CPTR    0x33ffull
#define EL2Q_VHE_EXIT_CNTHCTL 0x3ull

/* The non-VHE TCR_EL2 the firmware programmed, rewritten in the layout E2H
 * gives the register, keeping the same TTBR0 walk. The QEMU emulation of
 * slbounce's hand-off uses it (option "emulate-vhe"). */
uint64_t el2q_vhe_tcr(uint64_t tcr);

/* The non-VHE values that keep `in`'s translation, its caching and PAC
 * controls, or why that cannot be done. *out is written only on
 * EL2Q_VHE_EXIT_DONE, which here means "the plan is sound", not that the
 * switch ran. */
enum el2q_vhe_exit el2q_vhe_exit_plan(const struct el2q_vhe_regs *in, struct el2q_vhe_regs *out);

/* A fixed word for each outcome, for the report. */
const char *el2q_vhe_exit_word(unsigned state);

#endif
