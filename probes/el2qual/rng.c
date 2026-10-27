/* SPDX-License-Identifier: MIT */
/* See rng.h. */
#include "rng.h"

/* EFI_BUFFER_TOO_SMALL (UEFI 2.10, appendix D): GetInfo's answer when the
 * list does not fit, with the size it needs in *list_size. */
#define EFI_BUFFER_TOO_SMALL ((efi_status)1 << 63 | 5)

const struct efi_guid el2q_rng_protocol_guid =
    {0x3152bca5, 0xeade, 0x433d, {0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44}};

static const struct { struct efi_guid guid; uint8_t alg; } known[] = {
    {{0xe43176d7, 0xb6e8, 0x4827, {0xb7, 0x84, 0x7f, 0xfd, 0xc4, 0xb6, 0x85, 0x61}}, EL2Q_RNG_ALG_RAW},
    {{0x44f0de6e, 0x4d8c, 0x4045, {0xa8, 0xc7, 0x4d, 0xd1, 0x68, 0x85, 0x6b, 0x9e}}, EL2Q_RNG_ALG_CTR},
    {{0xa7af67cb, 0x603b, 0x4d42, {0xba, 0x21, 0x70, 0xbf, 0xb6, 0x29, 0x3f, 0x96}}, EL2Q_RNG_ALG_HASH},
    {{0xc5149b43, 0xae85, 0x4f53, {0x99, 0x82, 0xb9, 0x43, 0x35, 0xd3, 0xa9, 0xe7}}, EL2Q_RNG_ALG_HMAC},
    {{0xacd03321, 0x777e, 0x4d3d, {0xb1, 0xc8, 0x20, 0xcf, 0xd8, 0x88, 0x20, 0xc9}}, EL2Q_RNG_ALG_AES},
    {{0x63c4785a, 0xca34, 0x4012, {0xa3, 0xc8, 0x0b, 0x6a, 0x32, 0x4f, 0x55, 0x46}}, EL2Q_RNG_ALG_3DES},
    {{0x43d2fde3, 0x9d4e, 0x4d79, {0x02, 0x96, 0xa8, 0x9b, 0xca, 0x78, 0x08, 0x41}}, EL2Q_RNG_ALG_RNDR},
};

