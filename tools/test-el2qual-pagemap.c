/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/pagemap.c, el2qual's private Device map of
 * the SMMU window. Linked into tools/test-el2qual.c's binary by
 * tools/test-el2qual.py; test_pagemap() returns its check count.
 *
 * The fake memory logs every write and every maintenance step, in order.
 * The properties that matter most: a valid descriptor is never rewritten,
 * anything the walk does not recognise is refused without a write, building
 * writes only the clone, publication is break-before-make in the exact
 * order pagemap.h gives, and the new leaves carry the read-only and
 * execute-never bits of the regime they are walked in. */
#include "report.h"
#include "pagemap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

unsigned test_pagemap(void);

#define MAXW 8192
/* One logged event: a write (op EV_WRITE) or a maintenance step. */
#define EV_WRITE 100u
struct event { uint32_t op; uint64_t addr, val; };
static struct {
    uint64_t addr[MAXW], val[MAXW];
    unsigned n;
    uint64_t writes[MAXW];                     /* addresses written, in order */
    unsigned nwrites;
    struct event ev[MAXW];
    unsigned nev;
    uint64_t alloc_base;
    unsigned alloc_left, alloc_used;
    uint64_t unmapped[512];                    /* pages the AT check still refuses */
    unsigned nunmapped;
} f;

static void freset(void) { memset(&f, 0, sizeof f); }

static void fset(uint64_t a, uint64_t v)
{
    for (unsigned i = 0; i < f.n; i++)
        if (f.addr[i] == a) { f.val[i] = v; return; }
    f.addr[f.n] = a;
    f.val[f.n] = v;
    f.n++;
}

static uint64_t fread64(void *ctx, uint64_t a)
{
    (void)ctx;
    for (unsigned i = 0; i < f.n; i++)
        if (f.addr[i] == a) return f.val[i];
    return 0;
}

static void fwrite64(void *ctx, uint64_t a, uint64_t v)
{
    (void)ctx;
    if (f.nwrites < MAXW) f.writes[f.nwrites++] = a;
    if (f.nev < MAXW) f.ev[f.nev++] = (struct event){EV_WRITE, a, v};
    fset(a, v);
}

static void fsync(void *ctx, uint32_t op, uint64_t a, uint64_t size)
{
    (void)ctx;
    if (f.nev < MAXW) f.ev[f.nev++] = (struct event){op, a, size};
}

static uint64_t falloc(void *ctx)
{
    (void)ctx;
    if (!f.alloc_left) return 0;
    f.alloc_left--;
    f.alloc_used++;
    return f.alloc_base + (uint64_t)(f.alloc_used - 1) * 0x1000;
}

static int fmapped(void *ctx, uint64_t a)
{
    (void)ctx;
    for (unsigned i = 0; i < f.nunmapped; i++)
        if (f.unmapped[i] == a) return 1;
    return 0;
}

static const struct el2q_pm_hw fhw = {0, fread64, fwrite64, falloc, fmapped, fsync};

/* EDK2's MAIR: attr0 0x00 Device-nGnRnE, attr1 0x44, attr2 0xbb, attr3 0xff. */
#define MAIR  0xffbb4400ull
/* TCR_EL2 as the gaokun3 VHE exit leaves it (VERIFIED, run 11): T0SZ 24, TG0 4K. */
#define TCR   0x80823518ull
#define ROOT  0x100000ull
#define L1    0x101000ull
#define L2    0x102000ull
#define L3    0x103000ull
#define NEW   0x200000ull                     /* the allocation pool's first page */
#define BASE  0x15000000ull                   /* the SMMU window */
#define SIZE  0x100000ull
#define L2SLOT (L2 + 8ull * 168)              /* 0x15000000's level-2 entry */

/* The leaves pagemap.c writes, attr index 0 and AF. In the EL2 regime
 * (E2H 0): AP[2:1] 0b11 (AP[1] is RES1 there) and XN, bit 54; bit 53 is
 * RES0. In the EL2&0 regime (E2H 1): AP[2:1] 0b10, UXN and PXN. */
