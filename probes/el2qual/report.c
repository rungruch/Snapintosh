/* SPDX-License-Identifier: MIT */
/* el2qual's verdicts, report text and font renderer; the firmware readers
 * are in firmware.c. See report.h; tools/test-el2qual.c exercises all of it
 * on the host. */
#include "report.h"

/* The allocated ID-space encodings, in probes/idregs.h's order (Arm ARM DDI
 * 0487, AArch64 ID register index). tools/test-el2qual.py checks this table
 * against idregs.h's cases, so the two cannot drift. aarch64 marks the rows
 * the screen shows; the AArch32 view only goes to the serial log. */
const struct el2q_idreg el2q_idregs[EL2Q_ID_COUNT] = {
    {0, 0, 1, "MIDR"},      {0, 5, 1, "MPIDR"},     {0, 6, 1, "REVIDR"},
    {1, 0, 0, "ID_PFR0"},   {1, 1, 0, "ID_PFR1"},   {1, 2, 0, "ID_DFR0"},
    {1, 3, 0, "ID_AFR0"},   {1, 4, 0, "ID_MMFR0"},  {1, 5, 0, "ID_MMFR1"},
    {1, 6, 0, "ID_MMFR2"},  {1, 7, 0, "ID_MMFR3"},  {2, 0, 0, "ID_ISAR0"},
    {2, 1, 0, "ID_ISAR1"},  {2, 2, 0, "ID_ISAR2"},  {2, 3, 0, "ID_ISAR3"},
    {2, 4, 0, "ID_ISAR4"},  {2, 5, 0, "ID_ISAR5"},  {2, 6, 0, "ID_MMFR4"},
    {2, 7, 0, "ID_ISAR6"},  {3, 0, 0, "MVFR0"},     {3, 1, 0, "MVFR1"},
    {3, 2, 0, "MVFR2"},     {3, 4, 0, "ID_PFR2"},   {3, 5, 0, "ID_DFR1"},
    {3, 6, 0, "ID_MMFR5"},  {4, 0, 1, "AA64PFR0"},  {4, 1, 1, "AA64PFR1"},
    {4, 2, 1, "AA64PFR2"},  {4, 4, 1, "AA64ZFR0"},  {4, 5, 1, "AA64SMFR0"},
    {4, 7, 1, "AA64FPFR0"}, {5, 0, 1, "AA64DFR0"},  {5, 1, 1, "AA64DFR1"},
    {5, 2, 1, "AA64DFR2"},  {5, 4, 1, "AA64AFR0"},  {5, 5, 1, "AA64AFR1"},
    {6, 0, 1, "AA64ISAR0"}, {6, 1, 1, "AA64ISAR1"}, {6, 2, 1, "AA64ISAR2"},
    {6, 3, 1, "AA64ISAR3"}, {7, 0, 1, "AA64MMFR0"}, {7, 1, 1, "AA64MMFR1"},
    {7, 2, 1, "AA64MMFR2"}, {7, 3, 1, "AA64MMFR3"}, {7, 4, 1, "AA64MMFR4"},
};

int el2q_id_index(unsigned crm, unsigned op2)
{
    for (int i = 0; i < EL2Q_ID_COUNT; i++)
        if (el2q_idregs[i].crm == crm && el2q_idregs[i].op2 == op2) return i;
    return -1;
}

static uint64_t id_of(const uint64_t *id, unsigned crm, unsigned op2)
{
    int i = el2q_id_index(crm, op2);
    return i < 0 ? 0 : id[i];
}

static unsigned field(uint64_t value, unsigned lsb) { return (unsigned)(value >> lsb) & 0xf; }

/* ---- Verdicts ------------------------------------------------------------ */

enum { LR_NOT_RUN, LR_UNDEF, LR_EXCEPTION, LR_RW, LR_RAZ, LR_DIFF };

static unsigned lr_code(const struct el2q_cpu *c)
{
    if (!c->lr_ran) return LR_NOT_RUN;
    if (c->lr_esr) return (c->lr_esr >> 26) == 0 ? LR_UNDEF : LR_EXCEPTION;
    if (c->lr_back == EL2Q_LR_PATTERN) return LR_RW;
    return c->lr_back ? LR_DIFF : LR_RAZ;
}

const char *el2q_lr_word(const struct el2q_cpu *c)
{
    static const char *const words[] = {"-", "UD", "EX", "RW", "RAZ", "DIFF"};
    return words[lr_code(c)];
}

enum el2q_verdict el2q_pauth_signs(const struct el2q_cpu *c)
{
    if (c->current_el != 2 || c->state != EL2Q_CPU_DONE || !c->pac_ran) return EL2Q_NOT_RUN;
    unsigned changed = 0;
    for (unsigned i = 0; i < EL2Q_PAC_TRIALS; i++) changed += c->pac_out[i] != c->pac_in[i];
    int keys = c->probe_esr[EL2Q_PROBE_APIAKEY] == 0;
    /* A sign then authenticate with the same key and modifier must give the
     * pointer back, and the signed form must differ from it. */
    int round_trip = c->pac_rt_ran && !c->pac_rt_esr && c->pac_rt[0] != c->pac_in[0] &&
                     c->pac_rt[1] == c->pac_in[0] && c->pac_rt[2] == c->pac_in[0];
    if (!keys && !changed) return EL2Q_NO;
    if (keys && changed && round_trip) return EL2Q_YES;
    return EL2Q_INCONCLUSIVE;
}

/* probes/run.py's reading of the four trials, unchanged: the controls decide
 * whether trial D means anything. */
enum el2q_verdict el2q_walks_16k(const struct el2q_cpu *c)
{
    const uint64_t *t = c->tgran;
    if (c->current_el != 2 || c->state != EL2Q_CPU_DONE || !t[6]) return EL2Q_NOT_RUN;
    if ((t[1] & 1) || !(t[2] & 1) || !(t[3] & 1)) return EL2Q_INCONCLUSIVE;
    if (t[4] & 1) return EL2Q_NO;
    return (t[4] & 0x000ffffffffff000ull) == t[6] ? EL2Q_YES : EL2Q_INCONCLUSIVE;
}

/* ---- Text --------------------------------------------------------------- */

struct lb { char *p; unsigned n; };

static void put_c(struct lb *b, char c)
{
    if (b->n < EL2Q_TEXT_COLS) b->p[b->n++] = c;
    b->p[b->n] = 0;
}

static void put_s(struct lb *b, const char *s) { while (*s) put_c(b, *s++); }

static void put_hex(struct lb *b, uint64_t v, unsigned digits)
{
    while (digits--) put_c(b, "0123456789ABCDEF"[(v >> (digits * 4)) & 15]);
}

static void put_dec(struct lb *b, uint64_t v)
{
    char d[20];
    unsigned n = 0;
    do d[n++] = (char)('0' + v % 10); while ((v /= 10) && n < sizeof d);
    while (n) put_c(b, d[--n]);
}

static void pad(struct lb *b, unsigned col, char fill) { while (b->n < col) put_c(b, fill); }

