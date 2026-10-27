/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/report.c and firmware.c: the ACPI and FDT
 * readers against well-formed and corrupt tables, the PAuth and 16K verdicts
 * against consistent and deliberately inconsistent measurements, the report
 * text and summary lines, and the renderer's bounds. Built and run by
 * tools/test-el2qual.py.
 *
 * Register fixtures: "A16" uses the field values docs/FEASIBILITY.md records
 * as measured (MMFR0 verbatim; NV2, APA3/GPA3 and RNDR as fields). "gaokun3"
 * uses the values Windows cached at EL1 on a MateBook E Go (OBSERVED, under
 * Qualcomm's hypervisor, so EL2-only fields may be hidden); the measurement
 * fields around them are synthetic. */
#include "report.h"
#include "thermal.h"
#include "../compat/sc8280xp-thermal.h"
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
    CHECK(has_line("PAUTH KEYS, ROUND TRIP .. OK, OK AUTH OK"));
    CHECK(has_line("PACGA ................... OK"));
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
    /* Nor may the probes it skipped read as passed: their fault codes are
     * still zero, which alone would print OK (gaokun3 runs 6, 8 and 9). */
    CHECK(has_line("PAUTH KEYS, ROUND TRIP .. NOT RUN"));
    CHECK(!has_line("AUTH OK"));
    CHECK(has_line("PACGA ................... NOT RUN"));
    CHECK(!has_line("PACGA ................... OK"));
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

/* ---- Device-tree framebuffer ------------------------------------------------ */

/* One framebuffer node under `parent` ("chosen" unless given). ac/sc 0 leave
 * #address-cells/#size-cells out (the spec's defaults, 2 and 1, then apply);
 * NULL strings and the omit bits (1 width, 2 height, 4 stride, 8 reg) leave
 * properties out. */
struct sfb_spec {
    const char *parent, *node, *status, *format;
    const char *compat;
    uint32_t compat_len, ac, sc, width, height, stride, omit;
    uint64_t base, size;
};

static void cell_prop(struct fdt_builder *f, const char *name, uint32_t v)
{
    uint8_t b[4];
    putbe(b, v);
    fdt_prop(f, name, b, 4);
}

static void sfb_node(struct fdt_builder *f, const struct sfb_spec *s)
{
    uint32_t ac = s->ac ? s->ac : 2, sc = s->sc ? s->sc : 1;
    fdt_begin(f, s->node ? s->node : "framebuffer@c6200000");
    if (s->compat) fdt_prop(f, "compatible", s->compat, s->compat_len);
    if (s->status) fdt_prop(f, "status", s->status, (uint32_t)strlen(s->status) + 1);
    if (!(s->omit & 8)) {
        uint8_t reg[16];
        unsigned n = 0;
        for (unsigned i = ac; i--;) putbe(reg + 4 * n++, i < 2 ? (uint32_t)(s->base >> 32 * i) : 0);
        for (unsigned i = sc; i--;) putbe(reg + 4 * n++, i < 2 ? (uint32_t)(s->size >> 32 * i) : 0);
        fdt_prop(f, "reg", reg, 4 * n);
    }
    if (!(s->omit & 1)) cell_prop(f, "width", s->width);
    if (!(s->omit & 2)) cell_prop(f, "height", s->height);
    if (!(s->omit & 4)) cell_prop(f, "stride", s->stride);
    if (s->format) fdt_prop(f, "format", s->format, (uint32_t)strlen(s->format) + 1);
    fdt_token(f, 2);
}

static uint32_t sfb_tree(struct fdt_builder *f, const struct sfb_spec *s, const struct sfb_spec *second)
{
    memset(f, 0, sizeof *f);
    fdt_begin(f, "");
    fdt_begin(f, s->parent ? s->parent : "chosen");
    if (s->ac) cell_prop(f, "#address-cells", s->ac);
    if (s->sc) cell_prop(f, "#size-cells", s->sc);
    sfb_node(f, s);
    if (second) sfb_node(f, second);
    fdt_token(f, 2);
    fdt_token(f, 2);
    return fdt_finish(f);
}

static const char sfb_compat[] = "simple-framebuffer";

/* The gaokun3's node, as its EL2 device tree has it. */
static struct sfb_spec gaokun3_fb(void)
{
    return (struct sfb_spec){.compat = sfb_compat, .compat_len = sizeof sfb_compat, .ac = 2,
                             .sc = 2, .width = 1600, .height = 2560, .stride = 6400,
                             .format = "a8r8g8b8", .base = 0xc6200000, .size = 0xfa0000};
}

static void test_simplefb(void)
{
    static struct fdt_builder f;
    struct el2q_simplefb fb;
    struct sfb_spec s = gaokun3_fb();
    uint32_t n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK);
    CHECK(fb.base == 0xc6200000 && fb.size == 0xfa0000 && fb.width == 1600 &&
          fb.height == 2560 && fb.stride == 6400 && fb.bgr == 1);

    s.format = "x8b8g8r8";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK && fb.bgr == 0);
    s.format = "r5g6b5";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_FORMAT);
    s.format = NULL;
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_FORMAT);

    /* Geometry the binding allows but a drawer cannot trust. */
    struct { uint64_t base, size; uint32_t width, height, stride, omit; } bad[] = {
        {0xc6200000, 0xfa0000 - 4, 1600, 2560, 6400, 0},      /* stride * height > size */
        {0xc6200000, 0xfa0000, 1600, 2560, 6396, 0},          /* stride < width * 4 */
        {0xc6200000, 0xfa0000, 1600, 2560, 6402, 0},          /* stride not whole pixels */
        {0xc6200002, 0xfa0000, 1600, 2560, 6400, 0},          /* unaligned base */
        {0, 0xfa0000, 1600, 2560, 6400, 0},                   /* no address */
        {0xc6200000, 0xfa0000, 0, 2560, 6400, 0},             /* empty */
        {0xc6200000, 0xfa0000, 20000, 1, 80000, 0},           /* absurd width */
        {0xfffffffffffff000ull, 0x2000, 1, 1, 4, 0},          /* wraps */
        {0xc6200000, 0xfa0000, 1600, 2560, 6400, 2},          /* no height */
        {0xc6200000, 0xfa0000, 1600, 2560, 6400, 4},          /* no stride */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        s = gaokun3_fb();
        s.base = bad[i].base;
        s.size = bad[i].size;
        s.width = bad[i].width;
        s.height = bad[i].height;
        s.stride = bad[i].stride;
        s.omit = bad[i].omit;
        n = sfb_tree(&f, &s, NULL);
        CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_GEOMETRY);
    }

    /* Status, compatible matching and where the node sits. */
    s = gaokun3_fb();
    s.status = "disabled";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_NO_NODE);
    s.status = "okay";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK);
    s.status = "ok";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK);
    static const char longer[] = "simple-framebuffer-x", listed[] = "vendor,fb\0simple-framebuffer";
    s = gaokun3_fb();
    s.compat = longer;
    s.compat_len = sizeof longer;
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_NO_NODE);
    s.compat = listed;
    s.compat_len = sizeof listed;
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK);
    s = gaokun3_fb();
    s.parent = "soc@0";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_NO_NODE);
    s.parent = "chosen-not";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_NO_NODE);

    /* Cell sizes: the spec's defaults when /chosen gives none; unsupported
     * or short reg is unreadable rather than guessed at. */
    s = gaokun3_fb();
    s.ac = s.sc = 0;
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK && fb.base == 0xc6200000 &&
          fb.size == 0xfa0000);
    s.ac = 1;
    s.sc = 1;
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK && fb.base == 0xc6200000);
    s.ac = 3;
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_BAD_DT);
    s = gaokun3_fb();
    s.omit = 8;
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_BAD_DT);

    /* A failing node does not hide a good one after it; with none good,
     * the failure is reported. */
    struct sfb_spec broken = gaokun3_fb(), good = gaokun3_fb();
    broken.node = "framebuffer@0";
    broken.stride = 4;
    good.base = 0xc7200000;
    n = sfb_tree(&f, &broken, &good);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_OK && fb.base == 0xc7200000);
    n = sfb_tree(&f, &broken, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_GEOMETRY);

    /* A tree without /chosen, a missing one, a corrupt one. */
    n = gpu_tree(&f, adreno_compat, sizeof adreno_compat, "zap-shader", "disabled");
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_NO_NODE);
    CHECK(el2q_fdt_simplefb(NULL, 0, &fb) == EL2Q_SFB_NO_DT);
    s = gaokun3_fb();
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n - 1, &fb) == EL2Q_SFB_BAD_DT);
    f.out[0] ^= 1;
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_BAD_DT);
    /* No FDT_END: the walk must stop at the struct block's end. (Without
     * /chosen, since closing it already decides the answer.) */
    s.parent = "soc@0";
    n = sfb_tree(&f, &s, NULL);
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_NO_NODE);
    putbe(f.out + 36, (uint32_t)(f.sl - 4));
    CHECK(el2q_fdt_simplefb(f.out, n, &fb) == EL2Q_SFB_BAD_DT);

    for (uint32_t i = 0; i <= EL2Q_SFB_OK; i++)
        CHECK(strcmp(el2q_sfb_words(i), "?") && strlen(el2q_sfb_words(i)) <= EL2Q_TEXT_COLS - 16);
    CHECK(!strcmp(el2q_sfb_words(EL2Q_SFB_OK + 1), "?"));
}