#define NEWLEAF(page) ((page) | (1ull << 10) | (3ull << 6) | (1ull << 54) | 3)
#define NEWLEAF_E2H(page) ((page) | (1ull << 10) | (2ull << 6) | (3ull << 53) | 3)
/* UEFI's table entry for the region, with hierarchical limits set
 * (XNTable/UXNTable and PXNTable): the swapped entry must keep them. */
#define TABLE_ATTRS (3ull << 59)
/* A leaf as UEFI writes one: attr index 0, AF, read-write, executable. */
#define OLDLEAF(page) ((page) | (1ull << 10) | 3)

/* The gaokun3's shape (run 20): sCR0's page, GR1's and bank 6's mapped as
 * Device identity, the rest of the window absent. T0SZ 24 starts at level 0;
 * the window's indices are L0 0, L1 0, L2 168, L3 0..255. */
static void gaokun3_tables(void)
{
    freset();
    f.alloc_base = NEW;
    f.alloc_left = 2;
    fset(ROOT + 0, L1 | 3);
    fset(L1 + 0, L2 | 3);
    fset(L2SLOT, TABLE_ATTRS | L3 | 3);
    fset(L3 + 8 * 0, OLDLEAF(0x15000000));
    fset(L3 + 8 * 1, OLDLEAF(0x15001000));
    fset(L3 + 8 * 134, OLDLEAF(0x15086000));
}

/* The gaokun3 runs the map after the VHE exit: the EL2 regime, E2H 0. */
static uint32_t prepare(uint64_t base, uint64_t size, uint64_t tcr, uint64_t ttbr0,
                        uint64_t mair, struct el2q_pm_result *r)
{
    return el2q_pm_prepare(base, size, tcr, ttbr0, mair, 0, &fhw, r);
}

/* Writes into a pre-existing page (outside the freshly allocated table). */
static unsigned foreign_writes(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < f.nwrites; i++)
        n += f.writes[i] < NEW || f.writes[i] >= NEW + f.alloc_used * 0x1000;
    return n;
}

static void test_regime(void)
{
    struct el2q_pm_result r;

    /* Misuse: no operations at all. */
    gaokun3_tables();
    CHECK(el2q_pm_prepare(BASE, SIZE, TCR, ROOT, MAIR, 0, 0, &r) == EL2Q_PM_REFUSED_REGIME);
    struct el2q_pm_hw nosync = fhw;
    nosync.sync = 0;
    CHECK(el2q_pm_prepare(BASE, SIZE, TCR, ROOT, MAIR, 0, &nosync, &r) == EL2Q_PM_REFUSED_REGIME);
    /* Publishing anything not BUILT writes nothing. */
    r = (struct el2q_pm_result){.state = EL2Q_PM_NOT_NEEDED, .l2_slot = L2SLOT, .table = NEW};
    CHECK(el2q_pm_publish(&fhw, &r) == EL2Q_PM_REFUSED_REGIME);
    CHECK(f.nev == 0);
    CHECK(foreign_writes() == 0);

    /* The window must sit inside one level-2 region. */
    gaokun3_tables();
    CHECK(prepare(0x15100000, 0x200000, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_REGIME);
    CHECK(foreign_writes() == 0);

    /* TG0 16K, T0SZ out of range, a window past the translated span. */
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, TCR | 2ull << 14, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_REGIME);
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, (TCR & ~0x3full) | 15, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_REGIME);
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, (TCR & ~0x3full) | 40, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_REGIME);
}

