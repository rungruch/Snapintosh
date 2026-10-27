/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/s2bypass.c, the planned-but-never-made S2CR
 * bypass for USB0's stream. Linked into tools/test-el2qual.c's binary by
 * tools/test-el2qual.py; test_s2bypass() returns its check count.
 *
 * The module has no write callback, so the property that matters most is
 * structural rather than testable here: there is no path through it that
 * reaches the SMMU. What is tested is everything the plan must be sure of
 * before anyone writes that word by hand: it refuses unless the machine is
 * still in the state runs 17 to 24 measured, the group selects USB0's
 * stream and nothing else, no second group matches it, the register's page
 * is Device identity and writable at EL2, no SError is pending and no
 * global fault is latched; it reads nothing once it has refused on what it
 * was already told; and the word it plans changes one field. */
#include "usbprobe.h"
#include "s2bypass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

unsigned test_s2bypass(void);

/* Run 17 to 24's apps_smmu, as measured. */
#define BASE   0x15000000ull
#define STREAM 0x820u
#define SCR0   0x00240406u
#define IDR0   0x7C017ED4u      /* NUMSMRG 0xD4 = 212 */
#define SMR6   0x80000820u
#define S2CR6  0x00000006u
#define GROUP  6u
#define CBNDX  6u
#define S2CR_AT 0x15000C18ull
#define BYPASS 0x00010006u
#define ESR_SEA 0x96000010ull

#define MAXLOG 1024u
static struct {
    uint64_t reads[MAXLOG];
    unsigned nreads;
    uint32_t smr[256], s2cr[256];
    uint32_t scr0_now, gfsr;
    uint64_t fault_at, serror_at;
    int serror, serror_now, unmapped, readonly;
} f;

static void freset(void)
{
    memset(&f, 0, sizeof f);
    f.scr0_now = SCR0;
    f.s2cr[GROUP] = S2CR6;
    f.smr[GROUP] = SMR6;
}

static int fmapped(void *ctx, uint64_t a) { (void)ctx; (void)a; return f.unmapped; }
static int fwritable(void *ctx, uint64_t a) { (void)ctx; (void)a; return f.readonly; }
static int fserror(void *ctx) { (void)ctx; return f.serror || f.serror_now; }

static uint64_t fread32(void *ctx, uint64_t a, uint32_t *v)
{
    (void)ctx;
    if (f.nreads < MAXLOG) f.reads[f.nreads++] = a;
    if (a == f.fault_at) return ESR_SEA;
    if (a == f.serror_at) f.serror_now = 1;
    uint64_t off = a - BASE;
    if (off == 0x48) *v = f.gfsr;
    else if (off == 0) *v = f.scr0_now;
    else if (off >= 0xc00 && off < 0x1000) *v = f.s2cr[(off - 0xc00) / 4];
    else if (off >= 0x800 && off < 0xc00) *v = f.smr[(off - 0x800) / 4];
    else *v = 0;
    return 0;
}

static const struct el2q_s2b_hw hw = {0, fmapped, fwritable, fread32, fserror};

static struct el2q_s2b_seen base_seen(void)
{
    return (struct el2q_s2b_seen){
        .matched = 1, .bank_read = 1, .base = BASE, .stream = STREAM,
        .scr0 = SCR0, .idr0 = IDR0, .smr = SMR6, .s2cr = S2CR6,
        .group = GROUP, .cb_index = CBNDX,
    };
}

static void line(const struct el2q_s2b_result *r, unsigned i, const char *want)
{
    char out[EL2Q_S2B_LINE_COLS + 1];
    el2q_s2b_line(r, i, out);
    if (strcmp(out, want)) fprintf(stderr, "line %u: got \"%s\", want \"%s\"\n", i, out, want);
    CHECK(!strcmp(out, want));
}

/* Every read was one of the words the plan is allowed to read, in the
 * SMMU's first 4 KiB, and none twice. */
static void check_reads_allowed(void)
{
    for (unsigned i = 0; i < f.nreads; i++) {
        uint64_t off = f.reads[i] - BASE;
        CHECK(f.reads[i] >= BASE && off < 0x1000);
        CHECK(off == 0x48 || off == 0 || (off >= 0xc00 && off < 0x1000) ||
              (off >= 0x800 && off < 0x800 + 4 * 212));
        for (unsigned j = 0; j < i; j++) CHECK(f.reads[j] != f.reads[i]);
    }
}

