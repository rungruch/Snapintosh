/* SPDX-License-Identifier: MIT */
/* Host test for the Route B monitor's device models (Phase 3 P3).
 * monitor.c is freestanding EL2 code; here the mon_* helpers are fakes
 * and the "guest" is a small driver that builds ESR/ELR pairs by hand.
 * The instruction decoder is fed through a fake translation: ELR values
 * in the test map 1:1 onto a synthetic instruction buffer.
 *
 * Deliberately wrong answers the models must not give are checked as
 * negative cases: an unmodelled IPA stops by name, and the fixed
 * addresses are not answered without the diagnostic gate.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmapple-smoke/monitor.h"
#include "vmapple-smoke/layout.h"

/* --- fakes -------------------------------------------------------------- */
static uint64_t fake_lr0, fake_iar;
static char uart_capture[4096];
static size_t uart_len;
static uint32_t insn_page[1024] __attribute__((aligned(4096)));
#define insn_word (insn_page[0])

/* A second translation window for the panic tests: VAs in this range map
 * to panic_mem so panic_render's guest reads hit a real host buffer.
 * Every other VA keeps aliasing insn_page, as the decoder tests expect. */
static uint32_t panic_mem[1024] __attribute__((aligned(4096)));
/* The guest page after panic_mem's, deliberately not adjacent on the host,
 * so a string that crosses the guest page boundary is read correctly only
 * if each page is translated on its own. */
static uint32_t panic_mem2[1024] __attribute__((aligned(4096)));
#define PANIC_VA_BASE 0xfffffe00c0000000ull
static void pm_u64(unsigned off, uint64_t v) { memcpy((uint8_t *)panic_mem + off, &v, 8); }
static void pm_str(unsigned off, const char *s) { strcpy((char *)panic_mem + off, s); }

uint64_t mon_iar1_read(void) { return fake_iar; }
uint64_t mon_iar0_read(void) { return fake_iar; }
void mon_eoir0_write(uint64_t v) { (void)v; }
void mon_eoir1_write(uint64_t v) { (void)v; }
uint64_t mon_lr0_read(void) { return fake_lr0; }
void mon_lr0_write(uint64_t v) { fake_lr0 = v; }
static uint64_t fake_lr[4];
uint64_t mon_lr_read(unsigned n) { return fake_lr[n & 3]; }
void mon_lr_write(unsigned n, uint64_t v) { fake_lr[n & 3] = v; }
uint64_t mon_misr_read(void) { return 0; }
static uint32_t fake_cnthp_ticks;
static unsigned fake_cnthp_calls;
void mon_cnthp_rearm(uint32_t ticks) { fake_cnthp_ticks = ticks; fake_cnthp_calls++; }
static uint64_t fake_elr_el2 = 0xfffffe000a065974ull;
static uint64_t fake_spsr_el2 = 0x600003c5ull;
static uint64_t fake_esr_el1 = 0x02000000ull;
static uint64_t fake_far_el1 = 0x0ull;
static uint64_t fake_elr_el1 = 0xfffffe000a065974ull;
static uint64_t fake_sctlr_el1 = 0x30d00800ull;

uint64_t mon_elr_el2(void) { return fake_elr_el2; }
uint64_t mon_spsr_el2(void) { return fake_spsr_el2; }
uint64_t mon_esr_el1(void) { return fake_esr_el1; }
uint64_t mon_far_el1(void) { return fake_far_el1; }
uint64_t mon_elr_el1(void) { return fake_elr_el1; }
uint64_t mon_sctlr_el1(void) { return fake_sctlr_el1; }
static unsigned fake_tlbi_count;
void mon_tlbi_vmalle1is(void) { fake_tlbi_count++; }
static uint64_t fake_params_base;
uint64_t mon_params_base(void) { return fake_params_base; }
/* A fake platform GIC: the distributor's CTLR and, per redistributor
 * frame, TYPER, WAKER and IGROUPR0. Writes are logged and applied. */
