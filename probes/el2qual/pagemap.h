/* SPDX-License-Identifier: MIT */
/* el2qual's private Device map of an MMIO window in its own EL2 page tables
 * (load option "usb-smmu-map"; docs/GAOKUN3-USB-DEVICE-MODE.md).
 *
 * UEFI's tables leave most of apps_smmu's 1 MiB register window unmapped
 * (run 20: bank 126's page at 0x150FE000 is absent), so the read-only SMMU
 * stage cannot reach it. This module maps the window for el2qual alone: it
 * clones the window's level-3 table into a page el2qual owns, fills the
 * invalid window entries with Device, read-only, execute-never identity
 * leaves, and then swaps the level-2 entry to the clone. A valid entry is
 * never rewritten, and any shape the walk does not recognise is refused
 * without a write. The only stores into a pre-existing page are to that one
 * level-2 descriptor; after ExitBootServices every byte of memory is
 * el2qual's, and only the boot CPU runs here.
 *
 * el2q_pm_prepare builds the clone and writes nothing else. el2q_pm_publish
 * then swaps it in with break-before-make: the clone reaches the point of
 * coherency before anything points at it, the old entry is invalidated and
 * its translations flushed before the new one is written. The caller must
 * not touch the level-2 entry's 2 MiB region between the two; the window is
 * MMIO, so no code, stack or data of el2qual's is there. Nothing here
 * touches hardware: descriptor accesses, the page allocation and each
 * maintenance step go through callbacks, so tools/test-el2qual-pagemap.c
 * runs every path, and checks the order, on the host.
 *
 * Descriptor encodings are the Arm ARM's stage-1 format; the Device
 * attribute index is read from MAIR_EL2 at runtime, never assumed. The leaf
 * permission and execute-never bits differ between the EL2 regime
 * (HCR_EL2.E2H 0: one Exception level) and the EL2&0 regime (E2H 1), so the
 * caller says which one the tables are walked in. */
#ifndef EL2QUAL_PAGEMAP_H
#define EL2QUAL_PAGEMAP_H

#include <stdint.h>

enum el2q_pm {
    EL2Q_PM_NOT_ASKED,       /* option "usb-smmu-map" not given */
    EL2Q_PM_NOT_NEEDED,      /* every window page already maps Device identity */
    EL2Q_PM_BUILT,           /* the clone is built; publication is pending */
    EL2Q_PM_OK,              /* published, and the hardware walk agrees */
    EL2Q_PM_REFUSED_REGIME,  /* not a 4K TTBR0 walk this can follow, or misuse */
    EL2Q_PM_REFUSED_DEVICE,  /* MAIR_EL2 has no Device attribute index */
    EL2Q_PM_REFUSED_STRUCT,  /* the path or an entry is not one this adds to */
    EL2Q_PM_REFUSED_ALLOC,   /* no table page left */
    EL2Q_PM_REFUSED_VERIFY,  /* after publication the hardware walk disagrees */
    EL2Q_PM_COUNT
};

/* The maintenance steps el2q_pm_publish asks for, in its order. */
enum el2q_pm_op {
    EL2Q_PM_CLEAN,           /* clean [address, address + size) to the point of coherency */
    EL2Q_PM_DSB,             /* DSB SY */
    EL2Q_PM_TLBI,            /* TLBI ALLE2: every EL2 (or EL2&0) translation */
    EL2Q_PM_ISB
};

struct el2q_pm_hw {
    void *ctx;
    /* Descriptor access; page tables are ordinary mapped memory here. */
    uint64_t (*read64)(void *ctx, uint64_t address);
    void (*write64)(void *ctx, uint64_t address, uint64_t value);
    /* A 4 KiB table page, 4 KiB aligned, any contents; 0 when none is left. */
    uint64_t (*alloc)(void *ctx);
    /* The hardware-walk check the SMMU stage uses: 0 when a read of address
     * translates to the same physical address as Device memory (AT S1E2R). */
    int (*mapped)(void *ctx, uint64_t address);
    /* One maintenance step (enum el2q_pm_op); address and size for CLEAN only. */
    void (*sync)(void *ctx, uint32_t op, uint64_t address, uint64_t size);
};

struct el2q_pm_result {
    uint32_t state;           /* enum el2q_pm */
    uint32_t attr_index;      /* the MAIR Device index used for new leaves */
    uint32_t mapped_before;   /* window pages already Device identity */
    uint32_t mapped_after;    /* window pages passing after publication */
    uint64_t detail;          /* REFUSED_STRUCT: the descriptor refused */
    uint64_t l2_slot;         /* BUILT: the level-2 slot to swap */
    uint64_t l2_new;          /* BUILT: the descriptor to swap in */
    uint64_t table;           /* BUILT: the clone */
    uint32_t cloned;          /* BUILT: 1 when UEFI had a level-3 table there, 0 for a new one */
};

/* Walk the TTBR0_EL2 regime for [base, base+size) and build the clone, as
 * above; e2h is HCR_EL2.E2H. Returns out->state. BUILT means the caller
 * must call el2q_pm_publish and then el2q_pm_verify. */
uint32_t el2q_pm_prepare(uint64_t base, uint64_t size, uint64_t tcr, uint64_t ttbr0,
                         uint64_t mair, int e2h, const struct el2q_pm_hw *hw,
                         struct el2q_pm_result *out);

/* Swap a BUILT clone in. Returns io->state: BUILT once done (verify next),
 * REFUSED_REGIME if io is not BUILT. */
uint32_t el2q_pm_publish(const struct el2q_pm_hw *hw, struct el2q_pm_result *io);

/* After publication: every window page must pass hw->mapped. */
uint32_t el2q_pm_verify(uint64_t base, uint64_t size, const struct el2q_pm_hw *hw,
                        struct el2q_pm_result *io);

/* A fixed word for each outcome, for the report. */
const char *el2q_pm_word(uint32_t state);

#endif