static void test_planned(void)
{
    struct el2q_s2b_result r;
    freset();
    struct el2q_s2b_seen seen = base_seen();
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_PLANNED);
    /* One field of one word, and the way back. */
    CHECK(r.at == S2CR_AT && r.old == S2CR6 && r.planned == BYPASS && r.restore == S2CR6);
    CHECK((r.planned & ~(3u << 16)) == (r.old & ~(3u << 16)));   /* only TYPE changes */
    CHECK((r.planned >> 16 & 3) == 1 && (r.old >> 16 & 3) == 0);
    CHECK(r.matches == 1 && r.scanned == 212 && r.mask == 0 && r.gfsr == 0 && r.group == GROUP);
    CHECK(f.nreads == 215);                                      /* sGFSR, sCR0, S2CR, 212 SMRs */
    check_reads_allowed();
    line(&r, 0, "SMMU BYPASS S2CR06 AT 15000C18 00000006 -> 00010006");
    line(&r, 1, "SMMU BYPASS MATCHES 1/212 MASK 0000 GFSR 00000000");
    line(&r, 2, "SMMU BYPASS PLAN ONLY, NOTHING WRITTEN");
    line(&r, 3, "");

    /* A group whose S2CR carries bits this plan does not interpret keeps
     * them: only TYPE moves. */
    freset();
    seen.s2cr = f.s2cr[GROUP] = 0x03000006u;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_PLANNED);
    CHECK(r.planned == 0x03010006u && r.restore == 0x03000006u);
    line(&r, 0, "SMMU BYPASS S2CR06 AT 15000C18 03000006 -> 03010006");

    /* Another group index moves the address and the name. */
    freset();
    seen = base_seen();
    seen.group = 0xcd;
    f.smr[GROUP] = 0;
    f.s2cr[GROUP] = 0;
    f.smr[0xcd] = SMR6;
    f.s2cr[0xcd] = S2CR6;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_PLANNED);
    CHECK(r.at == BASE + 0xc00 + 4 * 0xcd);
    line(&r, 0, "SMMU BYPASS S2CRCD AT 15000F34 00000006 -> 00010006");
}

/* Refused on what it was already told: nothing read, nothing asked of AT. */
static void refused_unread(struct el2q_s2b_seen *seen, uint32_t state, const char *want)
{
    struct el2q_s2b_result r;
    f.nreads = 0;
    CHECK(el2q_s2b_plan(seen, &hw, &r) == state);
    CHECK(f.nreads == 0 && r.scanned == 0 && r.matches == 0);
    /* No hole before it: the report stops at the first empty line. */
    line(&r, 0, want);
    line(&r, 1, "SMMU BYPASS PLAN ONLY, NOTHING WRITTEN");
    line(&r, 2, "");
}