static void test_refusals(void)
{
    struct el2q_pm_result r;

    /* No Device attribute index in MAIR_EL2. */
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, TCR, ROOT, 0xffffffffffffffffull, &r) == EL2Q_PM_REFUSED_DEVICE);
    CHECK(foreign_writes() == 0);

    /* Only a reserved byte with the high nibble clear (0x03) and Normal ones
     * (unused MAIR bytes read 0x00, Device-nGnRnE, so all eight are set). */
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, TCR, ROOT, 0xffffffffffbb4403ull, &r) == EL2Q_PM_REFUSED_DEVICE);
    CHECK(foreign_writes() == 0 && f.alloc_used == 0);
    /* A reserved byte at index 0 is skipped for the Device byte at 1, and
     * UEFI's index-0 leaves are then not Device: refused, not rewritten. */
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, TCR, ROOT, 0xffffffffffbb0403ull, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.attr_index == 1 && r.detail == OLDLEAF(0x15000000) && f.alloc_used == 0);
    freset();
    f.alloc_base = NEW;
    f.alloc_left = 1;
    fset(ROOT + 0, L1 | 3);
    fset(L1 + 0, L2 | 3);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, 0xffffffffffbb0403ull, &r) == EL2Q_PM_BUILT);
    CHECK(r.attr_index == 1 && fread64(0, NEW) == (NEWLEAF(0x15000000) | 1ull << 2));

    /* A missing level-0 entry. */
    gaokun3_tables();
    fset(ROOT + 0, 0);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.detail == 0 && foreign_writes() == 0);

    /* A 1 GiB block at level 1: this adds nothing but level-3 leaves. */
    gaokun3_tables();
    fset(L1 + 0, (1ull << 10) | 1);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.detail == ((1ull << 10) | 1) && foreign_writes() == 0);

    /* A 2 MiB block, but not Device. */
    gaokun3_tables();
    fset(L2SLOT, 0x15000000 | (3ull << 2) | (1ull << 10) | 1);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.detail == (0x15000000 | (3ull << 2) | (1ull << 10) | 1));

    /* A 2 MiB Device block not at its own address. */
    gaokun3_tables();
    fset(L2SLOT, 0x15200000 | (1ull << 10) | 1);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.detail == (0x15200000 | (1ull << 10) | 1));

    /* A valid window entry that is not Device. */
    gaokun3_tables();
    fset(L3 + 8 * 254, 0x150FE000 | (3ull << 2) | (1ull << 10) | 3);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.detail == (0x150FE000 | (3ull << 2) | (1ull << 10) | 3));

    /* A valid window entry, Device but not identity. */
    gaokun3_tables();
    fset(L3 + 8 * 254, 0x20000000 | (1ull << 10) | 3);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.detail == (0x20000000 | (1ull << 10) | 3));

    /* An invalid level-2 entry that still carries bits. */
    gaokun3_tables();
    fset(L2SLOT, L3 | 2);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(r.detail == (L3 | 2) && foreign_writes() == 0 && f.alloc_used == 0);
    gaokun3_tables();
    fset(L2SLOT, 1ull << 54);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_STRUCT);
    CHECK(foreign_writes() == 0);

    /* No table page left. */
    gaokun3_tables();
    f.alloc_left = 0;
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_REFUSED_ALLOC);
    CHECK(foreign_writes() == 0);
}

/* Publication, exactly as pagemap.h orders it: the clone reaches the point
 * of coherency before anything points at it; the old entry is invalidated
 * and flushed (break) before the new one is written (make). */
static void check_publication(const struct el2q_pm_result *r, unsigned from)
{
    const struct event want[] = {
        {EL2Q_PM_CLEAN, NEW, 0x1000}, {EL2Q_PM_DSB, 0, 0},
        {EV_WRITE, r->l2_slot, 0}, {EL2Q_PM_CLEAN, r->l2_slot, 8}, {EL2Q_PM_DSB, 0, 0},
        {EL2Q_PM_TLBI, 0, 0}, {EL2Q_PM_DSB, 0, 0},
        {EV_WRITE, r->l2_slot, r->l2_new}, {EL2Q_PM_CLEAN, r->l2_slot, 8}, {EL2Q_PM_DSB, 0, 0},
        {EL2Q_PM_ISB, 0, 0},
    };
    unsigned n = sizeof want / sizeof want[0];
    CHECK(f.nev == from + n);
    for (unsigned i = 0; i < n && from + i < f.nev; i++) {
        CHECK(f.ev[from + i].op == want[i].op);
        CHECK(f.ev[from + i].addr == want[i].addr);
        CHECK(f.ev[from + i].val == want[i].val);
    }
}

