/* SPDX-License-Identifier: MIT */
/* Stage 2 of the S2CR bypass: the first SMMU write, as a host model
 * (docs/GAOKUN3-USB-DEVICE-MODE.md, step 1i).
 *
 * Stage 1 (run 25) showed the plan holds on the gaokun3: S2CR(6) at
 * 0x15000C18, 0x00000006 to 0x00010006. Stage 2 answers one question,
 * with the least change and for the shortest time: does that write take?
 * It writes the planned word, reads it back, writes the old word back at
 * once, reads that back, and invalidates the SMMU's TLBs as Linux's reset
 * does. USB0 must be halted throughout, so no transaction of its stream can
 * meet the bypass while it lasts.
 *
 * THIS IS NOT LINKED INTO EL2QUAL. probes/el2qual/build.py does not build
 * it, so no el2qual binary contains it, and the stick's el2qual.efi is
 * unchanged by it. It runs only in tools/test-el2qual-s2bwrite.c, against
 * a fake SMMU. Wiring it into el2qual, behind a compile-time gate and a
 * load option, is a later step that needs its own go-ahead, as does any
 * hardware run.
 *
 * Every write it can make, in order, at most once each:
 *   S2CR(group) <- planned        the bypass
 *   S2CR(group) <- old            the restore
 *   TLBIALLH    <- 0xffffffff     (GR0 0x6c)
 *   TLBIALLNSNH <- 0xffffffff     (GR0 0x68)
 *   sTLBGSYNC   <- 0xffffffff     (GR0 0x70), then sTLBGSTATUS polled
 * The order and the dummy value are Linux's (arm-smmu.c reset and
 * __arm_smmu_tlb_sync, QCOM_DUMMY_VAL -1), read as a reference. If the
 * bypass write does not take (the read-back still shows the old word),
 * nothing else is written. If the restore does not read back, the
 * invalidation is not attempted: the register is in an unknown state, and
 * the recovery is a power cycle, which reloads the firmware's SMMU setup.
 *
 * USB0 must be halted: xHCI USBCMD.R/S (bit 0) clear and USBSTS.HCH
 * (bit 0) set, at the operational registers (core + CAPLENGTH). Offsets and
 * bits from Linux drivers/usb/host/xhci.h (struct xhci_op_regs: command,
 * status) and xhci-ext-caps.h (XHCI_CMD_RUN, XHCI_STS_HALT), read as a
 * reference. Nothing is copied. */
#ifndef EL2QUAL_S2BWRITE_H
#define EL2QUAL_S2BWRITE_H

#include <stdint.h>
#include "s2bypass.h"

enum el2q_s2bw {
    EL2Q_S2BW_NOT_ASKED,
    /* Refused before any write: */
    EL2Q_S2BW_REFUSED_PLAN,     /* no PLANNED stage-1 result, or one that is inconsistent */
    EL2Q_S2BW_REFUSED_MOVED,    /* S2CR(group) no longer holds the plan's old word, or did not read */
    EL2Q_S2BW_REFUSED_PENDING,  /* an SError was already pending */
    EL2Q_S2BW_REFUSED_BUSY,     /* USB0 not halted, or its registers did not read */
    /* After the bypass write was issued: */
    EL2Q_S2BW_DROPPED,          /* it read back as the old word: the write did not take */
    EL2Q_S2BW_WRITE_FAULT,      /* the store, or its read-back, raised an exception */
    EL2Q_S2BW_UNEXPECTED,       /* it read back as neither word */
    EL2Q_S2BW_RESTORE_FAILED,   /* the restore did not read back as the old word */
    EL2Q_S2BW_SYNC_TIMEOUT,     /* restored, but the invalidation did not complete (timeout or fault) */
    EL2Q_S2BW_GFAULT,           /* restored and synced, but sGFSR reports a global fault */
    EL2Q_S2BW_SERROR,           /* restored and synced, but an SError is pending */
    EL2Q_S2BW_DONE,             /* the write took, was undone, and the SMMU reported nothing */
    EL2Q_S2BW_COUNT
};

struct el2q_s2bw_hw {
    void *ctx;
    /* One 32-bit load or store behind the fault fixup: 0, or the ESR of the
     * exception it raised. */
    uint64_t (*read32)(void *ctx, uint64_t address, uint32_t *value);
    uint64_t (*write32)(void *ctx, uint64_t address, uint32_t value);
    /* DSB SY: every earlier access has completed before any later one. */
    void (*barrier)(void *ctx);
    /* Nonzero when a physical SError is pending (ISR_EL1.A). */
    int (*serror)(void *ctx);
    /* A free-running counter, and how many of its ticks sTLBGSTATUS may stay
     * active (Linux allows about 1 s, TLB_LOOP_TIMEOUT). */
    uint64_t (*now)(void *ctx);
    uint64_t sync_ticks;
};

struct el2q_s2bw_result {
    uint32_t state;             /* enum el2q_s2bw */
    uint32_t group;
    uint32_t old, planned;      /* from the stage-1 plan */
    uint32_t usbcmd, usbsts;    /* USB0's xHCI, as read before writing */
    uint32_t wrote;             /* the bypass write was issued */
    uint32_t readback_ok;       /* ... and its read-back completed */
    uint32_t readback;
    uint32_t restored;          /* the restore write was issued */
    uint32_t restore_ok;        /* ... and its read-back completed */
    uint32_t restore_readback;
    uint32_t synced;            /* 0 not tried, 1 complete, 2 timed out, 3 an access faulted */
    uint32_t polls;             /* sTLBGSTATUS reads */
    uint32_t gfsr;              /* sGFSR after */
    uint32_t writes;            /* stores issued, of the five above */
    uint32_t fault_step;        /* 1 the bypass store, 2 its read-back, 3 the restore
                                   store, 4 its read-back; 0 none of these */
    uint64_t detail;            /* the ESR of a faulting access */
};

/* Run stage 2 for a stage-1 plan made in this boot. smmu is the SMMU's
 * register window, xhci_op USB0's xHCI operational registers. Returns
 * out->state. */
uint32_t el2q_s2bw_run(const struct el2q_s2b_result *plan, uint64_t smmu, uint64_t xhci_op,
                       const struct el2q_s2bw_hw *hw, struct el2q_s2bw_result *out);

/* Fixed-format report lines, empty past the last; see s2bwrite.c. */
#define EL2Q_S2BW_LINE_COLS 64
#define EL2Q_S2BW_LINES 4
void el2q_s2bw_line(const struct el2q_s2bw_result *r, unsigned index,
                    char out[EL2Q_S2BW_LINE_COLS + 1]);

#endif
