/* SPDX-License-Identifier: MIT */
/* See s2walk.h. */
#include "s2walk.h"

/* Stage-2 descriptor fields (the Arm ARM, VMSAv8-64 stage 2, 4 KiB granule). */
#define S2_VALID   1ull
#define S2_TYPE    3ull               /* bits 1:0: 0b11 table (levels 0-2) or page (level 3) */
#define S2_BLOCK   1ull               /* bits 1:0 at levels 1 and 2 */
#define S2_AP_RW   (3ull << 6)        /* S2AP: bit 6 read, bit 7 write */
#define S2_AF      (1ull << 10)
#define S2_OA      0x0000fffffffff000ull
#define TTBR_BADDR 0x0000fffffffffffeull  /* bit 0 is CnP */

/* PAR_EL1 after a successful AT (F clear). */
#define PAR_F      1ull
#define PAR_SH_IS  (3ull << 7)
#define PAR_PA     0x0000fffffffff000ull

/* VTCR.PS, in bits. Values above 5 are reserved for a 4 KiB granule here. */
static const unsigned output_bits[] = {32, 36, 40, 42, 44, 48};

/* A MAIR-style attribute nibble that is Normal write-back: 0b11RW, or 0b01RW
 * with RW not 00 (0b0100 is non-cacheable). */
static int write_back(unsigned nibble)
{
    return (nibble & 0xc) == 0xc || ((nibble & 0xc) == 0x4 && (nibble & 3));
}

/* The SMMU walks these tables coherently (VTCR: inner shareable, write-back),
 * so only a cacheable, inner-shareable identity read sees what it sees. */
static int table_readable(uint64_t par, uint64_t page)
{
    unsigned attr = (unsigned)(par >> 56);
    return !(par & PAR_F) && !((par ^ page) & PAR_PA) && write_back(attr & 0xf) &&
           write_back(attr >> 4) && (par & PAR_SH_IS) == PAR_SH_IS;
}

static uint64_t level_size(unsigned level)
{
    return 1ull << (12 + 9 * (3 - level));
}

/* Both passes read addresses that only increase, so a descriptor or table
 * page left behind at a level is never needed again: one entry per level is
 * enough to read each descriptor once and check each page once. */
struct walker {
    const struct el2q_s2w_hw *hw;
    uint32_t *reads, limit;
    uint64_t *at, *detail;                   /* where a stop is recorded */
    uint64_t desc_at[4], desc[4], page_ok[4];
    uint32_t desc_valid[4], page_valid[4];
};

/* 0 and *value set, or the pass's new state. */
static uint32_t read_descriptor(struct walker *w, unsigned level, uint64_t address, uint64_t *value)
{
    if (w->desc_valid[level] && w->desc_at[level] == address) {
        *value = w->desc[level];
        return 0;
    }
    uint64_t page = address & ~0xfffull;
    if (!w->page_valid[level] || w->page_ok[level] != page) {
        uint64_t par = w->hw->par(w->hw->ctx, page);
        if (!table_readable(par, page)) {
            *w->at = page;
            *w->detail = par;
            return EL2Q_S2W_TABLE_REFUSED;
        }
        w->page_ok[level] = page;
        w->page_valid[level] = 1;
    }
    if (*w->reads >= w->limit) return EL2Q_S2W_LIMIT;
    ++*w->reads;
    uint64_t esr = w->hw->read64(w->hw->ctx, address, value);
    if (esr) {
        *w->at = address;
        *w->detail = esr;
        return EL2Q_S2W_FAULT;
    }
    if (w->hw->serror(w->hw->ctx)) {
        *w->at = address;
        return EL2Q_S2W_SERROR;
    }
    w->desc_at[level] = address;
    w->desc[level] = *value;
    w->desc_valid[level] = 1;
    return 0;
}

/* The bank's stage-2 format, as both passes follow it. */
struct format {
    unsigned start, input, oas;   /* start level; input and output sizes in bits */
    uint64_t root, entries;       /* the start-level table and its entry count */
};

/* Everything checked before the first read, for both passes: 0 with *f
 * filled, or the state to stop in. */