static void test_refusals(void)
{
    struct el2q_s2b_result r;
    struct el2q_s2b_seen seen;
    freset();

    /* Not measured: no match, or the bank was never read. */
    seen = base_seen(); seen.matched = 0;
    refused_unread(&seen, EL2Q_S2B_NOT_MEASURED, "SMMU BYPASS NOT MEASURED");
    seen = base_seen(); seen.bank_read = 0;
    refused_unread(&seen, EL2Q_S2B_NOT_MEASURED, "SMMU BYPASS NOT MEASURED");

    /* The SMMU off, all ones, or matching streams another way. */
    seen = base_seen(); seen.scr0 = SCR0 | 1u;
    refused_unread(&seen, EL2Q_S2B_REFUSED_SCR0, "SMMU BYPASS REFUSED: SCR0 00240407");
    seen = base_seen(); seen.scr0 = SCR0 | 8u;
    refused_unread(&seen, EL2Q_S2B_REFUSED_SCR0, "SMMU BYPASS REFUSED: SCR0 0024040E");
    seen = base_seen(); seen.scr0 = 0xffffffffu;
    refused_unread(&seen, EL2Q_S2B_REFUSED_SCR0, "SMMU BYPASS REFUSED: SCR0 FFFFFFFF");

    /* Already bypassing, or not the translating S2CR that named the bank. */
    seen = base_seen(); seen.s2cr = 0x00010006u;
    refused_unread(&seen, EL2Q_S2B_NOT_NEEDED, "SMMU BYPASS NOT NEEDED: ALREADY BYPASSING");
    seen = base_seen(); seen.s2cr = 0x00020006u;
    refused_unread(&seen, EL2Q_S2B_REFUSED_TYPE, "SMMU BYPASS REFUSED: S2CR TYPE");
    seen = base_seen(); seen.s2cr = 0x00030006u;
    refused_unread(&seen, EL2Q_S2B_REFUSED_TYPE, "SMMU BYPASS REFUSED: S2CR TYPE");
    seen = base_seen(); seen.cb_index = 7;
    refused_unread(&seen, EL2Q_S2B_REFUSED_TYPE, "SMMU BYPASS REFUSED: S2CR TYPE");

    /* The group must select USB0's stream alone. */
    seen = base_seen(); seen.smr = 0x800F0820u;
    refused_unread(&seen, EL2Q_S2B_REFUSED_MASK, "SMMU BYPASS REFUSED: STREAM MASK 000F");
    seen = base_seen(); seen.smr = 0x7FFF0820u;      /* not valid */
    refused_unread(&seen, EL2Q_S2B_REFUSED_MASK, "SMMU BYPASS REFUSED: STREAM MASK 7FFF");
    seen = base_seen(); seen.smr = 0x80000821u;      /* another stream */
    refused_unread(&seen, EL2Q_S2B_REFUSED_MASK, "SMMU BYPASS REFUSED: STREAM MASK 0000");

    /* Past NUMSMRG, or past the groups whose S2CR shares the first page. */
    seen = base_seen(); seen.group = 212;
    refused_unread(&seen, EL2Q_S2B_REFUSED_RANGE, "SMMU BYPASS REFUSED: OUT OF RANGE");
    seen = base_seen(); seen.idr0 = (IDR0 & ~0xffu) | 0xffu; seen.group = 255;
    refused_unread(&seen, EL2Q_S2B_REFUSED_RANGE, "SMMU BYPASS REFUSED: OUT OF RANGE");

    /* The register's own page, and the machine's exception state. */
    seen = base_seen();
    f.serror = 1;
    refused_unread(&seen, EL2Q_S2B_REFUSED_PENDING, "SMMU BYPASS REFUSED: SERROR ALREADY PENDING");
    f.serror = 0;
    f.unmapped = 1;
    refused_unread(&seen, EL2Q_S2B_REFUSED_UNMAPPED, "SMMU BYPASS REFUSED: UNMAPPED 15000000");
    f.unmapped = 0;
    f.readonly = 1;
    refused_unread(&seen, EL2Q_S2B_REFUSED_READONLY, "SMMU BYPASS REFUSED: READ ONLY 15000000");
    f.readonly = 0;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_PLANNED);

    /* A missing callback is refused like a machine never measured. */
    const struct el2q_s2b_hw none = {0, 0, fwritable, fread32, fserror};
    CHECK(el2q_s2b_plan(&seen, &none, &r) == EL2Q_S2B_NOT_MEASURED);
    CHECK(el2q_s2b_plan(0, &hw, &r) == EL2Q_S2B_NOT_MEASURED);
}