/* A verdict row: label, dot leader to a fixed column, then the answer. */
static void label(struct lb *b, const char *s)
{
    put_s(b, s);
    put_c(b, ' ');
    pad(b, 25, '.');
    put_c(b, ' ');
}

static struct lb new_line(struct el2q_text *t, enum el2q_color color)
{
    static char overflow[EL2Q_TEXT_COLS + 1];
    if (t->count >= EL2Q_TEXT_LINES) return (struct lb){overflow, 0};
    t->color[t->count] = (uint8_t)color;
    t->line[t->count][0] = 0;
    return (struct lb){t->line[t->count++], 0};
}

static enum el2q_color verdict_color(enum el2q_verdict v)
{
    return v == EL2Q_YES ? EL2Q_GREEN : v == EL2Q_NO ? EL2Q_RED
         : v == EL2Q_INCONCLUSIVE ? EL2Q_YELLOW : EL2Q_WHITE;
}

static const char *verdict_word(enum el2q_verdict v)
{
    return v == EL2Q_YES ? "YES" : v == EL2Q_NO ? "NO"
         : v == EL2Q_INCONCLUSIVE ? "INCONCLUSIVE" : "NOT RUN";
}

static void put_esr(struct lb *b, uint64_t esr)
{
    /* EC 0, "unknown reason", is what an unimplemented instruction raises;
     * its ISS is always zero, so the word says everything. Anything else
     * (a trap to EL3, an alignment fault) is printed whole. */
    if (!esr) put_s(b, "OK");
    else if ((esr >> 26) == 0) put_s(b, "UNDEF");
    else {
        put_s(b, "ESR ");
        put_hex(b, esr, 8);
    }
}

static const char *core_name(uint64_t midr)
{
    static const struct { uint8_t imp; uint16_t part; char name[12]; } known[] = {
        {0x41, 0xd07, "CORTEX-A57"}, {0x41, 0xd08, "CORTEX-A72"}, {0x41, 0xd0b, "CORTEX-A76"},
        {0x41, 0xd0d, "CORTEX-A77"}, {0x41, 0xd41, "CORTEX-A78"}, {0x41, 0xd44, "CORTEX-X1"},
        {0x41, 0xd4b, "CORTEX-A78C"}, {0x41, 0xd4c, "CORTEX-X1C"},
        /* 0x001 is the X Elite's Oryon; the A16's X2 Elite reads 0x512f0021,
         * part 0x002 (docs/FEASIBILITY.md). */
        {0x51, 0x001, "ORYON"}, {0x51, 0x002, "ORYON"},
    };
    unsigned imp = (midr >> 24) & 0xff, part = (midr >> 4) & 0xfff;
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (known[i].imp == imp && known[i].part == part) return known[i].name;
    return imp == 0x61 ? "APPLE" : "";
}

static void put_midr(struct lb *b, uint64_t midr)
{
    put_s(b, "IMP ");
    put_hex(b, midr >> 24, 2);
    put_s(b, " PART ");
    put_hex(b, midr >> 4, 3);
    put_s(b, " R");
    put_dec(b, (midr >> 20) & 15);
    put_c(b, 'P');
    put_dec(b, midr & 15);
    put_c(b, ' ');
    put_s(b, core_name(midr));
}

/* The firmware every other line was measured under; until this, no run
 * recorded it. */
static void firmware(const struct el2q_firmware *f, struct el2q_text *t)
{
    static const char *const words[] = {
        [EL2Q_SMBIOS_NONE] = "NO SMBIOS TABLE",
        [EL2Q_SMBIOS_BAD] = "SMBIOS TABLE DOES NOT PARSE",
        [EL2Q_SMBIOS_NO_BIOS] = "SMBIOS HAS NO BIOS ENTRY",
    };
    struct lb b;
    if (f->state != EL2Q_SMBIOS_OK) {
        b = new_line(t, EL2Q_YELLOW);
        put_s(&b, "FIRMWARE: ");
        put_s(&b, f->state < EL2Q_SMBIOS_OK ? words[f->state] : "?");
        return;
    }
    b = new_line(t, EL2Q_WHITE);
    put_s(&b, "FIRMWARE ");
    put_s(&b, f->version[0] ? f->version : "-");
    put_s(&b, "  ");
    put_s(&b, f->date[0] ? f->date : "-");
    b = new_line(t, EL2Q_WHITE);
    put_s(&b, "VENDOR ");
    put_s(&b, f->vendor[0] ? f->vendor : "-");
    put_s(&b, "  SMBIOS ");
    put_dec(&b, f->major);
    put_c(&b, '.');
    put_dec(&b, f->minor);
    b = new_line(t, EL2Q_WHITE);
    put_s(&b, "PRODUCT ");
    put_s(&b, f->maker[0] ? f->maker : "-");
    put_s(&b, "  ");
    put_s(&b, f->product[0] ? f->product : "-");
}

static const char *const dt_words[] = {
    [EL2Q_DT_NONE] = "NO DEVICE TREE: WILL NOT SWITCH",
    [EL2Q_DT_BAD] = "DEVICE TREE UNREADABLE",
    [EL2Q_DT_NO_ZAP] = "NO ADRENO ZAP-SHADER: WILL NOT SWITCH",
    [EL2Q_DT_ZAP_OKAY] = "ZAP-SHADER ENABLED: WILL NOT SWITCH",
    [EL2Q_DT_ZAP_DISABLED] = "ZAP-SHADER DISABLED: OPEN",
};

/* A USB0 POWER line in green when the controller is on, yellow otherwise;
 * every other USB line in white. */
static enum el2q_color usb_line_color(const char *line)
{
    static const char prefix[] = "USB0 POWER";
    unsigned n = 0;
    while (prefix[n] && line[n] == prefix[n]) n++;
    if (prefix[n]) return EL2Q_WHITE;
    while (line[n]) n++;
    return n >= 3 && line[n - 3] == ' ' && line[n - 2] == 'O' && line[n - 1] == 'N' ? EL2Q_GREEN
                                                                                   : EL2Q_YELLOW;
}