static uint64_t last_redist_addr; static uint32_t last_redist_value;
static uint64_t redist_addrs[16]; static uint32_t redist_vals[16]; static unsigned redist_n;
static uint32_t fake_ctlr; static int fake_rwp_stuck;
static uint64_t fake_typer[8]; static uint32_t fake_waker[8], fake_igroupr[8];
static uint64_t fake_mpidr;
static int gicr_frame(uint64_t addr, uint64_t *off)
{
    if (addr < PHYS_GICR_BASE || addr >= PHYS_GICR_BASE + 8 * 0x20000) return -1;
    *off = (addr - PHYS_GICR_BASE) % 0x20000;
    return (int)((addr - PHYS_GICR_BASE) / 0x20000);
}
void mon_redist_write(uint64_t addr, uint32_t value)
{
    last_redist_addr = addr; last_redist_value = value;
    if (redist_n < 16) { redist_addrs[redist_n] = addr; redist_vals[redist_n] = value; redist_n++; }
    uint64_t off;
    int f = gicr_frame(addr, &off);
    if (addr == PHYS_GICD_BASE) fake_ctlr = (value & ~(1u << 31)) | (fake_ctlr & (1u << 6));
    else if (f >= 0 && off == 0x14) fake_waker[f] = (value & 2u) ? value : (value & ~4u);
    else if (f >= 0 && off == 0x10080) fake_igroupr[f] = value;
}
uint32_t mon_mmio_read32(uint64_t addr)
{
    uint64_t off;
    int f = gicr_frame(addr, &off);
    if (addr == PHYS_GICD_BASE) return fake_ctlr | (fake_rwp_stuck ? (1u << 31) : 0);
    if (f >= 0 && off == 0x14) return fake_waker[f];
    if (f >= 0 && off == 0x10080) return fake_igroupr[f];
    return 0;
}
uint64_t mon_mmio_read64(uint64_t addr)
{
    uint64_t off;
    int f = gicr_frame(addr, &off);
    return (f >= 0 && off == 0x8) ? fake_typer[f] : 0;
}
uint64_t mon_mpidr(void) { return fake_mpidr; }
static void fake_gic(uint32_t ctlr, uint64_t mpidr)
{
    memset(fake_typer, 0, sizeof(fake_typer));
    memset(fake_waker, 0, sizeof(fake_waker));
    memset(fake_igroupr, 0, sizeof(fake_igroupr));
    for (unsigned f = 0; f < 8; f++) fake_typer[f] = ((uint64_t)(f << 8) << 32) | (f == 7 ? (1u << 4) : 0);
    fake_ctlr = ctlr; fake_rwp_stuck = 0; fake_mpidr = mpidr; redist_n = 0;
}
void mon_uart_putc(char c) { if (uart_len < sizeof(uart_capture) - 1) uart_capture[uart_len++] = c; }
static uint64_t cleaned_pa, cleaned_size;
void mon_clean_range(uint64_t pa, uint64_t size) { cleaned_pa = pa; cleaned_size = size; }
uint64_t mon_cntpct(void) { return 5000000000ull; }
uint64_t mon_cntfrq(void) { return 1000000000ull; }
uint64_t mon_translate_va(uint64_t va)
{
    if (va >= PANIC_VA_BASE && va < PANIC_VA_BASE + 0x1000)
        return (uint64_t)(uintptr_t)panic_mem;   /* page aligned; low 12 bits select the datum */
    if (va >= PANIC_VA_BASE + 0x1000 && va < PANIC_VA_BASE + 0x2000)
        return (uint64_t)(uintptr_t)panic_mem2;
    if (va >= PANIC_VA_BASE + 0x2000 && va < PANIC_VA_BASE + 0x3000)
        return 1;                                /* PAR.F: a page that does not translate */
    return (uint64_t)(uintptr_t)insn_page;       /* PAR form: PA in [47:12] */
}

/* An array-backed panic_args for the pure panic_format tests: a %s value
 * is an index into `strs`, so no guest translation is needed. */
struct fake_pa { const uint64_t *vals; unsigned n; const char *const *strs; unsigned nstr; };
static int fpa_get(void *ctx, unsigned i, uint64_t *out)
{
    struct fake_pa *f = (struct fake_pa *)ctx;
    if (i >= f->n) return -1;
    *out = f->vals[i];
    return 0;
}
static int fpa_str(void *ctx, uint64_t va, char *buf, unsigned cap)
{
    struct fake_pa *f = (struct fake_pa *)ctx;
    if (va >= f->nstr) { buf[0] = 0; return -1; }
    const char *s = f->strs[va];
    unsigned i = 0;
    for (; i + 1 < cap && s[i]; i++) buf[i] = s[i];
    buf[i] = 0;
    return 0;
}
static unsigned fmt_check(const char *fmt, const uint64_t *vals, unsigned n,
                          const char *const *strs, unsigned nstr, char *out, unsigned cap)
{
    struct fake_pa f = { vals, n, strs, nstr };
    struct panic_args a = { fpa_get, fpa_str, &f };
    return panic_format(out, cap, fmt, &a);
}

/* --- the test driver ------------------------------------------------------ */
static int checks, failures;
#define CHECK(cond, text) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: %s\n", text); } \
} while (0)

static uint64_t frame[31];

/* An ESR with a full syndrome (ISV set) for a size-byte access. */
static uint64_t esr_syndrome(unsigned size_log2, unsigned rt, int write, int sign)
{
    return (0x24ull << 26) | (1ull << 24) | ((uint64_t)size_log2 << 22)
           | ((uint64_t)sign << 21) | ((uint64_t)rt << 16) | (write ? (1ull << 6) : 0);
}

static int access(uint64_t ipa, unsigned size_log2, unsigned rt, int write, uint64_t value)
{
    frame[rt] = value;
    return monitor_data_abort(esr_syndrome(size_log2, rt, write, 0), ipa, 0, frame);
}

