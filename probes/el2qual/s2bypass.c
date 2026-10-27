/* SPDX-License-Identifier: MIT */
/* See s2bypass.h. Nothing here writes. */
#include "s2bypass.h"

#define SMMU_sCR0      0x0u
#define SMMU_sGFSR     0x48u
#define SMMU_SMR(n)    (0x800u + 4u * (n))
#define SMMU_S2CR(n)   (0xc00u + 4u * (n))
#define sCR0_CLIENTPD  (1u << 0)
#define sCR0_EXIDENABLE (1u << 3)
#define SMR_VALID      (1u << 31)
#define S2CR_TYPE      (3u << 16)
#define S2CR_TRANSLATE (0u << 16)
#define S2CR_BYPASS    (1u << 16)
#define S2CR_CBNDX     0xffu
/* NUMSMRG is 8 bits, so groups are at most 0 to 254, and S2CR(254) at
 * 0xff8 still lies in the first 4 KiB of the global register space: a
 * group below NUMSMRG is always in the one page this plan would write. The
 * clamp mirrors usbprobe.c's EL2Q_USB_SMR_LIMIT. */
#define GROUP_LIMIT    255u

/* A valid group whose id, outside its mask, equals the stream. The mask is
 * 15 bits because extended stream IDs are refused above. */
static int group_matches(uint32_t smr, uint32_t stream)
{
    if (!(smr & SMR_VALID)) return 0;
    uint32_t mask = smr >> 16 & 0x7fffu, id = smr & 0xffffu;
    return ((id ^ stream) & ~mask & 0xffffu) == 0;
}

uint32_t el2q_s2b_plan(const struct el2q_s2b_seen *seen, const struct el2q_s2b_hw *hw,
                       struct el2q_s2b_result *out)
{
    *out = (struct el2q_s2b_result){.state = EL2Q_S2B_NOT_MEASURED};
    if (!seen) return out->state;
    out->scr0 = seen->scr0;
    out->group = seen->group;
    if (!hw || !hw->mapped || !hw->writable || !hw->read32 || !hw->serror)
        return out->state;
    if (!seen->matched || !seen->bank_read) return out->state;

    /* The SMMU must be on, and matching streams the way the read assumed.
     * With extended stream IDs a group's validity lives in S2CR, which the
     * write would have to preserve; that is not this plan. */
    if (seen->scr0 == 0xffffffffu || (seen->scr0 & sCR0_CLIENTPD) ||
        (seen->scr0 & sCR0_EXIDENABLE))
        return out->state = EL2Q_S2B_REFUSED_SCR0;

    out->mask = seen->smr >> 16 & 0x7fffu;
    out->old = seen->s2cr;
    if ((seen->s2cr & S2CR_TYPE) == S2CR_BYPASS) return out->state = EL2Q_S2B_NOT_NEEDED;
    /* It must still be the translating S2CR that named the bank el2qual
     * read; anything else and the plan is about a machine state that was
     * never measured. */
    if ((seen->s2cr & S2CR_TYPE) != S2CR_TRANSLATE ||
        (seen->s2cr & S2CR_CBNDX) != (seen->cb_index & S2CR_CBNDX))
        return out->state = EL2Q_S2B_REFUSED_TYPE;

    /* The group must select USB0's stream and nothing else: an exact match,
     * so no other device's transactions change behaviour. */
    if (!group_matches(seen->smr, seen->stream) || out->mask != 0 ||
        (seen->smr & 0xffffu) != seen->stream)
        return out->state = EL2Q_S2B_REFUSED_MASK;

    unsigned groups = seen->idr0 & 0xffu;                /* NUMSMRG */
    if (groups > GROUP_LIMIT) groups = GROUP_LIMIT;
    if (seen->group >= groups) return out->state = EL2Q_S2B_REFUSED_RANGE;
    out->at = seen->base + SMMU_S2CR(seen->group);

