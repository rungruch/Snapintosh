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
void mon_cnthp_rearm(uint32_t ticks) { (void)ticks; }
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
    (void)va;
    return (uint64_t)(uintptr_t)insn_page;   /* PAR form: PA in [47:12] */
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
    const uint32_t ppis = (1u << MON_INTID_MAINT) | (1u << MON_INTID_VTIMER);
    /* Two security states, as the gaokun3's GIC most likely has: the
     * firmware left ARE_NS and EnableGrp1A set, so the distributor is not
     * written; the boot CPU (MPIDR affinity 0.0.1.0) is frame 1. */
    fake_gic(0x12, 0x81000100ull);
    CHECK(monitor_init(0) == 1 && monitor_gic_report() == 1, "two states, enables set: no stop");
    CHECK(redist_n == 1 && redist_addrs[0] == PHYS_GICR_BASE + 0x20000 + 0x10100 && redist_vals[0] == ppis,
          "two states, enables set: the only write enables PPIs 25 and 27 on the boot CPU's frame");
    /* EnableGrp1A clear: set it alone, keeping ARE_NS. */
    fake_gic(0x10, 0x81000000ull);
    monitor_init(0);
    CHECK(redist_n == 2 && redist_addrs[0] == PHYS_GICD_BASE && redist_vals[0] == 0x12,
          "two states: GICD_CTLR gains EnableGrp1A and keeps ARE_NS");
    CHECK(redist_addrs[1] == PHYS_GICR_BASE + 0x10100 && redist_vals[1] == ppis,
          "two states: no IGROUPR0 or WAKER write, then PPIs 25 and 27 only");
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
    CHECK(redist_addrs[2] == PHYS_GICR_BASE + 0x10080 && redist_vals[2] == (1u << MON_INTID_MAINT),
          "one state: the maintenance PPI moves to Group 1");
    CHECK(redist_vals[3] == ppis, "one state: PPIs 25 and 27 only");
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

    if (failures) {
        printf("FAIL test-vmapple-monitor: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS test-vmapple-monitor: %d checks\n", checks);
    return 0;
}
