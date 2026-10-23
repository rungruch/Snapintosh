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
 *   emulate-rotated  treat the firmware's GOP mode as having no framebuffer
 *               address, as the gaokun3's does; test only
 *   emulate-no-fb    treat every GOP mode that way; test only
 *
 * With no mode that has a framebuffer address and no "serial" UART, nothing
 * could show the results, so el2qual says so on the firmware console and
 * stops before ExitBootServices: no Secure Launch is attempted for nothing. */
#include <stddef.h>
#include <stdint.h>
#include "el2qual.h"
#include "report.h"

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

extern char el2q_vectors[];
void el2q_secondary_entry(void);
uint64_t el2q_smc(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3);
void el2q_use_spx(void);
void el2q_enter_vhe(uint64_t tcr);
uint64_t el2q_try_rndr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_vncr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_cntpoff(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_hfgrtr(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_apiakey(struct el2q_ctx *ctx, uint64_t *value);
uint64_t el2q_try_pacga(struct el2q_ctx *ctx, uint64_t *value, uint64_t pointer, uint64_t modifier);
uint64_t el2q_try_pac(struct el2q_ctx *ctx, const uint64_t *in, const uint64_t *modifier,
                      uint64_t *out, uint64_t *round_trip, uint64_t count);
void el2q_apia_get(uint64_t key[2]);
void el2q_apia_set(uint64_t lo, uint64_t hi);
void tgran16_probe(uint64_t *out, uint64_t table);       /* probes/tgran16.c */
void tgran16_probe_el12(uint64_t *out, uint64_t table);  /* same, -DTGRAN16_EL12 */

void el2q_secondary_main(uint64_t slot) __attribute__((noreturn));
void el2q_fatal(struct el2q_ctx *ctx) __attribute__((noreturn));

/* ---- UEFI (UEFI 2.10 specification, only what is used) ------------------ */

typedef uint64_t efi_status;
typedef uint16_t char16;
#define EFI_ERROR(s) ((int64_t)(s) < 0)

struct efi_guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };

struct efi_table_header { uint64_t signature; uint32_t revision, size, crc32, reserved; };

struct efi_text_output {
    void *reset;
    efi_status (*output_string)(struct efi_text_output *self, const char16 *text);
};

struct efi_config_table { struct efi_guid guid; void *table; };

struct efi_boot_services {
    struct efi_table_header header;
    void *raise_tpl, *restore_tpl;
    efi_status (*allocate_pages)(uint32_t type, uint32_t memory_type, uint64_t pages,
                                 uint64_t *address);
    void *free_pages;
    efi_status (*get_memory_map)(uint64_t *size, void *map, uint64_t *key,
                                 uint64_t *descriptor_size, uint32_t *descriptor_version);
    void *allocate_pool, *free_pool, *create_event, *set_timer, *wait_for_event;
    void *signal_event, *close_event, *check_event, *install_protocol;
    void *reinstall_protocol, *uninstall_protocol;
    efi_status (*handle_protocol)(void *handle, const struct efi_guid *guid, void **out);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table, *load_image, *start_image, *exit, *unload_image;
    efi_status (*exit_boot_services)(void *image, uint64_t map_key);
    void *get_next_monotonic_count, *stall;
    efi_status (*set_watchdog_timer)(uint64_t timeout, uint64_t code, uint64_t size,
                                     const char16 *data);
    void *connect_controller, *disconnect_controller, *open_protocol, *close_protocol;
    void *open_protocol_information, *protocols_per_handle, *locate_handle_buffer;
    efi_status (*locate_protocol)(const struct efi_guid *guid, void *registration, void **out);
};
_Static_assert(offsetof(struct efi_boot_services, allocate_pages) == 40, "UEFI 7.2");
_Static_assert(offsetof(struct efi_boot_services, get_memory_map) == 56, "UEFI 7.2");
_Static_assert(offsetof(struct efi_boot_services, handle_protocol) == 152, "UEFI 7.3");
_Static_assert(offsetof(struct efi_boot_services, exit_boot_services) == 232, "UEFI 7.4");
_Static_assert(offsetof(struct efi_boot_services, set_watchdog_timer) == 256, "UEFI 7.5");
_Static_assert(offsetof(struct efi_boot_services, locate_protocol) == 320, "UEFI 7.3");

struct efi_system_table {
    struct efi_table_header header;
    char16 *vendor;
    uint32_t revision;
    void *console_in_handle, *con_in, *console_out_handle;
    struct efi_text_output *con_out;
    void *standard_error_handle, *std_err, *runtime_services;
    struct efi_boot_services *boot_services;
    uint64_t table_count;
    struct efi_config_table *tables;
};
_Static_assert(offsetof(struct efi_system_table, con_out) == 64, "UEFI 4.3");
_Static_assert(offsetof(struct efi_system_table, boot_services) == 96, "UEFI 4.3");
_Static_assert(offsetof(struct efi_system_table, tables) == 112, "UEFI 4.3");

struct efi_loaded_image {
    uint32_t revision;
    void *parent, *system_table, *device, *file_path, *reserved;
    uint32_t options_size;
    void *options;
    void *image_base;
    uint64_t image_size;
};
_Static_assert(offsetof(struct efi_loaded_image, options_size) == 48, "UEFI 9.1");
_Static_assert(offsetof(struct efi_loaded_image, image_base) == 64, "UEFI 9.1");

struct efi_gop_mode_info {
    uint32_t version, width, height, format, mask[4], stride;
};
struct efi_gop_mode {
    uint32_t max_mode, mode;
    struct efi_gop_mode_info *info;
    uint64_t info_size, framebuffer, framebuffer_size;
};
struct efi_gop {
    efi_status (*query_mode)(struct efi_gop *self, uint32_t mode, uint64_t *info_size,
                             struct efi_gop_mode_info **info);
    efi_status (*set_mode)(struct efi_gop *self, uint32_t mode);
    void *blt;
    struct efi_gop_mode *mode;
};
_Static_assert(offsetof(struct efi_gop, mode) == 24, "UEFI 12.9");
_Static_assert(offsetof(struct efi_gop_mode, framebuffer) == 24, "UEFI 12.9");

enum { GOP_RGBX = 0, GOP_BGRX = 1 };

static const struct efi_guid acpi20_guid =
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static const struct efi_guid dtb_guid =
    {0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};
static const struct efi_guid gop_guid =
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static const struct efi_guid loaded_image_guid =
    {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

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

#define HCR_E2H        (1ull << 34)
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

/* The non-VHE TCR_EL2 the firmware programmed, rewritten in the layout E2H
 * gives the register (TCR_EL1's), keeping the same TTBR0 walk: T0SZ, the
 * cacheability, shareability and granule fields stay where they are; PS moves
 * to IPS, TBI to TBI0, HA/HD/HPD to their EL1 positions; TTBR1 walks are
 * disabled since UEFI's identity map lives in TTBR0 alone. */
static uint64_t vhe_tcr(uint64_t tcr)
{
    uint64_t out = tcr & 0xff3full;                        /* T0SZ..TG0, not EPD0 */
    out |= (tcr & 0x3f) << 16;                             /* T1SZ: unused, but sane */
    out |= 1ull << 23;                                     /* EPD1 */
    out |= 2ull << 30;                                     /* TG1 4K, unused */
    out |= ((tcr >> 16) & 7) << 32;                        /* PS -> IPS */
    out |= ((tcr >> 20) & 1) << 37;                        /* TBI -> TBI0 */
    out |= ((tcr >> 21) & 3) << 39;                        /* HA, HD */
    out |= ((tcr >> 24) & 1) << 41;                        /* HPD -> HPD0 */
    return out;
}

/* ---- State ------------------------------------------------------------------ */

static struct el2q_run run;
static struct el2q_text text;
static struct {
    int serial, poweroff, nopac, nosmp, emulate_vhe, emulate_rotated, emulate_no_fb;
} opt;
static struct {
    uint32_t *base;
    int bgr, gop;
} screen;
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
    if (screen.base)
        el2q_render(&text, screen.base, run.fb_width, run.fb_height, run.fb_stride, screen.bgr);
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

/* Everything one CPU contributes, run on that CPU at EL2. */
static void measure(unsigned slot)
{
    struct el2q_cpu *c = &run.cpu[slot];
    struct el2q_ctx *x = &ctx[slot];
    c->current_el = current_el();
    c->mpidr = RD("mpidr_el1");
    c->hcr = RD("hcr_el2");
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
    if (c->hcr & HCR_E2H) tgran16_probe_el12(c->tgran, table);
    else tgran16_probe(c->tgran, table);
    if (!opt.nopac) measure_pac(slot);
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
        if (word_is(w, len, "emulate-rotated")) opt.emulate_rotated = 1;
        if (word_is(w, len, "emulate-no-fb")) opt.emulate_no_fb = 1;
    }
}

static void find_tables(struct efi_system_table *st)
{
    void *rsdp = 0, *fdt = 0;
    for (uint64_t i = 0; i < st->table_count; i++) {
        if (guid_eq(&st->tables[i].guid, &acpi20_guid)) rsdp = st->tables[i].table;
        if (guid_eq(&st->tables[i].guid, &dtb_guid)) fdt = st->tables[i].table;
    }
    if (rsdp) el2q_acpi_parse(rsdp, &run.acpi);
    /* The FDT header's totalsize bounds the parse; 64 MiB is a ceiling on a
     * corrupt one, not a size any real tree approaches. */
    run.dt_state = el2q_fdt_zap_state(fdt, 64u << 20);
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
    if (!screen.base && !uart()) stop_before_ebs();
    if (screen.base) {
        char where[96], *p = append(where, "SCREEN: GOP MODE ");
        p = append_dec(p, run.fb_mode);
        p = append(p, " ");
        p = append_dec(p, run.fb_width);
        p = append(p, "X");
        p = append_dec(p, run.fb_height);
        p = append(p, " FB ");
        put_hex(p, run.fb_base, 16);
        p = append(p + 16, run.fb_mode != run.fb_first_mode ? " FROM MODE " : "");
        if (run.fb_mode != run.fb_first_mode) append_dec(p, run.fb_first_mode);
        progress(where);
    }

    /* tgran16 needs one 16 KiB-aligned, 16 KiB table: eight pages hold one. */
    uint64_t pages = 0;
    if (!EFI_ERROR(bs->allocate_pages(0 /* AnyPages */, 2 /* LoaderData */, 8, &pages)))
        table = (pages + 0x3fff) & ~0x3fffull;

    char line[] = "SNAPINTOSH EL2QUAL: STARTED AT EL., CALLING EXITBOOTSERVICES";
    line[33] = (char)('0' + run.start_el);
    progress(line);
    if (!table) progress("NO MEMORY FOR THE 16K TABLE: STOPPING");
    if (!table) finish();

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
    if (opt.emulate_vhe && run.start_el == 2 && !(RD("hcr_el2") & HCR_E2H)) {
        el2q_enter_vhe(vhe_tcr(RD("tcr_el2")));
        run.emulated_vhe = 1;
    }
    run.hcr_entry = RD("hcr_el2");
    run.sctlr_entry = RD("sctlr_el2");
    run.psci_version = psci(PSCI_VERSION, 0, 0, 0);
    measure(0);
    run.cpu[0].state = EL2Q_CPU_DONE;
    run.cpu[0].done = 1;
    if (!opt.nosmp && run.psci_version >= 0) start_secondaries();
    finish();
}