    if (hw->serror(hw->ctx)) return out->state = EL2Q_S2B_REFUSED_PENDING;
    /* The write would go through the mapping el2qual runs on, so that page
     * must translate to itself as Device and allow a write at EL2. */
    uint64_t page = out->at & ~0xfffull;
    if (hw->mapped(hw->ctx, page)) {
        out->detail = page;
        return out->state = EL2Q_S2B_REFUSED_UNMAPPED;
    }
    if (hw->writable(hw->ctx, page)) {
        out->detail = page;
        return out->state = EL2Q_S2B_REFUSED_READONLY;
    }

#define READ(offset, dest) do { \
        uint32_t v_ = 0; \
        uint64_t f_ = hw->read32(hw->ctx, seen->base + (offset), &v_); \
        if (f_) { out->detail = f_; return out->state = EL2Q_S2B_REFUSED_READ; } \
        (dest) = v_; \
    } while (0)
    /* A global fault already latched would make one raised by the write
     * impossible to attribute. The plan refuses rather than clearing it:
     * clearing sGFSR is a write. */
    READ(SMMU_sGFSR, out->gfsr);
    if (out->gfsr) return out->state = EL2Q_S2B_REFUSED_FAULT;
    /* Nothing may have moved since the SMMU read: the same sCR0, and the
     * same S2CR in the group about to be planned. */
    uint32_t now = 0;
    READ(SMMU_sCR0, now);
    if (now != seen->scr0) return out->state = EL2Q_S2B_REFUSED_MOVED;
    READ(SMMU_S2CR(seen->group), now);
    if (now != seen->s2cr) return out->state = EL2Q_S2B_REFUSED_MOVED;

    /* usbprobe.c stops at the first matching group; a second one would also
     * translate USB0's stream, and bypassing only the first would leave the
     * SMMU with two answers for it. The group the read found must also
     * still be the match: a count of one alone would accept SMR(group)
     * having moved to another stream while some other group took 0x820, and
     * the plan would then name an S2CR serving a different device. */
    for (unsigned n = 0; n < groups; n++) {
        uint32_t smr = 0;
        READ(SMMU_SMR(n), smr);
        out->scanned++;
        if (n == seen->group && smr != seen->smr) return out->state = EL2Q_S2B_REFUSED_MOVED;
        out->matches += group_matches(smr, seen->stream);
    }
#undef READ
    if (out->matches != 1) return out->state = EL2Q_S2B_REFUSED_SHARED;
    /* PLANNED must also mean the plan's own 215 reads raised nothing. */
    if (hw->serror(hw->ctx)) return out->state = EL2Q_S2B_REFUSED_SERROR;

    out->planned = (seen->s2cr & ~S2CR_TYPE) | S2CR_BYPASS;
    out->restore = seen->s2cr;
    return out->state = EL2Q_S2B_PLANNED;
}

/* ---- Report lines --------------------------------------------------------------
 * Nothing when not asked. Otherwise, first the one word the write would
 * change, or why there is none:
 *   SMMU BYPASS S2CR<nn> AT <8 hex> <8 hex> -> <8 hex>
 *   SMMU BYPASS <NOT MEASURED|NOT NEEDED: ALREADY BYPASSING|
 *                REFUSED: <SCR0 <8 hex>|S2CR TYPE|STREAM MASK <4 hex>|
 *                <n> GROUPS MATCH|OUT OF RANGE|READ ESR <8 hex>|
 *                UNMAPPED <8 hex>|READ ONLY <8 hex>|SERROR ALREADY PENDING|
 *                GFSR <8 hex>|MOVED SINCE READ|SERROR DURING THE CHECK>>
 * then, once the scan ran:
 *   SMMU BYPASS MATCHES <n>/<n> MASK <4 hex> GFSR <8 hex>
 * and always, because it is the point of this stage:
 *   SMMU BYPASS PLAN ONLY, NOTHING WRITTEN */