static void header(const struct el2q_run *r, struct el2q_text *t)
{
    struct lb b = new_line(t, EL2Q_CYAN);
    put_s(&b, "SNAPINTOSH EL2QUAL ");
    put_dec(&b, EL2Q_VERSION);

    b = new_line(t, r->end_el == 2 ? EL2Q_WHITE : EL2Q_RED);
    put_s(&b, "START EL");
    put_dec(&b, r->start_el);
    put_s(&b, "  AFTER EBS EL");
    put_dec(&b, r->end_el);
    put_s(&b, r->ebs_status ? "  EBS FAILED " : "  EBS OK, TRIES ");
    if (r->ebs_status) put_hex(&b, r->ebs_status, 16);
    else put_dec(&b, r->ebs_tries);

    if (r->end_el == 2) {
        b = new_line(t, EL2Q_WHITE);
        put_s(&b, "ENTRY HCR_EL2 ");
        put_hex(&b, r->hcr_entry, 16);
        put_s(&b, " SCTLR ");
        put_hex(&b, r->sctlr_entry, 16);
    }
    if (r->emulated_vhe) {
        b = new_line(t, EL2Q_YELLOW);
        put_s(&b, "TEST ONLY: SLBOUNCE E2H STATE EMULATED");
    }
    if (r->vhe_exit != EL2Q_VHE_EXIT_NOT_ASKED) {
        b = new_line(t, r->vhe_exit == EL2Q_VHE_EXIT_DONE ? EL2Q_GREEN
                      : r->vhe_exit == EL2Q_VHE_EXIT_NOT_VHE ? EL2Q_WHITE : EL2Q_RED);
        put_s(&b, "VHE EXIT .. ");
        put_s(&b, el2q_vhe_exit_word(r->vhe_exit));
        if (r->vhe_exit == EL2Q_VHE_EXIT_DONE) {
            put_s(&b, "  HCR_EL2 ");
            put_hex(&b, r->vhe_out.hcr, 16);
        }
        /* The registers as found and as left. The gaokun3's SPCR names a
         * Qualcomm UART (interface type 0x11) that el2qual's serial output,
         * PL011 and SBSA only, does not drive, so the screen is the only
         * record of slbounce's TCR, TTBR0, CPTR and CNTHCTL there. */
        char regs[EL2Q_TEXT_COLS + 1];
        for (unsigned i = 1;; i++) {
            el2q_vhe_line(r, i, regs);
            if (!regs[0]) break;
            b = new_line(t, EL2Q_WHITE);
            put_s(&b, regs);
        }
    }
    /* The USB read, near the top, where a long CPU table cannot push it off
     * the screen. */
    _Static_assert(EL2Q_USB_LINE_COLS <= EL2Q_TEXT_COLS, "a USB line fits a report line");
    for (unsigned i = 0;; i++) {
        char usb[EL2Q_USB_LINE_COLS + 1];
        el2q_usb_line(&r->usb, i, usb);
        if (!usb[0]) break;
        b = new_line(t, usb_line_color(usb));
        put_s(&b, usb);
    }

    enum el2q_color c = r->start_el == 2 ? EL2Q_WHITE
                      : r->dt_state == EL2Q_DT_ZAP_DISABLED ? EL2Q_GREEN : EL2Q_RED;
    b = new_line(t, c);
    put_s(&b, "SLBOUNCE GATE: ");
    put_s(&b, r->start_el == 2 ? "NOT NEEDED, STARTED AT EL2" : dt_words[r->dt_state]);

    b = new_line(t, EL2Q_WHITE);
    put_s(&b, "ACPI CPUS ");
    put_dec(&b, r->acpi.cpu_count);
    put_s(&b, "  GIC V");
    put_dec(&b, r->acpi.gic_version);
    put_s(&b, "  PSCI ");
    if (r->psci_version < 0) put_s(&b, "NONE");
    else {
        put_dec(&b, (uint64_t)r->psci_version >> 16);
        put_c(&b, '.');
        put_dec(&b, (uint64_t)r->psci_version & 0xffff);
    }
    put_s(&b, "  FB ");
    put_dec(&b, r->fb_width);
    put_c(&b, 'X');
    put_dec(&b, r->fb_height);

    firmware(&r->fw, t);

    /* What the report is drawn on, so a photo records it: the device tree's
     * framebuffer, or a GOP mode, and a switch away from the firmware's. */
    if (r->fb_source == EL2Q_FB_DT) {
        b = new_line(t, EL2Q_YELLOW);
        put_s(&b, "SCREEN DT SIMPLE-FB  FB ");
        put_hex(&b, r->fb_base, 16);
        put_s(&b, "  NO GOP ADDRESS");
        return;
    }
    int moved = r->fb_mode != r->fb_first_mode;
    b = new_line(t, moved || !r->fb_base ? EL2Q_YELLOW : EL2Q_WHITE);
    put_s(&b, "SCREEN GOP MODE ");
    put_dec(&b, r->fb_mode);
    if (r->fb_base) {
        put_s(&b, "  FB ");
        put_hex(&b, r->fb_base, 16);
    } else {
        put_s(&b, "  NO FRAMEBUFFER ADDRESS");
    }
    if (moved) {
        put_s(&b, "  FROM MODE ");
        put_dec(&b, r->fb_first_mode);
    }
}

/* The two ways forward from docs research, as minimum CPU requirements. The
 * J700 route is the current monitor: it refuses to start without ECV and
 * RNDR, and the stock kernel needs NV2 to run below EL2. The VirtualMac
 * route needs only what an EL1 arm64e guest needs. Neither line is a claim
 * that the route works; a NO is a claim that it cannot. A requirement that
 * was not measured, or measured inconclusively, is listed with a "?": it
 * keeps the route from CPU OK without claiming the route is impossible. */
static void requirement(struct lb *b, const char *name, enum el2q_verdict v)
{
    if (v == EL2Q_YES) return;
    put_c(b, ' ');
    put_s(b, name);
    if (v != EL2Q_NO) put_c(b, '?');
}

static void routes(const struct el2q_cpu *c, struct el2q_text *t)
{
    const uint64_t *id = c->id;
    enum el2q_verdict signs = el2q_pauth_signs(c), walks = el2q_walks_16k(c);
    int nv2 = field(id_of(id, 7, 2), 24) >= 2, ecv = field(id_of(id, 7, 0), 60) >= 1;
    int rndr = field(id_of(id, 6, 0), 60) >= 1;
    int j700 = nv2 && ecv && rndr && signs == EL2Q_YES;
    int vmac = signs == EL2Q_YES && walks == EL2Q_YES;

    struct lb b = new_line(t, j700 ? EL2Q_GREEN : EL2Q_RED);
    label(&b, "J700 ROUTE (AS ON A16)");
    if (j700) put_s(&b, "CPU OK");
    else {
        put_s(&b, "BLOCKED:");
        if (!nv2) put_s(&b, " NV2");
        if (!ecv) put_s(&b, " ECV");
        if (!rndr) put_s(&b, " RNDR");
        requirement(&b, "PAUTH", signs);
    }

    b = new_line(t, vmac ? EL2Q_GREEN : EL2Q_RED);
    label(&b, "VIRTUALMAC ROUTE (EL1)");
    if (vmac) put_s(&b, "CPU OK");
    else {
        put_s(&b, "BLOCKED:");
        requirement(&b, "PAUTH", signs);
        requirement(&b, "16K", walks);
    }
}

/* Whether a guest's interrupts could be delivered through the GICv3 list
 * registers rather than a fully modelled controller: EL2 must reach the
 * system registers (ICC_SRE_EL2.SRE), the virtual CPU interface must exist
 * (ICH_VTR_EL2) and its list registers must keep what is written. */