static void descriptor(uint8_t *d, uint32_t type, uint64_t start, uint64_t pages)
{
    memset(d, 0, 48);
    put32(d, type);
    put64(d + 8, start);
    put64(d + 24, pages);
}

static void test_memmap(void)
{
    /* The gaokun3's map around the framebuffer, 48-byte descriptors as its
     * firmware reports them: Conventional, the 36 MiB Reserved region, and
     * a Reserved neighbour that adjoins it. */
    uint8_t map[4 * 48];
    descriptor(map, 7, 0xc8600000, 0x10000);
    descriptor(map + 48, 0, 0xc6200000, 9216);
    descriptor(map + 96, 0, 0xc4200000, 0x2000);
    descriptor(map + 144, 0, 0, UINT64_MAX >> 8);                    /* overflowing pages */
    CHECK(el2q_memmap_reserved(map, sizeof map, 48, 0xc6200000, 0xfa0000));
    CHECK(el2q_memmap_reserved(map, sizeof map, 48, 0xc8500000, 0x100000));   /* ends at the edge */
    CHECK(!el2q_memmap_reserved(map, sizeof map, 48, 0xc8500000, 0x100001));  /* one byte past */
    CHECK(!el2q_memmap_reserved(map, sizeof map, 48, 0xc6100000, 0x200000));  /* spans two */
    CHECK(!el2q_memmap_reserved(map, sizeof map, 48, 0xc8600000, 0x1000));    /* Conventional */
    CHECK(!el2q_memmap_reserved(map, sizeof map, 48, 0xc6200000, 0));
    CHECK(!el2q_memmap_reserved(map, sizeof map, 48, UINT64_MAX - 0xfff, 0x2000));
    CHECK(!el2q_memmap_reserved(map, sizeof map, 32, 0xc6200000, 0xfa0000));  /* short desc */
    CHECK(!el2q_memmap_reserved(map, 48 + 47, 48, 0xc6200000, 0xfa0000));    /* truncated */
    CHECK(!el2q_memmap_reserved(NULL, sizeof map, 48, 0xc6200000, 0xfa0000));
    CHECK(el2q_memmap_reserved(map + 48, 48, 48, 0xc6200000, 0xfa0000));

    /* The report says where it is drawn. */
    base_run();
    run.fb_source = EL2Q_FB_DT;
    run.fb_base = 0xc6200000;
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("SCREEN DT SIMPLE-FB  FB 00000000C6200000  NO GOP ADDRESS"));
    CHECK(!has_line("SCREEN GOP MODE"));
}