static int same_guid(const struct efi_guid *a, const struct efi_guid *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (unsigned i = 0; i < sizeof *a; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

static uint8_t classify(const struct efi_guid *g)
{
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (same_guid(g, &known[i].guid)) return known[i].alg;
    return EL2Q_RNG_ALG_OTHER;
}

static void read_twice(struct el2q_rng_protocol *p, const struct efi_guid *algorithm,
                       struct el2q_rng_read *out)
{
    uint8_t a[EL2Q_RNG_BYTES], b[EL2Q_RNG_BYTES];
    *out = (struct el2q_rng_read){.asked = 1};
    /* Zeroed first: a driver that reports success without writing must read
     * as all zero, not as whatever the stack held. */
    for (unsigned i = 0; i < EL2Q_RNG_BYTES; i++) a[i] = b[i] = 0;
    out->status[0] = p->get_rng(p, algorithm, EL2Q_RNG_BYTES, a);
    out->status[1] = p->get_rng(p, algorithm, EL2Q_RNG_BYTES, b);
    if (out->status[0] != 0 || out->status[1] != 0) return;
    int zero_a = 1, zero_b = 1, same = 1;
    uint8_t seen[32] = {0};
    for (unsigned i = 0; i < EL2Q_RNG_BYTES; i++) {
        zero_a &= a[i] == 0;
        zero_b &= b[i] == 0;
        same &= a[i] == b[i];
        seen[a[i] >> 3] |= (uint8_t)(1u << (a[i] & 7));
    }
    out->nonzero = !zero_a && !zero_b;
    out->differ = !same;
    for (unsigned i = 0; i < sizeof seen; i++)
        for (uint8_t v = seen[i]; v; v &= (uint8_t)(v - 1)) out->distinct++;
}

static int usable(const struct el2q_rng_read *r)
{
    return r->asked && r->status[0] == 0 && r->status[1] == 0 && r->nonzero && r->differ;
}

void el2q_rng_probe(el2q_rng_locate_fn locate, void *ctx, struct el2q_rng *out)
{
    *out = (struct el2q_rng){.state = EL2Q_RNG_NOT_FOUND};
    if (!locate) return;
    void *found = 0;
    out->locate_status = locate(ctx, &el2q_rng_protocol_guid, &found);
    if (out->locate_status != 0) return;
    if (!found) {
        out->null_interface = 1;
        return;
    }
    struct el2q_rng_protocol *p = found;

    struct efi_guid list[EL2Q_RNG_MAX_ALGS];
    uint64_t size = sizeof list;
    out->info_status = p->get_info(p, &size, list);
    int raw = 0;
    if (out->info_status == 0 || out->info_status == EFI_BUFFER_TOO_SMALL)
        out->listed = (uint32_t)(size / sizeof list[0]);
    /* Only a successful GetInfo filled the list; a too-small buffer left it
     * as it was, so nothing in it is named. */
    if (out->info_status == 0)
        for (unsigned i = 0; i < out->listed && i < EL2Q_RNG_MAX_ALGS; i++) {
            out->alg[i] = classify(&list[i]);
            raw |= out->alg[i] == EL2Q_RNG_ALG_RAW;
        }

    read_twice(p, 0, &out->def);
    if (raw) read_twice(p, &known[0].guid, &out->raw);

    /* RAW is preferred: it is the one that claims to come straight from the
     * source, which is what Phase 0C condition 3 asks for. */
    out->state = usable(&out->raw) ? EL2Q_RNG_USABLE_RAW
               : usable(&out->def) ? EL2Q_RNG_USABLE_DEFAULT : EL2Q_RNG_UNUSABLE;
}

/* ---- Report lines -----------------------------------------------------------
 *   RNG PROTOCOL <FOUND|NOT FOUND, STATUS <16 hex>|FOUND, NO INTERFACE>
 * then, when found:
 *   RNG ALGORITHMS <n>: <names>|RNG ALGORITHMS NOT LISTED, STATUS <16 hex>
 *   RNG DEFAULT 256B <result>
 *   RNG RAW 256B <result>                      (only when RAW is listed)
 * and last
 *   RNG VERDICT: <USABLE, RAW|USABLE, DEFAULT ALGORITHM|UNUSABLE|NO SOURCE>
 * A result is OK <NONZERO|ALL ZERO> <DIFFER|SAME TWICE> DISTINCT <n>, or
 * FAILED, STATUS <16 hex>, or SECOND FAILED, STATUS <16 hex>. A list longer
 * than EL2Q_RNG_MAX_ALGS ends in "+<n>"; one GetInfo could not fit is counted
 * but not named. */

struct lbuf { char *s; unsigned n; };

static void put_s(struct lbuf *b, const char *s)
{
    while (*s && b->n < EL2Q_RNG_LINE_COLS) b->s[b->n++] = *s++;
    b->s[b->n] = 0;
}

static void put_hex(struct lbuf *b, uint64_t v)
{
    char t[17];
    for (unsigned i = 0; i < 16; i++) t[i] = "0123456789ABCDEF"[(v >> (4 * (15 - i))) & 15];
    t[16] = 0;
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

static const char *const alg_names[EL2Q_RNG_ALG_COUNT] = {"?", "RAW", "CTR", "HASH", "HMAC",
                                                         "AES", "3DES", "RNDR"};

static void read_result(struct lbuf *b, const struct el2q_rng_read *r)
{
    if (r->status[0] != 0) {
        put_s(b, "FAILED, STATUS ");
        put_hex(b, r->status[0]);
    } else if (r->status[1] != 0) {
        put_s(b, "SECOND FAILED, STATUS ");
        put_hex(b, r->status[1]);
    } else {
        put_s(b, r->nonzero ? "OK NONZERO" : "OK ALL ZERO");
        put_s(b, r->differ ? " DIFFER DISTINCT " : " SAME TWICE DISTINCT ");
        put_dec(b, r->distinct);
    }
}

void el2q_rng_line(const struct el2q_rng *r, unsigned index, char out[EL2Q_RNG_LINE_COLS + 1])
{
    struct lbuf b = {out, 0};
    out[0] = 0;
    if (r->state == EL2Q_RNG_NOT_ASKED) return;
    int found = r->state != EL2Q_RNG_NOT_FOUND;
    /* The lines that exist, in order, so none leaves a hole. */
    enum { PROTOCOL, ALGORITHMS, DEFAULT, RAW, VERDICT } order[EL2Q_RNG_LINES];
    unsigned n = 0;
    order[n++] = PROTOCOL;
    if (found) {
        order[n++] = ALGORITHMS;
        order[n++] = DEFAULT;
        if (r->raw.asked) order[n++] = RAW;
    }
    order[n++] = VERDICT;
    if (index >= n) return;

    switch (order[index]) {
    case PROTOCOL:
        put_s(&b, "RNG PROTOCOL ");
        if (r->null_interface) put_s(&b, "FOUND, NO INTERFACE");
        else if (!found) {
            put_s(&b, "NOT FOUND, STATUS ");
            put_hex(&b, r->locate_status);
        } else put_s(&b, "FOUND");
        break;
    case ALGORITHMS:
        put_s(&b, "RNG ALGORITHMS ");
        if (r->info_status != 0 && r->listed == 0) {
            put_s(&b, "NOT LISTED, STATUS ");
            put_hex(&b, r->info_status);
            break;
        }
        put_dec(&b, r->listed);
        if (r->info_status != 0) {
            put_s(&b, ", MORE THAN FIT, NOT NAMED");
            break;
        }
        put_s(&b, ":");
        for (unsigned i = 0; i < r->listed && i < EL2Q_RNG_MAX_ALGS; i++) {
            put_s(&b, " ");
            put_s(&b, r->alg[i] < EL2Q_RNG_ALG_COUNT ? alg_names[r->alg[i]] : "?");
        }
        if (r->listed > EL2Q_RNG_MAX_ALGS) {
            put_s(&b, " +");
            put_dec(&b, r->listed - EL2Q_RNG_MAX_ALGS);
        }
        break;
    case DEFAULT:
        put_s(&b, "RNG DEFAULT 256B ");
        read_result(&b, &r->def);
        break;
    case RAW:
        put_s(&b, "RNG RAW 256B ");
        read_result(&b, &r->raw);
        break;
    case VERDICT:
        put_s(&b, "RNG VERDICT: ");
        put_s(&b, r->state == EL2Q_RNG_USABLE_RAW ? "USABLE, RAW"
                : r->state == EL2Q_RNG_USABLE_DEFAULT ? "USABLE, DEFAULT ALGORITHM"
                : r->state == EL2Q_RNG_UNUSABLE ? "UNUSABLE" : "NO SOURCE");
        break;
    }
}