int main(void)
{
    monitor_set_policy(0);

    /* PL011: FR reports both FIFOs empty; a byte written to DR reaches
     * the monitor UART. */
    CHECK(access(MON_UART_IPA + 0x18, 2, 5, 0, 0) == 1 && frame[5] == 0x90,
          "PL011 FR is TXFE|RXFE");
    uart_len = 0;
    CHECK(access(MON_UART_IPA, 0, 5, 1, 'X') == 1 && uart_len == 1 && uart_capture[0] == 'X',
          "PL011 DR write reaches the monitor UART");

    /* PL031: DR is counter seconds (5 s at the fake 1 GHz). */
    CHECK(access(MON_PL031_IPA, 2, 5, 0, 0) == 1 && frame[5] == 5, "PL031 DR ticks");

    /* PCIe config: absent functions answer all-ones, by size. */
    CHECK(access(MON_PCIE_IPA + 0x40, 2, 5, 0, 0) == 1 && frame[5] == 0xffffffffull,
          "PCIe config read is all-ones");
    CHECK(access(MON_PCIE_IPA + 0x40, 0, 5, 0, 0) == 1 && frame[5] == 0xff,
          "PCIe config byte read is one byte of ones");

    /* GICD: TYPER, IIDR, RWP clear; enable set/clear semantics. */
    CHECK(access(MON_GICD_IPA + 4, 2, 5, 0, 0) == 1 && frame[5] == 0x037a0208,
          "GICD TYPER matches the measured QEMU value");
    CHECK(access(MON_GICD_IPA + 8, 2, 5, 0, 0) == 1 && frame[5] == 0x43b, "GICD IIDR");
    access(MON_GICD_IPA + 0x100, 2, 5, 1, 0x10);                 /* ISENABLER0 */
    CHECK(access(MON_GICD_IPA + 0x100, 2, 6, 0, 0) == 1 && frame[6] == 0x10,
          "ISENABLER write sets enable bits");
    access(MON_GICD_IPA + 0x180, 2, 5, 1, 0x10);                 /* ICENABLER0 */
    CHECK(access(MON_GICD_IPA + 0x100, 2, 6, 0, 0) == 1 && frame[6] == 0,
          "ICENABLER write clears enable bits");

    /* GICR: TYPER marks the last redistributor; WAKER never sleeps. */
    CHECK(access(MON_GICR_IPA + 8, 3, 5, 0, 0) == 1 && (frame[5] & (1ull << 4)),
          "GICR TYPER Last is set");
    access(MON_GICR_IPA + 0x14, 2, 5, 1, 0x2);                   /* WAKER sleep bit */
    CHECK(access(MON_GICR_IPA + 0x14, 2, 6, 0, 0) == 1 && !(frame[6] & (1u << 2)),
          "GICR WAKER never reports ChildrenAsleep");

    /* Default-deny: an unmodelled IPA stops by name. */
    CHECK(access(0x20000000, 2, 5, 0, 0) == 0 && monitor_stop_ipa == 0x20000000,
          "an unmodelled IPA stops by name");

    /* The MSI frame: the type register the PCIe MSI controller checks. */
    CHECK(access(MON_GIC_MSI_IPA + 8, 2, 5, 0, 0) == 1 && (frame[5] >> 31) == 1,
          "MSI frame: the type register has its valid bit, bit 31, which the kernel requires");
    CHECK((frame[5] & 0x1fff) == 128 && ((frame[5] >> 16) & 0x1fff) == 128,
          "MSI frame: both 13-bit range fields are 128, INTIDs 128..255 either way");
    CHECK(access(MON_GIC_MSI_IPA + 0x40, 2, 5, 1, 130) == 0,
          "MSI frame: a write is a named stop (no MSI source, meaning unknown)");
    CHECK(access(MON_GIC_MSI_IPA + 0xfcc, 2, 5, 0, 0) == 0,
          "MSI frame: a read of an unknown register is a named stop");
    CHECK(access(MON_GIC_MSI_IPA + 8, 3, 5, 0, 0) == 0,
          "MSI frame: a 64-bit read of the type is a named stop");

    /* Decision 5: the backdoor interface answers as QEMU's vmapple does. */
    CHECK(access(MON_BDIF_IPA + 0x10000, 2, 5, 0, 0) == 1 && frame[5] == 1,
          "backdoor: the AUX STATUS reads 1, the value the kernel checks for");
    CHECK(access(MON_BDIF_IPA + 0x420, 2, 5, 0, 0) == 1 && frame[5] == 0x8000000,
          "backdoor: ROOT NEXT_DEVICE");
    CHECK(access(MON_BDIF_IPA + 0x10420, 2, 5, 0, 0) == 1 && frame[5] == 0x10000,
          "backdoor: AUX NEXT_DEVICE");
    CHECK(access(MON_BDIF_IPA + 0x10024, 2, 5, 0, 0) == 1 && frame[5] == 0xffffffffull,
          "backdoor: an undocumented register reads all-ones, as QEMU's");
    CHECK(access(MON_BDIF_IPA + 0x10408, 3, 5, 1, 0x1000) == 0
          && monitor_stop_ipa == MON_BDIF_IPA + 0x10408,
          "backdoor: a disk command is a named stop (no disk)");
    CHECK(access(MON_BDIF_IPA + 0x10004, 2, 5, 1, 7) == 1, "backdoor: other writes are ignored");

    /* The configuration region: QEMU's table, non-identifying defaults. */
    static uint64_t boot_args[4] = {0, 0, 0, 0x1C000000};
    monitor_set_guest((uint64_t)(uintptr_t)boot_args);
    CHECK(access(MON_CFG_IPA, 3, 5, 0, 0) == 1 && frame[5] == (2ull | (1ull << 32)),
          "config: version 2, one CPU");
    CHECK(access(MON_CFG_IPA + 0x20, 3, 5, 0, 0) == 1 && frame[5] == 0x1C000000,
          "config: the RAM size is boot_args.memSize");
    CHECK(access(MON_CFG_IPA + 0x500, 2, 5, 0, 0) == 1 && frame[5] == 0x34333231,
          "config: the serial is QEMU's placeholder \"1234\"");
    CHECK(access(MON_CFG_IPA + 0x584, 0, 5, 0, 0) == 1 && frame[5] == 'A',
          "config: the SoC name");
    CHECK(access(MON_CFG_IPA + 0x18, 3, 5, 0, 0) == 1 && frame[5] == 0,
          "config: the ECID is zero, QEMU's default, never the machine's");
    CHECK(access(MON_CFG_IPA + 0x6cc, 2, 5, 0, 0) == 1 && frame[5] == 0,
          "config: past the known fields reads zero, as QEMU's");
    CHECK(access(MON_CFG_IPA + 0x38, 2, 5, 0, 0) == 0,
          "config: rnd is a named stop, not a fabricated random number");
    CHECK(access(MON_CFG_IPA + 0x34, 3, 5, 0, 0) == 0, "config: a read overlapping rnd stops");
    CHECK(access(MON_CFG_IPA + 0x100, 2, 5, 1, 1) == 0, "config: a write is a named stop");

    /* The mailbox: the request store is absorbed and logged, nothing else. */
    uart_len = 0;
    CHECK(access(MON_MAILBOX_IPA + 0xff8, 3, 5, 1, 0x4aa8fb60) == 1,
          "mailbox: the request store is absorbed");
    uart_capture[uart_len] = 0;
    CHECK(strstr(uart_capture, "mailbox: request at") != NULL, "mailbox: the request is logged");
    CHECK(access(MON_MAILBOX_IPA + 0xff8, 3, 5, 0, 0) == 0, "mailbox: a read is a named stop");
    CHECK(access(MON_MAILBOX_IPA + 0x10, 2, 5, 1, 0) == 0, "mailbox: another store is a named stop");

    /* Trapped system registers: IMPDEF space, the guest's SGI, the rest. */
    #define SYSREG(op0, op1, crn, crm, op2, rt, read) ((0x18ull << 26) | ((uint64_t)(op0) << 20) \
        | ((uint64_t)(op2) << 17) | ((uint64_t)(op1) << 14) | ((uint64_t)(crn) << 10) \
        | ((uint64_t)(rt) << 5) | ((uint64_t)(crm) << 1) | (read))
    CHECK(monitor_sysreg(SYSREG(3, 0, 15, 2, 0, 1, 1), frame) == 2,
          "an IMPDEF register (CRn 15) is left to the stub's RAZ/WI");
    memset(fake_lr, 0, sizeof(fake_lr));
    frame[2] = (3ull << 24) | 1;                                 /* SGI 3 to CPU 0 */
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 0), frame) == 1
          && (fake_lr[1] >> 62) == 1 && ((fake_lr[1] >> 60) & 1) && (fake_lr[1] & 0xffffffff) == 3,
          "ICC_SGI1R_EL1 to itself: a pending Group 1 SGI in LR1");
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 0), frame) == 1 && fake_lr[2] == 0,
          "the same SGI while pending takes no second LR");
    frame[2] = (5ull << 24) | 1;
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 0), frame) == 1
          && (fake_lr[2] & 0xffffffff) == 5, "a second SGI takes LR2");
    frame[2] = (6ull << 24) | (1ull << 40);
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 0), frame) == 1 && fake_lr[3] == 0,
          "IRM 1 (every other CPU) reaches no one");
    frame[2] = (6ull << 24) | 2;
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 0), frame) == 1 && fake_lr[3] == 0,
          "an SGI to an absent CPU is discarded");
    frame[2] = (7ull << 24) | 1;
    monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 0), frame);
    frame[2] = (8ull << 24) | 1;
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 0), frame) == 0,
          "no free list register is a named stop");
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 7, 2, 0), frame) == 0,
          "ICC_SGI0R_EL1 is a named stop, not silently ignored");
    CHECK(monitor_sysreg(SYSREG(3, 0, 12, 11, 5, 2, 1), frame) == 0,
          "a read of ICC_SGI1R_EL1 is a named stop");
    fake_tlbi_count = 0;
    CHECK(monitor_sysreg(SYSREG(1, 0, 8, 2, 5, 8, 0), frame) == 1 && fake_tlbi_count == 1,
          "TLBI RVALE1IS (op0=1, CRn=8): served via mon_tlbi_vmalle1is");
    CHECK(monitor_sysreg(SYSREG(1, 0, 8, 7, 0, 0, 0), frame) == 1 && fake_tlbi_count == 2,
          "TLBI VMALLE1IS (op0=1, CRn=8): served via mon_tlbi_vmalle1is");

    /* No syndrome (ISV 0): the instruction is decoded. LDR x5, [x6] and
     * STRB w7, [x6] against the PL011 flags and data registers. */
    insn_word = 0xf94000c5;                                      /* ldr x5, [x6] */
    frame[6] = 0;
    CHECK(monitor_data_abort(0x24ull << 26, MON_UART_IPA + 0x18, 0x1000, frame) == 1
          && frame[5] == 0x90, "a decoded 64-bit load is served");
    insn_word = 0x390000c7;                                      /* strb w7, [x6] */
    frame[7] = 'Y';
    uart_len = 0;
    CHECK(monitor_data_abort((0x24ull << 26) | (1ull << 6), MON_UART_IPA, 0x1000, frame) == 1
          && uart_len == 1, "a decoded byte store is served");
    insn_word = 0xf84180c5;                                      /* ldur x5, [x6, #0x18] (unscaled, no writeback) */
    frame[5] = 0;
    frame[6] = 0x5500;
    CHECK(monitor_data_abort(0x24ull << 26, MON_UART_IPA + 0x18, 0x1000, frame) == 1
          && frame[5] == 0x90 && frame[6] == 0x5500,
          "an unscaled immediate load (ldur) does not write back to the base register");
    insn_word = 0xf84184c5;                                      /* ldr x5, [x6], #0x18 (post-index writeback) */
    frame[5] = 0;
    frame[6] = 0x5500;
    CHECK(monitor_data_abort(0x24ull << 26, MON_UART_IPA + 0x18, 0x1000, frame) == 1
          && frame[5] == 0x90 && frame[6] == 0x5518,
          "a post-index load applies writeback to the base register");
    insn_word = 0xa9400be5;                                      /* ldp x5, x6, [x7] */
    CHECK(monitor_data_abort(0x24ull << 26, MON_UART_IPA, 0x1000, frame) == 0,
          "an instruction the decoder does not know stops by name");

    /* Timer delivery: a physical timer IRQ fills LR0 as a pending
     * virtual 27 with its EOI bit; a maintenance IRQ leaves it alone. */
    fake_lr0 = 0;
    fake_iar = MON_INTID_VTIMER;
    monitor_irq();
    CHECK((fake_lr0 >> 62) == 1 && (fake_lr0 & 0xffffffffull) == MON_INTID_VTIMER
          && (fake_lr0 & (1ull << 41)), "the timer IRQ is injected through LR0");
    fake_iar = MON_INTID_MAINT;
    monitor_irq();
    CHECK((fake_lr0 >> 62) == 1, "a maintenance IRQ does not clear LR0");
    /* After the guest deactivates, LR0 is invalid with EOI still set;
     * left so, it keeps the maintenance PPI asserted (an EL2 storm). */
    fake_lr0 &= ~(3ull << 62);
    fake_iar = MON_INTID_MAINT;
    monitor_irq();
    CHECK(fake_lr0 == 0, "a maintenance IRQ clears a spent LR0 and its EOI bit");
    fake_lr0 = 0;
    fake_iar = MON_INTID_VTIMER;
    monitor_fiq();
    CHECK((fake_lr0 >> 62) == 1 && (fake_lr0 & 0xffffffffull) == MON_INTID_VTIMER,
          "a timer FIQ (Group 0) is injected through LR0");

    /* monitor_init and the platform GIC (Phase 3 decision 4's writes). */
    const uint32_t ppis = (1u << MON_INTID_MAINT) | (1u << MON_INTID_HYPTIMER) | (1u << MON_INTID_VTIMER);
    /* Two security states, as the gaokun3's GIC most likely has: the
     * firmware left ARE_NS and EnableGrp1A set, so the distributor is not
     * written; the boot CPU (MPIDR affinity 0.0.1.0) is frame 1. */
    fake_gic(0x12, 0x81000100ull);
    CHECK(monitor_init(0) == 1 && monitor_gic_report() == 1, "two states, enables set: no stop");
    CHECK(redist_n == 1 && redist_addrs[0] == PHYS_GICR_BASE + 0x20000 + 0x10100 && redist_vals[0] == ppis,
          "two states, enables set: the only write enables PPIs 25, 26, and 27 on the boot CPU's frame");
    /* EnableGrp1A clear: set it alone, keeping ARE_NS. */
    fake_gic(0x10, 0x81000000ull);
    monitor_init(0);
    CHECK(redist_n == 2 && redist_addrs[0] == PHYS_GICD_BASE && redist_vals[0] == 0x12,
          "two states: GICD_CTLR gains EnableGrp1A and keeps ARE_NS");
    CHECK(redist_addrs[1] == PHYS_GICR_BASE + 0x10100 && redist_vals[1] == ppis,
          "two states: no IGROUPR0 or WAKER write, then PPIs 25, 26, and 27 only");
    /* ARE_NS clear: not the state the firmware should leave; stop first. */
    fake_gic(0x02, 0x81000000ull);
    CHECK(monitor_init(0) == 0 && redist_n == 0 && monitor_gic_report() == 0,
          "ARE_NS clear: a named stop before any write");
    /* One security state (QEMU's GIC): enable both groups, wake a sleeping
     * redistributor, move the maintenance PPI to Group 1. */
    fake_gic(0x40, 0x80000000ull);
    fake_waker[0] = 0x6;
    monitor_init(0);
    CHECK(redist_n == 4 && redist_addrs[0] == PHYS_GICD_BASE && redist_vals[0] == 0x43,
          "one state: GICD_CTLR keeps DS and enables both groups");
    CHECK(redist_addrs[1] == PHYS_GICR_BASE + 0x14 && redist_vals[1] == 0x4,
          "one state: a sleeping redistributor is woken, read-modify-write");
    CHECK(redist_addrs[2] == PHYS_GICR_BASE + 0x10080 && redist_vals[2] == ((1u << MON_INTID_MAINT) | (1u << MON_INTID_HYPTIMER)),
          "one state: the maintenance and hyp-timer PPIs move to Group 1");
    CHECK(redist_vals[3] == ppis, "one state: PPIs 25, 26, and 27 only");
    /* No frame for the boot CPU: stop before any write. */
    fake_gic(0x12, 0x81000500ull);
    fake_typer[2] |= 1u << 4;                                   /* last frame */
    CHECK(monitor_init(0) == 0 && redist_n == 0,
          "no redistributor matches the boot CPU: a named stop before any write");
    /* A distributor write that never completes: stop, nothing further. */
    fake_gic(0x10, 0x81000000ull);
    fake_rwp_stuck = 1;
    CHECK(monitor_init(0) == 0 && redist_n == 1, "RWP never clears: a named stop after that one write");
    /* The handler re-arms and masks only the guest timer, on the found frame. */
    fake_gic(0x12, 0x81000100ull);
    monitor_init(0);
    fake_lr0 = 0; redist_n = 0; fake_iar = MON_INTID_VTIMER;
    monitor_irq();
    CHECK(redist_n == 1 && redist_addrs[0] == PHYS_GICR_BASE + 0x20000 + 0x10180
          && redist_vals[0] == (1u << MON_INTID_VTIMER), "the timer IRQ masks PPI 27 on the found frame");
    fake_lr0 &= ~(3ull << 62); redist_n = 0; fake_iar = MON_INTID_MAINT;
    monitor_irq();
    CHECK(redist_n == 1 && redist_vals[0] == (1u << MON_INTID_VTIMER),
          "the maintenance IRQ re-arms PPI 27 alone");
    fake_iar = 30; redist_n = 0; fake_lr0 = 0;
    monitor_irq();
    CHECK(redist_n == 0 && fake_lr0 == 0, "INTID 30 is not the monitor's: nothing injected or written");
    fake_iar = MON_INTID_HYPTIMER;
    fake_cnthp_calls = 0;
    monitor_irq();
    CHECK(fake_cnthp_calls == 1 && fake_cnthp_ticks == 1000000000u, "the EL2 timer IRQ re-arms CNTHP");

    /* panic_format: the mini-printf used to render the guest's panic
     * message (Phase 3 step H4). Pure logic over an array-backed source. */
    {
        char buf[128];
        const char *strs[2] = { "Kernel data abort", "" };
        uint64_t v1[1] = { 0x1234 };
        CHECK(fmt_check("%016llx", v1, 1, 0, 0, buf, sizeof buf) == 16
              && strcmp(buf, "0000000000001234") == 0, "panic_format: %016llx zero-pads to width");
        uint64_t v2[1] = { 0xdead };
        CHECK(fmt_check("%p", v2, 1, 0, 0, buf, sizeof buf) && strcmp(buf, "0xdead") == 0,
              "panic_format: %p prefixes 0x");
        uint64_t v3[1] = { (uint64_t)(int64_t)-5 };
        CHECK(fmt_check("%d", v3, 1, 0, 0, buf, sizeof buf) && strcmp(buf, "-5") == 0,
              "panic_format: %d renders a negative value");
        uint64_t v4[1] = { 0 };                          /* %s value is a strs index */
        CHECK(fmt_check("%s", v4, 1, strs, 2, buf, sizeof buf) && strcmp(buf, "Kernel data abort") == 0,
              "panic_format: %s reads the string argument");
        CHECK(fmt_check("a%%b", 0, 0, 0, 0, buf, sizeof buf) && strcmp(buf, "a%b") == 0,
              "panic_format: %% is a literal percent");
        uint64_t v5[1] = { 7 };
        CHECK(fmt_check("%q", v5, 1, 0, 0, buf, sizeof buf) && strcmp(buf, "%q") == 0,
              "panic_format: an unknown conversion is emitted literally, consuming no argument");
        uint64_t v6[1] = { 5 };
        CHECK(fmt_check("%d %d", v6, 1, 0, 0, buf, sizeof buf) && strcmp(buf, "5 ") == 0,
              "panic_format: a missing argument ends the message");
        /* The exact kernel-abort format XNU used in run 40. */
        uint64_t v7[5] = { 0, 0xfffffe000a065974ull, 0xfffffe0009f0a918ull, 0x4000, 1 };
        fmt_check("%s at pc 0x%016llx, lr 0x%016llx (saved state: %p%s)",
                  v7, 5, strs, 2, buf, sizeof buf);
        CHECK(strcmp(buf, "Kernel data abort at pc 0xfffffe000a065974, lr 0xfffffe0009f0a918"
                          " (saved state: 0x4000)") == 0,
              "panic_format: the XNU kernel-abort format string renders fully");
        unsigned k = 7;                                  /* overrun guard */
        CHECK(fmt_check("abcdefghij", 0, 0, 0, 0, buf, k) == k - 1 && buf[k - 1] == 0,
              "panic_format: output is bounded by cap and NUL-terminated");
    }

    /* panic_render: read the guest's format string and varargs through the
     * fake translation and format them, including the two-level va_list. */
    {
        char plines[2][PANIC_LINE_CAP];
        memset(panic_mem, 0, sizeof panic_mem);
        pm_str(0x000, "x=%d s=%s p=%p end");
        pm_str(0x100, "hi");
        pm_u64(0x200, 42);                               /* arg 0 */
        pm_u64(0x208, PANIC_VA_BASE + 0x100);            /* arg 1: the string VA */
        pm_u64(0x210, 0xdead);                           /* arg 2 */
        pm_u64(0x300, PANIC_VA_BASE + 0x200);            /* db_panic_args -> the va_list */
        unsigned ptot = panic_render(plines, 2, 44, 0, PANIC_VA_BASE, PANIC_VA_BASE + 0x300);
        CHECK(ptot == 22 && strcmp(plines[0], "x=42 s=hi p=0xdead end") == 0 && plines[1][0] == 0,
              "panic_render formats the format string with its two-level va_list arguments");

        char longfmt[101];
        for (int i = 0; i < 50; i++) longfmt[i] = 'A';
        for (int i = 50; i < 100; i++) longfmt[i] = 'B';
        longfmt[100] = 0;
        memset(panic_mem, 0, sizeof panic_mem);
        pm_str(0x000, longfmt);
        unsigned t0 = panic_render(plines, 2, 44, 0, PANIC_VA_BASE, PANIC_VA_BASE + 0x800);
        CHECK(t0 == 100 && strlen(plines[0]) == 44 && plines[0][0] == 'A',
              "panic_render page 0 shows the first window of a long message");
        unsigned t1 = panic_render(plines, 2, 44, 1, PANIC_VA_BASE, PANIC_VA_BASE + 0x800);
        CHECK(t1 == 100 && plines[0][0] == 'B' && strlen(plines[0]) == 12 && plines[1][0] == 0,
              "panic_render page 1 continues the long message");

        /* XNU's register dump separates lines with "\n\t  ": the band's
         * font drew those as "??" in run 41. Each run of white space is one
         * space now. */
        memset(panic_mem, 0, sizeof panic_mem);
        pm_str(0x000, "pc 0x%llx\n\t  x0:  0x%llx\n");
        pm_u64(0x200, 0x28);
        pm_u64(0x208, 0x40);
        pm_u64(0x300, PANIC_VA_BASE + 0x200);
        unsigned tw = panic_render(plines, 2, 44, 0, PANIC_VA_BASE, PANIC_VA_BASE + 0x300);
        CHECK(tw == 16 && strcmp(plines[0], "pc 0x28 x0: 0x40") == 0,
              "panic_render draws newlines, tabs and space runs as one space, none trailing");

        /* A format string that crosses a guest page boundary: the second
         * page is a different host buffer, so this passes only if the copy
         * translates each page on its own (run 41 cut the format at 256
         * bytes, before the saved ESR and FAR). */
        memset(panic_mem, 0, sizeof panic_mem);
        memset(panic_mem2, 0, sizeof panic_mem2);
        memcpy((char *)panic_mem + 0xff8, "ESR 0x%l", 8);
        strcpy((char *)panic_mem2, "lx FAR 0x%llx");
        pm_u64(0x200, 0x96000021);
        pm_u64(0x208, 0xfffffe000e004040ull);
        pm_u64(0x300, PANIC_VA_BASE + 0x200);
        unsigned tc = panic_render(plines, 2, 44, 0, PANIC_VA_BASE + 0xff8, PANIC_VA_BASE + 0x300);
        CHECK(tc == 37 && strcmp(plines[0], "ESR 0x96000021 FAR 0xfffffe000e004040") == 0,
              "panic_render reads a format string across a guest page boundary");

        /* A string that runs into a page that does not translate ends there
         * rather than reading past it. */
        memset(panic_mem2, 'Z', sizeof panic_mem2);
        memcpy((char *)panic_mem2 + 0xffc, "wxyz", 4);
        unsigned tu = panic_render(plines, 2, 44, 0, PANIC_VA_BASE + 0x1ffc, PANIC_VA_BASE + 0x300);
        CHECK(tu == 4 && strcmp(plines[0], "wxyz") == 0,
              "a format string ends at a page that does not translate");
        /* One with no terminator for longer than the buffer stops at it. */
        memcpy((char *)panic_mem + 0xffc, "abcd", 4);
        unsigned tl = panic_render(plines, 2, 44, 0, PANIC_VA_BASE + 0xffc, PANIC_VA_BASE + 0x300);
        CHECK(tl == 1023 && strncmp(plines[0], "abcdZZZZ", 8) == 0,
              "an unterminated format string is bounded by the 1 KiB buffer");
        CHECK(panic_render(plines, 2, 44, 0, PANIC_VA_BASE + 0x2000, PANIC_VA_BASE + 0x300) == 0
              && plines[0][0] == 0, "an untranslatable format string renders nothing");
    }

    /* The console scrollback (run 43): pages start a few lines before the
     * first "panic(" line, and the ring keeps the newest lines. */
    {
        const char *rows[4];
        char line[32];
        console_scroll_reset();
        CHECK(console_scroll_page(rows, 4, 0) == 0 && rows[0][0] == 0,
              "an empty scrollback has no pages");
        for (int i = 0; i < 10; i++) {
            snprintf(line, sizeof line, i == 5 ? "panic(cpu 0 caller 0x1): boom" : "line %d", i);
            console_scroll_add(line);
        }
        unsigned sp = console_scroll_page(rows, 4, 0);
        CHECK(sp == 2 && strcmp(rows[0], "line 2") == 0 && strcmp(rows[3], "panic(cpu 0 caller 0x1): boom") == 0,
              "the scrollback starts three lines before the first panic( line");
        console_scroll_page(rows, 4, 1);
        CHECK(strcmp(rows[0], "line 6") == 0 && strcmp(rows[3], "line 9") == 0,
              "the second page follows the panic line");
        console_scroll_page(rows, 4, 2);
        CHECK(strcmp(rows[0], "line 2") == 0, "the page number wraps to the first page");
        console_scroll_reset();
        for (int i = 0; i < 100; i++) {
            snprintf(line, sizeof line, "L%d", i);
            console_scroll_add(line);
        }
        sp = console_scroll_page(rows, 4, 0);
        CHECK(sp == 24 && strcmp(rows[0], "L4") == 0,
              "a full ring keeps the newest 96 lines and, with no panic( line, starts at the oldest");
        console_scroll_page(rows, 4, 23);
        CHECK(strcmp(rows[3], "L99") == 0, "the last page ends at the newest line");
        console_scroll_reset();
        console_scroll_add("panic(cpu 0 caller 0x2): first");
        console_scroll_add("panic(cpu 0 caller 0x3): nested");
        console_scroll_page(rows, 4, 0);
        CHECK(strcmp(rows[0], "panic(cpu 0 caller 0x2): first") == 0 && rows[2][0] == 0,
              "a nested panic pages from the first panic( line, short pages padded empty");
    }

    /* The 160-row monitor band: overwriting a note clears the old glyphs,
     * live telemetry writes line 4, timer snapshot writes lines 5..7,
     * and guest PL011 output pins the banner on line 4 and scrolls lines 5..7. */
    {
        static uint32_t fb[160 * 640];
        monitor_set_band(1, (uint64_t)(uintptr_t)fb, 640, 640 * 4, 0, 160);
        monitor_note("SETUP DONE: STAGE 2 READY, GIC NOT TOUCHED");
        monitor_note("OK");
        int tail_lit = 0;
        for (unsigned y = 6 + 2 * 20; y < 6 + 2 * 20 + 14; y++)
            for (unsigned x = 8 + 4 * 12; x < 640; x++)
                if (fb[y * 640 + x] != 0x00202020u) tail_lit = 1;
        CHECK(!tail_lit, "band_text clears the previous text on the line before drawing");

        monitor_start_telemetry();
        monitor_telemetry_hvc(0xc1000000);
        int line4_yellow = 0;
        for (unsigned y = 6 + 4 * 20; y < 6 + 4 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x00ffff40u) line4_yellow = 1;
        CHECK(line4_yellow, "monitor_start_telemetry and monitor_telemetry_hvc draw live telemetry on line 4");

        monitor_timer_snapshot();
        int line5_orange = 0, line6_orange = 0, line7_orange = 0;
        for (unsigned y = 6 + 5 * 20; y < 6 + 5 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x00ffa040u) line5_orange = 1;
        for (unsigned y = 6 + 6 * 20; y < 6 + 6 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x00ffa040u) line6_orange = 1;
        for (unsigned y = 6 + 7 * 20; y < 6 + 7 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x00ffa040u) line7_orange = 1;
        CHECK(line5_orange && line6_orange && line7_orange,
              "monitor_timer_snapshot draws registers on lines 5, 6, and 7");

        /* When panicked, monitor_timer_snapshot renders caller and panic details in green on lines 4..7 */
        fake_elr_el1 = 0xfffffe0009f0abc8ull;
        fake_elr_el2 = 0xfffffe0009eb2758ull;
        monitor_timer_snapshot();
        int panic_l4_green = 0, panic_l5_green = 0, panic_l6_green = 0;
        for (unsigned y = 6 + 4 * 20; y < 6 + 4 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x0080ff80u) panic_l4_green = 1;
        for (unsigned y = 6 + 5 * 20; y < 6 + 5 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x0080ff80u) panic_l5_green = 1;
        for (unsigned y = 6 + 6 * 20; y < 6 + 6 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x0080ff80u) panic_l6_green = 1;
        CHECK(panic_l4_green && panic_l5_green && panic_l6_green,
              "monitor_timer_snapshot renders panic caller and details in green on lines 4..7 when panicked");
        fake_elr_el1 = 0xfffffe000a065974ull;
        fake_elr_el2 = 0xfffffe000a065974ull;

        const char *stream = "Darwin Kernel Version 26.0.0\nline one\nline two\nline three\nline four\n";
        for (unsigned i = 0; stream[i]; i++)
            access(MON_UART_IPA, 0, 5, 1, (uint8_t)stream[i]);
        int line4_green = 0, line7_cyan = 0;
        for (unsigned y = 6 + 4 * 20; y < 6 + 4 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x0080ff80u) line4_green = 1;
        for (unsigned y = 6 + 7 * 20; y < 6 + 7 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x00c0ffffu) line7_cyan = 1;
        CHECK(line4_green && line7_cyan,
              "on the 160-row band, PL011 output pins the banner on line 4 and scrolls lines 5..7");

        monitor_telemetry_hvc(0xc1000001);
        monitor_timer_snapshot();
        int line4_still_green = 0;
        for (unsigned y = 6 + 4 * 20; y < 6 + 4 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x0080ff80u) line4_still_green = 1;
        CHECK(line4_still_green, "PL011 banner on line 4 is preserved against subsequent telemetry/snapshot");

        /* After the console starts, the EL2 timer keeps running (run 42
         * stopped it) and draws nothing while the guest has not panicked. */
        fake_iar = MON_INTID_HYPTIMER;
        fake_cnthp_calls = 0;
        monitor_irq();
        CHECK(fake_cnthp_calls == 1, "the EL2 timer is re-armed after the console has started");

        /* A panic after the console started pages the scrollback (cyan on
         * lines 4 to 7) under a yellow heading on line 2. */
        const char *plog = "panic(cpu 0 caller 0xfffffe0000000001): test reason\n";
        for (unsigned i = 0; plog[i]; i++) access(MON_UART_IPA, 0, 5, 1, (uint8_t)plog[i]);

        /* A console line longer than the band's 127 characters wraps. */
        for (unsigned i = 0; i < 200; i++) access(MON_UART_IPA, 0, 5, 1, 'A');
        access(MON_UART_IPA, 0, 5, 1, '\n');
        const char *rows[4];
        console_scroll_page(rows, 4, 1);
        CHECK(strlen(rows[0]) == 127 && strlen(rows[1]) == 73,
              "a 200-character console line is kept as 127 + 73, not cut");

        fake_elr_el2 = 0xfffffe0009eb2758ull;
        monitor_timer_snapshot();
        fake_elr_el2 = 0xfffffe000a065974ull;
        int head_yellow = 0, row4_cyan = 0, row4_green = 0;
        for (unsigned y = 6 + 2 * 20; y < 6 + 2 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++)
                if (fb[y * 640 + x] == 0x00ffff40u) head_yellow = 1;
        for (unsigned y = 6 + 4 * 20; y < 6 + 4 * 20 + 14; y++)
            for (unsigned x = 0; x < 640; x++) {
                if (fb[y * 640 + x] == 0x00c0ffffu) row4_cyan = 1;
                if (fb[y * 640 + x] == 0x0080ff80u) row4_green = 1;
            }
        CHECK(head_yellow && row4_cyan && !row4_green,
              "a panic after the console started pages the console scrollback on lines 4 to 7");
        console_scroll_page(rows, 4, 0);
        CHECK(strcmp(rows[0], "line two") == 0
              && strncmp(rows[3], "panic(cpu 0 caller", 18) == 0,
              "the console page shown first leads with the panic( line's context");
        CHECK(monitor_console_page(0, "PSCI SYSTEM_RESET: CONSOLE") == 2,
              "monitor_console_page draws a page and reports the page count");
        monitor_set_band(0, 0, 0, 0, 0, 0);
        CHECK(monitor_console_page(0, "X") == 0, "without the 160-row band nothing is paged");
    }

    if (failures) {
        printf("FAIL test-vmapple-monitor: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS test-vmapple-monitor: %d checks\n", checks);
    return 0;
}
