/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/report.c: the ACPI and FDT readers against
 * well-formed and corrupt tables, the PAuth and 16K verdicts against
 * consistent and deliberately inconsistent measurements, the report text and
 * summary lines, and the renderer's bounds. Built and run by
 * tools/test-el2qual.py.
 *
 * Register fixtures: "A16" uses the field values docs/FEASIBILITY.md records
 * as measured (MMFR0 verbatim; NV2, APA3/GPA3 and RNDR as fields). "gaokun3"
 * uses the values Windows cached at EL1 on a MateBook E Go (OBSERVED, under
 * Qualcomm's hypervisor, so EL2-only fields may be hidden); the measurement
 * fields around them are synthetic. */
#include "report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static void putbe(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }

/* ---- ACPI ------------------------------------------------------------------ */

static _Alignas(16) uint8_t rsdp[36], xsdt[36 + 8 * 4], madt[44 + 20 * 80 + 24], spcr[80], junk[36];

static void table(uint8_t *t, const char *sig, uint32_t len)
{
    memcpy(t, sig, 4);
    put32(t + 4, len);
}

static uint32_t gicc(uint8_t *t, uint32_t off, uint64_t mpidr, int enabled)
{
    t[off] = 0x0b;
    t[off + 1] = 80;
    put32(t + off + 12, enabled ? 1 : 0);
    put64(t + off + 68, mpidr);
    return off + 80;
}

static uint32_t build_madt(unsigned cpus)
{
    memset(madt, 0, sizeof madt);
    uint32_t off = 44;
    CHECK(44 + cpus * 80 + 24 <= sizeof madt);
    for (unsigned i = 0; i < cpus; i++)
        off = gicc(madt, off, 0x81000000ull | i << 8 | 0xff00000000ull, 1);
    madt[off] = 0x0c;                         /* GICD */
    madt[off + 1] = 24;
    madt[off + 20] = 3;
    off += 24;
    table(madt, "APIC", off);
    return off;
}

static void build_acpi(void)
{
    memset(rsdp, 0, sizeof rsdp);
    memcpy(rsdp, "RSD PTR ", 8);
    rsdp[15] = 2;
    put64(rsdp + 24, (uintptr_t)xsdt);
    memset(xsdt, 0, sizeof xsdt);
    table(xsdt, "XSDT", 36 + 8 * 4);
    put64(xsdt + 36, (uintptr_t)madt);
    put64(xsdt + 44, (uintptr_t)spcr);
    put64(xsdt + 52, 0);                     /* a null entry is skipped */
    put64(xsdt + 60, (uintptr_t)junk);
    table(junk, "JUNK", 8);                  /* shorter than a header: ignored */
    memset(spcr, 0, sizeof spcr);
    table(spcr, "SPCR", 80);
    spcr[36] = 0x03;
    spcr[40] = 0;
    put64(spcr + 44, 0x09000000);
}

static void test_acpi(void)
{
    struct el2q_acpi a;
    build_acpi();
    uint32_t len = build_madt(0);
    /* Two enabled CPUs, one disabled between them. */
    uint32_t off = 44;
    memset(madt, 0, sizeof madt);
    off = gicc(madt, off, 0x0000000000ull, 1);
    off = gicc(madt, off, 0x0000000100ull, 0);
    off = gicc(madt, off, 0x00ab0001ffull, 1);   /* bits 31:24 are not affinity */
    madt[off] = 0x0c; madt[off + 1] = 24; madt[off + 20] = 3; off += 24;
    table(madt, "APIC", off);
    CHECK(el2q_acpi_parse(rsdp, &a) == 0);
    CHECK(a.have_madt && a.have_spcr);
    CHECK(a.cpu_count == 2 && a.mpidr[0] == 0 && a.mpidr[1] == 0x00000001ffull);
    CHECK(a.gic_version == 3);
    CHECK(a.uart_type == 3 && a.uart_space == 0 && a.uart_base == 0x09000000);

    /* More CPUs than slots: capped, not overrun. */
    len = build_madt(20);
    (void)len;
    CHECK(el2q_acpi_parse(rsdp, &a) == 0 && a.cpu_count == EL2Q_MAX_CPUS);

    /* A zero-length MADT entry must stop the walk, not loop on it. */
    build_madt(2);
    madt[44 + 160 + 1] = 0;
    CHECK(el2q_acpi_parse(rsdp, &a) == 0 && a.cpu_count == 2 && a.gic_version == 0);
    /* An entry claiming to run past the table is not read. */
    build_madt(2);
    madt[44 + 1] = 200;
    CHECK(el2q_acpi_parse(rsdp, &a) == 0 && a.cpu_count == 0);

    /* Corrupt roots are refused outright and leave a zeroed result. */
    build_madt(2);
    rsdp[0] = 'X';
    CHECK(el2q_acpi_parse(rsdp, &a) == -1 && a.cpu_count == 0 && !a.have_madt);
    build_acpi();
    rsdp[15] = 0;                             /* ACPI 1.0: no XSDT */
    CHECK(el2q_acpi_parse(rsdp, &a) == -1);
    build_acpi();
    put32(xsdt + 4, 0x7fffffff);              /* absurd length */
    CHECK(el2q_acpi_parse(rsdp, &a) == -1);
    build_acpi();
    put32(madt + 4, 0x7fffffff);              /* absurd table length: skipped */
    CHECK(el2q_acpi_parse(rsdp, &a) == 0 && !a.have_madt && a.have_spcr);
    CHECK(el2q_acpi_parse(0, &a) == -1);
}

/* ---- FDT ------------------------------------------------------------------- */

struct fdt_builder {
    uint8_t st[2048], str[512], out[4096];
    uint32_t sl, strl;
};

static void fdt_token(struct fdt_builder *f, uint32_t t) { putbe(f->st + f->sl, t); f->sl += 4; }

static void fdt_begin(struct fdt_builder *f, const char *name)
{
    fdt_token(f, 1);
    size_t n = strlen(name) + 1;
    memcpy(f->st + f->sl, name, n);
    f->sl += (uint32_t)(n + 3) & ~3u;
}

static void fdt_prop(struct fdt_builder *f, const char *name, const void *v, uint32_t len)
{
    uint32_t nameoff = f->strl;
    size_t n = strlen(name) + 1;
    memcpy(f->str + f->strl, name, n);
    f->strl += (uint32_t)n;
    fdt_token(f, 3);
    fdt_token(f, len);
    fdt_token(f, nameoff);
    memcpy(f->st + f->sl, v, len);
    f->sl += (len + 3) & ~3u;
}

static uint32_t fdt_finish(struct fdt_builder *f)
{
    fdt_token(f, 9);
    memset(f->out, 0, sizeof f->out);
    uint32_t off_rsv = 40, off_struct = off_rsv + 16, off_strings = off_struct + f->sl;
    uint32_t total = off_strings + f->strl;
    putbe(f->out, 0xd00dfeed);
    putbe(f->out + 4, total);
    putbe(f->out + 8, off_struct);
    putbe(f->out + 12, off_strings);
    putbe(f->out + 16, off_rsv);
    putbe(f->out + 20, 17);
    putbe(f->out + 24, 16);
    putbe(f->out + 32, f->strl);
    putbe(f->out + 36, f->sl);
    memcpy(f->out + off_struct, f->st, f->sl);
    memcpy(f->out + off_strings, f->str, f->strl);
    return total;
}

static const char adreno_compat[] = "qcom,adreno-690.0\0qcom,adreno";

/* status: NULL for no property; zap: the zap node's name, NULL for none. */
static uint32_t gpu_tree(struct fdt_builder *f, const char *compat, uint32_t compat_len,
                         const char *zap, const char *status)
{
    memset(f, 0, sizeof *f);
    fdt_begin(f, "");
    fdt_begin(f, "soc@0");
    fdt_begin(f, "gpu@3d00000");
    fdt_prop(f, "compatible", compat, compat_len);
    if (zap) {
        fdt_begin(f, zap);
        if (status) fdt_prop(f, "status", status, (uint32_t)strlen(status) + 1);
        fdt_token(f, 2);
    }
    fdt_token(f, 2);
    fdt_token(f, 2);
    fdt_token(f, 2);
    return fdt_finish(f);
}

static void test_fdt(void)
{
    static struct fdt_builder f;
    uint32_t n;
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "disabled");
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_ZAP_DISABLED);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader@0", "disabled");
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_ZAP_DISABLED);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "okay");
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_ZAP_OKAY);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", NULL);
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_ZAP_OKAY);       /* default status */
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shaderx", "disabled");
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_NO_ZAP);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, NULL, NULL);
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_NO_ZAP);
    /* "qcom,adreno" must be a whole list entry, as libfdt matches it. */
    static const char prefix_only[] = "qcom,adreno-690.0";
    n = gpu_tree(&f, prefix_only, sizeof prefix_only, "zap-shader", "disabled");
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_NO_ZAP);

    /* A zap-shader that is a grandchild, not a child, does not count. */
    memset(&f, 0, sizeof f);
    fdt_begin(&f, "");
    fdt_begin(&f, "gpu");
    fdt_prop(&f, "compatible", adreno_compat, sizeof adreno_compat);
    fdt_begin(&f, "inner");
    fdt_begin(&f, "zap-shader");
    fdt_prop(&f, "status", "disabled", 9);
    fdt_token(&f, 2); fdt_token(&f, 2); fdt_token(&f, 2); fdt_token(&f, 2);
    n = fdt_finish(&f);
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_NO_ZAP);

    /* Corruption. */
    CHECK(el2q_fdt_zap_state(NULL, 0) == EL2Q_DT_NONE);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "disabled");
    CHECK(el2q_fdt_zap_state(f.out, n - 1) == EL2Q_DT_BAD);         /* totalsize > limit */
    CHECK(el2q_fdt_zap_state(f.out, 39) == EL2Q_DT_BAD);
    f.out[0] ^= 1;
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_BAD);             /* magic */
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "disabled");
    putbe(f.out + 36, 0x10000);                                       /* struct size */
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_BAD);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "disabled");
    /* The compatible property's length, pointing past the struct block. */
    uint32_t prop = 56 + 8 + 12 + 16;         /* struct block + root, soc@0, gpu nodes */
    CHECK((f.out[prop + 3]) == 3);
    putbe(f.out + prop + 4, 0x4000);
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_BAD);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "disabled");
    putbe(f.out + prop + 8, 0x4000);                                  /* name offset */
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_BAD);
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "disabled");
    putbe(f.out + prop, 7);                                           /* unknown token */
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_BAD);
    /* No FDT_END: the walk must stop at the struct block's end. (Without an
     * Adreno node, since closing one already decides the answer.) */
    static const char other[] = "vendor,other-gpu";
    n = gpu_tree(&f, other, sizeof other, NULL, NULL);
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_NO_ZAP);
    putbe(f.out + 36, (uint32_t)(f.sl - 4));
    CHECK(el2q_fdt_zap_state(f.out, n) == EL2Q_DT_BAD);
}