static uint32_t prepare(const struct el2q_s2w_bank *bank, uint64_t arena,
                        const struct el2q_s2w_hw *hw, struct format *f)
{
    if (!hw || !hw->par || !hw->read64 || !hw->serror) return EL2Q_S2W_FORMAT;
    if (!arena || (arena & 0xfff) || arena + EL2Q_S2W_ARENA_SIZE > (1ull << 32))
        return EL2Q_S2W_NO_ARENA;
    if (!bank) return EL2Q_S2W_NO_STAGE2;
    if (!(bank->sctlr & 1)) return EL2Q_S2W_NOT_NEEDED;

    /* VTCR: T0SZ 5:0, SL0 7:6, TG0 15:14, PS 18:16. */
    unsigned t0sz = bank->tcr & 0x3f, sl0 = (bank->tcr >> 6) & 3, ps = (bank->tcr >> 16) & 7;
    if (!(bank->cba2r & 1) || ((bank->tcr >> 14) & 3) || sl0 == 3 || ps > 5 || t0sz < 16)
        return EL2Q_S2W_FORMAT;
    unsigned start = 2 - sl0, input = 64 - t0sz, oas = output_bits[ps];
    /* The start level resolves bits [input-1 : 12 + 9*(3-start)]: at least
     * one bit and at most nine, since concatenated start tables are not
     * followed. */
    unsigned below = 12 + 9 * (3 - start);
    if (input <= below || input > below + 9 || input > oas) return EL2Q_S2W_FORMAT;
    uint64_t root = bank->ttbr & TTBR_BADDR, entries = 1ull << (input - below);
    if ((root & (entries * 8 - 1)) || (root >> oas)) return EL2Q_S2W_FORMAT;

    if (hw->serror(hw->ctx)) return EL2Q_S2W_PENDING;
    *f = (struct format){start, input, oas, root, entries};
    return 0;
}

/* A valid block or page descriptor that maps ipa, as the SMMU would treat
 * it: enum el2q_s2w_page. */
static unsigned leaf_class(uint64_t desc, unsigned level, uint64_t ipa, unsigned oas)
{
    uint64_t size = level_size(level), oa = desc & S2_OA & ~(size - 1);
    if (!(desc & S2_AF) || (oa >> oas)) return EL2Q_S2W_FAULTS;
    if (oa != (ipa & ~(size - 1))) return EL2Q_S2W_MOVED;
    return (desc & S2_AP_RW) == S2_AP_RW ? EL2Q_S2W_RW : EL2Q_S2W_LIMITED;
}

uint32_t el2q_s2w_walk(const struct el2q_s2w_bank *bank, uint64_t arena,
                       const struct el2q_s2w_hw *hw, struct el2q_s2w_result *out)
{
    *out = (struct el2q_s2w_result){.arena = arena};
    struct format f;
    uint32_t state = prepare(bank, arena, hw, &f);
    if (state) return out->state = state;
    unsigned start = f.start, input = f.input, oas = f.oas;
    uint64_t root = f.root, entries = f.entries;

    struct walker w = {.hw = hw, .reads = &out->reads, .limit = EL2Q_S2W_MAX_READS,
                       .at = &out->at, .detail = &out->detail};
    for (uint64_t i = 0; i < EL2Q_S2W_ARENA_PAGES; i++) {
        uint64_t ipa = arena + (i << 12), table = root, desc = 0;
        unsigned level = start, class = EL2Q_S2W_FAULTS;
        if (!(ipa >> input)) {
            for (;;) {
                uint64_t mask = level == start ? entries - 1 : 511;
                uint64_t at = table + ((ipa / level_size(level)) & mask) * 8;
                uint32_t stop = read_descriptor(&w, level, at, &desc);
                if (stop) return out->state = stop;
                if (!(desc & S2_VALID)) break;
                uint64_t type = desc & S2_TYPE;
                if (level < 3 && type == S2_TYPE) {
                    table = desc & S2_OA;
                    if (table >> oas) {
                        out->at = at;
                        out->detail = desc;
                        return out->state = EL2Q_S2W_STRUCTURE;
                    }
                    level++;
                    continue;
                }
                /* A level-3 descriptor with bits 1:0 0b01, and a block at
                 * level 0, are reserved encodings at 4 KiB: they fault. */
                if ((level == 3 && type != S2_TYPE) || (level == 0 && type == S2_BLOCK)) break;
                class = leaf_class(desc, level, ipa, oas);
                break;
            }
        }
        out->pages[class]++;
        if (i == 0 && out->reads) {
            out->leaf_seen = 1;
            out->leaf_level = level;
            out->leaf = desc;
        }
        if (class != EL2Q_S2W_RW && !out->odd_seen) {
            out->odd_seen = 1;
            out->odd_ipa = ipa;
            out->odd_level = level;
            out->odd = desc;
        }
    }
    return out->state = EL2Q_S2W_DONE;
}

