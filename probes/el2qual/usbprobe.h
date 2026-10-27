/* SPDX-License-Identifier: MIT */
/* el2qual's USB read (load option "usb"): whether the gaokun3's USB0
 * controller, the DWC3 behind the power-button port, is powered and clocked
 * before and after ExitBootServices, and if so what state firmware left it
 * in. Read only. Why, and what decides on it:
 * docs/GAOKUN3-USB-DEVICE-MODE.md.
 *
 * Reading a controller whose clock or power domain is off can hang the
 * interconnect, so the controller is read only after the always-on clock
 * controller (GCC) says its power domain is on and the clocks its register
 * port needs are running. usbprobe.c holds no hardware access of its own;
 * el2qual.c supplies the operations below, tools/test-el2qual-usb-probe.c
 * synthetic ones.
 *
 * Addresses are SC8280XP's. GCC at 0x00100000 and the GDSC, branch and reset
 * offsets are from Linux drivers/clk/qcom/gcc-sc8280xp.c; the USB0 core,
 * qscratch and apps_smmu addresses and USB0's stream ID from the gaokun3's
 * device tree; DWC3 offsets from Linux drivers/usb/dwc3/core.h; SMMU ones
 * from Linux drivers/iommu/arm/arm-smmu/arm-smmu.h. Facts only, read as
 * references; nothing is copied. */
#ifndef EL2QUAL_USBPROBE_H
#define EL2QUAL_USBPROBE_H

#include <stdint.h>
#include "pagemap.h"
#include "s2walk.h"
#include "s2bypass.h"

/* The GCC words read in each phase, in this order. */
enum el2q_usb_gcc {
    EL2Q_USB_GDSC,       /* USB30_PRIM_GDSC: bit 31 PWR_ON */
    EL2Q_USB_MASTER,     /* GCC_USB30_PRIM_MASTER_CLK: bit 31 CLK_OFF */
    EL2Q_USB_SLEEP,      /* GCC_USB30_PRIM_SLEEP_CLK */
    EL2Q_USB_UTMI,       /* GCC_USB30_PRIM_MOCK_UTMI_CLK */
    EL2Q_USB_AXI,        /* GCC_AGGRE_USB3_PRIM_AXI_CLK (voted) */
    EL2Q_USB_CNOC,       /* GCC_CFG_NOC_USB3_PRIM_AXI_CLK (voted) */
    EL2Q_USB1_GDSC,      /* USB30_SEC_GDSC, for comparison */
    EL2Q_USB1_MASTER,    /* GCC_USB30_SEC_MASTER_CLK, for comparison */
    EL2Q_USB_GCC_COUNT
};

/* The controller words, read after ExitBootServices only, in this order. */
enum el2q_usb_word {
    EL2Q_USB_XCAP,       /* xHCI CAPLENGTH/HCIVERSION (core + 0) */
    EL2Q_USB_XHCS,       /* xHCI HCSPARAMS1 (core + 4) */
    EL2Q_USB_SNPSID,     /* DWC3 GSNPSID */
    EL2Q_USB_VER,        /* DWC3 GVERNUM */
    EL2Q_USB_GCTL,
    EL2Q_USB_U2PHY,      /* GUSB2PHYCFG(0) */
    EL2Q_USB_U3PIPE,     /* GUSB3PIPECTL(0) */
    EL2Q_USB_DCFG,
    EL2Q_USB_DCTL,
    EL2Q_USB_EVNT,       /* GEVNTADRLO(0) */
    EL2Q_USB_QS_HS,      /* qscratch HS_PHY_CTRL */
    EL2Q_USB_QS_SS,      /* qscratch SS_PHY_CTRL */
    EL2Q_USB_WORD_COUNT
};

extern const uint64_t el2q_usb_gcc_address[EL2Q_USB_GCC_COUNT];
extern const uint64_t el2q_usb_word_address[EL2Q_USB_WORD_COUNT];

#define EL2Q_USB_SMMU_BASE   0x15000000ull
#define EL2Q_USB_SMMU_SIZE   0x100000ull  /* apps_smmu's register window (device tree) */
#define EL2Q_USB_STREAM      0x820u      /* USB0 on apps_smmu (device tree) */
/* IDR0.NUMSMRG is 8 bits, and Linux uses it as the group count with no other
 * limit; the gaokun3's apps_smmu reports 212 (VERIFIED, el2qual run 17). */
#define EL2Q_USB_SMR_LIMIT   255u

/* One word's outcome. */
enum el2q_usb_read { EL2Q_USB_NOT_READ, EL2Q_USB_READ_OK, EL2Q_USB_UNMAPPED, EL2Q_USB_FAULT };

/* What the GCC words say about USB0. */
enum el2q_usb_power {
    EL2Q_USB_POWER_UNREAD,      /* a word it needs was not read */
    EL2Q_USB_POWER_GDSC_OFF,    /* the power domain is collapsed */
    EL2Q_USB_POWER_CLOCKS_OFF,  /* powered, but a register-port clock is off */
    EL2Q_USB_POWER_ON           /* powered, master and both AXI clocks running */
};

