/* SPDX-License-Identifier: MIT */
/* The S2CR bypass for USB0's stream, planned but never performed (load
 * option "usb-s2-bypass-plan"; docs/GAOKUN3-USB-DEVICE-MODE.md).
 *
 * Runs 21 to 24 established what USB0's DMA meets: stream 0x820 matches
 * stream match group 6 alone, whose S2CR translates through context bank 6;
 * that bank has stage 1 off and nests stage-2 bank 126, whose translation
 * is on and which maps, below 4 GiB, only the 36 MiB the firmware also
 * reserves (0xC6200000-0xC85FFFFF). So q1n1 cannot give the DWC3 an address
 * of its own choosing and have it reach memory.
 *
 * The narrowest change that fixes this is one field of one register: set
 * S2CR(6).TYPE from translate to bypass, so transactions from stream 0x820
 * carry their addresses to memory unchanged. It touches no context bank, no
 * page table, and no other stream: group 6's stream match is exact, so the
 * blast radius is USB0 alone. The alternatives are worse. Repointing bank
 * 126's VTTBR, or editing the firmware's stage-2 tables at 0xB7609000,
 * changes translation for every stream under VMID 3; clearing
 * sCR0.CLIENTPD turns the whole SMMU off.
 *
 * THIS MODULE NEVER WRITES. It has no write callback, so there is no path
 * through it that can reach the SMMU. It reads what it must to be sure the
 * machine is still in the state the plan assumes, and then describes the
 * one word it would change. Performing the write is a separate stage that
 * does not exist yet; the design, and what must be true before it is
 * written, are in docs/GAOKUN3-USB-DEVICE-MODE.md.
 *
 * Register layout from Linux drivers/iommu/arm/arm-smmu/arm-smmu.h, read as
 * a reference: S2CR(n) at 0xc00 + 4n, TYPE in bits 17:16 (0 translate, 1
 * bypass, 2 fault), CBNDX in bits 7:0; SMR(n) at 0x800 + 4n, VALID bit 31,
 * mask 30:16, id 15:0; sCR0 bit 0 CLIENTPD, bit 3 EXIDENABLE; sGFSR at
 * 0x48. Nothing is copied. */
#ifndef EL2QUAL_S2BYPASS_H
#define EL2QUAL_S2BYPASS_H

#include <stdint.h>

enum el2q_s2b {
    EL2Q_S2B_NOT_ASKED,         /* option "usb-s2-bypass-plan" not given */
    EL2Q_S2B_NOT_MEASURED,      /* no stream match, or its bank was not read */
    EL2Q_S2B_NOT_NEEDED,        /* the group already bypasses */
    EL2Q_S2B_REFUSED_SCR0,      /* the SMMU is off, or extended stream IDs are on */
    EL2Q_S2B_REFUSED_TYPE,      /* S2CR does not translate through the bank that was read */
    EL2Q_S2B_REFUSED_MASK,      /* the group does not match USB0's stream alone */
    EL2Q_S2B_REFUSED_SHARED,    /* another valid group matches it too */
    EL2Q_S2B_REFUSED_RANGE,     /* the group is not below NUMSMRG */
    EL2Q_S2B_REFUSED_READ,      /* a read for the check faulted */
    EL2Q_S2B_REFUSED_UNMAPPED,  /* the register's page is not Device identity at EL2 */
    EL2Q_S2B_REFUSED_READONLY,  /* ... or EL2 may not write it */
    EL2Q_S2B_REFUSED_PENDING,   /* an SError was already pending */
    EL2Q_S2B_REFUSED_FAULT,     /* the SMMU already reports a global fault */
    EL2Q_S2B_REFUSED_MOVED,     /* sCR0, the group's SMR or its S2CR changed since it was read */
    EL2Q_S2B_REFUSED_SERROR,    /* an SError became pending during the plan's own reads */
    EL2Q_S2B_PLANNED,
    EL2Q_S2B_COUNT
};

/* What the SMMU read (usbprobe.c) found, as the plan must still find it. */
struct el2q_s2b_seen {
    int matched;                /* USB0's stream was matched by a group */
    int bank_read;              /* that group's context bank was read in full */
    uint64_t base;              /* the SMMU's register window */
    uint32_t stream;            /* USB0's stream id */
    uint32_t scr0, idr0, smr, s2cr, group, cb_index;
};

struct el2q_s2b_hw {
    void *ctx;
    /* 0 when a read of address translates, at the current level, to the same
     * physical address as Device memory (AT S1E2R). Must not fault. */
    int (*mapped)(void *ctx, uint64_t address);
    /* The same for a write (AT S1E2W): 0 when EL2 may write it. */
    int (*writable)(void *ctx, uint64_t address);
    /* One 32-bit read: 0 and *value set, or the ESR of the exception. */
    uint64_t (*read32)(void *ctx, uint64_t address, uint32_t *value);
    /* Nonzero when a physical SError is pending (ISR_EL1.A). */
    int (*serror)(void *ctx);
};

struct el2q_s2b_result {
    uint32_t state;             /* enum el2q_s2b */
    uint64_t at;                /* S2CR(group)'s address */
    uint32_t old, planned;      /* the one word, as read and as it would be written */
    uint32_t restore;           /* what a later quiesce would write back: old */
    uint32_t group;             /* the matching group, as the plan still sees it */
    uint32_t scr0;              /* sCR0, as the SMMU read found it */
    uint32_t matches, scanned;  /* valid groups matching USB0's stream; groups scanned */
    uint32_t mask;              /* the matching group's stream mask */
    uint32_t gfsr;              /* sGFSR before */
    uint64_t detail;            /* READ: the ESR; UNMAPPED, READONLY: the address */
};

/* Decide whether the one-field change is safe to make, and describe it.
 * Returns out->state; PLANNED means out->at, out->old and out->planned
 * describe a write that nothing here performs. */
uint32_t el2q_s2b_plan(const struct el2q_s2b_seen *seen, const struct el2q_s2b_hw *hw,
                       struct el2q_s2b_result *out);

/* Fixed-format report lines, empty past the last; see s2bypass.c. */
#define EL2Q_S2B_LINE_COLS 64
#define EL2Q_S2B_LINES 3
void el2q_s2b_line(const struct el2q_s2b_result *r, unsigned index,
                   char out[EL2Q_S2B_LINE_COLS + 1]);

#endif