/* ---- Verdicts ----------------------------------------------------------------- */

static void set_id(struct el2q_cpu *c, unsigned crm, unsigned op2, uint64_t v)
{
    int i = el2q_id_index(crm, op2);
    CHECK(i >= 0);
    c->id[i] = v;
}

/* A CPU whose PAuth and 16K trials all came out the way a working part
 * answers. */
static void good_cpu(struct el2q_cpu *c)
{
    memset(c, 0, sizeof *c);
    c->current_el = 2;
    c->state = EL2Q_CPU_DONE;
    c->pac_ran = 1;
    for (unsigned i = 0; i < EL2Q_PAC_TRIALS; i++) {
        c->pac_in[i] = 0x40001000ull + i * 0x1000;
        c->pac_out[i] = c->pac_in[i] | 0x002a000000000000ull;
    }
    c->pac_rt_ran = 1;
    c->pac_rt[0] = c->pac_out[0];
    c->pac_rt[1] = c->pac_in[0];
    c->pac_rt[2] = c->pac_in[0];
    c->tgran[0] = 0x80000000;
    c->tgran[1] = 0x0000000000000f00ull;            /* A resolved */
    c->tgran[2] = 0x0000000000000809ull;            /* B faulted */
    c->tgran[3] = 0x0000000000000809ull;            /* C faulted */
    c->tgran[6] = 0x4001000;
    c->tgran[4] = 0xff00000004001f00ull;            /* D resolved to the expected PA */
}

