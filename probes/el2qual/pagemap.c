/* SPDX-License-Identifier: MIT */
/* See pagemap.h. */
#include "pagemap.h"

/* Stage-1 descriptor encodings (the Arm ARM; shadow/mmu.h uses the same). */
#define PT_VALID   1ull
#define PT_TABLE   2ull
#define PT_AF      (1ull << 10)
#define PT_OA      0x0000fffffffff000ull
/* A table descriptor's hierarchical limits (NSTable, APTable, XNTable/
 * UXNTable, PXNTable). They bound everything below the entry, so the swapped
 * entry keeps them: dropping one could widen access to UEFI's own leaves. */
#define PT_TABLE_ATTRS 0xf800000000000000ull
/* Leaf permissions. In the EL2 regime (E2H 0) there is one Exception level:
 * AP[1] is RES1, so read-only is AP[2:1] 0b11, and bit 54 is XN while bit 53
 * is RES0. In the EL2&0 regime (E2H 1) read-only without EL0 access is 0b10,
 * and bits 54 and 53 are UXN and PXN. Device memory must be execute-never:
 * otherwise speculative instruction fetches may reach the window. (The Arm
 * ARM's stage-1 attribute fields; Linux's arm64 KVM hyp page tables encode
 * the same split for nVHE and hVHE.) */
#define PT_RO_EL2  (3ull << 6)
#define PT_RO_EL20 (2ull << 6)
#define PT_XN      (1ull << 54)
#define PT_PXN     (1ull << 53)

#define L2_SIZE    (2ull << 20) /* what one level-2 entry maps at 4K granule */
#define ENTRIES    512u

static unsigned slot_index(unsigned level, uint64_t va)
{
    return (unsigned)(va >> (12 + (3 - level) * 9)) & (ENTRIES - 1);
}

/* A MAIR byte that encodes a Device memory type (nGnRnE, nGnRE, nGRE, GRE):
 * every bit outside 0x0c is clear. Other bytes are Normal or reserved, and
 * a reserved one must fail closed, not be guessed at. */
static int device_byte(uint64_t mair, unsigned index)
{
    return !((mair >> (8 * index)) & 0xf3ull);
}

uint32_t el2q_pm_prepare(uint64_t base, uint64_t size, uint64_t tcr, uint64_t ttbr0,
                         uint64_t mair, int e2h, const struct el2q_pm_hw *hw,
                         struct el2q_pm_result *out)
{
    *out = (struct el2q_pm_result){.state = EL2Q_PM_REFUSED_STRUCT, .attr_index = 0xffu};
    if (!hw || !hw->read64 || !hw->write64 || !hw->alloc || !hw->mapped || !hw->sync)
        return out->state = EL2Q_PM_REFUSED_REGIME;

    /* The window must sit inside a single level-2 entry's region, and inside
     * the VA span T0SZ describes. */
    uint64_t region = base & ~(L2_SIZE - 1);
    if ((base | size) & 0xfff || !size || size > L2_SIZE ||
        ((base + size - 1) & ~(L2_SIZE - 1)) != region)
        return out->state = EL2Q_PM_REFUSED_REGIME;
    uint64_t t0sz = tcr & 0x3f;
    if (((tcr >> 14) & 3) != 0 || t0sz < 16 || t0sz > 39 ||
        base + size - 1 >= (1ull << (64 - t0sz)))
        return out->state = EL2Q_PM_REFUSED_REGIME;
    /* The level the walk starts at, from granule and T0SZ (mmu_start_level's
     * formula, 4K only); this adds at level 3, so the start must be above. */
    unsigned va_bits = (unsigned)(64 - t0sz);
    unsigned levels = (va_bits - 12 + 8) / 9;
    if (levels < 1 || levels > 4) return out->state = EL2Q_PM_REFUSED_REGIME;
    unsigned start = 4 - levels;
    if (start > 2) return out->state = EL2Q_PM_REFUSED_REGIME;

    unsigned attr = 8;
    for (unsigned i = 0; i < 8 && attr == 8; i++)
        if (device_byte(mair, i)) attr = i;
    if (attr == 8) return out->state = EL2Q_PM_REFUSED_DEVICE;
    out->attr_index = attr;

    uint64_t table = ttbr0 & PT_OA;
    if (!table) return out->state = EL2Q_PM_REFUSED_STRUCT;
    uint64_t l2_slot = 0, l2_desc = 0;
    for (unsigned level = start; level <= 2; level++) {
        uint64_t slot = table + 8ull * slot_index(level, base);
        uint64_t desc = hw->read64(hw->ctx, slot);
        if (level == 2) {
            l2_slot = slot;
            l2_desc = desc;
            break;
        }
        if ((desc & 3) != 3) {
            out->detail = desc;
            return out->state = EL2Q_PM_REFUSED_STRUCT;
        }
        table = desc & PT_OA;
    }

    unsigned first = slot_index(3, base), count = (unsigned)(size >> 12);

    /* A level-2 block already covering the region as Device identity leaves
     * the window exactly as wanted: nothing to write. */
    if ((l2_desc & 3) == 1) {
        if ((l2_desc & PT_OA) == region && device_byte(mair, (l2_desc >> 2) & 7)) {
            out->state = EL2Q_PM_NOT_NEEDED;
            out->mapped_before = count;
        } else {
            out->detail = l2_desc;
            out->state = EL2Q_PM_REFUSED_STRUCT;
        }
        return out->state;
    }
    /* An invalid entry that still carries bits is something this does not
     * know the meaning of: refused, as an invalid but non-zero leaf is. */
    if (l2_desc && (l2_desc & 3) != 3) {
        out->detail = l2_desc;
        return out->state = EL2Q_PM_REFUSED_STRUCT;
    }