/* ---- SMBIOS -------------------------------------------------------------------- */

/* A serial number no report may ever carry. */
#define SERIAL "SN-SECRET-0123"

static _Alignas(16) uint8_t smbios_entry[32], smbios_table[512];

static int contains(const void *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp((const uint8_t *)hay + i, needle, k)) return 1;
    return 0;
}

/* The byte that makes the n bytes at p sum to zero, with p's own slot 0. */
static uint8_t zero_sum(const uint8_t *p, unsigned n)
{
    uint8_t s = 0;
    while (n--) s = (uint8_t)(s + *p++);
    return (uint8_t)-s;
}

/* One structure at `at`: a formatted area of `length` bytes with string
 * numbers set by fields[] as (offset, number), then the strings, then the
 * terminating NUL (two when there are none). Returns the end. */
static unsigned put_structure(unsigned at, uint8_t type, uint8_t length,
                              const uint8_t (*fields)[2], unsigned nfields,
                              const char *const *strings, unsigned nstrings)
{
    uint8_t *t = smbios_table;
    memset(t + at, 0, length);
    t[at] = type;
    t[at + 1] = length;
    t[at + 2] = (uint8_t)at;                                   /* handle */
    for (unsigned i = 0; i < nfields; i++) t[at + fields[i][0]] = fields[i][1];
    unsigned p = at + length;
    for (unsigned i = 0; i < nstrings; i++) {
        size_t n = strlen(strings[i]) + 1;
        memcpy(t + p, strings[i], n);
        p += (unsigned)n;
    }
    if (!nstrings) t[p++] = 0;
    t[p++] = 0;
    return p;
}

static const uint8_t bios_fields[][2] = {{4, 1}, {5, 2}, {8, 3}};
static const uint8_t system_fields[][2] = {{4, 1}, {5, 2}, {6, 3}, {7, 4}};

/* Type 0, type 1 with a serial number, type 127; as a firmware lays it out. */
static unsigned build_smbios_table(void)
{
    static const char *const bios[] = {"Test Vendor", "2.10 (gk3)", "05/06/2024"};
    static const char *const system[] = {"Maker", "Product X", "1.0", SERIAL};
    memset(smbios_table, 0xee, sizeof smbios_table);
    unsigned at = put_structure(0, 0, 0x1a, bios_fields, 3, bios, 3);
    at = put_structure(at, 1, 0x1b, system_fields, 4, system, 4);
    return put_structure(at, 127, 4, NULL, 0, NULL, 0);
}

static void seal_smbios3(void)
{
    smbios_entry[5] = 0;
    smbios_entry[5] = zero_sum(smbios_entry, smbios_entry[6]);
}

static void build_smbios3(uint32_t size, uint64_t address)
{
    memset(smbios_entry, 0, sizeof smbios_entry);
    memcpy(smbios_entry, "_SM3_", 5);
    smbios_entry[6] = 0x18;
    smbios_entry[7] = 3;
    smbios_entry[8] = 3;
    smbios_entry[10] = 1;
    put32(smbios_entry + 0x0c, size);
    put64(smbios_entry + 0x10, address);
    seal_smbios3();
}

static void seal_smbios2(void)
{
    smbios_entry[0x15] = 0;
    smbios_entry[0x15] = zero_sum(smbios_entry + 0x10, 0x0f);
    smbios_entry[4] = 0;
    smbios_entry[4] = zero_sum(smbios_entry, 0x1f);
}

static void build_smbios2(uint16_t length, uint32_t address)
{
    memset(smbios_entry, 0, sizeof smbios_entry);
    memcpy(smbios_entry, "_SM_", 4);
    smbios_entry[5] = 0x1f;
    smbios_entry[6] = 2;
    smbios_entry[7] = 8;
    memcpy(smbios_entry + 0x10, "_DMI_", 5);
    smbios_entry[0x16] = (uint8_t)length;
    smbios_entry[0x17] = (uint8_t)(length >> 8);
    put32(smbios_entry + 0x18, address);
    seal_smbios2();
}