/* ---- The survey ---------------------------------------------------------------- */

/* One level-1 or level-2 entry for the survey's map; see s2walk.h. Blocks
 * are classed as the walk classes a page. */
static char entry_char(uint64_t desc, unsigned level, uint64_t ipa, unsigned oas)
{
    if (!(desc & S2_VALID)) return '.';
    if ((desc & S2_TYPE) == S2_TYPE) return (desc & S2_OA) >> oas ? 'X' : 'T';
    return "ILMF"[leaf_class(desc, level, ipa, oas)];
}
_Static_assert(EL2Q_S2W_RW == 0 && EL2Q_S2W_LIMITED == 1 && EL2Q_S2W_MOVED == 2 &&
               EL2Q_S2W_FAULTS == 3, "entry_char's letters follow enum el2q_s2w_page");

uint32_t el2q_s2w_survey(const struct el2q_s2w_bank *bank, uint64_t arena,
                         const struct el2q_s2w_hw *hw, struct el2q_s2w_survey *out)
{
    *out = (struct el2q_s2w_survey){.gib = (uint32_t)(arena >> 30)};
    for (unsigned i = 0; i < 512; i++) out->l1[i] = out->l2[i] = '-';
    struct format f;
    uint32_t state = prepare(bank, arena, hw, &f);
    if (state) return out->state = state;
    /* The root holds level-1 entries only when the walk starts at level 1. */
    if (f.start != 1) return out->state = EL2Q_S2W_FORMAT;
    out->l1_entries = (uint32_t)f.entries;

    struct walker w = {.hw = hw, .reads = &out->reads, .limit = EL2Q_S2W_SURVEY_MAX_READS,
                       .at = &out->at, .detail = &out->detail};
    uint64_t desc = 0;
    for (uint64_t i = 0; i < f.entries; i++) {
        uint32_t stop = read_descriptor(&w, 1, f.root + i * 8, &desc);
        if (stop) return out->state = stop;
        out->l1[i] = entry_char(desc, 1, i << 30, f.oas);
        if (i == out->gib) {
            out->gib_read = 1;
            out->gib_desc = desc;
        }
    }
    if (!out->gib_read) {
        out->l2_state = EL2Q_S2W_L2_NO_ENTRY;
        return out->state = EL2Q_S2W_DONE;
    }
    if (out->l1[out->gib] != 'T') {
        out->l2_state = EL2Q_S2W_L2_NOT_TABLE;
        return out->state = EL2Q_S2W_DONE;
    }
    out->l2_state = EL2Q_S2W_L2_READ;
    out->l2_table = out->gib_desc & S2_OA;
    for (uint64_t i = 0; i < 512; i++) {
        uint32_t stop = read_descriptor(&w, 2, out->l2_table + i * 8, &desc);
        if (stop) return out->state = stop;
        out->l2[i] = entry_char(desc, 2, ((uint64_t)out->gib << 30) + (i << 21), f.oas);
    }
    return out->state = EL2Q_S2W_DONE;
}

/* ---- The test fixture ------------------------------------------------------- */

#define TEST_LEAF  (S2_TYPE | (0xfull << 2) | S2_AP_RW | (3ull << 8) | S2_AF)
#define TEST_BLOCK ((TEST_LEAF & ~S2_TYPE) | S2_BLOCK)