static void gic_virt(const struct el2q_cpu *c, struct el2q_text *t)
{
    const uint64_t *esr = c->probe_esr, *v = c->probe_value;
    uint64_t sre = v[EL2Q_PROBE_ICC_SRE], vtr = v[EL2Q_PROBE_ICH_VTR];
    int sre_ok = !esr[EL2Q_PROBE_ICC_SRE] && (sre & 1);
    struct lb b = new_line(t, sre_ok ? EL2Q_WHITE : EL2Q_YELLOW);
    label(&b, "GIC SYSREGS AT EL2");
    if (esr[EL2Q_PROBE_ICC_SRE]) {
        put_s(&b, "ICC_SRE_EL2 ");
        put_esr(&b, esr[EL2Q_PROBE_ICC_SRE]);
    } else {
        put_s(&b, sre_ok ? "YES  ICC_SRE_EL2 " : "OFF  ICC_SRE_EL2 ");
        put_hex(&b, sre, 2);
    }

    unsigned lr = lr_code(c);
    int vtr_ok = !esr[EL2Q_PROBE_ICH_VTR];
    enum el2q_verdict verdict = !vtr_ok ? EL2Q_NO : lr == LR_RW ? EL2Q_YES : EL2Q_INCONCLUSIVE;
    b = new_line(t, verdict_color(verdict));
    label(&b, "GIC VIRTUAL INTERFACE");
    if (!vtr_ok) {
        put_s(&b, "NO  ICH_VTR_EL2 ");
        put_esr(&b, esr[EL2Q_PROBE_ICH_VTR]);
        return;
    }
    put_s(&b, verdict == EL2Q_YES ? "YES  " : "?  ");
    put_dec(&b, (vtr & 0x1f) + 1);
    put_s(&b, " LRS  PRI ");
    put_dec(&b, ((vtr >> 29) & 7) + 1);
    put_s(&b, "  ID ");
    put_dec(&b, (vtr >> 23) & 7 ? 24 : 16);
    put_s(&b, "  LR0 ");
    put_s(&b, el2q_lr_word(c));
}

static void verdicts(const struct el2q_cpu *c, struct el2q_text *t)
{
    const uint64_t *id = c->id;
    uint64_t isar1 = id_of(id, 6, 1), isar2 = id_of(id, 6, 2);
    uint64_t mmfr0 = id_of(id, 7, 0), mmfr1 = id_of(id, 7, 1);
    uint64_t pfr0 = id_of(id, 4, 0), pfr1 = id_of(id, 4, 1);
    static const unsigned parange[] = {32, 36, 40, 42, 44, 48, 52, 56};

    struct lb b = new_line(t, EL2Q_CYAN);
    put_s(&b, "BOOT CPU, MEASURED AT EL2");

    b = new_line(t, EL2Q_WHITE);
    label(&b, "CPU");
    put_midr(&b, id_of(id, 0, 0));

    unsigned apa = field(isar1, 4), api = field(isar1, 8), apa3 = field(isar2, 12);
    unsigned gpa = field(isar1, 24), gpi = field(isar1, 28), gpa3 = field(isar2, 8);
    int advertised = apa || api || apa3;
    enum el2q_verdict signs = el2q_pauth_signs(c);
    b = new_line(t, advertised ? EL2Q_GREEN : EL2Q_RED);
    label(&b, "PAUTH ADVERTISED");
    put_s(&b, advertised ? "YES" : "NO");
    put_s(&b, "  APA ");  put_dec(&b, apa);
    put_s(&b, " API ");   put_dec(&b, api);
    put_s(&b, " APA3 ");  put_dec(&b, apa3);
    put_s(&b, " G ");     put_dec(&b, gpa | gpi | gpa3);

    unsigned changed = 0;
    for (unsigned i = 0; i < EL2Q_PAC_TRIALS; i++) changed += c->pac_out[i] != c->pac_in[i];
    b = new_line(t, verdict_color(signs));
    label(&b, "PAUTH SIGNS (PACIA1716)");
    put_s(&b, verdict_word(signs));
    put_s(&b, "  CHANGED ");
    put_dec(&b, changed);
    put_c(&b, '/');
    put_dec(&b, EL2Q_PAC_TRIALS);
    if (signs == EL2Q_YES && !advertised) put_s(&b, " HIDDEN?");

    /* With nopac these probes never run, and their fault codes stay at the
     * zero that put_esr prints as OK: say NOT RUN instead, so a skipped probe
     * cannot be read as a passed one (gaokun3 runs 6, 8 and 9 did). */
    b = new_line(t, EL2Q_WHITE);
    label(&b, "PAUTH KEYS, ROUND TRIP");
    if (!c->pac_ran) put_s(&b, verdict_word(EL2Q_NOT_RUN));
    else put_esr(&b, c->probe_esr[EL2Q_PROBE_APIAKEY]);
    if (c->pac_ran && c->pac_rt_ran) {
        put_s(&b, ", ");
        put_esr(&b, c->pac_rt_esr);
        if (!c->pac_rt_esr) put_s(&b, c->pac_rt[1] == c->pac_in[0] ? " AUTH OK" : " AUTH BAD");
    }

    b = new_line(t, EL2Q_WHITE);
    label(&b, "PACGA");
    if (!c->pac_ran) put_s(&b, verdict_word(EL2Q_NOT_RUN));
    else put_esr(&b, c->probe_esr[EL2Q_PROBE_PACGA]);

    unsigned tg16 = field(mmfr0, 20), tg16_2 = field(mmfr0, 32);
    b = new_line(t, tg16 ? EL2Q_GREEN : EL2Q_RED);
    label(&b, "16K ADVERTISED");
    put_s(&b, tg16 ? "YES" : "NO");
    put_s(&b, "  TGRAN16 ");
    put_dec(&b, tg16);
    put_s(&b, " TGRAN16_2 ");
    put_dec(&b, tg16_2);

    enum el2q_verdict walks = el2q_walks_16k(c);
    b = new_line(t, verdict_color(walks));
    label(&b, "16K WALK (TRIAL D)");
    put_s(&b, walks == EL2Q_YES ? "WALKS" : walks == EL2Q_NO ? "DOES NOT WALK" : verdict_word(walks));
    if (walks != EL2Q_NOT_RUN) {
        put_s(&b, "  PAR ");
        put_hex(&b, c->tgran[4], 9);
        b = new_line(t, walks == EL2Q_INCONCLUSIVE ? EL2Q_YELLOW : EL2Q_WHITE);
        put_s(&b, "  CONTROLS A ");
        put_s(&b, c->tgran[1] & 1 ? "FAULT" : "OK");
        put_s(&b, "  B ");
        put_s(&b, c->tgran[2] & 1 ? "FAULT" : "OK");
        put_s(&b, "  C ");
        put_s(&b, c->tgran[3] & 1 ? "FAULT" : "OK");
        put_s(&b, "  (WANT OK FAULT FAULT)");
    }

    static const struct { const char *name, *reg; unsigned crm, op2, lsb, want, probe; } feats[] = {
        {"NV2", "VNCR_EL2", 7, 2, 24, 2, EL2Q_PROBE_VNCR},
        {"ECV", "CNTPOFF_EL2", 7, 0, 60, 1, EL2Q_PROBE_CNTPOFF},
        {"FGT", "HFGRTR_EL2", 7, 0, 56, 1, EL2Q_PROBE_HFGRTR},
        {"RNDR", "RNDR", 6, 0, 60, 1, EL2Q_PROBE_RNDR},
    };
    for (unsigned i = 0; i < sizeof feats / sizeof feats[0]; i++) {
        unsigned v = field(id_of(id, feats[i].crm, feats[i].op2), feats[i].lsb);
        int have = v >= feats[i].want;
        b = new_line(t, have ? EL2Q_GREEN : EL2Q_RED);
        label(&b, feats[i].name);
        put_s(&b, have ? "YES  FIELD " : "NO  FIELD ");
        put_dec(&b, v);
        put_c(&b, ' ');
        put_s(&b, feats[i].reg);
        put_c(&b, ' ');
        put_esr(&b, c->probe_esr[feats[i].probe]);
    }

    b = new_line(t, EL2Q_WHITE);
    label(&b, "VHE, PA SIZE");
    put_s(&b, "VH ");
    put_dec(&b, field(mmfr1, 8));
    put_s(&b, "  PARANGE ");
    put_dec(&b, parange[field(mmfr0, 0) & 7]);
    put_s(&b, " BITS");

    b = new_line(t, EL2Q_WHITE);
    label(&b, "SVE, SME, GIC SYSREGS");
    put_dec(&b, field(pfr0, 32));
    put_s(&b, ", ");
    put_dec(&b, field(pfr1, 24));
    put_s(&b, ", ");
    put_dec(&b, field(pfr0, 24));

    /* The rate a VirtualMac guest's device tree must state, since without
     * ECV the counter cannot be shown at any other. */
    b = new_line(t, c->cntfrq ? EL2Q_WHITE : EL2Q_YELLOW);
    label(&b, "COUNTER (CNTFRQ_EL0)");
    put_dec(&b, c->cntfrq);
    put_s(&b, " HZ");

    gic_virt(c, t);
    routes(c, t);
}