static void test_smbios(void)
{
    struct el2q_firmware fw;
    uint64_t table = (uint64_t)(uintptr_t)smbios_table;
    unsigned size = build_smbios_table();

    build_smbios3(size, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_OK);
    CHECK(fw.state == EL2Q_SMBIOS_OK && fw.major == 3 && fw.minor == 3);
    CHECK(!strcmp(fw.vendor, "Test Vendor"));
    CHECK(!strcmp(fw.version, "2.10 (gk3)"));
    CHECK(!strcmp(fw.date, "05/06/2024"));
    CHECK(!strcmp(fw.maker, "Maker"));
    CHECK(!strcmp(fw.product, "Product X"));
    CHECK(!contains(&fw, sizeof fw, SERIAL));
    /* SMBIOS 3 gives a maximum size; the walk ends at type 127, not there. */
    build_smbios3(sizeof smbios_table, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_OK);

    /* The report names the firmware and never the serial number. */
    base_run();
    run.cpu_count = 1;
    gaokun3_cpu(&run.cpu[0], 0x410fd4b0);
    CHECK(el2q_smbios_parse(smbios_entry, &run.fw) == EL2Q_SMBIOS_OK);
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("FIRMWARE 2.10 (gk3)  05/06/2024"));
    CHECK(has_line("VENDOR Test Vendor  SMBIOS 3.3"));
    CHECK(has_line("PRODUCT Maker  Product X"));
    for (unsigned i = 0; i < text.count; i++) CHECK(!strstr(text.line[i], SERIAL));
    run.fw.state = EL2Q_SMBIOS_NONE;
    el2q_report(&run, &text);
    CHECK(has_line("FIRMWARE: NO SMBIOS TABLE"));
    run.fw.state = EL2Q_SMBIOS_BAD;
    el2q_report(&run, &text);
    CHECK(has_line("FIRMWARE: SMBIOS TABLE DOES NOT PARSE"));

    /* Entry points that must be refused. */
    CHECK(el2q_smbios_parse(NULL, &fw) == EL2Q_SMBIOS_NONE && fw.state == EL2Q_SMBIOS_NONE);
    build_smbios3(size, table);
    smbios_entry[4] = '4';
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* anchor */
    build_smbios3(size, table);
    smbios_entry[0x0c] ^= 1;
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* checksum */
    CHECK(fw.vendor[0] == 0);
    build_smbios3(size, table);
    smbios_entry[6] = 0x10;
    seal_smbios3();
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* entry too short */
    build_smbios3(size, 0);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* no address */
    build_smbios3((1u << 20) + 1, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* over 1 MiB */
    build_smbios3(3, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* under a header */

    /* Structure tables that must be refused. */
    build_smbios3(0x10, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* type 0 past the end */
    build_smbios3(0x1a + 5, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* strings unterminated */
    build_smbios3(size, table);
    smbios_table[1] = 3;
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* length under 4 */
    smbios_table[1] = 0x1a;

    /* A valid table without type 0, two type 0s, and odd strings. */
    static const char *const system[] = {"Maker", "Product X", "1.0", SERIAL};
    static const char *const first[] = {"First", "v1", "01/01/2001"};
    static const char *const second[] = {"Second", "v2", "02/02/2002"};
    memset(smbios_table, 0xee, sizeof smbios_table);
    unsigned at = put_structure(0, 1, 0x1b, system_fields, 4, system, 4);
    at = put_structure(at, 127, 4, NULL, 0, NULL, 0);
    build_smbios3(at, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_NO_BIOS);
    at = put_structure(0, 0, 0x1a, bios_fields, 3, first, 3);
    at = put_structure(at, 0, 0x1a, bios_fields, 3, second, 3);
    at = put_structure(at, 127, 4, NULL, 0, NULL, 0);
    build_smbios3(at, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_OK && !strcmp(fw.vendor, "First"));
    static const uint8_t odd_fields[][2] = {{4, 0}, {5, 9}, {8, 1}};
    static const char *const odd[] = {"A\x01" "B\xff" "C0123456789012345678901234567890123456789"};
    at = put_structure(0, 0, 0x1a, odd_fields, 3, odd, 1);
    at = put_structure(at, 127, 4, NULL, 0, NULL, 0);
    build_smbios3(at, table);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_OK);
    CHECK(fw.vendor[0] == 0);                                  /* string 0: none */
    CHECK(fw.version[0] == 0);                                 /* past the last string */
    CHECK(strlen(fw.date) == EL2Q_FW_TEXT && !strncmp(fw.date, "A?B?C012", 8));

    /* SMBIOS 2.1: the entry point and its "_DMI_" part are both checked. */
    size = build_smbios_table();
    build_smbios2((uint16_t)size, 0);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* no address */
    build_smbios2((uint16_t)size, 0x1000);
    smbios_entry[0x14] = 'X';
    seal_smbios2();
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* "_DMI_" anchor */
    build_smbios2((uint16_t)size, 0x1000);
    smbios_entry[0x15] ^= 1;
    smbios_entry[4] = 0;
    smbios_entry[4] = zero_sum(smbios_entry, 0x1f);
    CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_BAD);   /* "_DMI_" checksum */
    /* The 2.1 table address is 32 bits; the walk itself is the one SMBIOS 3
     * uses above, so it is exercised only where the fixture sits that low. */
    if (table <= UINT32_MAX) {
        build_smbios2((uint16_t)size, (uint32_t)table);
        CHECK(el2q_smbios_parse(smbios_entry, &fw) == EL2Q_SMBIOS_OK && fw.major == 2 && fw.minor == 8);
    } else {
        printf("test-el2qual: SKIP SMBIOS 2.1 walk: fixture above 4 GiB\n");
    }
}

/* ---- GICv3 and the counter --------------------------------------------------- */

static void gic_cpu(struct el2q_cpu *c, uint64_t midr)
{
    gaokun3_cpu(c, midr);
    c->cntfrq = 19200000;
    c->probe_value[EL2Q_PROBE_ICC_SRE] = 0x7;                 /* SRE, DFB, DIB */
    c->probe_value[EL2Q_PROBE_ICH_VTR] = 0x90000003;          /* 4 LRs, 5 PRI, 5 PRE, 16 ID */
    c->lr_ran = 1;
    c->lr_back = EL2Q_LR_PATTERN;
}

static void test_gic(void)
{
    char line[EL2Q_TEXT_COLS + 1];
    struct el2q_cpu c;
    good_cpu(&c);
    CHECK(!strcmp(el2q_lr_word(&c), "-"));
    c.lr_ran = 1;
    c.lr_back = EL2Q_LR_PATTERN;
    CHECK(!strcmp(el2q_lr_word(&c), "RW"));
    c.lr_back = 0;
    CHECK(!strcmp(el2q_lr_word(&c), "RAZ"));
    c.lr_back = EL2Q_LR_PATTERN & ~0xffull;
    CHECK(!strcmp(el2q_lr_word(&c), "DIFF"));
    c.lr_esr = 0x02000000;
    CHECK(!strcmp(el2q_lr_word(&c), "UD"));
    c.lr_esr = 0x5e000000;
    CHECK(!strcmp(el2q_lr_word(&c), "EX"));

    base_run();
    run.cpu_count = 2;
    gic_cpu(&run.cpu[0], 0x410fd4b0);
    gic_cpu(&run.cpu[1], 0x410fd4c0);
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("COUNTER (CNTFRQ_EL0) .... 19200000 HZ"));
    CHECK(has_line("GIC SYSREGS AT EL2 ...... YES  ICC_SRE_EL2 07"));
    CHECK(has_line("GIC VIRTUAL INTERFACE ... YES  4 LRS  PRI 5  ID 16  LR0 RW"));
    CHECK(!has_line("1  CNTFRQ_EL0") && !has_line("LR0 TEST") && !has_line("ICH_VTR "));
    el2q_gic_line(&run, 0, line);
    CHECK(!strcmp(line, "GIC 0 FRQ=19200000 SRE=7/OK VTR=90000003/OK LR=RW HCR=0/OK"));

    /* A core that reads another rate, interface or test result is named. */
    run.cpu[1].cntfrq = 24000000;
    run.cpu[1].probe_value[EL2Q_PROBE_ICH_VTR] = 0x90000001;
    run.cpu[1].lr_back = 0;
    el2q_report(&run, &text);
    check_widths();
    CHECK(has_line("1  CNTFRQ_EL0 24000000"));
    CHECK(has_line("1  ICH_VTR    0000000090000001"));
    CHECK(has_line("1  LR0 TEST   RAZ"));

    /* No virtual interface: NO, and no list-register claim. */
    gic_cpu(&run.cpu[0], 0x410fd4b0);
    run.cpu[0].probe_esr[EL2Q_PROBE_ICH_VTR] = 0x02000000;
    run.cpu[0].probe_value[EL2Q_PROBE_ICH_VTR] = 0;
    run.cpu[0].lr_ran = 0;
    el2q_report(&run, &text);
    CHECK(has_line("GIC VIRTUAL INTERFACE ... NO  ICH_VTR_EL2 UNDEF"));
    el2q_gic_line(&run, 0, line);
    CHECK(!strcmp(line, "GIC 0 FRQ=19200000 SRE=7/OK VTR=0/UD LR=- HCR=0/OK"));
    /* Present, but the list registers not shown to hold state: not YES. */
    gic_cpu(&run.cpu[0], 0x410fd4b0);
    run.cpu[0].lr_ran = 0;
    el2q_report(&run, &text);
    CHECK(has_line("GIC VIRTUAL INTERFACE ... ?  4 LRS  PRI 5  ID 16  LR0 -"));
    run.cpu[0].lr_ran = 1;
    run.cpu[0].lr_back = 0;
    el2q_report(&run, &text);
    CHECK(has_line("GIC VIRTUAL INTERFACE ... ?  4 LRS  PRI 5  ID 16  LR0 RAZ"));
    /* System registers off at EL2, or undefined there. */
    gic_cpu(&run.cpu[0], 0x410fd4b0);
    run.cpu[0].probe_value[EL2Q_PROBE_ICC_SRE] = 0x6;
    el2q_report(&run, &text);
    CHECK(has_line("GIC SYSREGS AT EL2 ...... OFF  ICC_SRE_EL2 06"));
    run.cpu[0].probe_esr[EL2Q_PROBE_ICC_SRE] = 0x02000000;
    el2q_report(&run, &text);
    CHECK(has_line("GIC SYSREGS AT EL2 ...... ICC_SRE_EL2 UNDEF"));
    /* 24-bit IDs, 16 list registers and 8 priority bits decode. */
    gic_cpu(&run.cpu[0], 0x410fd4b0);
    run.cpu[0].probe_value[EL2Q_PROBE_ICH_VTR] = 7u << 29 | 1u << 23 | 15;
    el2q_report(&run, &text);
    CHECK(has_line("GIC VIRTUAL INTERFACE ... YES  16 LRS  PRI 8  ID 24  LR0 RW"));
    /* A missing counter rate is shown as such. */
    run.cpu[0].cntfrq = 0;
    el2q_report(&run, &text);
    CHECK(has_line("COUNTER (CNTFRQ_EL0) .... 0 HZ"));

    /* The widest GIC line still fits, HCR last so a cut loses only it. ICC_SRE
     * has four defined bits and ICH_VTR thirty-two; ICH_HCR can use all 64. */
    static const char widest[] = "GIC 0 FRQ=1000000000 SRE=F/OK VTR=FFFFFFFF/OK LR=DIFF HCR=";
    run.cpu[0].cntfrq = 1000000000;
    run.cpu[0].probe_value[EL2Q_PROBE_ICC_SRE] = 0xf;
    run.cpu[0].probe_value[EL2Q_PROBE_ICH_VTR] = UINT32_MAX;
    run.cpu[0].probe_value[EL2Q_PROBE_ICH_HCR] = UINT64_MAX;
    run.cpu[0].lr_back = 1;
    el2q_gic_line(&run, 0, line);
    CHECK(strlen(line) == EL2Q_TEXT_COLS);
    CHECK(!strncmp(line, widest, sizeof widest - 1));
    /* Unfinished and absent slots give no line. */
    run.cpu[1].state = EL2Q_CPU_FAULT;
    el2q_gic_line(&run, 1, line);
    CHECK(line[0] == 0);
    el2q_gic_line(&run, 5, line);
    CHECK(line[0] == 0);
}