int el2q_s2w_build_test(uint64_t arena, uint64_t pool, int odd,
                        void (*write64)(void *ctx, uint64_t address, uint64_t value), void *ctx,
                        struct el2q_s2w_bank *bank)
{
    /* Level 1 at pool page 0 (64 entries for 36 bits), two level-2 tables
     * for the arena's one or two 1 GiB regions, and two level-3 tables for
     * its one or two 2 MiB regions. */
    if (!arena || (arena & 0xfff) || arena + EL2Q_S2W_ARENA_SIZE > (1ull << 36) || (pool & 0xfff))
        return 1;
    uint64_t l1 = pool, last = arena + EL2Q_S2W_ARENA_SIZE - 1;
    for (uint64_t p = 0; p < EL2Q_S2W_TEST_PAGES * 0x1000; p += 8) write64(ctx, pool + p, 0);
    for (unsigned g = 0; g < 2; g++) {
        uint64_t gib = (g ? last : arena) >> 30, l2 = pool + 0x1000 * (1 + g);
        if (g && gib == arena >> 30) break;
        write64(ctx, l1 + gib * 8, l2 | S2_TYPE);
        for (unsigned m = 0; m < 2; m++) {
            uint64_t two = (m ? last : arena) >> 21;
            if ((two >> 9) != gib || (m && two == arena >> 21)) continue;
            uint64_t l3 = pool + 0x1000 * (3 + m);
            write64(ctx, l2 + (two & 511) * 8, l3 | S2_TYPE);
            for (uint64_t e = 0; e < 512; e++) write64(ctx, l3 + e * 8, ((two << 21) + (e << 12)) | TEST_LEAF);
        }
    }
    if (odd) {
        /* Pages 1 to 3 may sit in the second level-3 table. */
        for (unsigned k = 1; k <= 3; k++) {
            uint64_t ipa = arena + ((uint64_t)k << 12);
            uint64_t l3 = pool + 0x1000 * (3 + ((ipa >> 21) != (arena >> 21)));
            uint64_t slot = l3 + ((ipa >> 12) & 511) * 8;
            uint64_t v = k == 1 ? (ipa + (2ull << 20)) | TEST_LEAF
                       : k == 2 ? 0 : (ipa | TEST_LEAF) & ~(2ull << 6);
            write64(ctx, slot, v);
        }
        /* For the survey: a 1 GiB identity block at level 1 entry 0, and
         * at level-2 entries 0 to 2 of the arena's first 1 GiB a moved, a
         * read-only and an identity block, wherever the arena is not. */
        if (arena >> 30 && last >> 30)
            write64(ctx, l1, TEST_BLOCK);
        uint64_t gib = arena >> 30, l2 = pool + 0x1000;
        for (uint64_t e = 0; e < 3; e++) {
            uint64_t ipa = (gib << 30) + (e << 21);
            if (ipa >> 21 == arena >> 21 || ipa >> 21 == last >> 21) continue;
            uint64_t v = e == 0 ? (ipa + (64ull << 21)) | TEST_BLOCK
                       : e == 1 ? (ipa | TEST_BLOCK) & ~(2ull << 6) : ipa | TEST_BLOCK;
            write64(ctx, l2 + e * 8, v);
        }
    }
    *bank = (struct el2q_s2w_bank){.sctlr = 0xe1, .tcr = 0x1355c, .cba2r = 1, .ttbr = l1};
    return 0;
}

