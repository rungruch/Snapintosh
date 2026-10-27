/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/rng.c, the entropy check. Linked into
 * tools/test-el2qual.c's binary by tools/test-el2qual.py; test_rng() returns
 * its check count.
 *
 * A fake firmware answers LocateProtocol, GetInfo and GetRNG every way the
 * real one might. Its GUIDs are its own copy, typed from UEFI 2.10 (as EDK2's
 * Guid/Rng.h publishes them), so a slip in rng.c's table fails here rather
 * than being shared. */
#include "rng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

unsigned test_rng(void);

#define ERR(n) ((efi_status)1 << 63 | (n))
#define NOT_FOUND ERR(14)
#define UNSUPPORTED ERR(3)
#define BUFFER_TOO_SMALL ERR(5)
#define DEVICE_ERROR ERR(7)
#define NOT_READY ERR(6)

static const struct efi_guid PROTOCOL =
    {0x3152bca5, 0xeade, 0x433d, {0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44}};
static const struct efi_guid RAW =
    {0xe43176d7, 0xb6e8, 0x4827, {0xb7, 0x84, 0x7f, 0xfd, 0xc4, 0xb6, 0x85, 0x61}};
static const struct efi_guid CTR =
    {0x44f0de6e, 0x4d8c, 0x4045, {0xa8, 0xc7, 0x4d, 0xd1, 0x68, 0x85, 0x6b, 0x9e}};
static const struct efi_guid HASH =
    {0xa7af67cb, 0x603b, 0x4d42, {0xba, 0x21, 0x70, 0xbf, 0xb6, 0x29, 0x3f, 0x96}};
static const struct efi_guid HMAC =
    {0xc5149b43, 0xae85, 0x4f53, {0x99, 0x82, 0xb9, 0x43, 0x35, 0xd3, 0xa9, 0xe7}};
static const struct efi_guid AES =
    {0xacd03321, 0x777e, 0x4d3d, {0xb1, 0xc8, 0x20, 0xcf, 0xd8, 0x88, 0x20, 0xc9}};
static const struct efi_guid TDES =
    {0x63c4785a, 0xca34, 0x4012, {0xa3, 0xc8, 0x0b, 0x6a, 0x32, 0x4f, 0x55, 0x46}};
static const struct efi_guid RNDR =
    {0x43d2fde3, 0x9d4e, 0x4d79, {0x02, 0x96, 0xa8, 0x9b, 0xca, 0x78, 0x08, 0x41}};
static const struct efi_guid UNKNOWN =
    {0x01234567, 0x89ab, 0xcdef, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};

/* How one algorithm answers GetRNG. */
enum how { GOOD, ZERO, REPEAT, FAIL_FIRST, FAIL_SECOND, WARN, NO_WRITE };

static struct fake {
    struct el2q_rng_protocol proto;           /* first: the probe hands it back as self */
    efi_status locate_status;
    int null_interface;
    efi_status info_status;
    uint64_t info_size;                       /* bytes GetInfo reports */
    struct efi_guid list[16];
    unsigned list_count;                      /* entries GetInfo writes, capped by the buffer */
    enum how def, raw;
    efi_status fail;
    unsigned default_calls, raw_calls, other_calls;
    uint64_t state;                           /* xorshift for GOOD */
} f;

static int same(const struct efi_guid *a, const struct efi_guid *b) { return !memcmp(a, b, sizeof *a); }

static efi_status fake_get_info(struct el2q_rng_protocol *self, uint64_t *size, struct efi_guid *list)
{
    CHECK(self == &f.proto);
    uint64_t room = *size / sizeof *list;
    /* A careless driver might write even when it says the buffer is too
     * small; the probe must not believe what it finds there. */
    for (unsigned i = 0; i < f.list_count && i < room; i++) list[i] = f.list[i];
    *size = f.info_size;
    return f.info_status;
}

static efi_status fake_get_rng(struct el2q_rng_protocol *self, const struct efi_guid *alg,
                               uint64_t length, uint8_t *value)
{
    CHECK(self == &f.proto && length == EL2Q_RNG_BYTES && value);
    enum how how;
    unsigned call;
    if (!alg) {
        how = f.def;
        call = f.default_calls++;
    } else if (same(alg, &RAW)) {
        how = f.raw;
        call = f.raw_calls++;
    } else {
        f.other_calls++;
        return UNSUPPORTED;
    }
    if ((how == FAIL_FIRST && call % 2 == 0) || (how == FAIL_SECOND && call % 2 == 1)) return f.fail;
    if (how == NO_WRITE) return 0;
    for (uint64_t i = 0; i < length; i++) {
        if (how == ZERO) value[i] = 0;
        else if (how == REPEAT) value[i] = (uint8_t)(i * 7 + 1);
        else {
            f.state ^= f.state << 13; f.state ^= f.state >> 7; f.state ^= f.state << 17;
            value[i] = (uint8_t)f.state;
        }
    }
    return how == WARN ? 1 : 0;                /* a warning, not success */
}