static void test_reads(void)
{
    struct el2q_s2b_result r;
    struct el2q_s2b_seen seen = base_seen();

    /* A global fault already latched. */
    freset();
    f.gfsr = 0x80000002u;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_FAULT);
    CHECK(f.nreads == 1 && r.scanned == 0);
    line(&r, 0, "SMMU BYPASS REFUSED: GFSR 80000002");
    line(&r, 1, "SMMU BYPASS PLAN ONLY, NOTHING WRITTEN");
    line(&r, 2, "");

    /* sCR0 or the S2CR moved since the SMMU read: nothing scanned. */
    freset();
    f.scr0_now = SCR0 ^ 1u;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_MOVED);
    CHECK(f.nreads == 2 && r.scanned == 0);
    line(&r, 0, "SMMU BYPASS REFUSED: MOVED SINCE READ");
    freset();
    f.s2cr[GROUP] = 0x00010006u;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_MOVED);
    CHECK(f.nreads == 3 && r.scanned == 0);

    /* The gap a count of one alone would leave: SMR(6) moved to another
     * stream while another group took 0x820. Exactly one group still
     * matches, but it is not the one whose S2CR the plan would name. */
    freset();
    f.smr[GROUP] = 0x80000821u;
    f.smr[9] = SMR6;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_MOVED);
    CHECK(r.scanned == GROUP + 1);
    /* Its mask widened, still matching 0x820: also moved. */
    freset();
    f.smr[GROUP] = 0x80010820u;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_MOVED);

    /* A second valid group matching USB0's stream, exactly or by mask. */
    freset();
    f.smr[200] = SMR6;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_SHARED);
    CHECK(r.matches == 2 && r.scanned == 212);
    line(&r, 0, "SMMU BYPASS REFUSED: 2 GROUPS MATCH");
    line(&r, 1, "SMMU BYPASS MATCHES 2/212 MASK 0000 GFSR 00000000");
    freset();
    f.smr[3] = 0x800F0820u;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_SHARED && r.matches == 2);
    /* An invalid second group, or one for another stream, is not a match. */
    freset();
    f.smr[3] = 0x00000820u;
    f.smr[4] = 0x80000821u;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_PLANNED && r.matches == 1);
    /* A group past NUMSMRG is not scanned, so it cannot be a second match. */
    freset();
    f.smr[250] = SMR6;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_PLANNED && r.scanned == 212);

    /* The matching group itself disappearing from the scan. */
    freset();
    f.smr[GROUP] = 0;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_MOVED);

    /* An SError raised by the plan's own reads: no PLANNED, and the scan's
     * line stays so the reads that ran are visible. */
    freset();
    f.serror_at = BASE + 0x800 + 4 * 100;
    CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_SERROR);
    CHECK(r.scanned == 212 && r.matches == 1);
    line(&r, 0, "SMMU BYPASS REFUSED: SERROR DURING THE CHECK");
    line(&r, 1, "SMMU BYPASS MATCHES 1/212 MASK 0000 GFSR 00000000");
    line(&r, 2, "SMMU BYPASS PLAN ONLY, NOTHING WRITTEN");

    /* A fault on each of the reads in turn: the ESR is reported and the
     * scan stops there. */
    const uint64_t at[] = {BASE + 0x48, BASE, S2CR_AT, BASE + 0x800 + 4 * 9};
    for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
        freset();
        f.fault_at = at[i];
        CHECK(el2q_s2b_plan(&seen, &hw, &r) == EL2Q_S2B_REFUSED_READ);
        CHECK(r.detail == ESR_SEA && f.reads[f.nreads - 1] == at[i]);
        check_reads_allowed();
    }
    line(&r, 0, "SMMU BYPASS REFUSED: READ ESR 96000010");
}

/* The bypass lines come last of the USB block, before the SError line. */
static void test_usb_lines(void)
{
    struct el2q_usb u;
    memset(&u, 0, sizeof u);
    u.after.ran = 1;
    u.bypass.state = EL2Q_S2B_NOT_MEASURED;
    char out[EL2Q_USB_LINE_COLS + 1], prev[EL2Q_USB_LINE_COLS + 1] = "";
    unsigned seen_plan = 0;
    for (unsigned i = 0;; i++) {
        el2q_usb_line(&u, i, out);
        if (!out[0]) break;
        if (!strcmp(out, "SMMU BYPASS PLAN ONLY, NOTHING WRITTEN")) seen_plan = 1;
        if (!strcmp(out, "USB SERROR NONE"))
            CHECK(!strcmp(prev, "SMMU BYPASS PLAN ONLY, NOTHING WRITTEN"));
        strcpy(prev, out);
    }
    CHECK(seen_plan && !strcmp(prev, "USB SERROR NONE"));
}

unsigned test_s2bypass(void)
{
    test_planned();
    test_refusals();
    test_reads();
    test_usb_lines();
    return checks;
}