/* ---- Report lines --------------------------------------------------------------
 * Nothing when not asked. The walk, before it could start:
 *   SMMU WALK <NOT RUN: NO ARENA|NOT RUN: NO STAGE 2|NOT RUN: FORMAT|
 *              NOT RUN: SERROR ALREADY PENDING|NOT NEEDED: STAGE 2 OFF>
 * and once it started:
 *   SMMU WALK ARENA <8 hex> READS <n>
 *   SMMU WALK RW <n> LIMITED <n> MOVED <n> FAULTS <n>
 *     or <stop>: REFUSED: TABLE <16 hex> PAR <16 hex> | REFUSED: DESCRIPTOR
 *        <16 hex> | FAULT AT <16 hex> ESR <8 hex> | SERROR PENDING AFTER
 *        <16 hex> | REFUSED: OVER <n> READS
 *   SMMU WALK LEAF L<n> <16 hex>          (the arena's first page, when read)
 *   SMMU WALK ODD <8 hex> L<n> <16 hex>   (the first page not RW, if any)
 * The survey, before it could start, SMMU SURVEY and the same words; once
 * it started:
 *   SMMU SURVEY READS <n>
 *   SMMU SURVEY <stop>                    (only when it stopped early)
 *   SMMU SURVEY L1 <3 hex> <32 entries>   (each row of the root read from)
 *   SMMU SURVEY GIB <2 hex> <16 hex>      (the arena's level-1 descriptor)
 *   SMMU SURVEY L2 AT <16 hex> EMPTY ROWS <n>, then each row of its 512
 *     entries that is neither all '.' nor all '-' (never read):
 *   SMMU SURVEY L2 <8 hex> <32 entries>   (the IPA of the row's first entry)
 *     or SMMU SURVEY L2 NONE: <NOT A TABLE|OUTSIDE THE INPUT SIZE>
 * The first line ends " (TEST TABLES)" when the tables are el2qual's own,
 * and the walk's " (FIXED)" when it walked EL2Q_S2W_FIXED_ARENA. The
 * candidate region's memory map:
 *   UEFI MAP <8 hex>-<8 hex> ENTRIES <n>    (or UEFI MAP NOT AVAILABLE)
 *   UEFI MAP <10 hex start> <10 hex end> <type> <16 hex attributes>
 *   UEFI MAP AND <n> MORE                    (past EL2Q_S2W_MAP_ENTRIES)
 * the first ending " (TEST RANGE)" on QEMU's stand-in range. */

struct lbuf { char *s; unsigned n; };

static void put_s(struct lbuf *b, const char *s)
{
    while (*s && b->n < EL2Q_S2W_LINE_COLS) b->s[b->n++] = *s++;
    b->s[b->n] = 0;
}

static void put_hex(struct lbuf *b, uint64_t v, unsigned digits)
{
    char t[17];
    for (unsigned i = 0; i < digits; i++) t[i] = "0123456789ABCDEF"[(v >> (4 * (digits - 1 - i))) & 15];
    t[digits] = 0;
    put_s(b, t);
}

static void put_dec(struct lbuf *b, uint64_t v)
{
    char d[20];
    unsigned n = 0;
    do d[n++] = (char)('0' + v % 10); while ((v /= 10) && n < sizeof d);
    while (n) {
        char c[2] = {d[--n], 0};
        put_s(b, c);
    }
}

/* The words for a pass that did not start; 0 for one that did. */
static const char *not_run_words(uint32_t state)
{
    static const char *const words[] = {
        [EL2Q_S2W_NO_ARENA] = "NOT RUN: NO ARENA",
        [EL2Q_S2W_NO_STAGE2] = "NOT RUN: NO STAGE 2",
        [EL2Q_S2W_NOT_NEEDED] = "NOT NEEDED: STAGE 2 OFF",
        [EL2Q_S2W_FORMAT] = "NOT RUN: FORMAT",
        [EL2Q_S2W_PENDING] = "NOT RUN: SERROR ALREADY PENDING",
    };
    return state && state <= EL2Q_S2W_PENDING ? words[state] : 0;
}

static void put_stop(struct lbuf *b, uint32_t state, uint64_t at, uint64_t detail, uint32_t limit)
{
    if (state == EL2Q_S2W_TABLE_REFUSED) {
        put_s(b, "REFUSED: TABLE "); put_hex(b, at, 16);
        put_s(b, " PAR "); put_hex(b, detail, 16);
    } else if (state == EL2Q_S2W_STRUCTURE) {
        put_s(b, "REFUSED: DESCRIPTOR "); put_hex(b, detail, 16);
    } else if (state == EL2Q_S2W_FAULT) {
        put_s(b, "FAULT AT "); put_hex(b, at, 16);
        put_s(b, " ESR "); put_hex(b, detail, 8);
    } else if (state == EL2Q_S2W_SERROR) {
        put_s(b, "SERROR PENDING AFTER "); put_hex(b, at, 16);
    } else {
        put_s(b, "REFUSED: OVER "); put_dec(b, limit); put_s(b, " READS");
    }
}