static void test_verdicts(void)
{
    struct el2q_cpu c;
    good_cpu(&c);
    CHECK(el2q_pauth_signs(&c) == EL2Q_YES);
    CHECK(el2q_walks_16k(&c) == EL2Q_YES);

    good_cpu(&c); c.pac_ran = 0;
    CHECK(el2q_pauth_signs(&c) == EL2Q_NOT_RUN);           /* nopac */
    good_cpu(&c); c.current_el = 1;
    CHECK(el2q_pauth_signs(&c) == EL2Q_NOT_RUN && el2q_walks_16k(&c) == EL2Q_NOT_RUN);
    good_cpu(&c); c.state = EL2Q_CPU_FAULT;
    CHECK(el2q_pauth_signs(&c) == EL2Q_NOT_RUN && el2q_walks_16k(&c) == EL2Q_NOT_RUN);

    /* Absent: key access undefined, hints were NOPs, XPACI undefined. */
    good_cpu(&c);
    c.probe_esr[EL2Q_PROBE_APIAKEY] = 0x02000000;
    for (unsigned i = 0; i < EL2Q_PAC_TRIALS; i++) c.pac_out[i] = c.pac_in[i];
    c.pac_rt_esr = 0x02000000;
    CHECK(el2q_pauth_signs(&c) == EL2Q_NO);
    /* Faulty: authentication does not give the pointer back. */
    good_cpu(&c); c.pac_rt[1] ^= 1ull << 53;
    CHECK(el2q_pauth_signs(&c) == EL2Q_INCONCLUSIVE);
    /* Faulty: keys exist but nothing was signed (EnIA not in effect). */
    good_cpu(&c);
    for (unsigned i = 0; i < EL2Q_PAC_TRIALS; i++) c.pac_out[i] = c.pac_in[i];
    c.pac_rt[0] = c.pac_in[0];
    CHECK(el2q_pauth_signs(&c) == EL2Q_INCONCLUSIVE);
    /* Faulty: pointers changed but the key registers are undefined. */
    good_cpu(&c); c.probe_esr[EL2Q_PROBE_APIAKEY] = 0x02000000;
    CHECK(el2q_pauth_signs(&c) == EL2Q_INCONCLUSIVE);
    /* Faulty: XPACI trapped somewhere unexpected. */
    good_cpu(&c); c.pac_rt_esr = 0x5e000000;
    CHECK(el2q_pauth_signs(&c) == EL2Q_INCONCLUSIVE);

    good_cpu(&c); c.tgran[4] = 0x809;
    CHECK(el2q_walks_16k(&c) == EL2Q_NO);
    good_cpu(&c); c.tgran[1] = 0x809;
    CHECK(el2q_walks_16k(&c) == EL2Q_INCONCLUSIVE);       /* harness broken */
    good_cpu(&c); c.tgran[2] = 0xf00;
    CHECK(el2q_walks_16k(&c) == EL2Q_INCONCLUSIVE);       /* MMU not in effect */
    good_cpu(&c); c.tgran[3] = 0xf00;
    CHECK(el2q_walks_16k(&c) == EL2Q_INCONCLUSIVE);       /* no discrimination */
    good_cpu(&c); c.tgran[4] = 0x2001f00;
    CHECK(el2q_walks_16k(&c) == EL2Q_INCONCLUSIVE);       /* wrong PA */
    good_cpu(&c); c.tgran[6] = 0;
    CHECK(el2q_walks_16k(&c) == EL2Q_NOT_RUN);
}