/* ---- The tsens read (thermal.c) -------------------------------------------------
 * A synthetic SC8280XP: the gaokun3 DTB's four controllers, a counter that
 * advances a fixed step per read, and page tables that map what each case
 * says. The guard itself is the real compat/sc8280xp-thermal.c. */

#define TS_HZ UINT64_C(19200000)
#define TS_VALID (1u << 21)
static const struct { uint64_t tm, srot; unsigned sensors; } ts_dt[4] = {
    {0x0c263000, 0x0c222000, 14}, {0x0c265000, 0x0c223000, 16},
    {0x0c251000, 0x0c224000, 11}, {0x0c252000, 0x0c225000, 5},
};

struct fake_tsens {
    uint32_t version[4], control[4], trdy[4], status[4][16];
    uint64_t now, step, isr;
    uint64_t unmapped_at, abort_at;  /* one word that is unmapped / faults, or 0 */
    int all_unmapped, not_device, vary;
    unsigned mapped_calls, reads;
};
static struct fake_tsens ft;

static int ft_mapped(void *ctx, uint64_t address, uint64_t *par)
{
    struct fake_tsens *f = ctx;
    f->mapped_calls++;
    CHECK(f->reads == 0);                       /* every word checked before any read */
    if (f->all_unmapped || address == f->unmapped_at) {
        *par = 0x809;                           /* F set: a translation fault */
        return 1;
    }
    /* Device-nGnRE (0x04), or Normal write-back (0xff) when not_device. */
    *par = (address & ~0xfffull) | (uint64_t)(f->not_device ? 0xff : 0x04) << 56;
    return f->not_device ? 2 : 0;
}

