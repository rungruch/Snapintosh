/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/s2walk.c, el2qual's read-only walk of the
 * SMMU stage-2 tables for a DMA arena. Linked into tools/test-el2qual.c's
 * binary by tools/test-el2qual.py; test_s2walk() returns its check count.
 *
 * The fake memory answers AT with a PAR per page and logs every load. The
 * properties that matter most, because the real tables may sit behind the
 * security controller: nothing is read from a page before the AT check has
 * passed it as cacheable, inner-shareable identity memory; nothing at all is
 * read when the registers are not a format the walk follows or an SError is
 * already pending; the walk stops at the first fault or SError; each
 * descriptor is read once; and each arena page is classed as the SMMU would
 * treat it. */
#include "report.h"
#include "usbprobe.h"
#include "s2walk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

unsigned test_s2walk(void);

#define SLOTS 16384u
#define MAXLOG 2048u
#define GOOD_ATTR (0xffull << 56 | 3ull << 7)   /* Normal WB inner and outer, inner shareable */
#define ESR_SEA 0x96000010ull
/* What el2q_s2w_build_test writes for an identity page. */
#define LEAF (3ull | 0xfull << 2 | 3ull << 6 | 3ull << 8 | 1ull << 10)

static struct {
    uint64_t addr[SLOTS], val[SLOTS];
    uint8_t used[SLOTS];
    uint64_t par_page[64], par_value[64];       /* overrides; others get GOOD_ATTR identity */
    unsigned npar;
    uint64_t asked[MAXLOG];                      /* pages AT was asked about, in order */
    unsigned nasked;
    uint64_t reads[MAXLOG];
    unsigned nreads;
    uint64_t fault_at, serror_at;
    int serror_before, serror_now;
} f;

static void freset(void) { memset(&f, 0, sizeof f); }

static unsigned slot_of(uint64_t a)
{
    unsigned h = (unsigned)((a >> 3) * 2654435761u) % SLOTS;
    while (f.used[h] && f.addr[h] != a) h = (h + 1) % SLOTS;
    return h;
}

static void fwrite64(void *ctx, uint64_t a, uint64_t v)
{
    (void)ctx;
    unsigned h = slot_of(a);
    f.used[h] = 1;
    f.addr[h] = a;
    f.val[h] = v;
}

static uint64_t fpeek(uint64_t a)
{
    unsigned h = slot_of(a);
    return f.used[h] ? f.val[h] : 0;
}

static void fpar(uint64_t page, uint64_t par)
{
    f.par_page[f.npar] = page;
    f.par_value[f.npar++] = par;
}

static uint64_t par_of(uint64_t a)
{
    for (unsigned i = 0; i < f.npar; i++)
        if (f.par_page[i] == (a & ~0xfffull)) return f.par_value[i];
    return (a & 0x0000fffffffff000ull) | GOOD_ATTR;
}

static uint64_t ffpar(void *ctx, uint64_t a)
{
    (void)ctx;
    if (f.nasked < MAXLOG) f.asked[f.nasked++] = a;
    return par_of(a);
}

static uint64_t fread64(void *ctx, uint64_t a, uint64_t *v)
{
    (void)ctx;
    if (f.nreads < MAXLOG) f.reads[f.nreads++] = a;
    if (a == f.fault_at) return ESR_SEA;
    if (a == f.serror_at) f.serror_now = 1;
    *v = fpeek(a);
    return 0;
}

static int fserror(void *ctx)
{
    (void)ctx;
    return f.serror_before || f.serror_now;
}

static const struct el2q_s2w_hw hw = {0, ffpar, fread64, fserror};

/* Every load hit a page AT had been asked about and answered for with a
 * successful translation, and none was loaded twice. */
static void check_reads_disciplined(void)
{
    for (unsigned i = 0; i < f.nreads; i++) {
        int passed = 0;
        for (unsigned j = 0; j < f.nasked && !passed; j++)
            if (f.asked[j] == (f.reads[i] & ~0xfffull) && !(par_of(f.asked[j]) & 1)) passed = 1;
        CHECK(passed);
        for (unsigned j = 0; j < i; j++) CHECK(f.reads[j] != f.reads[i]);
    }
}

static struct el2q_s2w_bank fixture(uint64_t arena, uint64_t pool, int odd)
{
    struct el2q_s2w_bank bank;
    CHECK(el2q_s2w_build_test(arena, pool, odd, fwrite64, 0, &bank) == 0);
    return bank;
}

static void line(const struct el2q_s2w_result *r, unsigned i, const char *want)
{
    char out[EL2Q_S2W_LINE_COLS + 1];
    el2q_s2w_line(r, i, out);
    if (strcmp(out, want)) fprintf(stderr, "line %u: got \"%s\", want \"%s\"\n", i, out, want);
    CHECK(!strcmp(out, want));
}

#define POOL  0xb7609000ull   /* run 21's VTTBR, as a stand-in */
#define ARENA 0x9f600000ull   /* 2 MiB aligned, inside one level-3 table */