/* ---- Report ------------------------------------------------------------------- */

static struct el2q_run run;
static struct el2q_text text;

static int has_line(const char *needle)
{
    for (unsigned i = 0; i < text.count; i++)
        if (strstr(text.line[i], needle)) return 1;
    return 0;
}

static void a16_cpu(struct el2q_cpu *c)
{
    good_cpu(c);
    set_id(c, 0, 0, 0x512f0021);
    set_id(c, 7, 0, 0x2100122100001024ull);           /* FEASIBILITY.md, verbatim */
    set_id(c, 7, 2, 2ull << 24);                      /* NV = 2 */
    set_id(c, 6, 2, 5ull << 12 | 1ull << 8);          /* APA3 = 5, GPA3 = 1 */
    set_id(c, 6, 0, 1ull << 60);                      /* RNDR */
    c->tgran[4] = 0x809;                              /* trial D faults (VERIFIED) */
}

static void gaokun3_cpu(struct el2q_cpu *c, uint64_t midr)
{
    good_cpu(c);
    set_id(c, 0, 0, midr);
    set_id(c, 4, 0, 0x1100000011111112ull);
    set_id(c, 6, 0, 0x0010100010211120ull);
    set_id(c, 6, 1, 0x0000000001200031ull);
    set_id(c, 7, 0, 0x0000011100101022ull);
    set_id(c, 7, 1, 0x0000000000201002ull);
    set_id(c, 7, 2, 0x0000000100001011ull);
    for (unsigned i = EL2Q_PROBE_RNDR; i <= EL2Q_PROBE_HFGRTR; i++) c->probe_esr[i] = 0x02000000;
}