static uint64_t ft_read(void *ctx, uint64_t address, uint64_t *value)
{
    struct fake_tsens *f = ctx;
    CHECK(f->mapped_calls == 58);
    CHECK(sc8280xp_thermal_address_allowed(address));
    f->reads++;
    if (address == f->abort_at) return 0x96000010;   /* data abort, synchronous external */
    for (unsigned b = 0; b < 4; b++) {
        if (address == ts_dt[b].srot) { *value = f->version[b]; return 0; }
        if (address == ts_dt[b].srot + 4) { *value = f->control[b]; return 0; }
        if (address == ts_dt[b].tm + 0xe4) { *value = f->trdy[b]; return 0; }
        uint64_t first = ts_dt[b].tm + 0xa0;
        if (address >= first && address < first + 4 * ts_dt[b].sensors) {
            uint32_t v = f->status[b][(address - first) / 4];
            /* Live sensors: a 0.1 C step every few reads, as silicon does. */
            if (f->vary && (v & TS_VALID)) v ^= (f->reads >> 6) & 1;
            *value = v;
            return 0;
        }
    }
    CHECK(0);
    return 1;
}

static uint64_t ft_counter(void *ctx)
{
    struct fake_tsens *f = ctx;
    uint64_t t = f->now;
    f->now += f->step;
    return t;
}

static uint64_t ft_isr(void *ctx) { return ((struct fake_tsens *)ctx)->isr; }