static void test_fixture_identity(void)
{
    struct el2q_s2w_result r;
    /* The gaokun3's shape: level 1, 2 and 3 tables. */
    freset();
    struct el2q_s2w_bank b = fixture(ARENA, POOL, 0);
    CHECK(b.tcr == 0x1355c && b.sctlr == 0xe1 && b.cba2r == 1 && b.ttbr == POOL);
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_RW] == 256 && r.pages[EL2Q_S2W_LIMITED] == 0 &&
          r.pages[EL2Q_S2W_MOVED] == 0 && r.pages[EL2Q_S2W_FAULTS] == 0);
    CHECK(r.reads == 258);
    CHECK(r.leaf_seen && r.leaf_level == 3 && r.leaf == (ARENA | LEAF));
    CHECK(!r.odd_seen);
    CHECK(f.reads[0] == POOL + (ARENA >> 30) * 8);
    check_reads_disciplined();
    line(&r, 0, "SMMU WALK ARENA 9F600000 READS 258");
    line(&r, 1, "SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0");
    line(&r, 2, "SMMU WALK LEAF L3 000000009F6007FF");
    line(&r, 3, "");
    r.test = 1;
    line(&r, 0, "SMMU WALK ARENA 9F600000 READS 258 (TEST TABLES)");

    /* Across a 2 MiB boundary: a second level-2 entry and level-3 table. */
    freset();
    b = fixture(0x9f780000, POOL, 0);
    CHECK(el2q_s2w_walk(&b, 0x9f780000, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_RW] == 256 && r.reads == 259);
    check_reads_disciplined();

    /* Across a 1 GiB boundary: two level-1 entries too. */
    freset();
    b = fixture(0xbff80000, POOL, 0);
    CHECK(el2q_s2w_walk(&b, 0xbff80000, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_RW] == 256 && r.reads == 260);
    check_reads_disciplined();

    /* The odd fixture, its pages 1 to 3 in either level-3 table. */
    const uint64_t at[] = {ARENA, 0x9f7fe000, 0x9f7ff000};
    for (unsigned k = 0; k < 3; k++) {
        freset();
        b = fixture(at[k], POOL, 1);
        CHECK(el2q_s2w_walk(&b, at[k], &hw, &r) == EL2Q_S2W_DONE);
        CHECK(r.pages[EL2Q_S2W_RW] == 253 && r.pages[EL2Q_S2W_LIMITED] == 1 &&
              r.pages[EL2Q_S2W_MOVED] == 1 && r.pages[EL2Q_S2W_FAULTS] == 1);
        CHECK(r.odd_seen && r.odd_ipa == at[k] + 0x1000 && r.odd_level == 3 &&
              r.odd == ((at[k] + 0x1000 + (2ull << 20)) | LEAF));
        check_reads_disciplined();
    }
    line(&r, 1, "SMMU WALK RW 253 LIMITED 1 MOVED 1 FAULTS 1");
    line(&r, 3, "SMMU WALK ODD 9F800000 L3 000000009FA007FF");
    line(&r, 4, "");

    /* The fixture refuses an arena it cannot describe. */
    CHECK(el2q_s2w_build_test(0, POOL, 0, fwrite64, 0, &b) == 1);
    CHECK(el2q_s2w_build_test(ARENA + 8, POOL, 0, fwrite64, 0, &b) == 1);
    CHECK(el2q_s2w_build_test(1ull << 36, POOL, 0, fwrite64, 0, &b) == 1);
}

/* A level-1 table at POOL, a level-2 one at POOL + 0x1000, for ARENA. */
static struct el2q_s2w_bank two_levels(uint64_t l2_entry)
{
    freset();
    fwrite64(0, POOL + (ARENA >> 30) * 8, (POOL + 0x1000) | 3);
    fwrite64(0, POOL + 0x1000 + ((ARENA >> 21) & 511) * 8, l2_entry);
    return (struct el2q_s2w_bank){.sctlr = 0xe1, .tcr = 0x1355c, .cba2r = 1, .ttbr = POOL};
}

static void test_classes(void)
{
    struct el2q_s2w_result r;
    struct el2q_s2w_bank b;
    const uint64_t block = 1 | 0xfull << 2 | 3ull << 6 | 3ull << 8 | 1ull << 10;

    /* A 2 MiB identity block: two reads for the whole arena. */
    b = two_levels(ARENA | block);
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_RW] == 256 && r.reads == 2 && r.leaf_level == 2);
    /* The same block elsewhere, or with its access flag clear, or read only,
     * or write only, or beyond the 36-bit output size. */
    b = two_levels((ARENA + (2ull << 20)) | block);
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_MOVED] == 256);
    b = two_levels((ARENA | block) & ~(1ull << 10));
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_FAULTS] == 256);
    b = two_levels((ARENA | block) & ~(2ull << 6));
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_LIMITED] == 256);
    b = two_levels((ARENA | block) & ~(1ull << 6));
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_LIMITED] == 256);
    b = two_levels((1ull << 36) | ARENA | block);
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_FAULTS] == 256);
    /* An invalid level-2 entry: one fault class, read once. */
    b = two_levels(0);
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_FAULTS] == 256 && r.reads == 2 && r.odd_level == 2 && r.odd == 0);
    check_reads_disciplined();

    /* A 1 GiB identity block at level 1. */
    freset();
    fwrite64(0, POOL + 2 * 8, (2ull << 30) | block);
    b = (struct el2q_s2w_bank){.sctlr = 0xe1, .tcr = 0x1355c, .cba2r = 1, .ttbr = POOL};
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_RW] == 256 && r.reads == 1 && r.leaf_level == 1);

    /* A level-3 descriptor with bits 1:0 0b01 is reserved: it faults. */
    b = two_levels((POOL + 0x2000) | 3);
    for (unsigned i = 0; i < 256; i++)
        fwrite64(0, POOL + 0x2000 + (((ARENA >> 12) + i) & 511) * 8, (ARENA + (i << 12)) | (LEAF & ~2ull));
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_FAULTS] == 256);

    /* A block at level 0 is reserved at 4 KiB: SL0 2, T0SZ 24 (40 bits). */
    freset();
    fwrite64(0, POOL, block);
    b = (struct el2q_s2w_bank){.sctlr = 0xe1, .tcr = 24 | 2u << 6 | 0x3500 | 2u << 16, .cba2r = 1, .ttbr = POOL};
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_FAULTS] == 256 && r.odd_level == 0 && r.reads == 1);

    /* An arena past the input size faults without a read: T0SZ 33, 31 bits. */
    freset();
    b = (struct el2q_s2w_bank){.sctlr = 0xe1, .tcr = 33 | 1u << 6 | 0x3500 | 1u << 16, .cba2r = 1, .ttbr = POOL};
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.pages[EL2Q_S2W_FAULTS] == 256 && r.reads == 0 && !r.leaf_seen && r.odd_seen);
    line(&r, 2, "SMMU WALK ODD 9F600000 L1 0000000000000000");

    /* CnP in VTTBR bit 0 is not part of the root's address. */
    b = two_levels(ARENA | block);
    b.ttbr |= 1;
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_RW] == 256);
}