static void base_run(void)
{
    memset(&run, 0, sizeof run);
    run.start_el = 1;
    run.end_el = 2;
    run.ebs_tries = 1;
    run.dt_state = EL2Q_DT_ZAP_DISABLED;
    run.acpi.cpu_count = 8;
    run.acpi.gic_version = 3;
    run.psci_version = 0x10001;
    run.fb_width = 1600;
    run.fb_height = 2560;
}

static void check_widths(void)
{
    CHECK(text.count <= EL2Q_TEXT_LINES);
    for (unsigned i = 0; i < text.count; i++) CHECK(strlen(text.line[i]) <= EL2Q_TEXT_COLS);
}

static void test_report(void)
{
    char line[EL2Q_TEXT_COLS + 1];

    base_run();
    run.start_el = 2;
    run.cpu_count = 1;
    a16_cpu(&run.cpu[0]);
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("ORYON"));
    CHECK(has_line("J700 ROUTE (AS ON A16) .. CPU OK"));
    CHECK(has_line("VIRTUALMAC ROUTE (EL1) .. BLOCKED: 16K"));
    CHECK(has_line("16K WALK (TRIAL D) ...... DOES NOT WALK"));
    CHECK(has_line("SLBOUNCE GATE: NOT NEEDED, STARTED AT EL2"));
    el2q_summary_line(&run, 0, line);
    CHECK(!strcmp(line, "SUM 0 EL2 DONE PAC=Y 16K=N NV2=2/OK ECV=2/OK FGT=1/OK RNDR=1/OK"));

    /* gaokun3 as Windows saw it, PAuth signing assumed, 16K walking. */
    base_run();
    run.cpu_count = 2;
    gaokun3_cpu(&run.cpu[0], 0x410fd4b0);
    gaokun3_cpu(&run.cpu[1], 0x410fd4c0);
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("CORTEX-A78C"));
    CHECK(has_line("PAUTH ADVERTISED ........ YES  APA 3 API 0 APA3 0 G 1"));
    CHECK(has_line("16K ADVERTISED .......... YES  TGRAN16 1 TGRAN16_2 1"));
    CHECK(has_line("J700 ROUTE (AS ON A16) .. BLOCKED: NV2 ECV RNDR"));
    CHECK(!has_line("BLOCKED: NV2 ECV RNDR PAUTH"));
    CHECK(has_line("VIRTUALMAC ROUTE (EL1) .. CPU OK"));
    CHECK(has_line("SLBOUNCE GATE: ZAP-SHADER DISABLED: OPEN"));
    CHECK(has_line("NV2 ..................... NO  FIELD 0 VNCR_EL2 UNDEF"));
    /* The two cores differ in MIDR only; that is the one difference shown. */
    CHECK(has_line("1  MIDR       00000000410FD4C0"));
    el2q_summary_line(&run, 1, line);
    CHECK(!strcmp(line, "SUM 1 EL2 DONE PAC=Y 16K=Y NV2=0/UD ECV=0/UD FGT=0/UD RNDR=0/UD"));

    /* PAuth skipped: the routes must not claim it is absent. */
    run.cpu[0].pac_ran = 0;
    el2q_report(&run, &text);
    CHECK(has_line("J700 ROUTE (AS ON A16) .. BLOCKED: NV2 ECV RNDR PAUTH?"));
    CHECK(has_line("VIRTUALMAC ROUTE (EL1) .. BLOCKED: PAUTH?"));
    CHECK(has_line("PAUTH SIGNS (PACIA1716) . NOT RUN"));
    /* Measured absent: no question mark. */
    run.cpu[0].pac_ran = 1;
    run.cpu[0].probe_esr[EL2Q_PROBE_APIAKEY] = 0x02000000;
    for (unsigned i = 0; i < EL2Q_PAC_TRIALS; i++) run.cpu[0].pac_out[i] = run.cpu[0].pac_in[i];
    run.cpu[0].pac_rt_esr = 0x02000000;
    el2q_report(&run, &text);
    CHECK(has_line("VIRTUALMAC ROUTE (EL1) .. BLOCKED: PAUTH"));
    CHECK(!has_line("PAUTH?"));

    /* Secondary failures are each named. */
    base_run();
    run.cpu_count = 4;
    gaokun3_cpu(&run.cpu[0], 0x410fd4b0);
    run.cpu[1].state = EL2Q_CPU_PSCI_FAILED;
    run.cpu[1].psci_on = -2;
    run.cpu[2].state = EL2Q_CPU_WRONG_EL;
    run.cpu[2].current_el = 1;
    run.cpu[2].early = 0xe12e000000000004ull;
    run.cpu[3].state = EL2Q_CPU_FAULT;
    run.cpu[3].current_el = 2;
    run.cpu[3].fault_vector = 5;
    run.cpu[3].fault_esr = 0x96000045;
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("PSCI FAIL"));
    CHECK(has_line("WRONG EL"));
    CHECK(has_line("ENTRY MARK E12E000000000004"));
    CHECK(has_line("V5 ESR 96000045"));
    el2q_summary_line(&run, 2, line);
    CHECK(!strcmp(line, "SUM 2 EL1 WRONG EL"));
    el2q_summary_line(&run, 9, line);
    CHECK(line[0] == 0);

    /* Never reached EL2: no verdicts, only the starting view. */
    base_run();
    run.end_el = 1;
    run.dt_state = EL2Q_DT_ZAP_OKAY;
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("NOT AT EL2: ONLY THE STARTING VIEW BELOW"));
    CHECK(has_line("SLBOUNCE GATE: ZAP-SHADER ENABLED: WILL NOT SWITCH"));
    CHECK(!has_line("J700 ROUTE"));

    /* Sixteen cores that all differ: the text is cut, visibly. */
    base_run();
    run.cpu_count = EL2Q_MAX_CPUS;
    for (unsigned s = 0; s < EL2Q_MAX_CPUS; s++) {
        gaokun3_cpu(&run.cpu[s], 0x410fd4b0);
        for (unsigned i = 0; i < EL2Q_ID_COUNT; i++) run.cpu[s].id[i] += s;
    }
    el2q_report(&run, &text);
    check_widths();
    CHECK(text.count == EL2Q_TEXT_LINES);
    CHECK(!strcmp(text.line[EL2Q_TEXT_LINES - 1], "... TRUNCATED"));

    el2q_raw_line(&run, EL2Q_MAX_CPUS, 0, line);
    CHECK(!strcmp(line, "RAW START MIDR 0000000000000000"));
    el2q_raw_line(&run, 1, 0, line);
    CHECK(!strcmp(line, "RAW 1 MIDR 00000000410FD4B1"));
    el2q_raw_line(&run, 1, EL2Q_ID_COUNT, line);
    CHECK(line[0] == 0);
}