static const char *state_word(uint32_t s)
{
    static const char *const words[] = {"UNTESTED", "PSCI FAIL", "NO REPLY", "WRONG EL", "FAULT", "DONE"};
    return s < sizeof words / sizeof words[0] ? words[s] : "?";
}

static void cpu_table(const struct el2q_run *r, struct el2q_text *t)
{
    struct lb b = new_line(t, EL2Q_CYAN);
    put_s(&b, "SL MPIDR      MIDR     EL STATE     PAC  16K  PSCI");
    for (unsigned i = 0; i < r->cpu_count; i++) {
        const struct el2q_cpu *c = &r->cpu[i];
        enum el2q_verdict pac = el2q_pauth_signs(c), walk = el2q_walks_16k(c);
        b = new_line(t, c->state == EL2Q_CPU_DONE ? EL2Q_WHITE : EL2Q_YELLOW);
        put_dec(&b, i);
        pad(&b, 3, ' ');
        put_hex(&b, c->target_mpidr, 10);
        put_c(&b, ' ');
        if (c->state == EL2Q_CPU_DONE) put_hex(&b, id_of(c->id, 0, 0), 8);
        else put_s(&b, "--------");
        put_c(&b, ' ');
        put_dec(&b, c->current_el);
        pad(&b, 26, ' ');
        put_s(&b, state_word(c->state));
        pad(&b, 36, ' ');
        put_s(&b, pac == EL2Q_YES ? "YES" : pac == EL2Q_NO ? "NO" : pac == EL2Q_NOT_RUN ? "-" : "?");
        pad(&b, 41, ' ');
        put_s(&b, walk == EL2Q_YES ? "YES" : walk == EL2Q_NO ? "NO" : walk == EL2Q_NOT_RUN ? "-" : "?");
        pad(&b, 46, ' ');
        if (i == 0) put_s(&b, "BOOT");
        else {
            put_dec(&b, (uint64_t)(c->psci_on < 0 ? -c->psci_on : c->psci_on));
            if (c->psci_on < 0) put_s(&b, " NEG");
            put_s(&b, " AFF ");
            put_dec(&b, (uint64_t)(c->psci_affinity < 0 ? 9 : c->psci_affinity));
        }
        if (c->state == EL2Q_CPU_FAULT) {
            b = new_line(t, EL2Q_RED);
            put_s(&b, "   V");
            put_dec(&b, c->fault_vector);
            put_s(&b, " ESR ");
            put_hex(&b, c->fault_esr, 8);
            put_s(&b, " ELR ");
            put_hex(&b, c->fault_elr, 12);
            put_s(&b, " FAR ");
            put_hex(&b, c->fault_far, 12);
        }
        if (c->state == EL2Q_CPU_WRONG_EL || c->state == EL2Q_CPU_NO_RESPONSE) {
            b = new_line(t, EL2Q_YELLOW);
            put_s(&b, "   ENTRY MARK ");
            put_hex(&b, c->early, 16);
        }
    }
}

static void id_table(const struct el2q_run *r, struct el2q_text *t)
{
    const struct el2q_cpu *c = &r->cpu[0];
    int have_el2 = r->end_el == 2 && c->state == EL2Q_CPU_DONE;
    struct lb b = new_line(t, EL2Q_CYAN);
    put_s(&b, "REGISTER   AT START (EL");
    put_dec(&b, r->start_el);
    put_s(&b, ")      AT EL2, BOOT CPU");
    for (unsigned i = 0; i < EL2Q_ID_COUNT; i++) {
        if (!el2q_idregs[i].aarch64) continue;
        int differs = have_el2 && r->start_id[i] != c->id[i];
        b = new_line(t, differs ? EL2Q_YELLOW : EL2Q_WHITE);
        put_s(&b, el2q_idregs[i].name);
        pad(&b, 11, ' ');
        put_hex(&b, r->start_id[i], 16);
        put_s(&b, "  ");
        if (have_el2) put_hex(&b, c->id[i], 16);
        else put_s(&b, "-");
        if (differs) put_s(&b, " *");
    }
}

/* Each other core reports its full set; only what differs from the boot
 * CPU fits on screen. MPIDR always differs and is in the CPU table. */