/* Refusals made before any read: no AT question about a table, no load. */
static void refused_unread(const struct el2q_s2w_bank *b, uint64_t arena, uint32_t state, const char *want)
{
    struct el2q_s2w_result r;
    f.nreads = f.nasked = 0;
    CHECK(el2q_s2w_walk(b, arena, &hw, &r) == state);
    CHECK(f.nreads == 0 && f.nasked == 0 && r.reads == 0);
    line(&r, 0, want);
    line(&r, 1, "");
}

static void test_refusals(void)
{
    struct el2q_s2w_result r;
    const struct el2q_s2w_bank good = {.sctlr = 0xe1, .tcr = 0x1355c, .cba2r = 1, .ttbr = POOL};
    struct el2q_s2w_bank b;
    freset();
    fixture(ARENA, POOL, 0);

    refused_unread(&good, 0, EL2Q_S2W_NO_ARENA, "SMMU WALK NOT RUN: NO ARENA");
    refused_unread(&good, ARENA + 0x800, EL2Q_S2W_NO_ARENA, "SMMU WALK NOT RUN: NO ARENA");
    refused_unread(&good, 0xfff80000, EL2Q_S2W_NO_ARENA, "SMMU WALK NOT RUN: NO ARENA");
    refused_unread(0, ARENA, EL2Q_S2W_NO_STAGE2, "SMMU WALK NOT RUN: NO STAGE 2");
    b = good; b.sctlr &= ~1u;
    refused_unread(&b, ARENA, EL2Q_S2W_NOT_NEEDED, "SMMU WALK NOT NEEDED: STAGE 2 OFF");

    const char *fmt = "SMMU WALK NOT RUN: FORMAT";
    b = good; b.cba2r = 0; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    b = good; b.tcr |= 1u << 14; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);   /* 64 KiB */
    b = good; b.tcr |= 2u << 14; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);   /* 16 KiB */
    b = good; b.tcr |= 3u << 6; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);    /* SL0 3 */
    b = good; b.tcr = (b.tcr & ~(7u << 16)) | 6u << 16; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    b = good; b.tcr = (b.tcr & ~0x3fu) | 15; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    /* 40 bits from level 1 needs concatenated tables; 36 from level 2 too.
     * The root is aligned for the concatenated size, so only the level
     * rule refuses them. */
    b = good; b.tcr = (b.tcr & ~(0x3fu | 7u << 16)) | 24 | 2u << 16; b.ttbr = 0xb7600000;
    refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    b = good; b.tcr &= ~(3u << 6); b.ttbr = 0xb7600000;
    refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    /* 36-bit input with a 32-bit output size. */
    b = good; b.tcr &= ~(7u << 16); refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    /* A root not aligned to its 64-entry, 512-byte table, or past 36 bits. */
    b = good; b.ttbr = POOL + 0x100; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    b = good; b.ttbr = 1ull << 36; refused_unread(&b, ARENA, EL2Q_S2W_FORMAT, fmt);
    /* The same 64 entries at a 512-byte boundary are accepted. */
    b = good; b.ttbr = POOL + 0x200;
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_FAULTS] == 256);

    f.serror_before = 1;
    refused_unread(&good, ARENA, EL2Q_S2W_PENDING, "SMMU WALK NOT RUN: SERROR ALREADY PENDING");
    f.serror_before = 0;
    CHECK(el2q_s2w_walk(&good, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    r.test = 1;
    r.state = EL2Q_S2W_NO_STAGE2;
    line(&r, 0, "SMMU WALK NOT RUN: NO STAGE 2 (TEST TABLES)");
    r.state = EL2Q_S2W_NOT_ASKED;
    line(&r, 0, "");
}

static void test_table_mapping(void)
{
    struct el2q_s2w_result r;
    const struct el2q_s2w_bank b = {.sctlr = 0xe1, .tcr = 0x1355c, .cba2r = 1, .ttbr = POOL};
    /* Each refused at the root, unread: absent, Device, Normal
     * non-cacheable, write-through, inner write-back with outer
     * non-cacheable, outer shareable, and not identity. */
    const uint64_t refused[] = {
        POOL | 0x809, POOL | 3ull << 7, POOL | 0x44ull << 56 | 3ull << 7,
        POOL | 0xbbull << 56 | 3ull << 7, POOL | 0x4full << 56 | 3ull << 7,
        POOL | 0xffull << 56 | 2ull << 7, (POOL + 0x1000) | GOOD_ATTR,
    };
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        freset();
        fixture(ARENA, POOL, 0);
        fpar(POOL, refused[i]);
        CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_TABLE_REFUSED);
        CHECK(f.nreads == 0 && r.reads == 0 && r.at == POOL && r.detail == refused[i]);
    }
    line(&r, 0, "SMMU WALK ARENA 9F600000 READS 0");
    line(&r, 1, "SMMU WALK REFUSED: TABLE 00000000B7609000 PAR FF000000B760A180");
    /* Write-back transient and no-allocate variants are cacheable. */
    const uint64_t accepted[] = {POOL | 0x77ull << 56 | 3ull << 7, POOL | 0xccull << 56 | 3ull << 7,
                                 POOL | 0x5dull << 56 | 3ull << 7};
    for (unsigned i = 0; i < 3; i++) {
        freset();
        fixture(ARENA, POOL, 0);
        fpar(POOL, accepted[i]);
        CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_RW] == 256);
    }
    /* A level-3 table page refused after both levels above were read. */
    freset();
    fixture(ARENA, POOL, 0);
    fpar(POOL + 0x3000, POOL + 0x3000 | 0x809);
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_TABLE_REFUSED);
    CHECK(f.nreads == 2 && r.reads == 2 && r.at == POOL + 0x3000);
    check_reads_disciplined();
}

