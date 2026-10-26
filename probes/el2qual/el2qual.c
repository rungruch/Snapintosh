/* SPDX-License-Identifier: MIT */
/* el2qual: the Phase 0A CPU qualification, as a UEFI application.
 *
 * Launched from a boot manager, it records the ID registers at the level
 * firmware runs at, calls ExitBootServices (where slbounce, if installed,
 * switches the boot CPU to EL2), and then measures at EL2, on the boot CPU and
 * on every CPU the MADT lists, what the port decision needs: the ID registers,
 * whether the NV2, ECV, FGT, RNG and PAuth instructions and registers exist,
 * whether pointer signing works, and whether the MMU walks a 16 KiB table
 * (probes/tgran16.c). The report is drawn on the GOP framebuffer and, when
 * asked for and an SPCR PL011 exists, written to that UART.
 *
 * It reads registers and executes test instructions only. It touches no
 * storage, clocks, voltages, GPU or interrupt controller, and ends in a WFE
 * loop: holding the power button ends the run. PSCI is used to start and
 * stop secondary CPUs and, with "poweroff", to end a QEMU run.
 *
 * Load options (space separated, any order):
 *   serial      also write the report to the SPCR UART (PL011/SBSA only)
 *   poweroff    PSCI SYSTEM_OFF after reporting (QEMU runs)
 *   nopac       skip every PAuth key access and PAC instruction
 *   nosmp       measure the boot CPU only
 *   emulate-vhe recreate slbounce's E2H|TGE handoff first; test only, and
 *               refused unless the machine started at EL2 without E2H
 *   exit-vhe    leave slbounce's E2H|TGE state for the non-VHE EL2 q1n1 and
 *               the monitor assume (vhe.c plans it, el2q_exit_vhe switches),
 *               then measure there. Refused, with the reason reported, if
 *               the entry state is not one the plan recognises. Off by
 *               default.
 *   emulate-rotated  treat the firmware's GOP mode as having no framebuffer
 *               address, as the gaokun3's does; test only
 *   emulate-no-fb    treat every GOP mode that way; test only
 *   emulate-dt-fb    describe the firmware's real GOP framebuffer as if it
 *               were the device tree's /chosen simple-framebuffer; test only
 *   thermal     after the CPU measurements, run the SC8280XP tsens guard
 *               (thermal.c) on the boot CPU for 12 s: read only, and only
 *               the guard's 58 words, each checked against the firmware's
 *               page tables before the first read. Off by default.
 *   usb         read whether the gaokun3's USB0 controller is powered, from
 *               the clock controller's status words, before and after
 *               ExitBootServices; after it, and only if powered and
 *               clocked, the controller's own state (usbprobe.c). Read
 *               only. Off by default.
 *   usb-smmu    "usb", then also apps_smmu's state for USB0's stream and,
 *               when it translates, the context bank it names, and the
 *               stage-2 bank that one nests, if any. Off by default.
 *   usb-smmu-map  "usb-smmu", but first add the SMMU's 1 MiB register
 *               window to el2qual's own EL2 tables as Device, read-only
 *               identity pages (pagemap.c), so the nested stage-2 bank the
 *               UEFI tables leave unmapped can be read too. Writes RAM (one
 *               table page of el2qual's own and one descriptor in UEFI's
 *               leftover tables), never hardware. Off by default.
 *   emulate-map-hole  with "usb-smmu-map", map two test windows QEMU's
 *               EDK2 leaves unmapped instead of the SMMU's, so the map is
 *               written and checked there; test only, reported as such
 *   usb-s2-walk "usb-smmu", then walk the stage-2 bank's tables for a 1 MiB
 *               DMA arena allocated below 4 GiB as q1n1 allocates its own
 *               (s2walk.c): descriptor reads only, through mappings UEFI's
 *               tables already have, stopping at the first fault or
 *               SError. Off by default.
 *   usb-s2-survey  "usb-s2-walk", then survey that stage 2's top two
 *               levels: every level-1 entry, and the level-2 table under
 *               the arena (s2walk.c), with the walk's guards. Off by
 *               default.
 *   emulate-s2-tables  with "usb-s2-walk", walk a stage-2 table set
 *               el2qual builds for its arena instead of the SMMU's, every
 *               page identity read-write; test only, reported as such
 *   emulate-s2-odd     the same, with arena pages 1 to 3 moved, invalid
 *               and read-only; test only
 *   emulate-s2-device  the same tables, but the root pointed at QEMU's
 *               PL011, a Device page the walk must refuse unread; test only
 *
 * With no GOP mode that has a framebuffer address, el2qual draws on the
 * device tree's /chosen simple-framebuffer if the firmware's memory map
 * reserves it (report.c). With neither and no "serial" UART, nothing could
 * show the results, so el2qual says so on the firmware console and stops
 * before ExitBootServices: no Secure Launch is attempted for nothing. */
#include <stddef.h>
#include <stdint.h>
#include "el2qual.h"
#include "report.h"
#include "thermal.h"
#include "vhe.h"
#include "pagemap.h"
#include "usbprobe.h"
#include "s2walk.h"
#include "uefi.h"

/* idregs.h is the monitor's: id_register() is shared so both read exactly
 * the allocated encodings; its id_forge() is not used here. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "../idregs.h"
#pragma clang diagnostic pop

/* ---- The assembly contract (el2qual.h) ---------------------------------- */

struct el2q_ctx {
    uint64_t fixup, fixup_esr;
    uint64_t fatal_esr, fatal_elr, fatal_far, fatal_spsr, fatal_vector;
    uint64_t slot;
};
_Static_assert(offsetof(struct el2q_ctx, fixup) == CTX_FIXUP, "el2q_ctx");
_Static_assert(offsetof(struct el2q_ctx, fixup_esr) == CTX_FIXUP_ESR, "el2q_ctx");
_Static_assert(offsetof(struct el2q_ctx, fatal_esr) == CTX_FATAL_ESR, "el2q_ctx");
_Static_assert(offsetof(struct el2q_ctx, fatal_elr) == CTX_FATAL_ELR, "el2q_ctx");
_Static_assert(offsetof(struct el2q_ctx, fatal_far) == CTX_FATAL_FAR, "el2q_ctx");
_Static_assert(offsetof(struct el2q_ctx, fatal_spsr) == CTX_FATAL_SPSR, "el2q_ctx");
_Static_assert(offsetof(struct el2q_ctx, fatal_vector) == CTX_FATAL_VECTOR, "el2q_ctx");
_Static_assert(offsetof(struct el2q_ctx, slot) == CTX_SLOT, "el2q_ctx");
_Static_assert(sizeof(struct el2q_ctx) == CTX_SIZE && CTX_SIZE == 1 << CTX_SHIFT, "el2q_ctx");

struct el2q_boot {
    uint64_t hcr, mair, tcr, ttbr0, sctlr, cptr, vbar, stacks, ctx, early;
};
_Static_assert(offsetof(struct el2q_boot, hcr) == BOOT_HCR, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, mair) == BOOT_MAIR, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, tcr) == BOOT_TCR, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, ttbr0) == BOOT_TTBR0, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, sctlr) == BOOT_SCTLR, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, cptr) == BOOT_CPTR, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, vbar) == BOOT_VBAR, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, stacks) == BOOT_STACKS, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, ctx) == BOOT_CTX, "el2q_boot");
_Static_assert(offsetof(struct el2q_boot, early) == BOOT_EARLY, "el2q_boot");
_Static_assert(sizeof(struct el2q_boot) == BOOT_SIZE, "el2q_boot");

