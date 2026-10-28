/* SPDX-License-Identifier: MIT */
/* The Route B monitor's device models for the QEMU smoke (Phase 3 P3).
 *
 * monitor.c is freestanding EL2 code called from stub.S; it is also built
 * on the host by tools/test-vmapple-monitor.c, which supplies the mon_*
 * helpers below as fakes. The models are the P3 answers to the windows P1
 * measured, under Phase 3 decision 5 (the owner's, 2026-10-28):
 * published-contract models for the PL011, PL031, PL061, the GICv3
 * distributor and redistributors, the MSI frame and PCIe configuration
 * space with no functions; the paravirtual devices are absent from the
 * tree (the loader drops their nodes); AppleVirtualPlatform's fixed
 * addresses answered as Apple's virtual machine does where that is known
 * (the backdoor interface and the configuration region), and the mailbox
 * store absorbed with no answer.
 *
 * Everything the guest sees is one of these models. Stage 2 maps only
 * guest RAM, so any other IPA faults here, and an IPA with no model is a
 * named stop, never a fabricated answer.
 */
#ifndef VMAPPLE_SMOKE_MONITOR_H
#define VMAPPLE_SMOKE_MONITOR_H

#include <stdint.h>

/* Helpers stub.S provides (faked by the host test). */
uint64_t mon_iar1_read(void);        /* ICC_IAR1_EL1: acknowledge physical IRQ */
uint64_t mon_iar0_read(void);        /* ICC_IAR0_EL1: acknowledge physical FIQ */
void mon_eoir0_write(uint64_t iar);  /* ICC_EOIR0_EL1 */
void mon_eoir1_write(uint64_t iar);  /* ICC_EOIR1_EL1: drop priority + deactivate */
uint64_t mon_lr0_read(void);         /* ICH_LR0_EL2 */
void mon_lr0_write(uint64_t value);  /* ICH_LR0_EL2 */
uint64_t mon_misr_read(void);        /* ICH_MISR_EL2 */
void mon_uart_putc(char c);          /* the monitor's own PL011 at EL2 */
void mon_redist_write(uint64_t addr, uint32_t value); /* physical GICR store */
uint64_t mon_cntpct(void);           /* CNTPCT_EL0 */
uint64_t mon_cntfrq(void);           /* CNTFRQ_EL0 */
uint64_t mon_translate_va(uint64_t va); /* PAR_EL1 after AT S1E1R, bit0 set on fault */
void mon_cnthp_rearm(uint32_t ticks);   /* CNTHP_TVAL_EL2: next EL2 timer tick */

/* Called once from stub.S when the monitor flag is set. */
void monitor_init(int trace);
/* Policy only, without the physical GIC writes (the host test's entry). */
void monitor_set_policy(int trace);
/* The guest's boot_args physical address, read once for its memSize (the
 * configuration region's RAM size). */
void monitor_set_guest(uint64_t boot_args_pa);
/* Debug only: answer the secondary windows as the Phase 0C smoke's QEMU
 * window did (all-ones reads, sunk writes), to bisect model fidelity. */
void monitor_set_baseline_answers(int on);
/* The split console (PARAM_FLAG_FRAMEBUFFER): clear the monitor's band
 * under the guest's framebuffer rows and draw its banner; a named stop then
 * draws its line there too. */
void monitor_set_band(int on);

/* A data abort from the guest. esr is ESR_EL2, ipa the faulting IPA from
 * HPFAR_EL2, elr the guest VA of the faulting instruction, frame the
 * saved x0..x30. Stage 2 translation faults carry no access syndrome
 * (ISV is 0), so the instruction is decoded when the syndrome is absent.
 * Returns 1 when a model handled the access (the caller advances ELR
 * past the instruction), 0 for a named stop: the stop has already been
 * printed with monitor_stop_ipa and friends filled. */
int monitor_data_abort(uint64_t esr, uint64_t ipa, uint64_t elr, uint64_t *frame);

/* A physical IRQ taken at EL2: timer delivery through list register 0
 * and the maintenance interrupt that re-arms it. */
void monitor_irq(void);
/* A physical FIQ taken at EL2. On this platform the timer PPIs are
 * Group 0 and signal as FIQ (the Phase 0C boot took 175 FIQs at EL1 and
 * one IRQ), so this is the path the guest's ticks actually take. */
void monitor_fiq(void);

/* Print the handled/stopped counters on the monitor's UART. */
void monitor_print_stats(void);

extern uint64_t monitor_handled;     /* accesses a model answered */
extern uint64_t monitor_stop_ipa;    /* IPA of the named stop, if any */
extern int monitor_stop_write;       /* the stopped access was a write */
extern uint64_t monitor_stop_count;  /* unmodelled accesses (stops at 1) */

#endif