static void test_stops(void)
{
    struct el2q_s2w_result r;
    const struct el2q_s2w_bank b = {.sctlr = 0xe1, .tcr = 0x1355c, .cba2r = 1, .ttbr = POOL};
    const uint64_t l2_desc = POOL + 0x1000 + ((ARENA >> 21) & 511) * 8;
    const uint64_t l3_page5 = POOL + 0x3000 + (((ARENA >> 12) + 5) & 511) * 8;

    /* A synchronous fault on the level-2 read: nothing after it. */
    freset();
    fixture(ARENA, POOL, 0);
    f.fault_at = l2_desc;
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_FAULT);
    CHECK(f.nreads == 2 && r.reads == 2 && r.at == l2_desc && r.detail == ESR_SEA);
    CHECK(f.reads[1] == l2_desc);
    line(&r, 0, "SMMU WALK ARENA 9F600000 READS 2");
    line(&r, 1, "SMMU WALK FAULT AT 00000000B760A7D8 ESR 96000010");
    line(&r, 2, "");

    /* An SError after page 5's descriptor: nothing after it. */
    freset();
    fixture(ARENA, POOL, 0);
    f.serror_at = l3_page5;
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_SERROR);
    CHECK(f.nreads == 8 && r.reads == 8 && r.at == l3_page5 && f.reads[7] == l3_page5);
    line(&r, 1, "SMMU WALK SERROR PENDING AFTER 00000000B760C028");

    /* A table descriptor past the 36-bit output size is not followed. */
    freset();
    fwrite64(0, POOL + 2 * 8, (1ull << 36) | 3);
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_STRUCTURE);
    CHECK(f.nreads == 1 && r.at == POOL + 16 && r.detail == ((1ull << 36) | 3));
    line(&r, 1, "SMMU WALK REFUSED: DESCRIPTOR 0000001000000003");

    r.state = EL2Q_S2W_LIMIT;
    line(&r, 1, "SMMU WALK REFUSED: OVER 264 READS");
}

/* The walk's lines sit after the stage-2 bank's and before the SError line. */
static void test_usb_lines(void)
{
    struct el2q_usb u;
    memset(&u, 0, sizeof u);
    u.after.ran = 1;
    u.walk.state = EL2Q_S2W_NOT_NEEDED;
    char out[EL2Q_USB_LINE_COLS + 1], prev[EL2Q_USB_LINE_COLS + 1] = "";
    unsigned i = 0;
    for (;; i++) {
        el2q_usb_line(&u, i, out);
        if (!out[0]) break;
        if (!strcmp(out, "USB SERROR NONE")) CHECK(!strcmp(prev, "SMMU WALK NOT NEEDED: STAGE 2 OFF"));
        strcpy(prev, out);
    }
    CHECK(!strcmp(prev, "USB SERROR NONE"));
}

/* ---- The survey ------------------------------------------------------------ */

static void sline(const struct el2q_s2w_survey *sv, unsigned i, const char *want)
{
    char out[EL2Q_S2W_LINE_COLS + 1];
    el2q_s2w_survey_line(sv, i, out);
    if (strcmp(out, want)) fprintf(stderr, "survey line %u: got \"%s\", want \"%s\"\n", i, out, want);
    CHECK(!strcmp(out, want));
}

/* "<prefix><32 characters>": fill everywhere, then each (index, char) pair. */
static const char *row(const char *prefix, char fill, unsigned n, const unsigned *at, const char *c)
{
    static char t[EL2Q_S2W_LINE_COLS + 1];
    size_t p = strlen(prefix);
    memcpy(t, prefix, p);
    memset(t + p, fill, 32);
    t[p + 32] = 0;
    for (unsigned i = 0; i < n; i++) t[p + at[i]] = c[i];
    return t;
}

static const struct el2q_s2w_bank bank126 = {.sctlr = 0xe1, .tcr = 0x1355c, .cba2r = 1, .ttbr = POOL};
#define BLOCK (1ull | 0xfull << 2 | 3ull << 6 | 3ull << 8 | 1ull << 10)