/* The controller words as a whole. */
enum el2q_usb_core {
    EL2Q_USB_CORE_NOT_TRIED,    /* before ExitBootServices, or power not ON */
    EL2Q_USB_CORE_UNMAPPED,     /* a word does not translate as Device at its own address */
    EL2Q_USB_CORE_FAULT,        /* a read raised an exception; nothing after it was read */
    EL2Q_USB_CORE_READ
};

enum el2q_usb_smmu {
    EL2Q_USB_SMMU_NOT_ASKED,    /* option "usb-smmu" not given */
    EL2Q_USB_SMMU_UNMAPPED,
    EL2Q_USB_SMMU_FAULT,
    EL2Q_USB_SMMU_ALL_ONES,     /* sCR0 or IDR0 read all ones: nothing answered there */
    EL2Q_USB_SMMU_NO_MATCH,     /* no valid stream match covers USB0's stream */
    EL2Q_USB_SMMU_MATCH
};

/* The context bank the matched stream translates through (S2CR.CBNDX). */
enum el2q_usb_cb {
    EL2Q_USB_CB_NOT_TRIED,      /* no stream match was read */
    EL2Q_USB_CB_NOT_TRANSLATED, /* S2CR says bypass or fault: no bank is used */
    EL2Q_USB_CB_OUT_OF_RANGE,   /* the bank is past NUMCB, or its page past the SMMU's window */
    EL2Q_USB_CB_UNMAPPED,
    EL2Q_USB_CB_FAULT,
    EL2Q_USB_CB_READ,
    EL2Q_USB_CB_NOT_STAGE2      /* the nested bank's own CBAR is not a stage-2 bank's */
};

/* USB0's xHCI run/halt state (option "usb-halt"). The operational registers
 * start at the controller base plus CAPLENGTH, the low byte of XCAP; q1n1
 * assumed 0x20, but the base must come from the controller itself (the A16
 * reports 0x30), so USBCMD and USBSTS are read only after XCAP, from the
 * measured length. */
enum el2q_usb_halt_state {
    EL2Q_USB_HALT_NOT_ASKED,   /* option "usb-halt" not given */
    EL2Q_USB_HALT_NO_CAP,      /* XCAP was not read, so CAPLENGTH is unknown */
    EL2Q_USB_HALT_UNMAPPED,    /* an operational register does not translate as Device */
    EL2Q_USB_HALT_FAULT,       /* a read raised an exception */
    EL2Q_USB_HALT_HALTED,      /* USBCMD.R/S clear and USBSTS.HCH set */
    EL2Q_USB_HALT_RUNNING      /* read, but not halted */
};

struct el2q_usb_phase {
    uint32_t ran;
    uint32_t gcc_read[EL2Q_USB_GCC_COUNT];   /* enum el2q_usb_read */
    uint32_t gcc[EL2Q_USB_GCC_COUNT];
    uint32_t power;                          /* enum el2q_usb_power */
    uint32_t core;                           /* enum el2q_usb_core */
    uint32_t word_read[EL2Q_USB_WORD_COUNT];
    uint32_t word[EL2Q_USB_WORD_COUNT];
    uint64_t esr, at;                        /* the first refused or faulting word */
};

struct el2q_usb {
    struct el2q_usb_phase before, after;     /* before and after ExitBootServices */
    /* Option "usb-smmu-map": el2qual's private Device map of the SMMU's
     * window, built before the SMMU read (el2qual.c, pagemap.c). */
    uint32_t map;                            /* enum el2q_pm */
    uint32_t map_test;                       /* "emulate-map-hole": a QEMU test window, not the SMMU's */
    uint32_t map_cloned, map_kept;           /* OK: UEFI's table cloned (or a new one); pages already mapped */
    uint64_t map_arg;                        /* STRUCTURE: the refused descriptor; VERIFY: pages passing */
    uint32_t smmu;                           /* enum el2q_usb_smmu */
    uint32_t scr0, idr0, smr, s2cr, group;   /* group: the matching SMR's index */
    uint32_t smmu_words;                     /* of sCR0 and IDR0, how many were read */
    uint32_t cb;                             /* enum el2q_usb_cb */
    uint32_t cb_index, id1;                  /* S2CR.CBNDX; SMMU IDR1 */
    uint64_t cb_page;                        /* the bank's register page */
    uint32_t cbar, cba2r;                    /* GR1 CBAR(n), CBA2R(n) */
    uint32_t cb_sctlr, cb_tcr, cb_tcr2, cb_mair0, cb_mair1;
    uint64_t cb_ttbr0;                       /* read as two 32-bit halves */
    uint32_t cb_words;                       /* bank words read, in the order above */
    uint64_t cb_esr, cb_at;
    /* The stage-2 bank a nested (CBAR TYPE 3) bank names in CBAR bits 15:8. */
    uint32_t s2;                             /* enum el2q_usb_cb */
    uint32_t s2_index;
    uint64_t s2_page;
    uint32_t s2_cbar, s2_cba2r, s2_sctlr, s2_tcr;
    uint64_t s2_ttbr0;                       /* VTTBR, read as two 32-bit halves */
    uint32_t s2_words;
    uint64_t s2_esr, s2_at;
    /* Option "usb-s2-walk": that stage 2 walked for a q1n1-style DMA arena
     * (el2qual.c, s2walk.c). */
    struct el2q_s2w_result walk;
    /* Option "usb-s2-survey": that stage 2's top two levels (s2walk.c). */
    struct el2q_s2w_survey survey;
    /* Option "usb-s2-fixed": UEFI's memory map over the candidate region. */
    struct el2q_s2w_region region;
    /* Option "usb-s2-bypass-plan": the one-word change that would give USB0
     * untranslated DMA, planned and never made (s2bypass.c). */
    struct el2q_s2b_result bypass;
    /* Option "usb-halt": USB0's xHCI run/halt state, read from the measured
     * CAPLENGTH (usbprobe.c). Stage 2 of the bypass requires HALTED, and
     * takes its operational-register base from op_base. */
    uint32_t halt;                           /* enum el2q_usb_halt_state */
    uint64_t op_base;                        /* controller base + measured CAPLENGTH */
    uint32_t usbcmd, usbsts;                 /* xHCI USBCMD, USBSTS, as read */
    uint64_t halt_esr, halt_at;              /* the refused or faulting register */
    uint32_t halt_test;                      /* the halt read ran against a stand-in (QEMU) */
    uint64_t smmu_esr;
    uint64_t isr;                            /* ISR_EL1 after the reads: a pending SError shows here */
};