/* ---- Rendering ----------------------------------------------------------------- */

static void test_render(void)
{
    enum { W = 200, H = 120, STRIDE = 232, GUARD = 0xdeadbeef };
    static uint32_t fb[(H + 2) * STRIDE];
    for (size_t i = 0; i < sizeof fb / 4; i++) fb[i] = GUARD;
    base_run();
    run.cpu_count = 1;
    gaokun3_cpu(&run.cpu[0], 0x410fd4b0);
    el2q_report(&run, &text);
    el2q_render(&text, fb, W, H, STRIDE, 1);
    unsigned lit = 0, white_bgr = 0;
    for (unsigned y = 0; y < H + 2; y++)
        for (unsigned x = 0; x < STRIDE; x++) {
            uint32_t p = fb[y * STRIDE + x];
            int inside = y < H && x < W;
            CHECK(inside ? p != GUARD : p == GUARD);     /* nothing outside the screen */
            lit += inside && p;
            white_bgr += p == 0xffffff;
        }
    CHECK(lit > 0 && white_bgr > 0);

    /* RGB order swaps red and blue; green stays. */
    struct el2q_text one = {.count = 1};
    one.color[0] = EL2Q_RED;
    strcpy(one.line[0], "#");
    static uint32_t small[60 * 40];
    el2q_render(&one, small, 60, 40, 60, 0);
    int red_rgb = 0;
    for (unsigned i = 0; i < 60 * 40; i++) red_rgb |= small[i] == 0x5050ff;
    CHECK(red_rgb);

    /* A screen too small for one cell must not be written past. */
    static uint32_t tiny[4 * 3];
    for (unsigned i = 0; i < 12; i++) tiny[i] = GUARD;
    el2q_render(&text, tiny, 2, 2, 4, 1);
    CHECK(tiny[2] == GUARD && tiny[3] == GUARD && tiny[8] == GUARD);
}