static efi_status fake_locate(void *ctx, const struct efi_guid *guid, void **out)
{
    CHECK(ctx == &f && same(guid, &PROTOCOL));
    if (f.locate_status) return f.locate_status;
    *out = f.null_interface ? NULL : &f.proto;
    return 0;
}

static void reset(void)
{
    memset(&f, 0, sizeof f);
    f.proto = (struct el2q_rng_protocol){fake_get_info, fake_get_rng};
    f.state = 0x9e3779b97f4a7c15ull;
    f.fail = DEVICE_ERROR;
}

static void lists(const struct efi_guid *g, unsigned n)
{
    for (unsigned i = 0; i < n; i++) f.list[i] = g[i];
    f.list_count = n;
    f.info_size = n * sizeof *g;
}

/* Runs the probe and checks every line fits and none leaves a hole; returns
 * the line count. */
static unsigned run(struct el2q_rng *r, char lines[EL2Q_RNG_LINES + 1][EL2Q_RNG_LINE_COLS + 1])
{
    el2q_rng_probe(fake_locate, &f, r);
    unsigned n = 0;
    for (unsigned i = 0; i <= EL2Q_RNG_LINES; i++) {
        el2q_rng_line(r, i, lines[i]);
        CHECK(strlen(lines[i]) <= EL2Q_RNG_LINE_COLS);
        if (lines[i][0]) {
            CHECK(n == i);                     /* no empty line before a full one */
            n++;
        }
    }
    CHECK(n <= EL2Q_RNG_LINES);
    return n;
}