_Static_assert(offsetof(struct el2q_vhe_regs, hcr) == VHE_HCR, "el2q_vhe_regs");
_Static_assert(offsetof(struct el2q_vhe_regs, sctlr) == VHE_SCTLR, "el2q_vhe_regs");
_Static_assert(offsetof(struct el2q_vhe_regs, tcr) == VHE_TCR, "el2q_vhe_regs");
_Static_assert(offsetof(struct el2q_vhe_regs, ttbr0) == VHE_TTBR0, "el2q_vhe_regs");
_Static_assert(offsetof(struct el2q_vhe_regs, cptr) == VHE_CPTR, "el2q_vhe_regs");
_Static_assert(offsetof(struct el2q_vhe_regs, cnthctl) == VHE_CNTHCTL, "el2q_vhe_regs");
_Static_assert(sizeof(struct el2q_vhe_regs) == VHE_SIZE, "el2q_vhe_regs");

extern char el2q_vectors[];
void el2q_secondary_entry(void);
uint64_t el2q_smc(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3);
void el2q_use_spx(void);
void el2q_enter_vhe(uint64_t tcr);
void el2q_exit_vhe(const struct el2q_vhe_regs *plan);
uint64_t el2q_try_rndr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_vncr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_cntpoff(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_hfgrtr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_apiakey(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_icc_sre(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_ich_vtr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_ich_hcr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_ich_lr0(struct el2q_ctx *ctx, uint64_t out[2], uint64_t pattern);
uint64_t el2q_try_pacga(struct el2q_ctx *ctx, uint64_t *value, uint64_t pointer, uint64_t modifier);
uint64_t el2q_try_pac(struct el2q_ctx *ctx, const uint64_t *in, const uint64_t *modifier,
                      uint64_t *out, uint64_t *round_trip, uint64_t count);
uint64_t el2q_try_read32(struct el2q_ctx *ctx, uint64_t *value, uint64_t address);
uint64_t el2q_try_read64(struct el2q_ctx *ctx, uint64_t *value, uint64_t address);
void el2q_apia_get(uint64_t key[2]);
void el2q_apia_set(uint64_t lo, uint64_t hi);
void tgran16_probe(uint64_t *out, uint64_t table);       /* probes/tgran16.c */
void tgran16_probe_el12(uint64_t *out, uint64_t table);  /* same, -DTGRAN16_EL12 */

void el2q_secondary_main(uint64_t slot) __attribute__((noreturn));
void el2q_fatal(struct el2q_ctx *ctx) __attribute__((noreturn));

/* ---- UEFI: the GUIDs looked up (the types are in uefi.h) ------------------ */

static const struct efi_guid acpi20_guid =
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static const struct efi_guid dtb_guid =
    {0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};
static const struct efi_guid gop_guid =
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static const struct efi_guid loaded_image_guid =
    {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
/* SMBIOS3_TABLE_GUID and SMBIOS_TABLE_GUID (UEFI 2.10, section 4.6). */
static const struct efi_guid smbios3_guid =
    {0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}};
static const struct efi_guid smbios_guid =
    {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

/* ---- Freestanding runtime ------------------------------------------------
 * The compiler may emit calls to these for structure copies and clears. */

__attribute__((no_builtin)) void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

__attribute__((no_builtin)) void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static int guid_eq(const struct efi_guid *a, const struct efi_guid *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (unsigned i = 0; i < sizeof *a; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

/* ---- Architecture ----------------------------------------------------------- */

#define RD(reg) ({ uint64_t v_; __asm__ volatile("mrs %0, " reg : "=r"(v_)); v_; })
#define WR(reg, v) __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(v)))
#define ISB() __asm__ volatile("isb" ::: "memory")
#define DSB() __asm__ volatile("dsb sy" ::: "memory")

#define SCTLR_EL2_ENIA (1ull << 31)
#define MPIDR_AFFINITY 0xff00ffffffull

#define PSCI_VERSION       0x84000000u
#define PSCI_CPU_OFF       0x84000002u
#define PSCI_SYSTEM_OFF    0x84000008u
#define PSCI_CPU_ON64      0xc4000003u
#define PSCI_AFFINITY_INFO 0xc4000004u
#define PSCI_AFFINITY_OFF  1

static uint64_t current_el(void) { return (RD("CurrentEL") >> 2) & 3; }

static uint64_t ticks(void)
{
    ISB();
    return RD("cntpct_el0");
}

/* PSCI returns a 32-bit signed value in w0. */
static int64_t psci(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
    return (int32_t)el2q_smc(function, a1, a2, a3);
}

enum cache_op { CLEAN, CLEAN_INVALIDATE, INVALIDATE };

static void dcache_range(const volatile void *start, size_t length, enum cache_op op)
{
    uint64_t line = 4ull << ((RD("ctr_el0") >> 16) & 15);
    uintptr_t p = (uintptr_t)start & ~(line - 1), end = (uintptr_t)start + length;
    DSB();
    for (; p < end; p += line) {
        if (op == CLEAN) __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
        else if (op == CLEAN_INVALIDATE) __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    }
    DSB();
}

/* ---- State ------------------------------------------------------------------ */

static struct el2q_run run;
static struct el2q_text text;
static struct {
    int serial, poweroff, nopac, nosmp, emulate_vhe, emulate_rotated, emulate_no_fb, emulate_dt_fb;
    int thermal, exit_vhe, usb, usb_smmu, usb_smmu_map, emulate_map_hole;
    int usb_s2_walk, usb_s2_survey, emulate_s2_tables, emulate_s2_odd, emulate_s2_device;
} opt;
/* clean: the display reads this buffer from memory, not through the CPU's
 * caches, and nothing says how firmware mapped it, so every draw is cleaned
 * to the point of coherency (a GOP framebuffer's mapping is the firmware's
 * business). */
static struct {
    uint32_t *base;
    int bgr, gop, clean;
} screen;
static const void *fdt_table;
/* The firmware's own mode, kept for emulate-dt-fb before any emulate-*
 * option hides its address. */
static struct el2q_simplefb gop_real;
static struct efi_text_output *con_out;
static uintptr_t image_base;
static uint64_t image_size, table;

struct el2q_boot el2q_boot;                         /* read by the secondary entry */
static _Alignas(64) struct el2q_ctx ctx[EL2Q_MAX_CPUS];
/* Written by secondaries with the MMU off; one line each (el2qual.h). */
static _Alignas(64) volatile uint64_t early[EL2Q_MAX_CPUS][1 << EL2Q_EARLY_SHIFT >> 3];
static _Alignas(16) uint8_t stacks[EL2Q_MAX_CPUS][EL2Q_STACK_SIZE];
static _Alignas(8) uint8_t memory_map[128 * 1024];

/* ---- Output ------------------------------------------------------------------ */

static void console_line(const char *s)
{
    if (!con_out) return;
    char16 buf[EL2Q_TEXT_COLS + 3];
    unsigned n = 0;
    while (*s && n < EL2Q_TEXT_COLS) buf[n++] = (char16)*s++;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    con_out->output_string(con_out, buf);
}

static void draw(void)
{
    if (!screen.base) return;
    el2q_render(&text, screen.base, run.fb_width, run.fb_height, run.fb_stride, screen.bgr);
    /* el2q_render rewrites every pixel, so every line is cleaned. */
    if (screen.clean)
        dcache_range(screen.base, (size_t)run.fb_stride * run.fb_height * 4, CLEAN);
}

/* A breadcrumb on screen (and console, while boot services last) before each
 * step that could hang or reset the machine, so the last one visible is the
 * step that did. */
static void progress(const char *s)
{
    if (text.count < EL2Q_TEXT_LINES) {
        unsigned i = 0;
        text.color[text.count] = EL2Q_YELLOW;
        while (s[i] && i < EL2Q_TEXT_COLS) {
            text.line[text.count][i] = s[i];
            i++;
        }
        text.line[text.count++][i] = 0;
    }
    console_line(s);
    draw();
}

/* SPCR interface types 3 (ARM PL011) and 0x0e (SBSA generic) share the data
 * and flag registers used here; nothing else is written. */
static volatile uint32_t *uart(void)
{
    const struct el2q_acpi *a = &run.acpi;
    if (!opt.serial || !a->have_spcr || a->uart_space != 0 || !a->uart_base) return 0;
    if (a->uart_type != 0x03 && a->uart_type != 0x0e) return 0;
    return (volatile uint32_t *)(uintptr_t)a->uart_base;
}

static void uart_line(volatile uint32_t *u, const char *s)
{
    for (int done = 0; !done;) {
        char c = *s ? *s++ : (done = 1, '\n');
        for (uint32_t spin = 0; (u[0x18 / 4] & (1u << 5)) && spin < 1000000; spin++) {}
        u[0] = (uint8_t)c;
    }
}

static void serial_report(void)
{
    volatile uint32_t *u = uart();
    if (!u) return;
    char line[EL2Q_TEXT_COLS + 1];
    uart_line(u, "EL2QUAL BEGIN");
    for (unsigned i = 0; i < text.count; i++) uart_line(u, text.line[i]);
    for (unsigned s = 0; s < run.cpu_count; s++) {
        el2q_summary_line(&run, s, line);
        uart_line(u, line);
        el2q_gic_line(&run, s, line);
        if (line[0]) uart_line(u, line);
    }
    for (unsigned i = 0;; i++) {
        el2q_vhe_line(&run, i, line);
        if (!line[0]) break;
        uart_line(u, line);
    }
    for (unsigned i = 0;; i++) {
        el2q_tsens_line(&run, i, line);
        if (!line[0]) break;
        uart_line(u, line);
    }
    static const char digits[] = "0123456789";
    char dt[] = "DT STATE ?";
    dt[9] = digits[run.dt_state % 10];
    uart_line(u, dt);
    for (unsigned i = 0; i < EL2Q_ID_COUNT; i++) {
        el2q_raw_line(&run, EL2Q_MAX_CPUS, i, line);
        uart_line(u, line);
    }
    for (unsigned s = 0; s < run.cpu_count; s++) {
        if (run.cpu[s].state != EL2Q_CPU_DONE) continue;
        for (unsigned i = 0; i < EL2Q_ID_COUNT; i++) {
            el2q_raw_line(&run, s, i, line);
            uart_line(u, line);
        }
    }
    uart_line(u, "EL2QUAL END");
}

__attribute__((noreturn)) static void finish(void)
{
    el2q_report(&run, &text);
    serial_report();
    draw();
    if (opt.poweroff && run.end_el == 2) psci(PSCI_SYSTEM_OFF, 0, 0, 0);
    __asm__ volatile("msr daifset, #15");
    for (;;) __asm__ volatile("wfe");
}

/* ---- Measurement -------------------------------------------------------------- */

/* Fixed keys and inputs make every core's signed pointers comparable: the
 * same algorithm gives the same answer on each. */
#define TEST_KEY_LO 0x0123456789abcdefull
#define TEST_KEY_HI 0xfedcba9876543210ull
static const uint64_t pac_pointers[EL2Q_PAC_TRIALS] = {
    0x0000000040001000ull, 0x0000000040fe2a40ull, 0x0000000012345678ull, 0x000000007ffff000ull,
};
static const uint64_t pac_modifiers[EL2Q_PAC_TRIALS] = {
    0, 1, 0x5a5a5a5a5a5a5a5aull, 0xffffffffffffffffull,
};

static void measure_pac(unsigned slot)
{
    struct el2q_cpu *c = &run.cpu[slot];
    struct el2q_ctx *x = &ctx[slot];
    uint64_t saved[2] = {0, 0};
    if (!slot) progress("PAC: READING APIAKEYLO_EL1. A HANG OR RESET HERE IS EL3");
    c->probe_esr[EL2Q_PROBE_APIAKEY] = el2q_try_apiakey(x, &c->probe_value[EL2Q_PROBE_APIAKEY]);
    int keys = !c->probe_esr[EL2Q_PROBE_APIAKEY];
    if (keys) {
        el2q_apia_get(saved);
        el2q_apia_set(TEST_KEY_LO, TEST_KEY_HI);
    }
    /* With EnIA clear PACIA* return the pointer unchanged, so the question
     * needs it set. Only this code runs while it is. */
    uint64_t sctlr = RD("sctlr_el2");
    WR("sctlr_el2", sctlr | SCTLR_EL2_ENIA);
    ISB();
    if (!slot) progress("PAC: SIGNING AT EL2. A HANG OR RESET HERE IS EL3");
    for (unsigned i = 0; i < EL2Q_PAC_TRIALS; i++) c->pac_in[i] = pac_pointers[i];
    c->pac_rt_esr = el2q_try_pac(x, c->pac_in, pac_modifiers, c->pac_out, c->pac_rt,
                                 EL2Q_PAC_TRIALS);
    c->pac_rt_ran = 1;
    c->probe_esr[EL2Q_PROBE_PACGA] = el2q_try_pacga(x, &c->probe_value[EL2Q_PROBE_PACGA],
                                                    pac_pointers[0], pac_modifiers[1]);
    WR("sctlr_el2", sctlr);
    ISB();
    if (keys) el2q_apia_set(saved[0], saved[1]);
    c->pac_ran = 1;
}

/* The GICv3 virtual CPU interface, read only, then list register 0 written
 * and restored. The write runs only where it cannot be seen: EL2 reaches the
 * system registers (ICC_SRE_EL2.SRE), the interface exists, and it is off
 * (ICH_HCR_EL2.En clear). The pattern's state is invalid, so it could not
 * signal even if the interface were on. */
static void measure_gic(unsigned slot)
{
    struct el2q_cpu *c = &run.cpu[slot];
    struct el2q_ctx *x = &ctx[slot];
    if (!slot) progress("GIC: READING ICH_*_EL2. A HANG OR RESET HERE IS EL3");
    c->probe_esr[EL2Q_PROBE_ICC_SRE] = el2q_try_icc_sre(x, &c->probe_value[EL2Q_PROBE_ICC_SRE]);
    c->probe_esr[EL2Q_PROBE_ICH_VTR] = el2q_try_ich_vtr(x, &c->probe_value[EL2Q_PROBE_ICH_VTR]);
    c->probe_esr[EL2Q_PROBE_ICH_HCR] = el2q_try_ich_hcr(x, &c->probe_value[EL2Q_PROBE_ICH_HCR]);
    if (c->probe_esr[EL2Q_PROBE_ICC_SRE] || !(c->probe_value[EL2Q_PROBE_ICC_SRE] & 1) ||
        c->probe_esr[EL2Q_PROBE_ICH_VTR] || c->probe_esr[EL2Q_PROBE_ICH_HCR] ||
        (c->probe_value[EL2Q_PROBE_ICH_HCR] & 1))
        return;
    uint64_t lr[2] = {0, 0};
    if (!slot) progress("GIC: WRITING, RESTORING ICH_LR0_EL2 (INVALID ENTRY)");
    c->lr_esr = el2q_try_ich_lr0(x, lr, EL2Q_LR_PATTERN);
    c->lr_saved = lr[0];
    c->lr_back = lr[1];
    c->lr_ran = 1;
}

/* Everything one CPU contributes, run on that CPU at EL2. */
static void measure(unsigned slot)
{
    struct el2q_cpu *c = &run.cpu[slot];
    struct el2q_ctx *x = &ctx[slot];
    c->current_el = current_el();
    c->mpidr = RD("mpidr_el1");
    c->hcr = RD("hcr_el2");
    c->cntfrq = RD("cntfrq_el0");
    for (unsigned i = 0; i < EL2Q_ID_COUNT; i++)
        c->id[i] = id_register(el2q_idregs[i].crm, el2q_idregs[i].op2);
    if (!slot) progress("PROBING RNDR, VNCR_EL2, CNTPOFF_EL2, HFGRTR_EL2");
    c->probe_esr[EL2Q_PROBE_RNDR] = el2q_try_rndr(x, &c->probe_value[EL2Q_PROBE_RNDR]);
    c->probe_esr[EL2Q_PROBE_VNCR] = el2q_try_vncr(x, &c->probe_value[EL2Q_PROBE_VNCR]);
    c->probe_esr[EL2Q_PROBE_CNTPOFF] = el2q_try_cntpoff(x, &c->probe_value[EL2Q_PROBE_CNTPOFF]);
    c->probe_esr[EL2Q_PROBE_HFGRTR] = el2q_try_hfgrtr(x, &c->probe_value[EL2Q_PROBE_HFGRTR]);
    /* tgran16 programs the EL1&0 regime; with E2H set only the _EL12
     * encodings reach it (probes/tgran16.c). */
    if (!slot) progress("16K WALK (PROBES/TGRAN16.C)");
    if (c->hcr & EL2Q_HCR_E2H) tgran16_probe_el12(c->tgran, table);
    else tgran16_probe(c->tgran, table);
    measure_gic(slot);
    if (!opt.nopac) measure_pac(slot);
}

/* ---- The VHE exit (option "exit-vhe"; vhe.c) ------------------------------
 * The boot CPU at EL2, alone, interrupts masked, VBAR_EL2 already ours (the
 * switch must not take an exception into slbounce's leftover EL1 vectors). */

static struct el2q_vhe_regs read_vhe_regs(void)
{
    return (struct el2q_vhe_regs){
        .hcr = RD("hcr_el2"), .sctlr = RD("sctlr_el2"), .tcr = RD("tcr_el2"),
        .ttbr0 = RD("ttbr0_el2"), .cptr = RD("cptr_el2"), .cnthctl = RD("cnthctl_el2"),
    };
}

static void exit_vhe(void)
{
    run.vhe_in = read_vhe_regs();
    run.vhe_exit = el2q_vhe_exit_plan(&run.vhe_in, &run.vhe_plan);
    if (run.vhe_exit != EL2Q_VHE_EXIT_DONE) {
        progress("VHE EXIT REFUSED: STAYING IN SLBOUNCE'S E2H STATE");
        return;
    }
    progress("VHE EXIT: CLEARING E2H. A HANG OR RESET HERE IS THE SWITCH");
    /* EL2's own timers. Whatever UEFI or slbounce left armed would fire once
     * interrupts are unmasked, and after the switch the EL2 virtual timer is
     * no longer reachable through the CNTV names. CNTHV_CTL_EL2, by encoding
     * for the assembler, exists with FEAT_VHE, which E2H being set proves. */
    WR("cnthp_ctl_el2", 0);
    WR("s3_4_c14_c3_1", 0);
    ISB();
    el2q_exit_vhe(&run.vhe_plan);
    run.vhe_out = read_vhe_regs();
    progress("VHE EXIT: NON-VHE EL2");
}

/* ---- The USB read (options "usb", "usb-smmu"; usbprobe.c) --------------------
 * Before ExitBootServices the exception vectors are the firmware's, so a read
 * that faults cannot be caught: only the clock controller's status words are
 * read then, which UEFI's own clock driver reads all the time, each after a
 * mapping check at the current level. After it, el2qual's fault fixup guards
 * every read, and the controller itself is read only if those words say it
 * is powered and clocked. */

static int tsens_mapped(void *unused, uint64_t address, uint64_t *par);

static int usb_mapped_now(void *unused, uint64_t address)
{
    (void)unused;
    if (current_el() == 2) __asm__ volatile("at s1e2r, %0" :: "r"(address) : "memory");
    else __asm__ volatile("at s1e1r, %0" :: "r"(address) : "memory");
    ISB();
    uint64_t p = RD("par_el1");
    return (p & 1) || ((p >> 56) & 0xf0) || ((p ^ address) & 0x0000fffffffff000ull);
}

static uint64_t usb_read_unguarded(void *unused, uint64_t address, uint64_t *value)
{
    (void)unused;
    *value = *(volatile uint32_t *)(uintptr_t)address;
    return 0;
}

static int usb_mapped_el2(void *unused, uint64_t address)
{
    uint64_t par;
    return tsens_mapped(unused, address, &par);
}

static uint64_t usb_read_guarded(void *unused, uint64_t address, uint64_t *value)
{
    (void)unused;
    return el2q_try_read32(&ctx[0], value, address);
}

/* ---- The private SMMU window map (option "usb-smmu-map"; pagemap.c) --------
 * The table page comes from a pool allocated before ExitBootServices, only
 * when the option is given. The only stores into a pre-existing page are to
 * the level-2 descriptor; after ExitBootServices all memory is el2qual's,
 * and only the boot CPU runs. */

static uint64_t pm_pool_next, pm_pool_end;

static uint64_t pm_alloc(void *ctx)
{
    (void)ctx;
    if (!pm_pool_next || pm_pool_next >= pm_pool_end) return 0;
    uint64_t page = pm_pool_next;
    pm_pool_next += 0x1000;
    return page;
}

static uint64_t pm_read64(void *ctx, uint64_t address)
{
    (void)ctx;
    return *(volatile uint64_t *)(uintptr_t)address;
}

static void pm_write64(void *ctx, uint64_t address, uint64_t value)
{
    (void)ctx;
    *(volatile uint64_t *)(uintptr_t)address = value;
}

static void pm_sync(void *ctx, uint32_t op, uint64_t address, uint64_t size)
{
    (void)ctx;
    if (op == EL2Q_PM_CLEAN) dcache_range((const volatile void *)(uintptr_t)address, size, CLEAN);
    else if (op == EL2Q_PM_DSB) DSB();
    else if (op == EL2Q_PM_TLBI) __asm__ volatile("tlbi alle2" ::: "memory");
    else if (op == EL2Q_PM_ISB) ISB();
}

/* "emulate-map-hole" (test only): on QEMU's virt machine EDK2 already maps
 * the SMMU window's addresses as Device, so the map would never be written.
 * Two 1 MiB windows in flash0's space past EDK2's firmware volume, which its
 * tables leave absent (OBSERVED), are mapped instead: the first gets a new
 * level-3 table; the second, in the same 2 MiB region, then clones that one,
 * so the swap of a valid entry with its break runs too. The first window
 * must still pass afterwards. Nothing reads either. */
#define EL2Q_PM_TEST_FIRST  0x02000000ull
#define EL2Q_PM_TEST_SECOND 0x02100000ull
#define EL2Q_PM_TEST_SIZE   0x100000ull

static uint32_t map_window(uint64_t base, uint64_t size, struct el2q_pm_result *r)
{
    const struct el2q_pm_hw pmhw = {0, pm_read64, pm_write64, pm_alloc, usb_mapped_el2, pm_sync};
    uint32_t state = el2q_pm_prepare(base, size, RD("tcr_el2"), RD("ttbr0_el2"), RD("mair_el2"),
                                     !!(RD("hcr_el2") & EL2Q_HCR_E2H), &pmhw, r);
    if (state == EL2Q_PM_BUILT) {
        el2q_pm_publish(&pmhw, r);
        state = el2q_pm_verify(base, size, &pmhw, r);
    }
    return state;
}

/* Build the clone, swap it in with break-before-make (pagemap.c), then
 * every window page must pass the stage's own AT check. */
static void map_smmu_window(void)
{
    struct el2q_pm_result r;
    uint32_t state;
    if (opt.emulate_map_hole) {
        progress("USB: MAPPING TWO TEST WINDOWS IN EL2QUAL'S OWN TABLES (TEST ONLY)");
        state = map_window(EL2Q_PM_TEST_FIRST, EL2Q_PM_TEST_SIZE, &r);
        if (state == EL2Q_PM_OK) state = map_window(EL2Q_PM_TEST_SECOND, EL2Q_PM_TEST_SIZE, &r);
        if (state == EL2Q_PM_OK) {
            const struct el2q_pm_hw pmhw = {0, pm_read64, pm_write64, pm_alloc, usb_mapped_el2, pm_sync};
            struct el2q_pm_result first = r;
            if (el2q_pm_verify(EL2Q_PM_TEST_FIRST, EL2Q_PM_TEST_SIZE, &pmhw, &first) != EL2Q_PM_OK) {
                state = EL2Q_PM_REFUSED_VERIFY;
                r.mapped_after = first.mapped_after;
            }
        }
    } else {
        progress("USB: MAPPING APPS_SMMU'S WINDOW IN EL2QUAL'S OWN TABLES");
        state = map_window(EL2Q_USB_SMMU_BASE, EL2Q_USB_SMMU_SIZE, &r);
    }
    run.usb.map = state;
    run.usb.map_test = opt.emulate_map_hole;
    run.usb.map_cloned = r.cloned;
    run.usb.map_kept = r.mapped_before;
    run.usb.map_arg = state == EL2Q_PM_REFUSED_VERIFY ? r.mapped_after : r.detail;
}

/* ---- The stage-2 walk (option "usb-s2-walk"; s2walk.c) --------------------
 * The arena is allocated before ExitBootServices exactly as q1n1's usb-ebs.c
 * allocates its DWC3 arena: EfiLoaderData pages below 4 GiB. The walk then
 * reads descriptors only, each behind the fault fixup, and only through
 * mappings UEFI's tables already have. */

static uint64_t s2w_arena, s2w_pool;

/* "emulate-s2-device": QEMU virt's PL011, a page EDK2 maps as Device. */
#define EL2Q_S2W_TEST_DEVICE_ROOT 0x09000000ull

static uint64_t s2w_par(void *unused, uint64_t address)
{
    (void)unused;
    __asm__ volatile("at s1e2r, %0" :: "r"(address) : "memory");
    ISB();
    return RD("par_el1");
}

static uint64_t s2w_read64(void *unused, uint64_t address, uint64_t *value)
{
    (void)unused;
    return el2q_try_read64(&ctx[0], value, address);
}

/* An external abort on a load the core does not report synchronously
 * becomes a pending SError once the load completes; the DSB waits for that,
 * and DAIF.A, set since ExitBootServices, keeps it pending to be seen here
 * rather than taken. */
static int s2w_serror(void *unused)
{
    (void)unused;
    DSB();
    ISB();
    return (RD("isr_el1") >> 8) & 1;
}

static void s2w_write64(void *unused, uint64_t address, uint64_t value)
{
    (void)unused;
    *(volatile uint64_t *)(uintptr_t)address = value;
}

static void walk_stage2(void)
{
    const struct el2q_s2w_hw hw = {0, s2w_par, s2w_read64, s2w_serror};
    struct el2q_s2w_bank bank, *walked = 0;
    if (opt.emulate_s2_tables) {
        progress("USB: WALKING EL2QUAL'S OWN TEST STAGE-2 TABLES (TEST ONLY)");
        if (s2w_pool && !el2q_s2w_build_test(s2w_arena, s2w_pool, opt.emulate_s2_odd, s2w_write64, 0, &bank)) {
            if (opt.emulate_s2_device) bank.ttbr = EL2Q_S2W_TEST_DEVICE_ROOT;
            walked = &bank;
        }
    } else {
        progress("USB: STAGE-2 WALK, READ ONLY. A HANG OR RESET HERE IS THE WALK");
        if (run.usb.s2 == EL2Q_USB_CB_READ) {
            bank = (struct el2q_s2w_bank){run.usb.s2_sctlr, run.usb.s2_tcr, run.usb.s2_cba2r, run.usb.s2_ttbr0};
            walked = &bank;
        }
    }
    el2q_s2w_walk(walked, s2w_arena, &hw, &run.usb.walk);
    run.usb.walk.test = opt.emulate_s2_tables;
    if (opt.usb_s2_survey) {
        progress("USB: STAGE-2 SURVEY, READ ONLY. A HANG OR RESET HERE IS IT");
        el2q_s2w_survey(walked, s2w_arena, &hw, &run.usb.survey);
        run.usb.survey.test = opt.emulate_s2_tables;
    }
}

static void measure_usb_before(void)
{
    const struct el2q_usb_hw hw = {0, usb_mapped_now, usb_read_unguarded};
    progress("USB: GCC STATUS BEFORE EXITBOOTSERVICES, READ ONLY");
    el2q_usb_gcc(&run.usb.before, &hw);
}

static void measure_usb_after(void)
{
    const struct el2q_usb_hw hw = {0, usb_mapped_el2, usb_read_guarded};
    progress("USB: GCC STATUS AFTER EXITBOOTSERVICES, READ ONLY");
    el2q_usb_gcc(&run.usb.after, &hw);
    if (run.usb.after.power == EL2Q_USB_POWER_ON)
        progress("USB: READING THE USB0 CONTROLLER. A HANG HERE IS A USB0 READ");
    el2q_usb_core(&run.usb.after, &hw);
    if (opt.usb_smmu) {
        if (opt.usb_smmu_map) map_smmu_window();
        progress("USB: READING APPS_SMMU. A HANG HERE IS AN SMMU READ");
        el2q_usb_smmu(&run.usb, &hw);
        if (opt.usb_s2_walk) walk_stage2();
    }
    run.usb.isr = RD("isr_el1");
}

/* ---- The tsens read (option "thermal"; thermal.c) ------------------------
 * The boot CPU at EL2, every other core off, interrupts masked, the
 * firmware's page tables still in place. */

/* AT S1E2R asks the walk the read would make, without making the access:
 * with E2H set, as slbounce leaves it, that is the EL2&0 regime. A word
 * counts as mapped only as Device memory (PAR_EL1.ATTR high nibble 0) at its
 * own physical address, the identity map UEFI uses. */
static int tsens_mapped(void *unused, uint64_t address, uint64_t *par)
{
    (void)unused;
    __asm__ volatile("at s1e2r, %0" :: "r"(address) : "memory");
    ISB();
    uint64_t p = RD("par_el1");
    *par = p;
    if (p & 1) return 1;
    if ((p >> 56) & 0xf0) return 2;
    if ((p ^ address) & 0x0000fffffffff000ull) return 3;
    return 0;
}

static uint64_t tsens_read(void *unused, uint64_t address, uint64_t *value)
{
    (void)unused;
    return el2q_try_read32(&ctx[0], value, address);
}

static uint64_t tsens_counter(void *unused)
{
    (void)unused;
    return ticks();
}

static uint64_t tsens_isr(void *unused)
{
    (void)unused;
    return RD("isr_el1");
}

static void measure_tsens(void)
{
    uint64_t hz = RD("cntfrq_el0");
    const struct el2q_tsens_hw hw = {0, tsens_mapped, tsens_read, tsens_counter, tsens_isr,
                                     hz, hz * EL2Q_TSENS_WINDOW_S};
    /* One breadcrumb before all of it: drawing between polls could delay
     * one past the guard's one-second gap rule. */
    progress("TSENS: 58 WORDS, READ ONLY, 12 S. A HANG HERE IS A TSENS READ");
    el2q_tsens_run(&run.tsens, &hw);
}

void el2q_secondary_main(uint64_t slot)
{
    measure((unsigned)slot);
    run.cpu[slot].state = EL2Q_CPU_DONE;
    __atomic_store_n(&run.cpu[slot].done, 1, __ATOMIC_RELEASE);
    psci(PSCI_CPU_OFF, 0, 0, 0);
    for (;;) __asm__ volatile("wfe");
}

void el2q_fatal(struct el2q_ctx *x)
{
    unsigned slot = (unsigned)x->slot;
    struct el2q_cpu *c = &run.cpu[slot];
    c->fault_esr = x->fatal_esr;
    c->fault_elr = x->fatal_elr;
    c->fault_far = x->fatal_far;
    c->fault_vector = x->fatal_vector;
    c->state = EL2Q_CPU_FAULT;
    if (slot) {
        __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
        psci(PSCI_CPU_OFF, 0, 0, 0);
        for (;;) __asm__ volatile("wfe");
    }
    finish();
}

static void put_hex(char *out, uint64_t v, unsigned digits)
{
    while (digits--) *out++ = "0123456789ABCDEF"[(v >> (digits * 4)) & 15];
}

/* One CPU at a time, so a core that never comes back cannot corrupt the
 * measurement of the next, and the shared 16K table is never shared. */
static void start_secondaries(void)
{
    struct el2q_boot *b = &el2q_boot;
    b->hcr = RD("hcr_el2");
    b->mair = RD("mair_el2");
    b->tcr = RD("tcr_el2");
    b->ttbr0 = RD("ttbr0_el2");
    b->sctlr = RD("sctlr_el2");
    b->cptr = RD("cptr_el2");
    b->vbar = (uintptr_t)el2q_vectors;
    b->stacks = (uintptr_t)stacks;
    b->ctx = (uintptr_t)ctx;
    b->early = (uintptr_t)early;
    /* The entry runs with the MMU and caches off: its code and el2q_boot must
     * be in memory, not only in this CPU's cache. */
    dcache_range((void *)image_base, image_size, CLEAN);

    uint64_t hz = RD("cntfrq_el0");
    for (unsigned i = 0; i < run.acpi.cpu_count && run.cpu_count < EL2Q_MAX_CPUS; i++) {
        uint64_t mpidr = run.acpi.mpidr[i];
        if (mpidr == run.cpu[0].target_mpidr) continue;
        unsigned slot = run.cpu_count++;
        struct el2q_cpu *c = &run.cpu[slot];
        c->target_mpidr = mpidr;
        c->psci_affinity = -1;
        ctx[slot] = (struct el2q_ctx){.slot = slot};
        early[slot][0] = 0;
        dcache_range(early[slot], sizeof early[slot], CLEAN_INVALIDATE);

        char line[] = "CPU_ON SLOT .. MPIDR ..........";
        line[12] = (char)('0' + slot / 10);
        line[13] = (char)('0' + slot % 10);
        put_hex(line + 21, mpidr, 10);
        progress(line);

        c->psci_on = psci(PSCI_CPU_ON64, mpidr, (uintptr_t)el2q_secondary_entry, slot);
        if (c->psci_on) {
            c->state = EL2Q_CPU_PSCI_FAILED;
            continue;
        }
        uint64_t start = ticks();
        while (!__atomic_load_n(&c->done, __ATOMIC_ACQUIRE) && ticks() - start < 2 * hz) {}
        dcache_range(early[slot], sizeof early[slot], INVALIDATE);
        c->early = early[slot][0];
        if (!__atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) {
            int entered = (c->early >> 48) == EL2Q_EARLY_MAGIC_HI;
            c->state = entered && (c->early & 0xc) != 8 ? EL2Q_CPU_WRONG_EL : EL2Q_CPU_NO_RESPONSE;
            if (c->state == EL2Q_CPU_WRONG_EL) c->current_el = (c->early >> 2) & 3;
        }
        /* CPU_OFF is the core's last act; give it time to be seen. */
        for (start = ticks(); ticks() - start < hz / 2;) {
            c->psci_affinity = psci(PSCI_AFFINITY_INFO, mpidr, 0, 0);
            if (c->psci_affinity == PSCI_AFFINITY_OFF) break;
        }
    }
}

/* ---- Before ExitBootServices ------------------------------------------------ */

static int word_is(const char16 *w, unsigned n, const char *s)
{
    unsigned i = 0;
    for (; i < n && s[i]; i++) {
        char16 c = w[i] >= 'A' && w[i] <= 'Z' ? (char16)(w[i] + 32) : w[i];
        if (c != (char16)s[i]) return 0;
    }
    return i == n && !s[i];
}

static void parse_options(const char16 *o, uint32_t bytes)
{
    unsigned n = bytes / 2;
    for (unsigned i = 0; i < n && o[i];) {
        while (i < n && o[i] == ' ') i++;
        unsigned s = i;
        while (i < n && o[i] && o[i] != ' ') i++;
        const char16 *w = o + s;
        unsigned len = i - s;
        if (word_is(w, len, "serial")) opt.serial = 1;
        if (word_is(w, len, "poweroff")) opt.poweroff = 1;
        if (word_is(w, len, "nopac")) opt.nopac = 1;
        if (word_is(w, len, "nosmp")) opt.nosmp = 1;
        if (word_is(w, len, "emulate-vhe")) opt.emulate_vhe = 1;
        if (word_is(w, len, "exit-vhe")) opt.exit_vhe = 1;
        if (word_is(w, len, "emulate-rotated")) opt.emulate_rotated = 1;
        if (word_is(w, len, "emulate-no-fb")) opt.emulate_no_fb = 1;
        if (word_is(w, len, "emulate-dt-fb")) opt.emulate_dt_fb = 1;
        if (word_is(w, len, "thermal")) opt.thermal = 1;
        if (word_is(w, len, "usb")) opt.usb = 1;
        if (word_is(w, len, "usb-smmu")) opt.usb = opt.usb_smmu = 1;
        if (word_is(w, len, "usb-smmu-map")) opt.usb = opt.usb_smmu = opt.usb_smmu_map = 1;
        if (word_is(w, len, "emulate-map-hole")) opt.emulate_map_hole = 1;
        if (word_is(w, len, "usb-s2-walk")) opt.usb = opt.usb_smmu = opt.usb_s2_walk = 1;
        if (word_is(w, len, "usb-s2-survey")) opt.usb = opt.usb_smmu = opt.usb_s2_walk = opt.usb_s2_survey = 1;
        if (word_is(w, len, "emulate-s2-tables")) opt.emulate_s2_tables = 1;
        if (word_is(w, len, "emulate-s2-odd")) opt.emulate_s2_tables = opt.emulate_s2_odd = 1;
        if (word_is(w, len, "emulate-s2-device")) opt.emulate_s2_tables = opt.emulate_s2_device = 1;
    }
}

static void find_tables(struct efi_system_table *st)
{
    void *rsdp = 0, *fdt = 0, *smbios3 = 0, *smbios = 0;
    for (uint64_t i = 0; i < st->table_count; i++) {
        if (guid_eq(&st->tables[i].guid, &acpi20_guid)) rsdp = st->tables[i].table;
        if (guid_eq(&st->tables[i].guid, &dtb_guid)) fdt = st->tables[i].table;
        if (guid_eq(&st->tables[i].guid, &smbios3_guid)) smbios3 = st->tables[i].table;
        if (guid_eq(&st->tables[i].guid, &smbios_guid)) smbios = st->tables[i].table;
    }
    if (rsdp) el2q_acpi_parse(rsdp, &run.acpi);
    /* Read now, before ExitBootServices, while the tables are still the
     * firmware's to describe; SMBIOS 3 when the firmware offers both. */
    el2q_smbios_parse(smbios3 ? smbios3 : smbios, &run.fw);
    /* The FDT header's totalsize bounds the parse; 64 MiB is a ceiling on a
     * corrupt one, not a size any real tree approaches. */
    run.dt_state = el2q_fdt_zap_state(fdt, 64u << 20);
    fdt_table = fdt;
}

/* Whether the GOP's current mode can still be drawn on after
 * ExitBootServices: a 32 bpp RGB or BGR layout, a framebuffer address, and
 * room for every line. `hide` pretends the address is 0 (emulate-* options). */
static int mode_writable(const struct efi_gop_mode *m, int hide)
{
    const struct efi_gop_mode_info *i = m->info;
    return i && !hide && m->framebuffer &&
           (i->format == GOP_RGBX || i->format == GOP_BGRX) &&
           (uint64_t)i->stride * i->height * 4 <= m->framebuffer_size;
}

static void use_mode(const struct efi_gop_mode *m)
{
    const struct efi_gop_mode_info *i = m->info;
    run.fb_mode = m->mode;
    run.fb_width = i->width;
    run.fb_height = i->height;
    run.fb_stride = i->stride;
    run.fb_format = i->format;
    run.fb_base = m->framebuffer;
    run.fb_source = EL2Q_FB_GOP;
    screen.base = (uint32_t *)(uintptr_t)m->framebuffer;
    screen.bgr = i->format == GOP_BGRX;
}

/* The firmware's mode if it has a framebuffer address, otherwise the first
 * mode in el2q_gop_order's ranking that has one once set (report.c says why
 * the gaokun3's does not). If none has, the firmware's mode is restored so
 * the console can say so, and screen.base stays 0. */
static void find_screen(struct efi_boot_services *bs)
{
    struct efi_gop *gop = 0;
    if (EFI_ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&gop)) || !gop || !gop->mode ||
        !gop->mode->info)
        return;
    screen.gop = 1;
    const struct efi_gop_mode *m = gop->mode;
    uint32_t first = m->mode;
    run.fb_first_mode = run.fb_mode = first;
    run.fb_width = m->info->width;
    run.fb_height = m->info->height;
    run.fb_stride = m->info->stride;
    run.fb_format = m->info->format;
    if (mode_writable(m, 0)) {
        const struct efi_gop_mode_info *i = m->info;
        gop_real = (struct el2q_simplefb){m->framebuffer, m->framebuffer_size, i->width,
                                          i->height, i->stride * 4, i->format == GOP_BGRX};
    }
    if (mode_writable(m, opt.emulate_rotated || opt.emulate_no_fb)) {
        use_mode(m);
        return;
    }

    struct el2q_gop_mode modes[EL2Q_GOP_MODES];
    unsigned count = m->max_mode < EL2Q_GOP_MODES ? m->max_mode : EL2Q_GOP_MODES;
    for (unsigned i = 0; i < count; i++) {
        /* The firmware allocates *info from pool; it is not freed, and the
         * few bytes are gone at ExitBootServices anyway. */
        struct efi_gop_mode_info *info = 0;
        uint64_t size = 0;
        modes[i] = (struct el2q_gop_mode){0, 0, 0, 0};
        if (!EFI_ERROR(gop->query_mode(gop, i, &size, &info)) && info && size >= sizeof *info)
            modes[i] = (struct el2q_gop_mode){info->width, info->height, info->format, 1};
    }
    unsigned order[EL2Q_GOP_MODES];
    unsigned n = el2q_gop_order(modes, count, first, order);
    for (unsigned j = 0; j < n; j++) {
        if (EFI_ERROR(gop->set_mode(gop, order[j]))) continue;
        if (mode_writable(gop->mode, opt.emulate_no_fb)) {
            use_mode(gop->mode);
            return;
        }
    }
    if (gop->mode->mode != first) gop->set_mode(gop, first);
}

/* The fallback when no GOP mode has an address: the device tree's /chosen
 * simple-framebuffer, if report.c finds it self-consistent and the
 * firmware's memory map reserves all of it (report.c says why both). Its
 * outcome is kept in run.fb_dt for the stop message. */
static void find_dt_screen(struct efi_boot_services *bs)
{
    struct el2q_simplefb fb;
    run.fb_dt = el2q_fdt_simplefb(fdt_table, 64u << 20, &fb);
    if (opt.emulate_dt_fb && gop_real.base) {
        fb = gop_real;
        run.fb_dt = EL2Q_SFB_OK;
    }
    if (run.fb_dt != EL2Q_SFB_OK) return;
    uint64_t size = sizeof memory_map, key = 0, descriptor_size = 0;
    uint32_t version = 0;
    if (EFI_ERROR(bs->get_memory_map(&size, memory_map, &key, &descriptor_size, &version))) {
        run.fb_dt = EL2Q_SFB_NO_MAP;
        return;
    }
    if (!el2q_memmap_reserved(memory_map, size, descriptor_size, fb.base, fb.size)) {
        run.fb_dt = EL2Q_SFB_NOT_RESERVED;
        return;
    }
    run.fb_source = EL2Q_FB_DT;
    run.fb_base = fb.base;
    run.fb_width = fb.width;
    run.fb_height = fb.height;
    run.fb_stride = fb.stride / 4;
    run.fb_format = fb.bgr ? GOP_BGRX : GOP_RGBX;
    screen.base = (uint32_t *)(uintptr_t)fb.base;
    screen.bgr = (int)fb.bgr;
    screen.clean = 1;
}

/* Breadcrumb text only; the report's own text is built in report.c. */
static char *append(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

static char *append_dec(char *p, uint64_t v)
{
    char d[20];
    unsigned n = 0;
    do d[n++] = (char)('0' + v % 10); while ((v /= 10) && n < sizeof d);
    while (n) *p++ = d[--n];
    *p = 0;
    return p;
}

/* With nothing to draw on and no UART, a run could show nothing, and on the
 * gaokun3 it would spend a Secure Launch (slbounce runs inside
 * ExitBootServices) for no result. Say why while the firmware console still
 * works, and stop with boot services up. */
__attribute__((noreturn)) static void stop_before_ebs(void)
{
    char line[96], *p = line;
    if (!screen.gop) {
        p = append(p, "NO GRAPHICS OUTPUT PROTOCOL");
    } else {
        p = append(p, "NO GOP MODE HAS A FRAMEBUFFER ADDRESS (MODE ");
        p = append_dec(p, run.fb_first_mode);
        p = append(p, ")");
    }
    console_line(line);
    if (screen.gop) {
        p = append(line, "DT FRAMEBUFFER: ");
        append(p, el2q_sfb_words(run.fb_dt));
        console_line(line);
    }
    console_line("STOPPED BEFORE EXITBOOTSERVICES: NOTHING WAS MEASURED");
    console_line("HOLD THE POWER BUTTON TO TURN THE MACHINE OFF");
    if (opt.poweroff && run.start_el == 2) psci(PSCI_SYSTEM_OFF, 0, 0, 0);
    for (;;) __asm__ volatile("wfe");
}

uint64_t efi_main(void *image, struct efi_system_table *st)
{
    struct efi_boot_services *bs = st->boot_services;
    con_out = st->con_out;
    run.start_el = current_el();
    for (unsigned i = 0; i < EL2Q_ID_COUNT; i++)
        run.start_id[i] = id_register(el2q_idregs[i].crm, el2q_idregs[i].op2);
    run.psci_version = -1;
    bs->set_watchdog_timer(0, 0, 0, 0);

    struct efi_loaded_image *li = 0;
    if (!EFI_ERROR(bs->handle_protocol(image, &loaded_image_guid, (void **)&li)) && li) {
        if (li->options) parse_options(li->options, li->options_size);
        image_base = (uintptr_t)li->image_base;
        image_size = li->image_size;
    }
    find_tables(st);
    find_screen(bs);
    if (!screen.base) find_dt_screen(bs);
    if (!screen.base && !uart()) stop_before_ebs();
    if (screen.base) {
        /* progress() writes the console before it draws, so on the device
         * tree's buffer, the one written outside any GOP's contract, this line
         * is the last one left if the first write faults. */
        int dt = run.fb_source == EL2Q_FB_DT;
        char where[96], *p = append(where, dt ? "SCREEN: DT SIMPLE-FB " : "SCREEN: GOP MODE ");
        if (!dt) {
            p = append_dec(p, run.fb_mode);
            p = append(p, " ");
        }
        p = append_dec(p, run.fb_width);
        p = append(p, "X");
        p = append_dec(p, run.fb_height);
        p = append(p, " FB ");
        put_hex(p, run.fb_base, 16);
        p += 16;
        *p = 0;
        if (dt) append(p, ", DRAWING");
        else if (run.fb_mode != run.fb_first_mode) append_dec(append(p, " FROM MODE "), run.fb_first_mode);
        progress(where);
    }

    /* tgran16 needs one 16 KiB-aligned, 16 KiB table: eight pages hold one. */
    uint64_t pages = 0;
    if (!EFI_ERROR(bs->allocate_pages(0 /* AnyPages */, 2 /* LoaderData */, 8, &pages)))
        table = (pages + 0x3fff) & ~0x3fffull;
    /* pagemap.c needs one 4 KiB table page per window it maps. */
    uint64_t pool = 0, pool_pages = opt.emulate_map_hole ? 2 : 1;
    if (opt.usb_smmu_map &&
        !EFI_ERROR(bs->allocate_pages(0 /* AnyPages */, 2 /* LoaderData */, pool_pages, &pool))) {
        pm_pool_next = pool;
        pm_pool_end = pool + pool_pages * 0x1000;
    }
    if (opt.usb_s2_walk) {
        uint64_t below = 0xffffffffull;
        if (!EFI_ERROR(bs->allocate_pages(1 /* MaxAddress */, 2 /* LoaderData */, EL2Q_S2W_ARENA_PAGES, &below)))
            s2w_arena = below;
        uint64_t fixture = 0;
        if (opt.emulate_s2_tables &&
            !EFI_ERROR(bs->allocate_pages(0 /* AnyPages */, 2 /* LoaderData */, EL2Q_S2W_TEST_PAGES, &fixture)))
            s2w_pool = fixture;
    }

    char line[] = "SNAPINTOSH EL2QUAL: STARTED AT EL., CALLING EXITBOOTSERVICES";
    line[33] = (char)('0' + run.start_el);
    progress(line);
    if (!table) progress("NO MEMORY FOR THE 16K TABLE: STOPPING");
    if (!table) finish();
    if (opt.usb) measure_usb_before();

    efi_status status = 0;
    uint64_t tries = 0;
    while (tries < 8) {
        uint64_t size = sizeof memory_map, key = 0, descriptor_size = 0;
        uint32_t version = 0;
        tries++;
        status = bs->get_memory_map(&size, memory_map, &key, &descriptor_size, &version);
        if (EFI_ERROR(status)) break;
        status = bs->exit_boot_services(image, key);
        if (!EFI_ERROR(status)) break;
    }
    __asm__ volatile("msr daifset, #15");
    con_out = 0;
    run.ebs_status = status;
    run.ebs_tries = tries;
    run.end_el = current_el();
    if (EFI_ERROR(status) || run.end_el != 2) finish();

    /* At EL2 from here on, alone on the machine. */
    el2q_use_spx();
    ctx[0] = (struct el2q_ctx){.slot = 0};
    WR("tpidr_el2", (uintptr_t)&ctx[0]);
    WR("vbar_el2", (uintptr_t)el2q_vectors);
    ISB();
    /* Slot 0 exists from here, so a fault anywhere below reports against it. */
    run.cpu_count = 1;
    run.cpu[0].target_mpidr = RD("mpidr_el1") & MPIDR_AFFINITY;
    run.cpu[0].psci_affinity = -1;
    if (opt.emulate_vhe && run.start_el == 2 && !(RD("hcr_el2") & EL2Q_HCR_E2H)) {
        el2q_enter_vhe(el2q_vhe_tcr(RD("tcr_el2")));
        run.emulated_vhe = 1;
    }
    run.hcr_entry = RD("hcr_el2");
    run.sctlr_entry = RD("sctlr_el2");
    if (opt.exit_vhe) exit_vhe();
    run.psci_version = psci(PSCI_VERSION, 0, 0, 0);
    measure(0);
    run.cpu[0].state = EL2Q_CPU_DONE;
    run.cpu[0].done = 1;
    if (!opt.nosmp && run.psci_version >= 0) start_secondaries();
    if (opt.thermal) measure_tsens();
    if (opt.usb) measure_usb_after();
    finish();
}
