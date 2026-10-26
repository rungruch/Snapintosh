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

unsigned test_s2walk(void)
{
    test_fixture_identity();
    test_classes();
    test_refusals();
    test_table_mapping();
    test_stops();
    test_usb_lines();
    return checks;
}