static void ft_reset(void)
{
    memset(&ft, 0, sizeof ft);
    ft.step = TS_HZ / 100000;                   /* 10 us per counter read */
    ft.vary = 1;
    for (unsigned b = 0; b < 4; b++) {
        ft.version[b] = EL2Q_TSENS_PIN;
        ft.control[b] = 1u | 1u << 21 | ((1u << ts_dt[b].sensors) - 1) << 3;
        ft.trdy[b] = 1;
        for (unsigned n = 0; n < ts_dt[b].sensors; n++) ft.status[b][n] = TS_VALID | (400 + b * 16 + n);
    }
}

static struct el2q_tsens ft_run(void)
{
    const struct el2q_tsens_hw hw = {&ft, ft_mapped, ft_read, ft_counter, ft_isr, TS_HZ,
                                     TS_HZ * EL2Q_TSENS_WINDOW_S};
    struct el2q_tsens s;
    el2q_tsens_run(&s, &hw);
    return s;
}

static void tsens_report(const struct el2q_tsens *s)
{
    base_run();
    run.cpu_count = 1;
    gaokun3_cpu(&run.cpu[0], 0x410fd4b0);
    run.tsens = *s;
    el2q_report(&run, &text);
    check_widths();
}

static void test_tsens(void)
{
    char line[EL2Q_TEXT_COLS + 1];
    struct el2q_tsens s;
    enum { HOT = 1, CONFIG = 2, READ = 5, UNCHANGED = 8 };     /* enum sc8280xp_thermal_reason */

    /* Not asked for: no section, no serial lines. */
    base_run();
    el2q_report(&run, &text);
    CHECK(!has_line("TSENS"));
    el2q_tsens_line(&run, 0, line);
    CHECK(line[0] == 0);

    /* Live sensors on the pin: admitted, polled the whole window, nothing tripped. */
    ft_reset();
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_PASSED && s.admit == 0 && s.reason == 0 && !s.abort_esr);
    CHECK(s.checked == 58 && s.unmapped == 0 && s.valid == 46 && s.reads == s.polls * 58);
    CHECK(s.polls >= 48 && s.polls <= 52 && s.elapsed_ticks >= TS_HZ * EL2Q_TSENS_WINDOW_S);
    CHECK(s.max_gap_ticks >= TS_HZ / 4 && s.max_gap_ticks < TS_HZ / 4 + TS_HZ / 1000);
    CHECK(s.peak_mc >= 44000 && s.peak_mc <= 45300);
    for (unsigned b = 0; b < 4; b++)
        CHECK(s.version[b] == EL2Q_TSENS_PIN && s.valid_mask[b] == (1u << ts_dt[b].sensors) - 1);
    tsens_report(&s);
    CHECK(has_line("TSENS GUARD (READ ONLY) . PASSED, NOTHING TRIPPED"));
    CHECK(has_line("  PIN 30000000  VALID 46/46  PEAK 4"));
    CHECK(has_line("  CTRL VERSION  CONTROL   TRDY      VALID"));
    CHECK(has_line("  3    30000000  002000F9  00000001  5"));
    CHECK(!has_line("SERROR") && !has_line("FAULTED"));
    el2q_tsens_line(&run, 0, line);
    CHECK(!strncmp(line, "TSENS PASSED CHECKED=58 UNMAPPED=0 ADMIT=0 POLLS=", 49));
    CHECK(strstr(line, " VALID=46") != 0);
    el2q_tsens_line(&run, 1, line);
    CHECK(!strcmp(line, "TSENSWHY REASON=OK AT=0 ESR=0 SERROR=0"));
    el2q_tsens_line(&run, 5, line);
    CHECK(!strcmp(line, "TSENSBLK 3 VER=30000000 CTRL=2000F9 TRDY=1 MASK=1F"));
    el2q_tsens_line(&run, 6, line);
    CHECK(line[0] == 0);

    /* Nothing mapped (QEMU's case): every word checked, none read. */
    ft_reset();
    ft.all_unmapped = 1;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_UNMAPPED && ft.reads == 0 && s.checked == 58 && s.unmapped == 58);
    CHECK(s.unmapped_at == 0x0c222000 && s.par == 0x809 && s.polls == 0);
    tsens_report(&s);
    CHECK(has_line("TSENS GUARD (READ ONLY) . NOT MAPPED, NOTHING READ"));
    CHECK(has_line("  58 OF 58 WORDS, FIRST 0C222000 PAR 0000000000000809"));
    CHECK(!has_line("PIN 30000000"));
    el2q_tsens_line(&run, 0, line);
    CHECK(!strcmp(line, "TSENS UNMAPPED CHECKED=58 UNMAPPED=58 ADMIT=0 POLLS=0 VALID=0"));
    el2q_tsens_line(&run, 1, line);
    CHECK(!strcmp(line, "TSENSWHY REASON=OK AT=C222000 ESR=0 SERROR=0"));

    /* One word missing, the last one: still nothing read. */
    ft_reset();
    ft.unmapped_at = 0x0c2520b0;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_UNMAPPED && ft.reads == 0 && s.unmapped == 1 &&
          s.unmapped_at == 0x0c2520b0);

    /* Mapped, but as Normal memory: refused the same way. */
    ft_reset();
    ft.not_device = 1;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_UNMAPPED && ft.reads == 0 && s.par >> 56 == 0xff);

    /* A read that faults: the guard trips READ there and reads nothing after. */
    ft_reset();
    ft.abort_at = ts_dt[1].tm + 0xe4;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_TRIPPED && s.admit == 1 && s.reason == READ);
    CHECK(s.location == ft.abort_at && s.abort_at == ft.abort_at && s.abort_esr == 0x96000010);
    CHECK(ft.reads == 14 + 3 + 3 && s.reads == ft.reads);
    tsens_report(&s);
    CHECK(has_line("TSENS GUARD (READ ONLY) . TRIPPED: READ AT 0C2650E4"));
    CHECK(has_line("  READ FAULTED AT 0C2650E4 ESR 96000010"));
    el2q_tsens_line(&run, 1, line);
    CHECK(!strcmp(line, "TSENSWHY REASON=READ AT=C2650E4 ESR=96000010 SERROR=0"));

    /* A different version word: CONFIG at the first SROT, the real word kept. */
    ft_reset();
    for (unsigned b = 0; b < 4; b++) ft.version[b] = 0x20010000;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_TRIPPED && s.reason == CONFIG && s.location == 0x0c222000);
    CHECK(s.version[0] == 0x20010000 && ft.reads == 1);

    /* Registers that never change: UNCHANGED, inside the 12 s window. */
    ft_reset();
    ft.vary = 0;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_TRIPPED && s.reason == UNCHANGED);
    CHECK(s.elapsed_ticks >= TS_HZ * 9 && s.elapsed_ticks < TS_HZ * EL2Q_TSENS_WINDOW_S);

    /* A sensor at the 95 C limit during the window. */
    ft_reset();
    ft.status[2][2] = TS_VALID | 950;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_TRIPPED && s.reason == HOT && s.location == ts_dt[2].tm + 0xa0 + 8);

    /* A sensor never valid: no admission, and no window. */
    ft_reset();
    ft.status[0][13] &= ~TS_VALID;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_NO_SAMPLE && s.admit == -1 && s.valid == 45 && s.elapsed_ticks == 0);
    tsens_report(&s);
    CHECK(has_line("TSENS GUARD (READ ONLY) . NO COMPLETE SAMPLE"));
    el2q_tsens_line(&run, 0, line);
    CHECK(strstr(line, " ADMIT=-1 ") != 0);

    /* A pending SError after the reads is reported, whatever the verdict. */
    ft_reset();
    ft.isr = 0x100;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_PASSED && s.isr == 0x100);
    tsens_report(&s);
    CHECK(has_line("  SERROR PENDING AFTER THE READS, ISR_EL1 00000100"));

    /* A counter that stops after admission ends the run instead of hanging it. */
    ft_reset();
    {
        const struct el2q_tsens_hw hw = {&ft, ft_mapped, ft_read, ft_counter, ft_isr, TS_HZ,
                                         TS_HZ * EL2Q_TSENS_WINDOW_S};
        el2q_tsens_run(&s, &hw);                 /* warm: admitted and passed */
        CHECK(s.state == EL2Q_TSENS_PASSED);
    }
    ft_reset();
    ft.step = 0;
    s = ft_run();
    CHECK(s.state == EL2Q_TSENS_STUCK && s.admit == 0);

    /* A missing operation reads nothing and runs nothing. */
    ft_reset();
    const struct el2q_tsens_hw partial = {&ft, ft_mapped, 0, ft_counter, ft_isr, TS_HZ, TS_HZ};
    el2q_tsens_run(&s, &partial);
    CHECK(s.state == EL2Q_TSENS_NOT_RUN && ft.mapped_calls == 0);
    el2q_tsens_run(&s, 0);
    CHECK(s.state == EL2Q_TSENS_NOT_RUN);

    /* The section survives a report the CPU tables overflow. */
    ft_reset();
    s = ft_run();
    base_run();
    run.cpu_count = EL2Q_MAX_CPUS;
    for (unsigned c = 0; c < EL2Q_MAX_CPUS; c++) {
        gaokun3_cpu(&run.cpu[c], 0x410fd4b0);
        for (unsigned i = 0; i < EL2Q_ID_COUNT; i++) run.cpu[c].id[i] += c;
    }
    run.tsens = s;
    el2q_report(&run, &text);
    check_widths();
    CHECK(text.count == EL2Q_TEXT_LINES && has_line("TSENS GUARD (READ ONLY) . PASSED"));
}

/* tools/test-el2qual-vhe.c: vhe.c and its report lines; returns its check count. */
unsigned test_vhe(void);
/* tools/test-el2qual-usbprobe.c: usbprobe.c, the read-only USB stage. */
unsigned test_usbprobe(void);
/* tools/test-el2qual-pagemap.c: pagemap.c, the private SMMU window map. */
unsigned test_pagemap(void);
unsigned test_s2walk(void);
unsigned test_s2bypass(void);
unsigned test_s2bwrite(void);

int main(void)
{
    test_acpi();
    test_fdt();
    test_verdicts();
    test_report();
    test_gop_order();
    test_simplefb();
    test_memmap();
    test_smbios();
    test_gic();
    test_tsens();
    checks += test_vhe();
    checks += test_usbprobe();
    checks += test_pagemap();
    checks += test_s2walk();
    checks += test_s2bypass();
    checks += test_s2bwrite();
    test_render();
    printf("test-el2qual: %u checks passed\n", checks);
    return 0;
}