    /* With a table, an existing window entry must already be the Device
     * identity page wanted: valid entries are never rewritten. */
    uint64_t l3 = l2_desc ? (l2_desc & PT_OA) : 0;
    if (l3) {
        for (unsigned i = 0; i < count; i++) {
            uint64_t page = base + (uint64_t)i * 0x1000;
            uint64_t e = hw->read64(hw->ctx, l3 + 8ull * (first + i));
            if (!e) continue;
            if ((e & 3) != 3 || (e & PT_OA) != page || !device_byte(mair, (e >> 2) & 7)) {
                out->detail = e;
                return out->state = EL2Q_PM_REFUSED_STRUCT;
            }
            out->mapped_before++;
        }
        if (out->mapped_before == count)
            return out->state = EL2Q_PM_NOT_NEEDED;
    }

    uint64_t fresh = hw->alloc(hw->ctx);
    if (!fresh || (fresh & 0xfff)) return out->state = EL2Q_PM_REFUSED_ALLOC;
    /* The new window leaves are read-only and execute-never: this map exists
     * to read, and a bug that stores through it faults instead of touching
     * the hardware behind the window. */
    const uint64_t leaf = (uint64_t)attr << 2 | PT_AF | PT_VALID | PT_TABLE |
                          (e2h ? PT_RO_EL20 | PT_XN | PT_PXN : PT_RO_EL2 | PT_XN);
    for (unsigned i = 0; i < ENTRIES; i++) {
        uint64_t e = l3 ? hw->read64(hw->ctx, l3 + 8ull * i) : 0;
        if (i >= first && i < first + count && !e)
            e = (base + (uint64_t)(i - first) * 0x1000) | leaf;
        hw->write64(hw->ctx, fresh + 8ull * i, e);
    }
    out->l2_slot = l2_slot;
    out->table = fresh;
    out->cloned = l3 != 0;
    out->l2_new = (l2_desc & PT_TABLE_ATTRS) | (fresh & PT_OA) | PT_VALID | PT_TABLE;
    return out->state = EL2Q_PM_BUILT;
}

uint32_t el2q_pm_publish(const struct el2q_pm_hw *hw, struct el2q_pm_result *io)
{
    if (!hw || !hw->write64 || !hw->sync || io->state != EL2Q_PM_BUILT || !io->l2_slot || !io->table)
        return io->state = EL2Q_PM_REFUSED_REGIME;
    /* The clone must be in memory before anything points at it: a walk that
     * does not snoop the caches would otherwise read whatever the page held. */
    hw->sync(hw->ctx, EL2Q_PM_CLEAN, io->table, 0x1000);
    hw->sync(hw->ctx, EL2Q_PM_DSB, 0, 0);
    /* Break: invalidate the old entry and every translation cached through
     * it, so no old and new translation of the region coexist. With an
     * invalid old entry this writes the zero it already holds; invalid
     * entries are never cached, so the flush is then only uniformity. */
    hw->write64(hw->ctx, io->l2_slot, 0);
    hw->sync(hw->ctx, EL2Q_PM_CLEAN, io->l2_slot, 8);
    hw->sync(hw->ctx, EL2Q_PM_DSB, 0, 0);
    hw->sync(hw->ctx, EL2Q_PM_TLBI, 0, 0);
    hw->sync(hw->ctx, EL2Q_PM_DSB, 0, 0);
    /* Make: the new entry, visible to the walker before the next access. */
    hw->write64(hw->ctx, io->l2_slot, io->l2_new);
    hw->sync(hw->ctx, EL2Q_PM_CLEAN, io->l2_slot, 8);
    hw->sync(hw->ctx, EL2Q_PM_DSB, 0, 0);
    hw->sync(hw->ctx, EL2Q_PM_ISB, 0, 0);
    return io->state;
}

uint32_t el2q_pm_verify(uint64_t base, uint64_t size, const struct el2q_pm_hw *hw,
                        struct el2q_pm_result *io)
{
    unsigned count = (unsigned)(size >> 12);
    io->mapped_after = 0;
    for (unsigned i = 0; i < count; i++)
        io->mapped_after += !hw->mapped(hw->ctx, base + ((uint64_t)i << 12));
    io->state = io->mapped_after == count ? EL2Q_PM_OK : EL2Q_PM_REFUSED_VERIFY;
    return io->state;
}

const char *el2q_pm_word(uint32_t state)
{
    static const char *const words[EL2Q_PM_COUNT] = {
        [EL2Q_PM_NOT_ASKED] = "NOT ASKED",
        [EL2Q_PM_NOT_NEEDED] = "NOT NEEDED",
        [EL2Q_PM_BUILT] = "BUILT",
        [EL2Q_PM_OK] = "OK",
        [EL2Q_PM_REFUSED_REGIME] = "REGIME",
        [EL2Q_PM_REFUSED_DEVICE] = "DEVICE",
        [EL2Q_PM_REFUSED_STRUCT] = "STRUCTURE",
        [EL2Q_PM_REFUSED_ALLOC] = "ALLOC",
        [EL2Q_PM_REFUSED_VERIFY] = "VERIFY",
    };
    return state < EL2Q_PM_COUNT ? words[state] : "UNKNOWN";
}
