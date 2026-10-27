/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/s2bwrite.c, stage 2 of the S2CR bypass:
 * the first SMMU write, as a model. Linked into tools/test-el2qual.c's
 * binary by tools/test-el2qual.py; test_s2bwrite() returns its check count.
 *
 * The fake SMMU logs every load, store and barrier, and can drop, corrupt
 * or fault each store and each read, and stall the TLB sync. What matters
 * most: nothing is written unless every precondition holds; the writes are
 * exactly the five s2bwrite.h lists, in that order, each at most once, each
 * followed by a barrier before the read that checks it; once the bypass
 * store has been issued the restore always follows unless the bypass
 * plainly did not take; a failed restore stops further writes; the sync
 * poll is bounded even with a counter that never moves; and the register
 * ends at its old value whenever the restore read back. */
#include "s2bwrite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

unsigned test_s2bwrite(void);

#define SMMU     0x15000000ull
#define XHCI_OP  0x0A600020ull     /* USB0 core + CAPLENGTH 0x20 (run 16) */
#define S2CR_AT  0x15000C18ull
#define OLD      0x00000006u
#define PLANNED  0x00010006u
#define ESR_SEA  0x96000010ull
#define TLBIALLH    (SMMU + 0x6c)
#define TLBIALLNSNH (SMMU + 0x68)
#define TLBGSYNC    (SMMU + 0x70)
#define TLBGSTATUS  (SMMU + 0x74)
#define GFSR        (SMMU + 0x48)

enum { EV_READ = 1, EV_WRITE, EV_BARRIER };
#define MAXEV 64
static struct {
    struct { int kind; uint64_t addr; uint32_t val; } ev[MAXEV];
    unsigned nev, nwrites, nreads;
    uint32_t s2cr, usbcmd, usbsts, gfsr;
    unsigned store_no;                 /* stores to S2CR so far */
    int drop_store[3], fault_store[3], apply_then_fault[3];
    uint32_t corrupt_store[3];         /* nonzero: what store n leaves instead */
    int fault_s2cr_read[4];            /* by S2CR read number (0: the pre-check) */
    unsigned s2cr_reads;
    uint64_t fault_write_at, fault_read_at;
    unsigned active_polls;             /* GSACTIVE stays set this many polls */
    int stuck, frozen, serror, serror_after_writes;
    uint64_t ticks;
} f;

static void freset(void)
{
    memset(&f, 0, sizeof f);
    f.s2cr = OLD;
    f.usbsts = 1;                      /* HCH: halted */
    f.active_polls = 2;
}

static void log_ev(int kind, uint64_t a, uint32_t v)
{
    if (f.nev < MAXEV) {
        f.ev[f.nev].kind = kind;
        f.ev[f.nev].addr = a;
        f.ev[f.nev].val = v;
        f.nev++;
    }
}

static uint64_t fread32(void *ctx, uint64_t a, uint32_t *v)
{
    (void)ctx;
    f.nreads++;
    if (a != TLBGSTATUS) log_ev(EV_READ, a, 0);
    if (a == f.fault_read_at) return ESR_SEA;
    if (a == S2CR_AT) {
        unsigned n = f.s2cr_reads++;
        if (n < 4 && f.fault_s2cr_read[n]) return ESR_SEA;
        *v = f.s2cr;
    } else if (a == XHCI_OP) *v = f.usbcmd;
    else if (a == XHCI_OP + 4) *v = f.usbsts;
    else if (a == GFSR) *v = f.gfsr;
    else if (a == TLBGSTATUS) {
        if (f.stuck || f.active_polls) {
            if (f.active_polls) f.active_polls--;
            *v = 1;
        } else *v = 0;
    } else *v = 0;
    return 0;
}

static uint64_t fwrite32(void *ctx, uint64_t a, uint32_t v)
{
    (void)ctx;
    f.nwrites++;
    log_ev(EV_WRITE, a, v);
    if (f.nwrites >= (unsigned)f.serror_after_writes && f.serror_after_writes) f.serror = 1;
    if (a == f.fault_write_at) return ESR_SEA;
    if (a == S2CR_AT) {
        unsigned n = f.store_no++;
        if (n < 3 && f.fault_store[n]) return ESR_SEA;
        if (n < 3 && f.apply_then_fault[n]) { f.s2cr = v; return ESR_SEA; }
        if (n < 3 && f.drop_store[n]) return 0;
        f.s2cr = n < 3 && f.corrupt_store[n] ? f.corrupt_store[n] : v;
    }
    return 0;
}

