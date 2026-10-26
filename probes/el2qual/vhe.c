/* SPDX-License-Identifier: MIT */
/* VHE hand-off values; see vhe.h. Bit positions are the Arm ARM's. */
#include "vhe.h"

#define BIT(n)           (1ull << (n))
#define FIELD(v, lo, w)  (((v) >> (lo)) & (BIT(w) - 1))

/* TCR_EL2 with E2H set uses TCR_EL1's layout. */
#define VTCR_EPD0        BIT(7)
#define VTCR_EPD1        BIT(23)
/* Fields this exit can translate or may drop. Dropped: T1SZ, A1, IRGN1,
 * ORGN1, SH1, TG1, TBI1 and HPD1, which only describe TTBR1 walks, refused
 * below unless disabled, and AS, the ASID size, since non-VHE EL2 has no
 * ASIDs. Anything else set (HWU, TBID, E0PD, TCMA, DS, a RES0 bit) is
 * refused: each changes the walk or the address checks, and 52-bit (DS)
 * would also move fields. */
#define VTCR_KNOWN       (0x3full | VTCR_EPD0 | (0x7ffffffull << 8) | (0x7full << 36))

/* TCR_EL2 with E2H clear. */
#define TCR_RES1         (BIT(23) | BIT(31))

/* SCTLR_EL2 with {E2H, TGE} == {1, 1} uses SCTLR_EL1's layout. These bits
 * mean the same EL2 control in both layouts: M, A, C, SA, I, EnDB, WXN,
 * IESB, EE, EnDA, EnIB, EnIA and DSSBS. The rest of the low word are EL0 and
 * EL1&0 controls (SA0, nTWI, SPAN, UCI ...) with no non-VHE EL2 meaning;
 * they are dropped and the non-VHE RES1 bits written in their place. A set
 * bit in the high word other than DSSBS (BT, TCF, ATA ...) is refused. */
#define SCTLR_CARRY      (BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(12) | BIT(13) | BIT(19) | \
                          BIT(21) | BIT(25) | BIT(27) | BIT(30) | BIT(31) | BIT(44))
#define SCTLR_HIGH_OK    BIT(44)
#define SCTLR_RES1       0x30c50830ull    /* bits 4, 5, 11, 16, 18, 22, 23, 28, 29 */

#define TTBR_ASID        (0xffffull << 48)

uint64_t el2q_vhe_tcr(uint64_t tcr)
{
    uint64_t out = tcr & 0xff3full;                        /* T0SZ..TG0, not EPD0 */
    out |= (tcr & 0x3f) << 16;                             /* T1SZ: unused, but sane */
    out |= VTCR_EPD1;
    out |= 2ull << 30;                                     /* TG1 4K, unused */
    out |= FIELD(tcr, 16, 3) << 32;                        /* PS -> IPS */
    out |= FIELD(tcr, 20, 1) << 37;                        /* TBI -> TBI0 */
    out |= FIELD(tcr, 21, 2) << 39;                        /* HA, HD */
    out |= FIELD(tcr, 24, 1) << 41;                        /* HPD -> HPD0 */
    return out;
}

enum el2q_vhe_exit el2q_vhe_exit_plan(const struct el2q_vhe_regs *in, struct el2q_vhe_regs *out)
{
    if (!(in->hcr & EL2Q_HCR_E2H)) return EL2Q_VHE_EXIT_NOT_VHE;
    /* slbounce leaves E2H, TGE and RW (VERIFIED). Any other HCR_EL2 control
     * means someone else configured EL2, and clearing it would be a guess. */
    if (!(in->hcr & EL2Q_HCR_TGE) || (in->hcr & ~(EL2Q_HCR_E2H | EL2Q_HCR_TGE | EL2Q_HCR_RW)))
        return EL2Q_VHE_EXIT_HCR;

    if (in->tcr & VTCR_EPD0) return EL2Q_VHE_EXIT_TTBR0_OFF;
    if (!(in->tcr & VTCR_EPD1)) return EL2Q_VHE_EXIT_TTBR1;
    if (in->tcr & ~VTCR_KNOWN) return EL2Q_VHE_EXIT_TCR;
    if ((in->sctlr >> 32) & ~(SCTLR_HIGH_OK >> 32)) return EL2Q_VHE_EXIT_SCTLR;

    uint64_t tcr = in->tcr & 0xff3full;                    /* T0SZ..TG0 keep their places */
    tcr |= FIELD(in->tcr, 32, 3) << 16;                    /* IPS -> PS */
    tcr |= FIELD(in->tcr, 37, 1) << 20;                    /* TBI0 -> TBI */
    tcr |= FIELD(in->tcr, 39, 2) << 21;                    /* HA, HD */
    tcr |= FIELD(in->tcr, 41, 1) << 24;                    /* HPD0 -> HPD */

    *out = (struct el2q_vhe_regs){
        .hcr = EL2Q_HCR_RW,
        .sctlr = (in->sctlr & SCTLR_CARRY) | SCTLR_RES1,
        .tcr = tcr | TCR_RES1,
        .ttbr0 = in->ttbr0 & ~TTBR_ASID,                   /* no ASID without E2H */
        .cptr = EL2Q_VHE_EXIT_CPTR,
        .cnthctl = EL2Q_VHE_EXIT_CNTHCTL,
    };
    return EL2Q_VHE_EXIT_DONE;
}

const char *el2q_vhe_exit_word(unsigned state)
{
    static const char *const words[EL2Q_VHE_EXIT_COUNT] = {
        [EL2Q_VHE_EXIT_NOT_ASKED] = "NOT-ASKED",
        [EL2Q_VHE_EXIT_DONE] = "DONE",
        [EL2Q_VHE_EXIT_NOT_VHE] = "NOT-VHE",
        [EL2Q_VHE_EXIT_HCR] = "REFUSED-HCR",
        [EL2Q_VHE_EXIT_TTBR0_OFF] = "REFUSED-TTBR0-OFF",
        [EL2Q_VHE_EXIT_TTBR1] = "REFUSED-TTBR1",
        [EL2Q_VHE_EXIT_TCR] = "REFUSED-TCR",
        [EL2Q_VHE_EXIT_SCTLR] = "REFUSED-SCTLR",
    };
    return state < EL2Q_VHE_EXIT_COUNT ? words[state] : "UNKNOWN";
}