static void test_survey_fixture(void)
{
    struct el2q_s2w_survey sv;
    /* Run 22's shape: the arena's 1 GiB (entry 2) a table, its 2 MiB
     * (level-2 entry 251) a table. 64 + 512 reads, each once. */
    freset();
    fixture(ARENA, POOL, 0);
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_DONE);
    CHECK(sv.reads == 576 && f.nreads == 576 && sv.l1_entries == 64 && sv.gib == 2);
    CHECK(sv.gib_read && sv.gib_desc == ((POOL + 0x1000) | 3) && sv.l2_table == POOL + 0x1000);
    CHECK(sv.l2_state == EL2Q_S2W_L2_READ);
    check_reads_disciplined();
    sline(&sv, 0, "SMMU SURVEY READS 576");
    sline(&sv, 1, row("SMMU SURVEY L1 000 ", '.', 1, (unsigned[]){2}, "T"));
    sline(&sv, 2, row("SMMU SURVEY L1 020 ", '.', 0, 0, ""));
    sline(&sv, 3, "SMMU SURVEY GIB 02 00000000B760A003");
    sline(&sv, 4, "SMMU SURVEY L2 AT 00000000B760A000 EMPTY ROWS 15");
    sline(&sv, 5, row("SMMU SURVEY L2 9C000000 ", '.', 1, (unsigned[]){27}, "T"));
    sline(&sv, 6, "");
    sv.test = 1;
    sline(&sv, 0, "SMMU SURVEY READS 576 (TEST TABLES)");

    /* The odd fixture's blocks: a 1 GiB identity block at entry 0, and a
     * moved, a read-only and an identity 2 MiB block at entries 0 to 2. */
    freset();
    fixture(ARENA, POOL, 1);
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_DONE);
    sline(&sv, 1, row("SMMU SURVEY L1 000 ", '.', 2, (unsigned[]){0, 2}, "IT"));
    sline(&sv, 4, "SMMU SURVEY L2 AT 00000000B760A000 EMPTY ROWS 14");
    sline(&sv, 5, row("SMMU SURVEY L2 80000000 ", '.', 3, (unsigned[]){0, 1, 2}, "MLI"));
    sline(&sv, 6, row("SMMU SURVEY L2 9C000000 ", '.', 1, (unsigned[]){27}, "T"));
    sline(&sv, 7, "");
    /* The walk over the same tables is unchanged by those blocks. */
    struct el2q_s2w_result r;
    CHECK(el2q_s2w_walk(&bank126, ARENA, &hw, &r) == EL2Q_S2W_DONE && r.pages[EL2Q_S2W_RW] == 253);

    /* An arena across a 1 GiB boundary: both level-1 entries are tables;
     * the level-2 table surveyed is the first one's. */
    freset();
    fixture(0xbff80000, POOL, 0);
    CHECK(el2q_s2w_survey(&bank126, 0xbff80000, &hw, &sv) == EL2Q_S2W_DONE && sv.reads == 576);
    sline(&sv, 1, row("SMMU SURVEY L1 000 ", '.', 2, (unsigned[]){2, 3}, "TT"));
    sline(&sv, 5, row("SMMU SURVEY L2 BC000000 ", '.', 1, (unsigned[]){31}, "T"));
}