static void fbarrier(void *ctx) { (void)ctx; log_ev(EV_BARRIER, 0, 0); }
static int fserror(void *ctx) { (void)ctx; return f.serror; }
static uint64_t fnow(void *ctx) { (void)ctx; return f.frozen ? f.ticks : f.ticks++; }

static const struct el2q_s2bw_hw hw = {0, fread32, fwrite32, fbarrier, fserror, fnow, 1000};

/* Stage 1's PLANNED result for run 25's SMMU. */
static struct el2q_s2b_result plan25(void)
{
    return (struct el2q_s2b_result){
        .state = EL2Q_S2B_PLANNED, .at = S2CR_AT, .old = OLD, .planned = PLANNED,
        .restore = OLD, .group = 6, .scr0 = 0x00240406u, .matches = 1, .scanned = 212,
    };
}

static void line(const struct el2q_s2bw_result *r, unsigned i, const char *want)
{
    char out[EL2Q_S2BW_LINE_COLS + 1];
    el2q_s2bw_line(r, i, out);
    if (strcmp(out, want)) fprintf(stderr, "line %u: got \"%s\", want \"%s\"\n", i, out, want);
    CHECK(!strcmp(out, want));
}

/* The writes, in order, are a prefix of the five s2bwrite.h lists, each
 * followed directly by a barrier; returns how many there were. */
static unsigned check_writes(void)
{
    const uint64_t at[] = {S2CR_AT, S2CR_AT, TLBIALLH, TLBIALLNSNH, TLBGSYNC};
    const uint32_t val[] = {PLANNED, OLD, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    unsigned n = 0;
    for (unsigned i = 0; i < f.nev; i++) {
        if (f.ev[i].kind != EV_WRITE) continue;
        CHECK(n < 5);
        CHECK(f.ev[i].addr == at[n] && f.ev[i].val == val[n]);
        CHECK(i + 1 < f.nev && f.ev[i + 1].kind == EV_BARRIER);
        n++;
    }
    CHECK(n == f.nwrites);
    return n;
}

static void test_done(void)
{
    struct el2q_s2bw_result r;
    struct el2q_s2b_result p = plan25();
    freset();
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_DONE);
    CHECK(check_writes() == 5 && r.writes == 5);
    CHECK(f.s2cr == OLD);                                   /* ends as it began */
    CHECK(r.readback == PLANNED && r.restore_readback == OLD && r.synced == 1 && r.polls == 3);
    /* Each S2CR store is read back right after its barrier. */
    for (unsigned i = 0; i + 2 < f.nev; i++)
        if (f.ev[i].kind == EV_WRITE && f.ev[i].addr == S2CR_AT)
            CHECK(f.ev[i + 1].kind == EV_BARRIER && f.ev[i + 2].kind == EV_READ &&
                  f.ev[i + 2].addr == S2CR_AT);
    /* USB0's halt was read before the first store. */
    unsigned first_write = 0;
    while (f.ev[first_write].kind != EV_WRITE) first_write++;
    int saw_cmd = 0, saw_sts = 0;
    for (unsigned i = 0; i < first_write; i++) {
        saw_cmd |= f.ev[i].kind == EV_READ && f.ev[i].addr == XHCI_OP;
        saw_sts |= f.ev[i].kind == EV_READ && f.ev[i].addr == XHCI_OP + 4;
    }
    CHECK(saw_cmd && saw_sts);
    line(&r, 0, "SMMU WRITE S2CR06 00000006 -> 00010006 READ BACK 00010006");
    line(&r, 1, "SMMU WRITE RESTORE 00000006 READ BACK 00000006");
    line(&r, 2, "SMMU WRITE TLBI SYNC OK POLLS 3 GFSR 00000000");
    line(&r, 3, "SMMU WRITE DONE: TOOK AND UNDONE, WRITES 5");
    line(&r, 4, "");
}