/* Events before publication: the clone's writes only, and no maintenance. */
static void check_build_only(void)
{
    for (unsigned i = 0; i < f.nev; i++) {
        CHECK(f.ev[i].op == EV_WRITE);
        CHECK(f.ev[i].addr >= NEW && f.ev[i].addr < NEW + 0x1000);
    }
    CHECK(f.nev == 512);
}

static void test_clone(void)
{
    struct el2q_pm_result r;

    /* The gaokun3's shape: clone the level-3 table, fill the holes. */
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_BUILT);
    CHECK(r.attr_index == 0 && r.mapped_before == 3 && r.l2_slot == L2SLOT && r.cloned == 1);
    CHECK(r.table == NEW && f.alloc_used == 1);
    /* Building writes only the clone: UEFI's slot still points at its table. */
    check_build_only();
    CHECK(foreign_writes() == 0 && fread64(0, L2SLOT) == (TABLE_ATTRS | L3 | 3));
    /* The swapped-in entry keeps UEFI's hierarchical limits. */
    CHECK(r.l2_new == (TABLE_ATTRS | NEW | 3));
    /* The clone: valid entries verbatim, holes filled, outside the window zero. */
    CHECK(fread64(0, NEW + 8 * 0) == OLDLEAF(0x15000000));
    CHECK(fread64(0, NEW + 8 * 1) == OLDLEAF(0x15001000));
    CHECK(fread64(0, NEW + 8 * 134) == OLDLEAF(0x15086000));
    CHECK(fread64(0, NEW + 8 * 254) == NEWLEAF(0x150FE000));
    CHECK(fread64(0, NEW + 8 * 255) == NEWLEAF(0x150FF000));
    CHECK(fread64(0, NEW + 8 * 135) == NEWLEAF(0x15087000));
    CHECK(fread64(0, NEW + 8 * 300) == 0);
    /* No new leaf sets bit 53, RES0 in the EL2 regime. */
    for (unsigned i = 0; i < 256; i++)
        CHECK(!(fread64(0, NEW + 8 * i) & (1ull << 53)));
    /* UEFI's level-3 table is never touched. */
    CHECK(fread64(0, L3 + 8 * 254) == 0 && fread64(0, L3 + 8 * 1) == OLDLEAF(0x15001000));

    unsigned built = f.nev;
    CHECK(el2q_pm_publish(&fhw, &r) == EL2Q_PM_BUILT);
    check_publication(&r, built);
    /* Two stores into a pre-existing page, both the level-2 slot. */
    CHECK(foreign_writes() == 2 && f.writes[f.nwrites - 1] == L2SLOT && f.writes[f.nwrites - 2] == L2SLOT);
    CHECK(fread64(0, L2SLOT) == (TABLE_ATTRS | NEW | 3));
    /* Publication verified by the hardware walk. */
    CHECK(el2q_pm_verify(BASE, SIZE, &fhw, &r) == EL2Q_PM_OK);
    CHECK(r.mapped_after == 256);

    /* The same, but the walk still refuses six pages. */
    gaokun3_tables();
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_BUILT);
    CHECK(el2q_pm_publish(&fhw, &r) == EL2Q_PM_BUILT);
    for (unsigned i = 0; i < 6; i++) f.unmapped[f.nunmapped++] = BASE + i * 0x1000;
    CHECK(el2q_pm_verify(BASE, SIZE, &fhw, &r) == EL2Q_PM_REFUSED_VERIFY);
    CHECK(r.mapped_after == 250);

    /* The EL2&0 regime (E2H 1): its own read-only and execute-never bits. */
    gaokun3_tables();
    CHECK(el2q_pm_prepare(BASE, SIZE, TCR, ROOT, MAIR, 1, &fhw, &r) == EL2Q_PM_BUILT);
    CHECK(fread64(0, NEW + 8 * 254) == NEWLEAF_E2H(0x150FE000));
    CHECK(fread64(0, NEW + 8 * 1) == OLDLEAF(0x15001000));
}