static void test_survey_classes(void)
{
    struct el2q_s2w_survey sv;
    freset();
    fwrite64(0, POOL + 0 * 8, (0ull << 30) | BLOCK);                 /* identity */
    fwrite64(0, POOL + 1 * 8, (2ull << 30) | BLOCK);                 /* elsewhere */
    fwrite64(0, POOL + 2 * 8, (POOL + 0x1000) | 3);                  /* the arena's table */
    fwrite64(0, POOL + 3 * 8, ((3ull << 30) | BLOCK) & ~(1ull << 10));  /* access flag clear */
    fwrite64(0, POOL + 4 * 8, (1ull << 36) | 3);                     /* table past 36 bits */
    fwrite64(0, POOL + 5 * 8, ((5ull << 30) | BLOCK) & ~(1ull << 7));   /* read only */
    fwrite64(0, POOL + 6 * 8, (1ull << 36) | BLOCK);                 /* block past 36 bits */
    fwrite64(0, POOL + 0x1000 + 0 * 8, (2ull << 30) | BLOCK);
    fwrite64(0, POOL + 0x1000 + 2 * 8, (1ull << 36) | 3);
    fwrite64(0, POOL + 0x1000 + 511 * 8, (POOL + 0x2000) | 3);
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_DONE && sv.reads == 576);
    check_reads_disciplined();
    sline(&sv, 1, row("SMMU SURVEY L1 000 ", '.', 7, (unsigned[]){0, 1, 2, 3, 4, 5, 6}, "IMTFXLF"));
    sline(&sv, 4, "SMMU SURVEY L2 AT 00000000B760A000 EMPTY ROWS 14");
    sline(&sv, 5, row("SMMU SURVEY L2 80000000 ", '.', 2, (unsigned[]){0, 2}, "IX"));
    sline(&sv, 6, row("SMMU SURVEY L2 BC000000 ", '.', 1, (unsigned[]){31}, "T"));
    /* No level-3 table is read: every read is in the root or level 2. */
    for (unsigned i = 0; i < f.nreads; i++) CHECK((f.reads[i] & ~0xfffull) != POOL + 0x2000);

    /* The arena's level-1 entry a block: no level-2 survey. */
    freset();
    fwrite64(0, POOL + 2 * 8, (2ull << 30) | BLOCK);
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_DONE);
    CHECK(sv.reads == 64 && f.nreads == 64 && sv.l2_state == EL2Q_S2W_L2_NOT_TABLE);
    sline(&sv, 1, row("SMMU SURVEY L1 000 ", '.', 1, (unsigned[]){2}, "I"));
    sline(&sv, 3, "SMMU SURVEY GIB 02 00000000800007FD");
    sline(&sv, 4, "SMMU SURVEY L2 NONE: NOT A TABLE");
    sline(&sv, 5, "");
    /* Invalid there too. */
    freset();
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_DONE && sv.reads == 64);
    sline(&sv, 4, "SMMU SURVEY L2 NONE: NOT A TABLE");

    /* An arena past the input size (31 bits, two entries). */
    freset();
    struct el2q_s2w_bank b = {.sctlr = 0xe1, .tcr = 33 | 1u << 6 | 0x3500 | 1u << 16, .cba2r = 1, .ttbr = POOL};
    CHECK(el2q_s2w_survey(&b, ARENA, &hw, &sv) == EL2Q_S2W_DONE);
    CHECK(sv.reads == 2 && sv.l1_entries == 2 && sv.l2_state == EL2Q_S2W_L2_NO_ENTRY && !sv.gib_read);
    sline(&sv, 1, "SMMU SURVEY L1 000 ..");
    sline(&sv, 2, "SMMU SURVEY L2 NONE: OUTSIDE THE INPUT SIZE");
    sline(&sv, 3, "");

    /* A 512-entry root (39 bits from level 1): 16 rows, and 1024 reads,
     * exactly the bound. */
    freset();
    b = (struct el2q_s2w_bank){.sctlr = 0xe1, .tcr = 25 | 1u << 6 | 0x3500 | 2u << 16, .cba2r = 1, .ttbr = POOL};
    fwrite64(0, POOL + 2 * 8, (POOL + 0x1000) | 3);
    CHECK(el2q_s2w_survey(&b, ARENA, &hw, &sv) == EL2Q_S2W_DONE && sv.reads == 1024);
    sline(&sv, 16, row("SMMU SURVEY L1 1E0 ", '.', 0, 0, ""));
    sline(&sv, 17, "SMMU SURVEY GIB 02 00000000B760A003");
    sline(&sv, 18, "SMMU SURVEY L2 AT 00000000B760A000 EMPTY ROWS 16");
    sline(&sv, 19, "");
}

static void test_survey_refusals(void)
{
    struct el2q_s2w_survey sv;
    freset();
    fixture(ARENA, POOL, 0);
    /* A walk that starts at level 2 has no level-1 entries to survey. */
    struct el2q_s2w_bank b = {.sctlr = 0xe1, .tcr = 34 | 0x3500 | 1u << 16, .cba2r = 1, .ttbr = POOL};
    struct el2q_s2w_result r;
    CHECK(el2q_s2w_walk(&b, ARENA, &hw, &r) == EL2Q_S2W_DONE);
    f.nreads = f.nasked = 0;
    CHECK(el2q_s2w_survey(&b, ARENA, &hw, &sv) == EL2Q_S2W_FORMAT && f.nreads == 0 && f.nasked == 0);
    sline(&sv, 0, "SMMU SURVEY NOT RUN: FORMAT");
    sline(&sv, 1, "");
    CHECK(el2q_s2w_survey(0, ARENA, &hw, &sv) == EL2Q_S2W_NO_STAGE2 && f.nreads == 0);
    sv.test = 1;
    sline(&sv, 0, "SMMU SURVEY NOT RUN: NO STAGE 2 (TEST TABLES)");
    b = bank126;
    b.sctlr = 0;
    CHECK(el2q_s2w_survey(&b, ARENA, &hw, &sv) == EL2Q_S2W_NOT_NEEDED && f.nreads == 0);
    f.serror_before = 1;
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_PENDING && f.nreads == 0);
    sv.state = EL2Q_S2W_NOT_ASKED;
    sline(&sv, 0, "");
}

static void test_survey_stops(void)
{
    struct el2q_s2w_survey sv;
    char dashes[] = "SMMU SURVEY L1 020 ................................";
    memset(dashes + 19, '-', 32);

    /* A fault on level-1 entry 5: entries 0 to 4 kept, nothing after. */
    freset();
    fixture(ARENA, POOL, 0);
    f.fault_at = POOL + 5 * 8;
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_FAULT);
    CHECK(sv.reads == 6 && f.nreads == 6 && sv.l2_state == EL2Q_S2W_L2_NOT_REACHED);
    sline(&sv, 0, "SMMU SURVEY READS 6");
    sline(&sv, 1, "SMMU SURVEY FAULT AT 00000000B7609028 ESR 96000010");
    sline(&sv, 2, "SMMU SURVEY L1 000 ..T..---------------------------");
    sline(&sv, 3, dashes);
    sline(&sv, 4, "SMMU SURVEY GIB 02 00000000B760A003");
    sline(&sv, 5, "");

    /* An SError after level-2 entry 10: entry 10 itself is not trusted,
     * so it stays unread, and the unread rows are left out. */
    freset();
    fixture(ARENA, POOL, 0);
    f.serror_at = POOL + 0x1000 + 10 * 8;
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_SERROR);
    CHECK(sv.reads == 75 && f.nreads == 75 && f.reads[74] == POOL + 0x1000 + 80);
    sline(&sv, 0, "SMMU SURVEY READS 75");
    sline(&sv, 1, "SMMU SURVEY SERROR PENDING AFTER 00000000B760A050");
    sline(&sv, 5, "SMMU SURVEY L2 AT 00000000B760A000 EMPTY ROWS 0");
    sline(&sv, 6, "SMMU SURVEY L2 80000000 ..........----------------------");
    sline(&sv, 7, "");

    /* The level-2 table's page refused: nothing of it read. */
    freset();
    fixture(ARENA, POOL, 0);
    fpar(POOL + 0x1000, (POOL + 0x1000) | 0x44ull << 56 | 3ull << 7);
    CHECK(el2q_s2w_survey(&bank126, ARENA, &hw, &sv) == EL2Q_S2W_TABLE_REFUSED);
    CHECK(sv.reads == 64 && f.nreads == 64 && sv.at == POOL + 0x1000);
    sline(&sv, 1, "SMMU SURVEY REFUSED: TABLE 00000000B760A000 PAR 44000000B760A180");
    sline(&sv, 5, "SMMU SURVEY L2 AT 00000000B760A000 EMPTY ROWS 0");
    sline(&sv, 6, "");
    check_reads_disciplined();
}