/* ---- Screen mode ------------------------------------------------------------ */

/* The gaokun3's 14 GOP modes and its current mode 7, as gaokun-android's
 * gk3probe recorded them (docs/hw/gk3probe-e3-20261005.txt): all BGRX, and
 * mode 7 the rotated twin that reports framebuffer address 0. */
static const struct el2q_gop_mode gaokun3_modes[14] = {
    {1600, 2560, 1, 1}, {480, 640, 1, 1},   {600, 800, 1, 1},   {720, 1280, 1, 1},
    {768, 1024, 1, 1},  {768, 1280, 1, 1},  {768, 1366, 1, 1},  {2560, 1600, 1, 1},
    {640, 480, 1, 1},   {800, 600, 1, 1},   {1280, 720, 1, 1},  {1024, 768, 1, 1},
    {1280, 768, 1, 1},  {1366, 768, 1, 1},
};

static void test_gop_order(void)
{
    unsigned order[EL2Q_GOP_MODES];
    static const unsigned want[] = {0, 6, 13, 5, 12, 3, 10, 4, 11, 2, 9, 1, 8};
    unsigned n = el2q_gop_order(gaokun3_modes, 14, 7, order);
    CHECK(n == sizeof want / sizeof want[0]);
    for (unsigned i = 0; i < n; i++) CHECK(order[i] == want[i]);   /* native mode 0 first */

    /* Formats other than RGBX/BGRX, failed queries and empty modes are never
     * tried; the next largest leads. */
    struct el2q_gop_mode m[14];
    memcpy(m, gaokun3_modes, sizeof m);
    m[0].format = 3;                                     /* PixelBltOnly */
    m[6].format = 2;                                     /* PixelBitMask */
    m[13].valid = 0;
    m[5].width = 0;
    n = el2q_gop_order(m, 14, 7, order);
    CHECK(n == 9 && order[0] == 12);
    for (unsigned i = 0; i < n; i++) CHECK(order[i] != 0 && order[i] != 6 && order[i] != 13 &&
                                           order[i] != 5 && order[i] != 7);

    /* Equal areas keep ascending mode numbers; a current mode outside the
     * list excludes nothing. */
    struct el2q_gop_mode same[5];
    for (unsigned i = 0; i < 5; i++) same[i] = (struct el2q_gop_mode){800, 600, 0, 1};
    n = el2q_gop_order(same, 5, 99, order);
    CHECK(n == 5);
    for (unsigned i = 0; i < n; i++) CHECK(order[i] == i);
    CHECK(el2q_gop_order(same, 5, 2, order) == 4 && order[2] == 3);

    /* More modes than the table holds: only the first EL2Q_GOP_MODES count. */
    struct el2q_gop_mode many[EL2Q_GOP_MODES + 8];
    for (unsigned i = 0; i < EL2Q_GOP_MODES + 8; i++)
        many[i] = (struct el2q_gop_mode){100 + i, 100, 1, 1};
    n = el2q_gop_order(many, EL2Q_GOP_MODES + 8, 0, order);
    CHECK(n == EL2Q_GOP_MODES - 1 && order[0] == EL2Q_GOP_MODES - 1);
    for (unsigned i = 0; i < n; i++) CHECK(order[i] < EL2Q_GOP_MODES);
    CHECK(el2q_gop_order(many, 0, 0, order) == 0);

    /* The report records the mode it is drawn on, and a switch. */
    base_run();
    run.fb_first_mode = 7;
    run.fb_mode = 0;
    run.fb_base = 0x80000000ull;
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("SCREEN GOP MODE 0  FB 0000000080000000  FROM MODE 7"));
    run.fb_first_mode = 0;
    el2q_report(&run, &text);
    CHECK(has_line("SCREEN GOP MODE 0  FB 0000000080000000") && !has_line("FROM MODE"));
    run.fb_base = 0;
    run.fb_mode = run.fb_first_mode = 7;
    el2q_report(&run, &text);
    CHECK(has_line("SCREEN GOP MODE 7  NO FRAMEBUFFER ADDRESS"));
}

int main(void)
{
    test_acpi();
    test_fdt();
    test_verdicts();
    test_report();
    test_gop_order();
    test_render();
    printf("test-el2qual: %u checks passed\n", checks);
    return 0;
}