static int starts(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

static void test_absent(void)
{
    struct el2q_rng r;
    char l[EL2Q_RNG_LINES + 1][EL2Q_RNG_LINE_COLS + 1];

    memset(&r, 0, sizeof r);
    el2q_rng_line(&r, 0, l[0]);
    CHECK(r.state == EL2Q_RNG_NOT_ASKED && l[0][0] == 0);   /* option not given: no lines */

    reset();
    f.locate_status = NOT_FOUND;
    CHECK(run(&r, l) == 2);
    CHECK(r.state == EL2Q_RNG_NOT_FOUND && r.locate_status == NOT_FOUND);
    CHECK(!strcmp(l[0], "RNG PROTOCOL NOT FOUND, STATUS 800000000000000E"));
    CHECK(!strcmp(l[1], "RNG VERDICT: NO SOURCE"));
    CHECK(f.default_calls == 0 && f.raw_calls == 0);

    reset();
    f.null_interface = 1;
    CHECK(run(&r, l) == 2);
    CHECK(r.state == EL2Q_RNG_NOT_FOUND && r.null_interface);
    CHECK(!strcmp(l[0], "RNG PROTOCOL FOUND, NO INTERFACE"));
    CHECK(!strcmp(l[1], "RNG VERDICT: NO SOURCE"));

    el2q_rng_probe(NULL, &f, &r);
    CHECK(r.state == EL2Q_RNG_NOT_FOUND);
}

static void test_usable(void)
{
    struct el2q_rng r;
    char l[EL2Q_RNG_LINES + 1][EL2Q_RNG_LINE_COLS + 1];

    /* RAW listed and good: preferred, both read twice. */
    reset();
    lists((struct efi_guid[]){RAW, CTR}, 2);
    CHECK(run(&r, l) == 5);
    CHECK(r.state == EL2Q_RNG_USABLE_RAW && r.listed == 2);
    CHECK(f.default_calls == 2 && f.raw_calls == 2 && f.other_calls == 0);
    CHECK(!strcmp(l[0], "RNG PROTOCOL FOUND"));
    CHECK(!strcmp(l[1], "RNG ALGORITHMS 2: RAW CTR"));
    CHECK(starts(l[2], "RNG DEFAULT 256B OK NONZERO DIFFER DISTINCT "));
    CHECK(starts(l[3], "RNG RAW 256B OK NONZERO DIFFER DISTINCT "));
    CHECK(!strcmp(l[4], "RNG VERDICT: USABLE, RAW"));
    CHECK(r.def.distinct > 100 && r.def.distinct <= 256);   /* 256 random bytes: about 162 */

    /* No RAW: the default is used, and RAW is never asked for. */
    reset();
    lists((struct efi_guid[]){CTR}, 1);
    CHECK(run(&r, l) == 4);
    CHECK(r.state == EL2Q_RNG_USABLE_DEFAULT && !r.raw.asked && f.raw_calls == 0);
    CHECK(!strcmp(l[1], "RNG ALGORITHMS 1: CTR"));
    CHECK(!strcmp(l[3], "RNG VERDICT: USABLE, DEFAULT ALGORITHM"));

    /* RAW listed but all zero: it does not count, and the default carries. */
    reset();
    lists((struct efi_guid[]){RAW}, 1);
    f.raw = ZERO;
    CHECK(run(&r, l) == 5);
    CHECK(r.state == EL2Q_RNG_USABLE_DEFAULT);
    CHECK(!strcmp(l[3], "RNG RAW 256B OK ALL ZERO SAME TWICE DISTINCT 1"));

    /* Every name UEFI defines, and one it does not. */
    reset();
    lists((struct efi_guid[]){RAW, CTR, HASH, HMAC, AES, TDES, RNDR, UNKNOWN}, 8);
    f.raw = ZERO;
    f.def = ZERO;
    run(&r, l);
    CHECK(!strcmp(l[1], "RNG ALGORITHMS 8: RAW CTR HASH HMAC AES 3DES RNDR ?"));
}

static void test_unusable(void)
{
    struct el2q_rng r;
    char l[EL2Q_RNG_LINES + 1][EL2Q_RNG_LINE_COLS + 1];
    const struct { enum how how; efi_status fail; const char *line; } cases[] = {
        {REPEAT, 0, "RNG DEFAULT 256B OK NONZERO SAME TWICE DISTINCT "},
        {ZERO, 0, "RNG DEFAULT 256B OK ALL ZERO SAME TWICE DISTINCT 1"},
        {NO_WRITE, 0, "RNG DEFAULT 256B OK ALL ZERO SAME TWICE DISTINCT 1"},  /* success, nothing written */
        {FAIL_FIRST, DEVICE_ERROR, "RNG DEFAULT 256B FAILED, STATUS 8000000000000007"},
        {FAIL_SECOND, NOT_READY, "RNG DEFAULT 256B SECOND FAILED, STATUS 8000000000000006"},
        {WARN, 0, "RNG DEFAULT 256B FAILED, STATUS 0000000000000001"},    /* a warning is not success */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset();
        lists((struct efi_guid[]){CTR}, 1);
        f.def = cases[i].how;
        if (cases[i].fail) f.fail = cases[i].fail;
        CHECK(run(&r, l) == 4);
        CHECK(r.state == EL2Q_RNG_UNUSABLE);
        CHECK(starts(l[2], cases[i].line));
        CHECK(!strcmp(l[3], "RNG VERDICT: UNUSABLE"));
    }
}

static void test_get_info(void)
{
    struct el2q_rng r;
    char l[EL2Q_RNG_LINES + 1][EL2Q_RNG_LINE_COLS + 1];

    /* GetInfo unsupported: still read the default; RAW cannot be known. */
    reset();
    f.info_status = UNSUPPORTED;
    CHECK(run(&r, l) == 4);
    CHECK(!strcmp(l[1], "RNG ALGORITHMS NOT LISTED, STATUS 8000000000000003"));
    CHECK(r.state == EL2Q_RNG_USABLE_DEFAULT && f.raw_calls == 0);

    /* Too small: counted, not named, and RAW not trusted from an unfilled list
     * even if a careless driver wrote one there. */
    reset();
    lists((struct efi_guid[]){RAW}, 1);
    f.info_status = BUFFER_TOO_SMALL;
    f.info_size = 9 * sizeof(struct efi_guid);
    CHECK(run(&r, l) == 4);
    CHECK(r.listed == 9 && !r.raw.asked && f.raw_calls == 0);
    CHECK(!strcmp(l[1], "RNG ALGORITHMS 9, MORE THAN FIT, NOT NAMED"));

    /* A driver that claims success for more than fit: the first eight are
     * named, the rest counted. */
    reset();
    lists((struct efi_guid[]){CTR, CTR, CTR, CTR, CTR, CTR, CTR, CTR}, 8);
    f.info_size = 10 * sizeof(struct efi_guid);
    run(&r, l);
    CHECK(r.listed == 10);
    CHECK(!strcmp(l[1], "RNG ALGORITHMS 10: CTR CTR CTR CTR CTR CTR CTR CTR +2"));
}

unsigned test_rng(void)
{
    test_absent();
    test_usable();
    test_unusable();
    test_get_info();
    return checks;
}