/* The survey's lines follow the walk's, before the SError line. */
static void test_survey_usb_lines(void)
{
    struct el2q_usb u;
    memset(&u, 0, sizeof u);
    u.after.ran = 1;
    u.walk.state = EL2Q_S2W_NOT_NEEDED;
    u.survey.state = EL2Q_S2W_NOT_NEEDED;
    char out[EL2Q_USB_LINE_COLS + 1], prev[2][EL2Q_USB_LINE_COLS + 1] = {"", ""};
    for (unsigned i = 0;; i++) {
        el2q_usb_line(&u, i, out);
        if (!out[0]) break;
        strcpy(prev[0], prev[1]);
        strcpy(prev[1], out);
        if (!strcmp(out, "USB SERROR NONE")) CHECK(!strcmp(prev[0], "SMMU SURVEY NOT NEEDED: STAGE 2 OFF"));
    }
    CHECK(!strcmp(prev[1], "USB SERROR NONE"));
}

/* ---- The candidate region: memory map and fixed walk ----------------------- */

#define DESC 48u   /* the gaokun3's descriptor size, larger than the structure */
static uint8_t mmap_buf[32 * DESC];
static unsigned mmap_n;

static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void mdesc(uint32_t type, uint64_t start, uint64_t pages, uint64_t attr)
{
    uint8_t *d = mmap_buf + mmap_n++ * DESC;
    memset(d, 0xa5, DESC);                       /* padding and VirtualStart: not read */
    for (unsigned i = 0; i < 4; i++) d[i] = (uint8_t)(type >> (8 * i));
    put64(d + 8, start);
    put64(d + 24, pages);
    put64(d + 32, attr);
}

static void rline(const struct el2q_s2w_region *r, unsigned i, const char *want)
{
    char out[EL2Q_S2W_LINE_COLS + 1];
    el2q_s2w_region_line(r, i, out);
    if (strcmp(out, want)) fprintf(stderr, "region line %u: got \"%s\", want \"%s\"\n", i, out, want);
    CHECK(!strcmp(out, want));
}