struct el2q_usb_hw {
    void *ctx;
    /* 0 when the address translates, for a read at the current EL, to the
     * same physical address as Device memory. Must not fault. */
    int (*mapped)(void *ctx, uint64_t address);
    /* One 32-bit read: 0 and *value set, or the ESR of the exception it
     * raised. Before ExitBootServices el2qual has no fault fixup, so it is
     * given only GCC words, which UEFI's own clock driver reads. */
    uint64_t (*read32)(void *ctx, uint64_t address, uint64_t *value);
};

/* Reads the GCC words, each after its own mapping check, and decides
 * phase->power. Never touches the controller. */
void el2q_usb_gcc(struct el2q_usb_phase *phase, const struct el2q_usb_hw *hw);

/* Only when phase->power is ON: checks every controller word's mapping
 * first, reads none if one fails, then reads them in order and stops at the
 * first fault. Otherwise leaves phase->core NOT_TRIED. */
void el2q_usb_core(struct el2q_usb_phase *phase, const struct el2q_usb_hw *hw);

/* apps_smmu's sCR0 and IDR0, then the first valid stream match group whose
 * ID, outside its mask, equals USB0's stream, and that group's S2CR. With
 * extended stream IDs (sCR0.EXIDENABLE) validity is S2CR.EXIDVALID and the
 * mask is 16 bits; otherwise SMR bit 31 and a 15-bit mask. At most
 * EL2Q_USB_SMR_LIMIT groups are read, and none after a fault.
 *
 * When that S2CR translates (TYPE 0), the context bank it names is read too:
 * IDR1 gives the page size and where the bank pages start (Linux arm-smmu.c:
 * pgshift from PAGESIZE, numpage = 1 << (NUMPAGENDXB + 1), bank n at page
 * numpage + n), and the bank must be below NUMCB with its page inside the
 * SMMU's window. Then GR1's CBAR(n) and CBA2R(n), and the bank's SCTLR, TCR,
 * TCR2, TTBR0, MAIR0 and MAIR1, each page checked for its mapping first and
 * nothing read after a fault.
 *
 * When that bank is nested (CBAR TYPE 3, stage 1 then stage 2), CBAR bits
 * 15:8 are read as the stage-2 bank's index (INFERRED from the SMMUv2
 * architecture; Linux defines no field there for this type). Its own CBAR
 * must then be TYPE 0, a stage-2 bank, before anything else of it is
 * interpreted; otherwise NOT_STAGE2. Read with the same range and mapping
 * rules: CBAR, CBA2R, SCTLR, TCR and TTBR0. */
void el2q_usb_smmu(struct el2q_usb *usb, const struct el2q_usb_hw *hw);

/* USB0's xHCI run/halt state (option "usb-halt"). Only when XCAP was read:
 * the operational-register base is the controller base plus XCAP's CAPLENGTH
 * byte (not an assumed offset), and USBCMD and USBSTS are read there, each
 * mapping-checked first and nothing read after a fault. HALTED means
 * USBCMD.R/S clear and USBSTS.HCH set. Reads nothing, and reports NO_CAP,
 * when the controller words were not read. */
void el2q_usb_halt(struct el2q_usb *usb, const struct el2q_usb_hw *hw);

/* Fixed-format report lines, empty past the last; see usbprobe.c. */
#define EL2Q_USB_LINE_COLS 64
_Static_assert(EL2Q_S2W_LINE_COLS == EL2Q_USB_LINE_COLS, "a walk line is a USB line");
void el2q_usb_line(const struct el2q_usb *usb, unsigned index, char out[EL2Q_USB_LINE_COLS + 1]);

#endif