static void differences(const struct el2q_run *r, struct el2q_text *t)
{
    const struct el2q_cpu *boot = &r->cpu[0];
    if (boot->state != EL2Q_CPU_DONE) return;
    struct lb b = new_line(t, EL2Q_CYAN);
    put_s(&b, "OTHER CPUS: WHAT DIFFERS FROM SLOT 0");
    unsigned shown = 0;
    for (unsigned s = 1; s < r->cpu_count; s++) {
        const struct el2q_cpu *c = &r->cpu[s];
        if (c->state != EL2Q_CPU_DONE) continue;
        for (unsigned i = 0; i < EL2Q_ID_COUNT; i++) {
            if (i == 1 || c->id[i] == boot->id[i]) continue;
            b = new_line(t, EL2Q_WHITE);
            put_dec(&b, s);
            pad(&b, 3, ' ');
            put_s(&b, el2q_idregs[i].name);
            pad(&b, 14, ' ');
            put_hex(&b, c->id[i], 16);
            shown++;
        }
        /* The counter rate and the GIC interface are per core too. */
        if (c->cntfrq != boot->cntfrq) {
            b = new_line(t, EL2Q_YELLOW);
            put_dec(&b, s);
            pad(&b, 3, ' ');
            put_s(&b, "CNTFRQ_EL0");
            pad(&b, 14, ' ');
            put_dec(&b, c->cntfrq);
            shown++;
        }
        const unsigned vtr = EL2Q_PROBE_ICH_VTR;
        if (c->probe_esr[vtr] != boot->probe_esr[vtr] ||
            c->probe_value[vtr] != boot->probe_value[vtr]) {
            b = new_line(t, EL2Q_YELLOW);
            put_dec(&b, s);
            pad(&b, 3, ' ');
            put_s(&b, "ICH_VTR");
            pad(&b, 14, ' ');
            if (c->probe_esr[vtr]) put_esr(&b, c->probe_esr[vtr]);
            else put_hex(&b, c->probe_value[vtr], 16);
            shown++;
        }
        if (lr_code(c) != lr_code(boot)) {
            b = new_line(t, EL2Q_YELLOW);
            put_dec(&b, s);
            pad(&b, 3, ' ');
            put_s(&b, "LR0 TEST");
            pad(&b, 14, ' ');
            put_s(&b, el2q_lr_word(c));
            shown++;
        }
    }
    if (!shown) {
        b = new_line(t, EL2Q_GREEN);
        put_s(&b, "NONE");
    }
}

/* ---- The tsens read ---------------------------------------------------------- */

static void put_hex_short(struct lb *b, uint64_t v);

/* enum sc8280xp_thermal_reason, in order (compat/sc8280xp-thermal.h). */
static const char *const tsens_reasons[] = {
    "OK", "HOT", "CONFIG", "INVALID", "LATE", "READ", "RANGE", "CRITICAL",
    "UNCHANGED", "SLOW", "CLOCK", "UNPINNED",
};
static const char *const tsens_states[] = {
    "NOTRUN", "UNMAPPED", "TRIPPED", "NOSAMPLE", "STUCK", "PASSED",
};

static const char *tsens_reason(uint64_t reason)
{
    return reason < sizeof tsens_reasons / sizeof tsens_reasons[0] ? tsens_reasons[reason] : "?";
}

static const char *tsens_state(uint32_t state)
{
    return state < sizeof tsens_states / sizeof tsens_states[0] ? tsens_states[state] : "?";
}

/* Millidegrees as degrees with one decimal; the sensors resolve 0.1 C. */
static void put_temp(struct lb *b, int64_t mc)
{
    if (mc == INT64_MIN) {
        put_s(b, "-");
        return;
    }
    if (mc < 0) {
        put_c(b, '-');
        mc = -mc;
    }
    put_dec(b, (uint64_t)mc / 1000);
    put_c(b, '.');
    put_dec(b, (uint64_t)mc / 100 % 10);
    put_s(b, " C");
}

/* Counter ticks as milliseconds with one decimal. */
static void put_ms(struct lb *b, uint64_t ticks, uint64_t hz)
{
    if (!hz) {
        put_s(b, "?");
        return;
    }
    uint64_t tenths = ticks / hz * 10000 + ticks % hz * 10000 / hz;
    put_dec(b, tenths / 10);
    put_c(b, '.');
    put_dec(b, tenths % 10);
    put_s(b, " MS");
}