void el2q_s2w_line(const struct el2q_s2w_result *r, unsigned index, char out[EL2Q_S2W_LINE_COLS + 1])
{
    struct lbuf b = {out, 0};
    out[0] = 0;
    if (r->state == EL2Q_S2W_NOT_ASKED || r->state >= EL2Q_S2W_COUNT) return;
    const char *not_run = not_run_words(r->state);
    if (not_run) {
        if (index) return;
        put_s(&b, "SMMU WALK ");
        put_s(&b, not_run);
        if (r->test) put_s(&b, " (TEST TABLES)");
        if (r->fixed) put_s(&b, " (FIXED)");
        return;
    }
    if (index == 0) {
        put_s(&b, "SMMU WALK ARENA ");
        put_hex(&b, r->arena, 8);
        put_s(&b, " READS ");
        put_dec(&b, r->reads);
        if (r->test) put_s(&b, " (TEST TABLES)");
        if (r->fixed) put_s(&b, " (FIXED)");
    } else if (index == 1) {
        put_s(&b, "SMMU WALK ");
        if (r->state == EL2Q_S2W_DONE) {
            put_s(&b, "RW "); put_dec(&b, r->pages[EL2Q_S2W_RW]);
            put_s(&b, " LIMITED "); put_dec(&b, r->pages[EL2Q_S2W_LIMITED]);
            put_s(&b, " MOVED "); put_dec(&b, r->pages[EL2Q_S2W_MOVED]);
            put_s(&b, " FAULTS "); put_dec(&b, r->pages[EL2Q_S2W_FAULTS]);
        } else {
            put_stop(&b, r->state, r->at, r->detail, EL2Q_S2W_MAX_READS);
        }
    } else if (r->state == EL2Q_S2W_DONE && index - 2 < r->leaf_seen + r->odd_seen) {
        if (index == 2 && r->leaf_seen) {
            put_s(&b, "SMMU WALK LEAF L"); put_dec(&b, r->leaf_level);
            put_s(&b, " "); put_hex(&b, r->leaf, 16);
        } else {
            put_s(&b, "SMMU WALK ODD "); put_hex(&b, r->odd_ipa, 8);
            put_s(&b, " L"); put_dec(&b, r->odd_level);
            put_s(&b, " "); put_hex(&b, r->odd, 16);
        }
    }
}

#define ROW EL2Q_S2W_SURVEY_ROW

/* A row of only c: '.' for an empty one, '-' for one never read. */
static int row_all(const char *map, unsigned row, char c)
{
    for (unsigned i = 0; i < ROW; i++)
        if (map[row * ROW + i] != c) return 0;
    return 1;
}

static void put_row(struct lbuf *b, const char *map, unsigned row, unsigned count)
{
    char t[ROW + 1];
    unsigned n = 0;
    for (; n < ROW && row * ROW + n < count; n++) t[n] = map[row * ROW + n];
    t[n] = 0;
    put_s(b, t);
}