static void test_shapes(void)
{
    struct el2q_pm_result r;

    /* Every window page already a Device identity leaf: nothing to write. */
    gaokun3_tables();
    for (unsigned i = 0; i < 256; i++) fset(L3 + 8 * i, OLDLEAF(BASE + i * 0x1000));
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_NOT_NEEDED);
    CHECK(r.mapped_before == 256 && f.alloc_used == 0 && f.nev == 0);

    /* A Device identity 2 MiB block: nothing to write. */
    gaokun3_tables();
    fset(L2SLOT, 0x15000000 | (1ull << 10) | 1);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_NOT_NEEDED);
    CHECK(r.mapped_before == 256 && f.alloc_used == 0 && f.nev == 0);

    /* An invalid level-2 entry: a fresh table with only the window mapped,
     * no table attributes to keep, and the same publication (its break
     * writes the zero the slot already holds). */
    gaokun3_tables();
    fset(L2SLOT, 0);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_BUILT);
    CHECK(r.mapped_before == 0 && f.alloc_used == 1 && r.cloned == 0 && r.l2_new == (NEW | 3));
    check_build_only();
    CHECK(fread64(0, NEW + 8 * 0) == NEWLEAF(0x15000000));
    CHECK(fread64(0, NEW + 8 * 255) == NEWLEAF(0x150FF000));
    CHECK(fread64(0, NEW + 8 * 256) == 0 && fread64(0, NEW + 8 * 511) == 0);
    unsigned built = f.nev;
    CHECK(el2q_pm_publish(&fhw, &r) == EL2Q_PM_BUILT);
    check_publication(&r, built);

    /* T0SZ 25 starts at level 1: the root is the level-1 table itself. */
    freset();
    f.alloc_base = NEW;
    f.alloc_left = 2;
    fset(ROOT + 0, L2 | 3);
    fset(L2SLOT, L3 | 3);
    fset(L3 + 8 * 1, OLDLEAF(0x15001000));
    CHECK(prepare(BASE, SIZE, (TCR & ~0x3full) | 25, ROOT, MAIR, &r) == EL2Q_PM_BUILT);
    CHECK(r.l2_slot == L2SLOT && r.l2_new == (NEW | 3));
    CHECK(fread64(0, NEW + 8 * 254) == NEWLEAF(0x150FE000));

    /* A second window in the same 2 MiB region clones el2qual's own first
     * clone (QEMU's "emulate-map-hole" does this): the first window's leaves
     * are carried over, the second's holes filled. */
    gaokun3_tables();
    fset(L2SLOT, 0);
    CHECK(prepare(BASE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_BUILT);
    CHECK(el2q_pm_publish(&fhw, &r) == EL2Q_PM_BUILT);
    CHECK(prepare(BASE + SIZE, SIZE, TCR, ROOT, MAIR, &r) == EL2Q_PM_BUILT);
    CHECK(r.cloned == 1 && r.mapped_before == 0 && r.table == NEW + 0x1000);
    CHECK(fread64(0, NEW + 0x1000 + 8 * 0) == NEWLEAF(0x15000000));
    CHECK(fread64(0, NEW + 0x1000 + 8 * 256) == NEWLEAF(0x15100000));
    CHECK(fread64(0, NEW + 0x1000 + 8 * 511) == NEWLEAF(0x151FF000));
}

unsigned test_pagemap(void)
{
    test_regime();
    test_refusals();
    test_clone();
    test_shapes();
    CHECK(!strcmp(el2q_pm_word(EL2Q_PM_REFUSED_STRUCT), "STRUCTURE"));
    CHECK(!strcmp(el2q_pm_word(0xffffffffu), "UNKNOWN"));
    return checks;
}