static void test_region(void)
{
    struct el2q_s2w_region r;
    const uint64_t base = EL2Q_S2W_REGION_BASE, end = EL2Q_S2W_REGION_END;
    CHECK(base == 0xc6200000 && end == 0xc8600000 && EL2Q_S2W_FIXED_ARENA == 0xc8000000);

    /* The likely shape, out of order: free memory up to the framebuffer,
     * the framebuffer reserved, free memory after, and a descriptor that
     * starts where the region ends. Only the two inside it count. */
    mmap_n = 0;
    mdesc(7, 0xc71a0000, 0x1460, 0xf);
    mdesc(7, 0xc0000000, 0x6200, 0xf);
    mdesc(4, 0xc8600000, 0x100, 0xf);
    mdesc(0, 0xc6200000, 0xfa0, 0x1);
    el2q_memmap_region(mmap_buf, mmap_n * DESC, DESC, base, end, &r);
    CHECK(r.state == EL2Q_S2W_REGION_READ && r.count == 2);
    rline(&r, 0, "UEFI MAP C6200000-C8600000 ENTRIES 2");
    rline(&r, 1, "UEFI MAP 00C6200000 00C71A0000 RESERVED 0000000000000001");
    rline(&r, 2, "UEFI MAP 00C71A0000 00C8600000 FREE 000000000000000F");
    rline(&r, 3, "");
    r.test = 1;
    rline(&r, 0, "UEFI MAP C6200000-C8600000 ENTRIES 2 (TEST RANGE)");

    /* Overlap at either end counts; no pages, a page count past 64 bits
     * and an end that wraps do not, wherever they start. */
    mmap_n = 0;
    mdesc(2, 0xc6000000, 0x400, 0x8000000000000000ull);
    mdesc(4, 0xc8500000, 0x200, 0xf);
    mdesc(7, 0xc7000000, 0, 0xf);
    mdesc(7, 0xc7000000, 1ull << 60, 0xf);
    mdesc(7, 0xfffffffffffff000ull, 2, 0xf);
    mdesc(7, 0xc7000000, (1ull << 52) - 1, 0xf);    /* inside, and wraps past 2^64 */
    el2q_memmap_region(mmap_buf, mmap_n * DESC, DESC, base, end, &r);
    CHECK(r.count == 2);
    rline(&r, 1, "UEFI MAP 00C6000000 00C6400000 LDR DATA 8000000000000000");
    rline(&r, 2, "UEFI MAP 00C8500000 00C8700000 BS DATA 000000000000000F");

    /* Twelve one-page descriptors, ascending and descending: the eight
     * lowest are kept in order either way, and the rest counted. */
    for (int order = 0; order < 2; order++) {
        mmap_n = 0;
        for (unsigned i = 0; i < 12; i++) {
            unsigned k = order ? 11 - i : i;
            mdesc(k < 15 ? k : 7, base + k * 0x100000ull, 1, k);
        }
        el2q_memmap_region(mmap_buf, mmap_n * DESC, DESC, base, end, &r);
        CHECK(r.count == 12);
        for (unsigned i = 0; i < 8; i++) CHECK(r.e[i].start == base + i * 0x100000ull && r.e[i].type == i);
        rline(&r, 0, "UEFI MAP C6200000-C8600000 ENTRIES 12");
        rline(&r, 8, "UEFI MAP 00C6900000 00C6901000 FREE 0000000000000007");
        rline(&r, 9, "UEFI MAP AND 4 MORE");
        rline(&r, 10, "");
    }

    /* An OEM type has no name. */
    mmap_n = 0;
    mdesc(0x70000000, base, 1, 0);
    el2q_memmap_region(mmap_buf, mmap_n * DESC, DESC, base, end, &r);
    rline(&r, 1, "UEFI MAP 00C6200000 00C6201000 T70000000 0000000000000000");

    /* No map, or descriptors too small to hold the structure. */
    el2q_memmap_region(0, mmap_n * DESC, DESC, base, end, &r);
    CHECK(r.state == EL2Q_S2W_REGION_NO_MAP);
    rline(&r, 0, "UEFI MAP NOT AVAILABLE");
    rline(&r, 1, "");
    el2q_memmap_region(mmap_buf, mmap_n * DESC, 32, base, end, &r);
    CHECK(r.state == EL2Q_S2W_REGION_NO_MAP);
    /* A map shorter than one descriptor has nothing in the region. */
    el2q_memmap_region(mmap_buf, DESC - 1, DESC, base, end, &r);
    CHECK(r.state == EL2Q_S2W_REGION_READ && r.count == 0);
    rline(&r, 0, "UEFI MAP C6200000-C8600000 ENTRIES 0");
    rline(&r, 1, "");
    r.state = EL2Q_S2W_REGION_NOT_ASKED;
    rline(&r, 0, "");
}

static void test_fixed_walk(void)
{
    struct el2q_s2w_result r;
    /* A 2 MiB identity block at 0xC8000000, as run 23's survey shows. */
    freset();
    fwrite64(0, POOL + 3 * 8, (POOL + 0x1000) | 3);
    fwrite64(0, POOL + 0x1000 + 64 * 8, 0xc8000000ull | BLOCK);
    CHECK(el2q_s2w_walk(&bank126, EL2Q_S2W_FIXED_ARENA, &hw, &r) == EL2Q_S2W_DONE);
    CHECK(r.reads == 2 && r.pages[EL2Q_S2W_RW] == 256 && r.leaf_level == 2);
    r.fixed = 1;
    line(&r, 0, "SMMU WALK ARENA C8000000 READS 2 (FIXED)");
    line(&r, 2, "SMMU WALK LEAF L2 00000000C80007FD");
    r.test = 1;
    line(&r, 0, "SMMU WALK ARENA C8000000 READS 2 (TEST TABLES) (FIXED)");
    CHECK(el2q_s2w_walk(0, EL2Q_S2W_FIXED_ARENA, &hw, &r) == EL2Q_S2W_NO_STAGE2);
    r.test = r.fixed = 1;
    line(&r, 0, "SMMU WALK NOT RUN: NO STAGE 2 (TEST TABLES) (FIXED)");
    /* The longest not-run line still fits. */
    r.state = EL2Q_S2W_PENDING;
    line(&r, 0, "SMMU WALK NOT RUN: SERROR ALREADY PENDING (TEST TABLES) (FIXED)");
}

/* The memory map's lines follow the survey's, before the SError line. */
static void test_region_usb_lines(void)
{
    struct el2q_usb u;
    memset(&u, 0, sizeof u);
    u.after.ran = 1;
    u.walk.state = EL2Q_S2W_NOT_NEEDED;
    u.region.state = EL2Q_S2W_REGION_NO_MAP;
    char out[EL2Q_USB_LINE_COLS + 1], prev[2][EL2Q_USB_LINE_COLS + 1] = {"", ""};
    for (unsigned i = 0;; i++) {
        el2q_usb_line(&u, i, out);
        if (!out[0]) break;
        strcpy(prev[0], prev[1]);
        strcpy(prev[1], out);
        if (!strcmp(out, "USB SERROR NONE")) CHECK(!strcmp(prev[0], "UEFI MAP NOT AVAILABLE"));
    }
    CHECK(!strcmp(prev[1], "USB SERROR NONE"));
}

unsigned test_s2walk(void)
{
    test_region();
    test_fixed_walk();
    test_region_usb_lines();
    test_fixture_identity();
    test_classes();
    test_refusals();
    test_table_mapping();
    test_stops();
    test_usb_lines();
    test_survey_fixture();
    test_survey_classes();
    test_survey_refusals();
    test_survey_stops();
    test_survey_usb_lines();
    return checks;
}
