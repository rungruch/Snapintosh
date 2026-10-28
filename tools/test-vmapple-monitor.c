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
uint64_t mon_misr_read(void) { return 0; }
void mon_cnthp_rearm(uint32_t ticks) { (void)ticks; }
static uint64_t last_redist_addr; static uint32_t last_redist_value;
void mon_redist_write(uint64_t addr, uint32_t value) { last_redist_addr = addr; last_redist_value = value; }
void mon_uart_putc(char c) { if (uart_len < sizeof(uart_capture) - 1) uart_capture[uart_len++] = c; }
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
    monitor_set_policy(0, 0);

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

    /* Default-deny: an unmodelled IPA and both fixed addresses stop. */
    CHECK(access(0x20000000, 2, 5, 0, 0) == 0 && monitor_stop_ipa == 0x20000000,
          "an unmodelled IPA stops by name");
    CHECK(access(MON_FIXED_ID_IPA, 2, 5, 0, 0) == 0 && monitor_stop_ipa == MON_FIXED_ID_IPA,
          "the 0x30010000 fixed address stops by default");
    CHECK(access(0x004ffff8, 3, 5, 1, 0x1000) == 0 && monitor_stop_ipa == 0x004ffff8,
          "the 0x004ffff8 fixed address stops by default");

    /* The diagnostic gate answers them as absent devices, nothing more. */
    monitor_set_policy(1, 0);
    CHECK(access(MON_FIXED_ID_IPA, 2, 5, 0, 0) == 1 && frame[5] == 0xffffffffull,
          "absent-fixed: the ID read is all-ones");
    CHECK(access(0x004ffff8, 3, 5, 1, 0x1000) == 1, "absent-fixed: the request store is sunk");

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
    fake_lr0 = 0;
    fake_iar = MON_INTID_VTIMER;
    monitor_fiq();
    CHECK((fake_lr0 >> 62) == 1 && (fake_lr0 & 0xffffffffull) == MON_INTID_VTIMER,
          "a timer FIQ (Group 0) is injected through LR0");

    if (failures) {
        printf("FAIL test-vmapple-monitor: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS test-vmapple-monitor: %d checks\n", checks);
    return 0;
}