static void test_not_taken(void)
{
    struct el2q_s2bw_result r;
    struct el2q_s2b_result p = plan25();
    /* Dropped: the read-back shows the old word, so nothing else is
     * written, not even the restore. */
    freset();
    f.drop_store[0] = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_DROPPED);
    CHECK(check_writes() == 1 && !r.restored && !r.synced && f.s2cr == OLD);
    line(&r, 0, "SMMU WRITE S2CR06 00000006 -> 00010006 READ BACK 00000006");
    line(&r, 1, "SMMU WRITE DROPPED: NOT TAKEN, WRITES 1");
    line(&r, 2, "");

    /* Neither word: restored, invalidated, reported. */
    freset();
    f.corrupt_store[0] = 0x00020006u;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_UNEXPECTED);
    CHECK(check_writes() == 5 && f.s2cr == OLD && r.synced == 1);
    line(&r, 0, "SMMU WRITE S2CR06 00000006 -> 00010006 READ BACK 00020006");
    line(&r, 3, "SMMU WRITE UNEXPECTED READ BACK, RESTORED, WRITES 5");
}

static void test_faults(void)
{
    struct el2q_s2bw_result r;
    struct el2q_s2b_result p = plan25();
    /* The bypass store faults having taken effect: the restore still
     * follows, and the register ends at its old value. */
    freset();
    f.apply_then_fault[0] = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_WRITE_FAULT);
    CHECK(check_writes() == 5 && r.restored && f.s2cr == OLD && r.fault_step == 1);
    line(&r, 0, "SMMU WRITE S2CR06 00000006 -> 00010006 READ BACK NOT DONE");
    line(&r, 3, "SMMU WRITE FAULT ESR 96000010, RESTORED, WRITES 5");
    /* ... or faults without effect: the same, harmlessly. */
    freset();
    f.fault_store[0] = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_WRITE_FAULT);
    CHECK(check_writes() == 5 && f.s2cr == OLD);
    /* The read-back faults. */
    freset();
    f.fault_s2cr_read[1] = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_WRITE_FAULT);
    CHECK(check_writes() == 5 && f.s2cr == OLD && r.fault_step == 2);
    line(&r, 0, "SMMU WRITE S2CR06 00000006 -> 00010006 READ BACK FAULT");

    /* The restore does not take: no more writes, and the outcome says what
     * recovers it. */
    freset();
    f.drop_store[1] = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_RESTORE_FAILED);
    CHECK(check_writes() == 2 && !r.synced && f.s2cr == PLANNED);
    line(&r, 1, "SMMU WRITE RESTORE 00000006 READ BACK 00010006");
    line(&r, 2, "SMMU WRITE RESTORE FAILED: POWER CYCLE, WRITES 2");
    line(&r, 3, "");
    freset();
    f.fault_store[1] = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_RESTORE_FAILED);
    CHECK(check_writes() == 2 && r.fault_step == 3);
    line(&r, 1, "SMMU WRITE RESTORE 00000006 READ BACK NOT DONE");
    freset();
    f.fault_s2cr_read[2] = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_RESTORE_FAILED);
    CHECK(check_writes() == 2 && r.fault_step == 4);
    line(&r, 1, "SMMU WRITE RESTORE 00000006 READ BACK FAULT");
    /* A restore that reads back as something else entirely. */
    freset();
    f.corrupt_store[1] = 0x12345678u;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_RESTORE_FAILED);
    CHECK(check_writes() == 2);
}

static void test_sync(void)
{
    struct el2q_s2bw_result r;
    struct el2q_s2b_result p = plan25();
    /* Stuck: bounded by the counter. */
    freset();
    f.stuck = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_SYNC_TIMEOUT);
    CHECK(r.synced == 2 && r.polls > 1 && r.polls <= 1001 && f.s2cr == OLD && check_writes() == 5);
    line(&r, 3, "SMMU WRITE RESTORED, SYNC INCOMPLETE, WRITES 5");
    /* Stuck with a counter that never moves: bounded by the poll limit. */
    freset();
    f.stuck = 1;
    f.frozen = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_SYNC_TIMEOUT);
    CHECK(r.synced == 2 && r.polls == (1u << 20));
    line(&r, 2, "SMMU WRITE TLBI SYNC TIMEOUT POLLS 1048576 GFSR 00000000");
    /* An invalidate store, or the status read, faulting. */
    freset();
    f.fault_write_at = TLBIALLNSNH;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_SYNC_TIMEOUT);
    CHECK(r.synced == 3 && check_writes() == 4 && f.s2cr == OLD);
    line(&r, 2, "SMMU WRITE TLBI SYNC FAULT POLLS 0 GFSR 00000000");
    freset();
    f.fault_read_at = TLBGSTATUS;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_SYNC_TIMEOUT && r.synced == 3);

    /* A global fault, or an unreadable sGFSR, after a complete sequence. */
    freset();
    f.gfsr = 0x80000002u;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_GFAULT && f.s2cr == OLD);
    line(&r, 2, "SMMU WRITE TLBI SYNC OK POLLS 3 GFSR 80000002");
    line(&r, 3, "SMMU WRITE RESTORED, GLOBAL FAULT, WRITES 5");
    freset();
    f.fault_read_at = GFSR;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_GFAULT && r.gfsr == 0xffffffffu);
    /* An SError raised during the writes. */
    freset();
    f.serror_after_writes = 1;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &r) == EL2Q_S2BW_SERROR && f.s2cr == OLD);
    line(&r, 3, "SMMU WRITE RESTORED, SERROR PENDING, WRITES 5");
}

