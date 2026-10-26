/* SPDX-License-Identifier: MIT */
/* el2qual's read-only walk of the SMMU stage-2 tables USB0's DMA goes
 * through (load option "usb-s2-walk"; docs/GAOKUN3-USB-DEVICE-MODE.md).
 *
 * Run 21 found USB0's stream translated by a nested bank whose stage 2
 * (bank 126) has translation on, with its tables at 0xB7609000, inside a
 * device-tree reserved region Linux never maps. q1n1 gives the DWC3
 * physical addresses in a 1 MiB arena that UEFI allocates below 4 GiB, so
 * device mode works only if stage 2 maps that arena 1:1, readable and
 * writable. This walk answers that for an arena el2qual allocates the same
 * way, page by page, as the SMMU would: it never writes, and it reads only
 * descriptors.
 *
 * The tables may be protected by the security controller (XPU), and a
 * refused read may come back as an asynchronous SError that no fault fixup
 * can catch. So:
 *   - a table page is read only through a mapping el2qual already has, and
 *     only if the hardware walk (AT S1E2R) says it is identity, Normal
 *     write-back inner and outer, inner shareable. The SMMU walks those
 *     tables coherently, so their last writes may sit in the CPU caches; a
 *     non-cacheable read could see stale memory, and nothing here may ever
 *     invalidate those lines. No mapping is added here: a page that fails
 *     is reported with its PAR and nothing of it is read;
 *   - an SError already pending before the walk refuses it, since a later
 *     one could not be told apart;
 *   - every read is a single 64-bit load behind the fault fixup, followed
 *     by a check for a pending SError, and the walk stops at the first fault
 *     or SError;
 *   - each descriptor is read at most once, and the total is bounded by
 *     EL2Q_S2W_MAX_READS.
 *
 * Formats followed (the Arm ARM's VMSAv8-64 stage-2 translation, with the
 * SMMUv2 bank registers in their AArch64 form, CBA2R.VA64 set): 4 KiB
 * granule only; the start level from VTCR.SL0 and T0SZ without concatenated
 * start tables; output size from VTCR.PS. Anything else is FORMAT, refused
 * before any read. Nothing here touches hardware: el2qual.c supplies the
 * callbacks, tools/test-el2qual-s2walk.c synthetic ones. */
#ifndef EL2QUAL_S2WALK_H
#define EL2QUAL_S2WALK_H

#include <stdint.h>

/* q1n1's QDWC3_ARENA_SIZE (q1n1 platform/uefi/qcom-dwc3.h), allocated as
 * EfiLoaderData pages below 4 GiB, as q1n1's usb-ebs.c does. */
#define EL2Q_S2W_ARENA_SIZE  0x100000ull
#define EL2Q_S2W_ARENA_PAGES (EL2Q_S2W_ARENA_SIZE >> 12)
/* 256 level-3 descriptors, plus two at each of up to three levels above for
 * an arena that crosses a 2 MiB and a 1 GiB boundary. */
#define EL2Q_S2W_MAX_READS   264u

enum el2q_s2w {
    EL2Q_S2W_NOT_ASKED,      /* option "usb-s2-walk" not given */
    EL2Q_S2W_NO_ARENA,       /* no page-aligned 1 MiB arena below 4 GiB */
    EL2Q_S2W_NO_STAGE2,      /* the stage-2 bank's registers were not all read */
    EL2Q_S2W_NOT_NEEDED,     /* SCTLR.M clear: stage 2 passes addresses through */
    EL2Q_S2W_FORMAT,         /* registers this walk does not follow; nothing read */
    EL2Q_S2W_PENDING,        /* an SError was already pending; nothing read */
    EL2Q_S2W_TABLE_REFUSED,  /* a table page is not mapped as described above */
    EL2Q_S2W_STRUCTURE,      /* a table descriptor points past the output size */
    EL2Q_S2W_FAULT,          /* a read raised a synchronous exception */
    EL2Q_S2W_SERROR,         /* an SError became pending after a read */
    EL2Q_S2W_LIMIT,          /* more than EL2Q_S2W_MAX_READS descriptors */
    EL2Q_S2W_DONE,
    EL2Q_S2W_COUNT
};