struct lbuf { char *s; unsigned n; };

static void put_s(struct lbuf *b, const char *s)
{
    while (*s && b->n < EL2Q_S2B_LINE_COLS) b->s[b->n++] = *s++;
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

void el2q_s2b_line(const struct el2q_s2b_result *r, unsigned index,
                   char out[EL2Q_S2B_LINE_COLS + 1])
{
    struct lbuf b = {out, 0};
    out[0] = 0;
    if (r->state == EL2Q_S2B_NOT_ASKED || r->state >= EL2Q_S2B_COUNT) return;
    if (index == 0) {
        put_s(&b, "SMMU BYPASS ");
        switch (r->state) {
        case EL2Q_S2B_PLANNED:
            put_s(&b, "S2CR");
            put_hex(&b, r->group, 2);
            put_s(&b, " AT "); put_hex(&b, r->at, 8);
            put_s(&b, " "); put_hex(&b, r->old, 8);
            put_s(&b, " -> "); put_hex(&b, r->planned, 8);
            break;
        case EL2Q_S2B_NOT_MEASURED: put_s(&b, "NOT MEASURED"); break;
        case EL2Q_S2B_NOT_NEEDED: put_s(&b, "NOT NEEDED: ALREADY BYPASSING"); break;
        case EL2Q_S2B_REFUSED_SCR0:
            put_s(&b, "REFUSED: SCR0 "); put_hex(&b, r->scr0, 8);
            break;
        case EL2Q_S2B_REFUSED_TYPE: put_s(&b, "REFUSED: S2CR TYPE"); break;
        case EL2Q_S2B_REFUSED_MASK:
            put_s(&b, "REFUSED: STREAM MASK "); put_hex(&b, r->mask, 4);
            break;
        case EL2Q_S2B_REFUSED_SHARED:
            put_s(&b, "REFUSED: "); put_dec(&b, r->matches); put_s(&b, " GROUPS MATCH");
            break;
        case EL2Q_S2B_REFUSED_RANGE: put_s(&b, "REFUSED: OUT OF RANGE"); break;
        case EL2Q_S2B_REFUSED_READ:
            put_s(&b, "REFUSED: READ ESR "); put_hex(&b, r->detail, 8);
            break;
        case EL2Q_S2B_REFUSED_UNMAPPED:
            put_s(&b, "REFUSED: UNMAPPED "); put_hex(&b, r->detail, 8);
            break;
        case EL2Q_S2B_REFUSED_READONLY:
            put_s(&b, "REFUSED: READ ONLY "); put_hex(&b, r->detail, 8);
            break;
        case EL2Q_S2B_REFUSED_PENDING: put_s(&b, "REFUSED: SERROR ALREADY PENDING"); break;
        case EL2Q_S2B_REFUSED_FAULT:
            put_s(&b, "REFUSED: GFSR "); put_hex(&b, r->gfsr, 8);
            break;
        case EL2Q_S2B_REFUSED_MOVED: put_s(&b, "REFUSED: MOVED SINCE READ"); break;
        case EL2Q_S2B_REFUSED_SERROR: put_s(&b, "REFUSED: SERROR DURING THE CHECK"); break;
        default: break;
        }
        return;
    }
    /* No holes: the report stops at the first empty line, and the last
     * line below is the one that must always be read. */
    if (r->scanned && index == 1) {
        put_s(&b, "SMMU BYPASS MATCHES ");
        put_dec(&b, r->matches);
        put_s(&b, "/");
        put_dec(&b, r->scanned);
        put_s(&b, " MASK "); put_hex(&b, r->mask, 4);
        put_s(&b, " GFSR "); put_hex(&b, r->gfsr, 8);
        return;
    }
    if (index == (r->scanned ? 2u : 1u)) put_s(&b, "SMMU BYPASS PLAN ONLY, NOTHING WRITTEN");
}