void el2q_s2w_survey_line(const struct el2q_s2w_survey *s, unsigned index,
                          char out[EL2Q_S2W_LINE_COLS + 1])
{
    struct lbuf b = {out, 0};
    out[0] = 0;
    if (s->state == EL2Q_S2W_NOT_ASKED || s->state >= EL2Q_S2W_COUNT) return;
    const char *not_run = not_run_words(s->state);
    if (not_run) {
        if (index) return;
        put_s(&b, "SMMU SURVEY ");
        put_s(&b, not_run);
        if (s->test) put_s(&b, " (TEST TABLES)");
        return;
    }
    if (index == 0) {
        put_s(&b, "SMMU SURVEY READS ");
        put_dec(&b, s->reads);
        if (s->test) put_s(&b, " (TEST TABLES)");
        return;
    }
    index--;
    if (s->state != EL2Q_S2W_DONE) {
        if (index == 0) {
            put_s(&b, "SMMU SURVEY ");
            put_stop(&b, s->state, s->at, s->detail, EL2Q_S2W_SURVEY_MAX_READS);
            return;
        }
        index--;
    }
    /* Level-1 rows, once anything of the root was read. */
    unsigned l1_rows = s->l1[0] == '-' ? 0 : (s->l1_entries + ROW - 1) / ROW;
    if (index < l1_rows) {
        put_s(&b, "SMMU SURVEY L1 ");
        put_hex(&b, index * ROW, 3);
        put_s(&b, " ");
        put_row(&b, s->l1, index, s->l1_entries);
        return;
    }
    index -= l1_rows;
    if (s->gib_read) {
        if (index == 0) {
            put_s(&b, "SMMU SURVEY GIB "); put_hex(&b, s->gib, 2);
            put_s(&b, " "); put_hex(&b, s->gib_desc, 16);
            return;
        }
        index--;
    }
    if (s->l2_state == EL2Q_S2W_L2_NOT_TABLE || s->l2_state == EL2Q_S2W_L2_NO_ENTRY) {
        if (index == 0)
            put_s(&b, s->l2_state == EL2Q_S2W_L2_NOT_TABLE ? "SMMU SURVEY L2 NONE: NOT A TABLE"
                                                           : "SMMU SURVEY L2 NONE: OUTSIDE THE INPUT SIZE");
        return;
    }
    if (s->l2_state != EL2Q_S2W_L2_READ) return;
    unsigned empty = 0;
    for (unsigned r = 0; r < 512 / ROW; r++) empty += row_all(s->l2, r, '.');
    if (index == 0) {
        put_s(&b, "SMMU SURVEY L2 AT "); put_hex(&b, s->l2_table, 16);
        put_s(&b, " EMPTY ROWS "); put_dec(&b, empty);
        return;
    }
    index--;
    for (unsigned r = 0; r < 512 / ROW; r++) {
        if (row_all(s->l2, r, '.') || row_all(s->l2, r, '-')) continue;
        if (index--) continue;
        put_s(&b, "SMMU SURVEY L2 ");
        put_hex(&b, ((uint64_t)s->gib << 30) + ((uint64_t)r * ROW << 21), 8);
        put_s(&b, " ");
        put_row(&b, s->l2, r, 512);
        return;
    }
}

/* EFI_MEMORY_TYPE (UEFI 2.10, 7.2), at most nine characters each. */
static const char *const map_types[] = {
    "RESERVED", "LDR CODE", "LDR DATA", "BS CODE", "BS DATA", "RT CODE", "RT DATA", "FREE",
    "UNUSABLE", "ACPI RECL", "ACPI NVS", "MMIO", "MMIO PORT", "PAL CODE", "PERSIST",
};

void el2q_s2w_region_line(const struct el2q_s2w_region *r, unsigned index,
                          char out[EL2Q_S2W_LINE_COLS + 1])
{
    struct lbuf b = {out, 0};
    out[0] = 0;
    if (r->state == EL2Q_S2W_REGION_NOT_ASKED || r->state > EL2Q_S2W_REGION_READ) return;
    if (r->state == EL2Q_S2W_REGION_NO_MAP) {
        if (!index) put_s(&b, "UEFI MAP NOT AVAILABLE");
        return;
    }
    unsigned shown = r->count < EL2Q_S2W_MAP_ENTRIES ? r->count : EL2Q_S2W_MAP_ENTRIES;
    if (index == 0) {
        put_s(&b, "UEFI MAP "); put_hex(&b, r->base, 8);
        put_s(&b, "-"); put_hex(&b, r->end, 8);
        put_s(&b, " ENTRIES "); put_dec(&b, r->count);
        if (r->test) put_s(&b, " (TEST RANGE)");
    } else if (index <= shown) {
        const struct el2q_s2w_map_entry *e = &r->e[index - 1];
        put_s(&b, "UEFI MAP "); put_hex(&b, e->start, 10);
        put_s(&b, " "); put_hex(&b, e->start + (e->pages << 12), 10);
        put_s(&b, " ");
        if (e->type < sizeof map_types / sizeof map_types[0]) put_s(&b, map_types[e->type]);
        else { put_s(&b, "T"); put_hex(&b, e->type, 8); }
        put_s(&b, " "); put_hex(&b, e->attr, 16);
    } else if (index == shown + 1 && r->count > shown) {
        put_s(&b, "UEFI MAP AND "); put_dec(&b, r->count - shown); put_s(&b, " MORE");
    }
}