/* What each arena page's DMA would meet. */
enum el2q_s2w_page {
    EL2Q_S2W_RW,             /* identity, access flag set, S2AP read and write */
    EL2Q_S2W_LIMITED,        /* identity, access flag set, not both read and write */
    EL2Q_S2W_MOVED,          /* a valid leaf, access flag set, at another address */
    EL2Q_S2W_FAULTS,         /* invalid, reserved, out of range, or access flag clear */
    EL2Q_S2W_PAGE_COUNT
};

/* The stage-2 bank's registers, as usbprobe.c read them. */
struct el2q_s2w_bank {
    uint32_t sctlr, tcr, cba2r;
    uint64_t ttbr;           /* VTTBR: the root's address in bits 47:1 */
};

struct el2q_s2w_hw {
    void *ctx;
    /* PAR_EL1 after AT S1E2R of address: how a read of it would translate
     * at the current level. Must not fault or access the address. */
    uint64_t (*par)(void *ctx, uint64_t address);
    /* One 64-bit load: 0 and *value set, or the ESR of the exception. */
    uint64_t (*read64)(void *ctx, uint64_t address, uint64_t *value);
    /* After the preceding loads complete (DSB): nonzero when a physical
     * SError is pending (ISR_EL1.A). */
    int (*serror)(void *ctx);
};

struct el2q_s2w_result {
    uint32_t state;                          /* enum el2q_s2w */
    uint32_t test;                           /* the tables are el2qual's own fixture */
    uint64_t arena;
    uint32_t pages[EL2Q_S2W_PAGE_COUNT];     /* DONE: arena pages in each class */
    uint32_t reads;                          /* descriptor loads made */
    uint32_t leaf_level;                     /* the arena's first page: where its walk ended */
    uint64_t leaf;                           /* ... and that descriptor; valid when leaf_seen */
    uint32_t leaf_seen;
    uint32_t odd_level;                      /* the first page not RW */
    uint64_t odd_ipa, odd;                   /* ... its address and last descriptor; valid when odd_seen */
    uint32_t odd_seen;
    uint64_t at;                             /* TABLE_REFUSED: the page; STRUCTURE, FAULT, SERROR: the descriptor's address */
    uint64_t detail;                         /* TABLE_REFUSED: its PAR; STRUCTURE: the descriptor; FAULT: the ESR */
};

/* Walk bank's stage 2 for every page of [arena, arena + 1 MiB), as above.
 * bank is NULL when the stage-2 bank was not read. Returns out->state. */
uint32_t el2q_s2w_walk(const struct el2q_s2w_bank *bank, uint64_t arena,
                       const struct el2q_s2w_hw *hw, struct el2q_s2w_result *out);

/* Test only ("emulate-s2-tables", QEMU): build a stage-2 table set for
 * arena in pool, laid out like the gaokun3's bank 126 (VTCR 0x1355C: 4 KiB,
 * 36-bit, start level 1), with every arena page an identity read-write
 * leaf through tables at every level. With odd set, arena pages 1, 2 and 3
 * become moved, invalid and read-only. pool must hold EL2Q_S2W_TEST_PAGES
 * pages. Returns 0 and fills bank, or 1 when the arena is out of range. */
#define EL2Q_S2W_TEST_PAGES 5u
int el2q_s2w_build_test(uint64_t arena, uint64_t pool, int odd,
                        void (*write64)(void *ctx, uint64_t address, uint64_t value), void *ctx,
                        struct el2q_s2w_bank *bank);

/* Fixed-format report lines, empty past the last; see s2walk.c. */
#define EL2Q_S2W_LINE_COLS 64
void el2q_s2w_line(const struct el2q_s2w_result *r, unsigned index, char out[EL2Q_S2W_LINE_COLS + 1]);

#endif