/* Refused before any write: no store at all. */
static void refused(const struct el2q_s2b_result *p, uint64_t smmu, uint32_t state, const char *want)
{
    struct el2q_s2bw_result r;
    unsigned before = f.nwrites;
    CHECK(el2q_s2bw_run(p, smmu, XHCI_OP, &hw, &r) == state);
    CHECK(f.nwrites == before && !r.wrote && r.writes == 0 && f.s2cr == OLD);
    line(&r, 0, want);
    line(&r, 1, "SMMU WRITE NOTHING WRITTEN");
    line(&r, 2, "");
}

static void test_refusals(void)
{
    struct el2q_s2b_result p;
    const char *plan = "SMMU WRITE REFUSED: PLAN";
    freset();
    refused(0, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25(); p.state = EL2Q_S2B_REFUSED_SHARED;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25(); p.at += 4;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25(); p.planned = 0x00020006u;                  /* fault, not bypass */
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25(); p.planned = 0x00010000u;                  /* other bits not kept */
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25(); p.old = p.restore = 0x00010006u; p.planned = 0x00010006u;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25(); p.restore = 0;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25(); p.group = 255; p.at = SMMU + 0xc00 + 4 * 255;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PLAN, plan);
    p = plan25();
    refused(&p, SMMU + 0x100000, EL2Q_S2BW_REFUSED_PLAN, plan);  /* another SMMU */
    const struct el2q_s2bw_hw none = {0, fread32, 0, fbarrier, fserror, fnow, 1000};
    struct el2q_s2bw_result r;
    CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &none, &r) == EL2Q_S2BW_REFUSED_PLAN && f.nwrites == 0);

    p = plan25();
    f.serror = 1;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_PENDING, "SMMU WRITE REFUSED: SERROR ALREADY PENDING");
    f.serror = 0;
    /* USB0 running, or not yet halted, or unreadable. */
    f.usbcmd = 1;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_BUSY,
            "SMMU WRITE REFUSED: USB0 NOT HALTED CMD 00000001 STS 00000001");
    f.usbcmd = 0;
    f.usbsts = 0;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_BUSY,
            "SMMU WRITE REFUSED: USB0 NOT HALTED CMD 00000000 STS 00000000");
    f.usbsts = 1;
    f.fault_read_at = XHCI_OP + 4;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_BUSY, "SMMU WRITE REFUSED: USB0 READ ESR 96000010");
    f.fault_read_at = 0;
    /* S2CR moved since stage 1, or unreadable. */
    f.s2cr = PLANNED;
    {
        struct el2q_s2bw_result m;
        CHECK(el2q_s2bw_run(&p, SMMU, XHCI_OP, &hw, &m) == EL2Q_S2BW_REFUSED_MOVED);
        CHECK(!m.wrote && f.nwrites == 0);
        line(&m, 0, "SMMU WRITE REFUSED: MOVED");
    }
    f.s2cr = OLD;
    f.s2cr_reads = 0;
    f.fault_s2cr_read[0] = 1;
    refused(&p, SMMU, EL2Q_S2BW_REFUSED_MOVED, "SMMU WRITE REFUSED: MOVED");

    struct el2q_s2bw_result q = {0};
    q.state = EL2Q_S2BW_NOT_ASKED;
    line(&q, 0, "");
}

unsigned test_s2bwrite(void)
{
    test_done();
    test_not_taken();
    test_faults();
    test_sync();
    test_refusals();
    return checks;
}