static unsigned popcount(uint64_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

static void tsens_section(const struct el2q_tsens *s, struct el2q_text *t)
{
    if (s->state == EL2Q_TSENS_NOT_RUN) return;
    enum el2q_color c = s->state == EL2Q_TSENS_PASSED ? EL2Q_GREEN
                      : s->state == EL2Q_TSENS_NO_SAMPLE ? EL2Q_YELLOW : EL2Q_RED;
    struct lb b = new_line(t, c);
    label(&b, "TSENS GUARD (READ ONLY)");
    switch (s->state) {
    case EL2Q_TSENS_PASSED: put_s(&b, "PASSED, NOTHING TRIPPED"); break;
    case EL2Q_TSENS_UNMAPPED: put_s(&b, "NOT MAPPED, NOTHING READ"); break;
    case EL2Q_TSENS_NO_SAMPLE: put_s(&b, "NO COMPLETE SAMPLE"); break;
    case EL2Q_TSENS_STUCK: put_s(&b, "COUNTER STOPPED"); break;
    default:
        put_s(&b, "TRIPPED: ");
        put_s(&b, tsens_reason(s->reason));
        if (s->location) {
            put_s(&b, " AT ");
            put_hex(&b, s->location, 8);
        }
    }
    if (s->state == EL2Q_TSENS_UNMAPPED) {
        b = new_line(t, EL2Q_RED);
        put_s(&b, "  ");
        put_dec(&b, s->unmapped);
        put_s(&b, " OF ");
        put_dec(&b, s->checked);
        put_s(&b, " WORDS, FIRST ");
        put_hex(&b, s->unmapped_at, 8);
        put_s(&b, " PAR ");
        put_hex(&b, s->par, 16);
    } else {
        b = new_line(t, EL2Q_WHITE);
        put_s(&b, "  PIN ");
        put_hex(&b, EL2Q_TSENS_PIN, 8);
        put_s(&b, "  VALID ");
        put_dec(&b, s->valid);
        put_s(&b, "/46  PEAK ");
        put_temp(&b, s->peak_mc);
        put_s(&b, "  POLLS ");
        put_dec(&b, s->polls);
        b = new_line(t, EL2Q_WHITE);
        put_s(&b, "  MAX POLL ");
        put_ms(&b, s->max_poll_ticks, s->hz);
        put_s(&b, "  MAX GAP ");
        put_ms(&b, s->max_gap_ticks, s->hz);
        put_s(&b, "  READS ");
        put_dec(&b, s->reads);
        b = new_line(t, EL2Q_CYAN);
        put_s(&b, "  CTRL VERSION  CONTROL   TRDY      VALID");
        for (unsigned i = 0; i < EL2Q_TSENS_BLOCKS; i++) {
            b = new_line(t, s->version[i] == EL2Q_TSENS_PIN ? EL2Q_WHITE : EL2Q_YELLOW);
            put_s(&b, "  ");
            put_dec(&b, i);
            pad(&b, 7, ' ');
            put_hex(&b, s->version[i], 8);
            put_s(&b, "  ");
            put_hex(&b, s->control[i], 8);
            put_s(&b, "  ");
            put_hex(&b, s->trdy[i], 8);
            put_s(&b, "  ");
            put_dec(&b, popcount(s->valid_mask[i]));
        }
    }
    if (s->abort_esr) {
        b = new_line(t, EL2Q_RED);
        put_s(&b, "  READ FAULTED AT ");
        put_hex(&b, s->abort_at, 8);
        put_s(&b, " ESR ");
        put_hex(&b, s->abort_esr, 8);
    }
    if (s->isr & (1u << 8)) {
        b = new_line(t, EL2Q_RED);
        put_s(&b, "  SERROR PENDING AFTER THE READS, ISR_EL1 ");
        put_hex(&b, s->isr, 8);
    }
}

static int vhe_same(const struct el2q_vhe_regs *a, const struct el2q_vhe_regs *b)
{
    return a->hcr == b->hcr && a->sctlr == b->sctlr && a->tcr == b->tcr &&
           a->ttbr0 == b->ttbr0 && a->cptr == b->cptr && a->cnthctl == b->cnthctl;
}

static void vhe_regs(struct lb *b, const char *first, const char *second,
                     const struct el2q_vhe_regs *v, unsigned half)
{
    put_s(b, half ? second : first);
    put_s(b, half ? " TTBR0=" : " HCR=");
    put_hex_short(b, half ? v->ttbr0 : v->hcr);
    put_s(b, half ? " CPTR=" : " SCTLR=");
    put_hex_short(b, half ? v->cptr : v->sctlr);
    put_s(b, half ? " CNTHCTL=" : " TCR=");
    put_hex_short(b, half ? v->cnthctl : v->tcr);
}

void el2q_vhe_line(const struct el2q_run *r, unsigned index, char out[EL2Q_TEXT_COLS + 1])
{
    struct lb b = {out, 0};
    int done = r->vhe_exit == EL2Q_VHE_EXIT_DONE;
    out[0] = 0;
    if (r->vhe_exit == EL2Q_VHE_EXIT_NOT_ASKED) return;
    if (index == 0) {
        put_s(&b, "VHEX ");
        put_s(&b, el2q_vhe_exit_word(r->vhe_exit));
        put_s(&b, " PLAN=");
        put_s(&b, !done ? "-" : vhe_same(&r->vhe_out, &r->vhe_plan) ? "MATCH" : "DIFF");
    } else if (index <= 2) {
        vhe_regs(&b, "VHEIN", "VHEIN2", &r->vhe_in, index - 1);
    } else if (index <= 4 && done) {
        vhe_regs(&b, "VHEOUT", "VHEOUT2", &r->vhe_out, index - 3);
    }
}

void el2q_tsens_line(const struct el2q_run *r, unsigned index, char out[EL2Q_TEXT_COLS + 1])
{
    struct lb b = {out, 0};
    const struct el2q_tsens *s = &r->tsens;
    out[0] = 0;
    if (s->state == EL2Q_TSENS_NOT_RUN) return;
    if (index == 0) {
        put_s(&b, "TSENS ");
        put_s(&b, tsens_state(s->state));
        put_s(&b, " CHECKED=");
        put_dec(&b, s->checked);
        put_s(&b, " UNMAPPED=");
        put_dec(&b, s->unmapped);
        put_s(&b, " ADMIT=");
        if (s->admit < 0) put_c(&b, '-');
        put_dec(&b, (uint64_t)(s->admit < 0 ? -s->admit : s->admit));
        put_s(&b, " POLLS=");
        put_dec(&b, s->polls);
        put_s(&b, " VALID=");
        put_dec(&b, s->valid);
    } else if (index == 1) {
        put_s(&b, "TSENSWHY REASON=");
        put_s(&b, tsens_reason(s->reason));
        put_s(&b, " AT=");
        put_hex_short(&b, s->state == EL2Q_TSENS_UNMAPPED ? s->unmapped_at : s->location);
        put_s(&b, " ESR=");
        put_hex_short(&b, s->abort_esr);
        put_s(&b, " SERROR=");
        put_c(&b, s->isr & (1u << 8) ? '1' : '0');
    } else if (index < 2 + EL2Q_TSENS_BLOCKS) {
        unsigned i = index - 2;
        put_s(&b, "TSENSBLK ");
        put_dec(&b, i);
        put_s(&b, " VER=");
        put_hex_short(&b, s->version[i]);
        put_s(&b, " CTRL=");
        put_hex_short(&b, s->control[i]);
        put_s(&b, " TRDY=");
        put_hex_short(&b, s->trdy[i]);
        put_s(&b, " MASK=");
        put_hex_short(&b, s->valid_mask[i]);
    }
}

void el2q_report(const struct el2q_run *r, struct el2q_text *t)
{
    t->count = 0;
    header(r, t);
    /* Ahead of the CPU tables, so truncation can never cut it off. */
    tsens_section(&r->tsens, t);
    if (r->end_el == 2 && r->cpu_count) {
        verdicts(&r->cpu[0], t);
        cpu_table(r, t);
    } else {
        struct lb b = new_line(t, EL2Q_RED);
        put_s(&b, "NOT AT EL2: ONLY THE STARTING VIEW BELOW");
    }
    id_table(r, t);
    if (r->end_el == 2) differences(r, t);
    if (t->count == EL2Q_TEXT_LINES) {
        t->color[EL2Q_TEXT_LINES - 1] = EL2Q_YELLOW;
        struct lb b = {t->line[EL2Q_TEXT_LINES - 1], 0};
        put_s(&b, "... TRUNCATED");
    }
}

void el2q_raw_line(const struct el2q_run *r, unsigned slot, unsigned index,
                   char out[EL2Q_TEXT_COLS + 1])
{
    struct lb b = {out, 0};
    out[0] = 0;
    if (index >= EL2Q_ID_COUNT) return;
    put_s(&b, "RAW ");
    if (slot >= EL2Q_MAX_CPUS) put_s(&b, "START");
    else put_dec(&b, slot);
    put_c(&b, ' ');
    put_s(&b, el2q_idregs[index].name);
    put_c(&b, ' ');
    put_hex(&b, slot >= EL2Q_MAX_CPUS ? r->start_id[index] : r->cpu[slot].id[index], 16);
}

static char verdict_letter(enum el2q_verdict v)
{
    return v == EL2Q_YES ? 'Y' : v == EL2Q_NO ? 'N' : v == EL2Q_INCONCLUSIVE ? 'I' : '-';
}

static const char *probe_word(uint64_t esr)
{
    return !esr ? "OK" : (esr >> 26) == 0 ? "UD" : "EX";
}

void el2q_summary_line(const struct el2q_run *r, unsigned slot, char out[EL2Q_TEXT_COLS + 1])
{
    struct lb b = {out, 0};
    out[0] = 0;
    if (slot >= r->cpu_count || slot >= EL2Q_MAX_CPUS) return;
    const struct el2q_cpu *c = &r->cpu[slot];
    put_s(&b, "SUM ");
    put_dec(&b, slot);
    put_s(&b, " EL");
    put_dec(&b, c->current_el);
    put_c(&b, ' ');
    put_s(&b, state_word(c->state));
    if (c->state != EL2Q_CPU_DONE) return;
    put_s(&b, " PAC=");
    put_c(&b, verdict_letter(el2q_pauth_signs(c)));
    put_s(&b, " 16K=");
    put_c(&b, verdict_letter(el2q_walks_16k(c)));
    static const struct { const char *name; unsigned crm, op2, lsb, probe; } feats[] = {
        {" NV2=", 7, 2, 24, EL2Q_PROBE_VNCR},   {" ECV=", 7, 0, 60, EL2Q_PROBE_CNTPOFF},
        {" FGT=", 7, 0, 56, EL2Q_PROBE_HFGRTR}, {" RNDR=", 6, 0, 60, EL2Q_PROBE_RNDR},
    };
    for (unsigned i = 0; i < sizeof feats / sizeof feats[0]; i++) {
        put_s(&b, feats[i].name);
        put_dec(&b, field(id_of(c->id, feats[i].crm, feats[i].op2), feats[i].lsb));
        put_c(&b, '/');
        put_s(&b, probe_word(c->probe_esr[feats[i].probe]));
    }
}

static void put_hex_short(struct lb *b, uint64_t v)
{
    unsigned digits = 1;
    while (digits < 16 && v >> (digits * 4)) digits++;
    put_hex(b, v, digits);
}

/* HCR goes last: it is the one field that can grow to 16 digits, and a cut
 * line then loses only it. */
void el2q_gic_line(const struct el2q_run *r, unsigned slot, char out[EL2Q_TEXT_COLS + 1])
{
    struct lb b = {out, 0};
    out[0] = 0;
    if (slot >= r->cpu_count || slot >= EL2Q_MAX_CPUS) return;
    const struct el2q_cpu *c = &r->cpu[slot];
    if (c->state != EL2Q_CPU_DONE) return;
    put_s(&b, "GIC ");
    put_dec(&b, slot);
    put_s(&b, " FRQ=");
    put_dec(&b, c->cntfrq);
    static const struct { const char *name; unsigned probe; } regs[] = {
        {" SRE=", EL2Q_PROBE_ICC_SRE}, {" VTR=", EL2Q_PROBE_ICH_VTR},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        put_s(&b, regs[i].name);
        put_hex_short(&b, c->probe_value[regs[i].probe]);
        put_c(&b, '/');
        put_s(&b, probe_word(c->probe_esr[regs[i].probe]));
    }
    put_s(&b, " LR=");
    put_s(&b, el2q_lr_word(c));
    put_s(&b, " HCR=");
    put_hex_short(&b, c->probe_value[EL2Q_PROBE_ICH_HCR]);
    put_c(&b, '/');
    put_s(&b, probe_word(c->probe_esr[EL2Q_PROBE_ICH_HCR]));
}

/* ---- Font ----------------------------------------------------------------
 * 5x7 glyphs for ASCII 0x20-0x5F, one byte per row, bit 4 leftmost; drawn for
 * this project. Lower case is drawn as upper case. Zero carries a diagonal
 * so a photographed 0 cannot be read as O. */
static const uint8_t font[64][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /*   ! */
    {0x0a,0x0a,0x00,0x00,0x00,0x00,0x00}, {0x0a,0x0a,0x1f,0x0a,0x1f,0x0a,0x0a}, /* " # */
    {0x04,0x0f,0x14,0x0e,0x05,0x1e,0x04}, {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* $ % */
    {0x0c,0x12,0x14,0x08,0x15,0x12,0x0d}, {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, /* & ' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ( ) */
    {0x00,0x04,0x15,0x0e,0x15,0x04,0x00}, {0x00,0x04,0x04,0x1f,0x04,0x04,0x00}, /* * + */
    {0x00,0x00,0x00,0x00,0x0c,0x04,0x08}, {0x00,0x00,0x00,0x1f,0x00,0x00,0x00}, /* , - */
    {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c}, {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, /* . / */
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, /* 0 1 */
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e}, /* 2 3 */
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}, /* 4 5 */
    {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}, {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}, /* 6 7 */
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}, /* 8 9 */
    {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}, {0x00,0x0c,0x0c,0x00,0x0c,0x04,0x08}, /* : ; */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, {0x00,0x00,0x1f,0x00,0x1f,0x00,0x00}, /* < = */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, {0x0e,0x11,0x01,0x02,0x04,0x00,0x04}, /* > ? */
    {0x0e,0x11,0x01,0x0d,0x15,0x15,0x0e}, {0x0e,0x11,0x11,0x11,0x1f,0x11,0x11}, /* @ A */
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, /* B C */
    {0x1c,0x12,0x11,0x11,0x11,0x12,0x1c}, {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, /* D E */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}, /* F G */
    {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}, /* H I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0c}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* J K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, /* L M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, /* N O */
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}, {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, /* P Q */
    {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, /* R S */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, /* T U */
    {0x11,0x11,0x11,0x11,0x11,0x0a,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, /* V W */
    {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, {0x11,0x11,0x11,0x0a,0x04,0x04,0x04}, /* X Y */
    {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}, {0x0e,0x08,0x08,0x08,0x08,0x08,0x0e}, /* Z [ */
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, {0x0e,0x02,0x02,0x02,0x02,0x02,0x0e}, /* \ ] */
    {0x04,0x0a,0x11,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x1f}, /* ^ _ */
};

static const uint8_t *glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c < 0x20 || c > 0x5f) c = '?';
    return font[c - 0x20];
}

void el2q_render(const struct el2q_text *t, uint32_t *fb, uint32_t width,
                 uint32_t height, uint32_t stride, int bgr)
{
    /* 0x00RRGGBB is the in-memory order of the BGR format; RGB swaps R and B. */
    static const uint32_t palette[] = {0xffffff, 0x40ff40, 0xff5050, 0xffff40, 0x40ffff};
    unsigned cols = 1, rows = t->count ? t->count : 1;
    for (unsigned i = 0; i < t->count; i++) {
        unsigned n = 0;
        while (t->line[i][n]) n++;
        if (n > cols) cols = n;
    }
    /* One spare cell of margin on each side. */
    unsigned sx = width / (6 * (cols + 2)), sy = height / (9 * (rows + 2));
    unsigned s = sx < sy ? sx : sy;
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) fb[(size_t)y * stride + x] = 0;
    for (unsigned i = 0; i < t->count; i++) {
        uint32_t color = palette[t->color[i] % 5];
        if (!bgr) color = (color & 0x00ff00) | (color >> 16 & 0xff) | (color & 0xff) << 16;
        unsigned y0 = (i + 1) * 9 * s;
        if (y0 + 7 * s > height) break;
        for (unsigned j = 0; t->line[i][j]; j++) {
            unsigned x0 = (j + 1) * 6 * s;
            if (x0 + 5 * s > width) break;
            const uint8_t *g = glyph(t->line[i][j]);
            for (unsigned row = 0; row < 7; row++)
                for (unsigned bit = 0; bit < 5; bit++) {
                    if (!(g[row] & (0x10 >> bit))) continue;
                    for (unsigned dy = 0; dy < s; dy++)
                        for (unsigned dx = 0; dx < s; dx++)
                            fb[(size_t)(y0 + row * s + dy) * stride + x0 + bit * s + dx] = color;
                }
        }
    }
}
