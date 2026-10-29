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
uint64_t mon_lr_read(unsigned n);    /* ICH_LR<n>_EL2, n 1 to 3 */
void mon_lr_write(unsigned n, uint64_t value);
uint64_t mon_misr_read(void);        /* ICH_MISR_EL2 */
void mon_uart_putc(char c);          /* the monitor's own PL011 at EL2 */
/* Clean [pa, pa+size) to the point of coherency (the real panel reads
 * memory, not caches); a no-op where the console is a UART. */
void mon_clean_range(uint64_t pa, uint64_t size);
void mon_redist_write(uint64_t addr, uint32_t value); /* physical GIC store */
uint32_t mon_mmio_read32(uint64_t addr);  /* physical GIC load */
uint64_t mon_mmio_read64(uint64_t addr);
uint64_t mon_mpidr(void);                 /* MPIDR_EL1 */
uint64_t mon_cntpct(void);           /* CNTPCT_EL0 */
uint64_t mon_cntfrq(void);           /* CNTFRQ_EL0 */
uint64_t mon_translate_va(uint64_t va); /* PAR_EL1 after AT S1E1R, bit0 set on fault */
void mon_cnthp_rearm(uint32_t ticks);   /* CNTHP_TVAL_EL2: next EL2 timer tick */
uint64_t mon_elr_el2(void);             /* ELR_EL2: guest PC when interrupted */
uint64_t mon_spsr_el2(void);            /* SPSR_EL2: guest PSTATE when interrupted */
uint64_t mon_esr_el1(void);             /* ESR_EL1: exception syndrome at EL1 */
uint64_t mon_far_el1(void);             /* FAR_EL1: fault address at EL1 */
uint64_t mon_elr_el1(void);             /* ELR_EL1: exception return address at EL1 */
uint64_t mon_sctlr_el1(void);           /* SCTLR_EL1: system control register at EL1 */
void mon_tlbi_vmalle1is(void);          /* Invalidate all EL1 translations at EL2 */
uint64_t mon_params_base(void);         /* Physical base address of smoke_params */

/* Called once from stub.S when the monitor flag is set: finds the boot
 * CPU's redistributor and enables the monitor's PPIs on the platform
 * GIC, read-modify-write. Returns 0 for a named stop. */
int monitor_init(int trace);
/* Arm CNTHP and start live telemetry on Line 4 right before dropping to EL1. */
void monitor_start_telemetry(void);
/* Real-time telemetry: record an HVC call and update Line 4. */
void monitor_telemetry_hvc(uint32_t fn_id);
/* Timer snapshot: capture EL1 registers and display on Lines 5..7. */
void monitor_timer_snapshot(void);
/* After monitor_set_band: draw monitor_init's GIC line, or its stop, in
 * the band. Returns 0 when monitor_init stopped; the stub then halts. */
int monitor_gic_report(void);
/* Draw a line in the band's second row and clean it to the panel
 * (diagnostic staging: the last step drawn before a halt or a reset
 * names where execution reached). */
void monitor_note(const char *s);
/* Draw an unexpected EL2 fault (vector offset, ESR_EL2, ELR_EL2) in the
 * band's stop row when no named stop has already claimed it. */
void monitor_fault(uint64_t vec, uint64_t esr, uint64_t elr);
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
 * draws its line there too. The geometry is the caller's: QEMU's RAM
 * framebuffer constants, or the real panel's from the parameter page. */
void monitor_set_band(int on, uint64_t fb_pa, unsigned width, unsigned stride,
                      unsigned guest_rows, unsigned band_rows);

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

/* A trapped system-register access (ESR_EL2 class 0x18). Returns 1 when
 * served (the caller skips the instruction), 2 for the implementation-
 * defined space the caller reads as zero and ignores writes to, and 0 for
 * a named stop, already printed. */
int monitor_sysreg(uint64_t esr, uint64_t *frame);

/* Print the handled/stopped counters on the monitor's UART. */
void monitor_print_stats(void);

/* ---- panic-message extraction (panic.c, Phase 3 step H4) --------------- */
/* The widest band line the renderer fills, including the terminator. */
#define PANIC_LINE_CAP 64

/* An abstract source of a panic format string's arguments, so the
 * formatter is host-testable without EL2 memory. `get` reads the index-th
 * 8-byte vararg slot (returns 0 on success); `str` copies the C string at
 * a guest VA into buf (returns 0 on success). */
struct panic_args {
    int (*get)(void *ctx, unsigned index, uint64_t *out);
    int (*str)(void *ctx, uint64_t va, char *buf, unsigned cap);
    void *ctx;
};

/* Format `fmt` with arguments from `a` into out[0..cap-1] (NUL-terminated).
 * Supports %s %p %x %X %u %d %i %c %% with optional width and zero-pad;
 * length modifiers are consumed and every slot is read as 64 bits. An
 * argument it cannot read ends the message; an unreadable %s becomes
 * "<?>". Returns the number of characters written. */
unsigned panic_format(char *out, unsigned cap, const char *fmt,
                      const struct panic_args *a);

/* EL2 only: read the guest's panic format string at fmt_va and its
 * varargs at args_va, format the message, and copy the `page`-th window
 * of up to `maxlines` lines of `width` chars into out. Returns the full
 * message length, so the caller can page a long message across snapshots.
 * Reads guest memory through mon_translate_va. */
unsigned panic_render(char out[][PANIC_LINE_CAP], unsigned maxlines, unsigned width,
                      unsigned page, uint64_t fmt_va, uint64_t args_va);

extern uint64_t monitor_handled;     /* accesses a model answered */
extern uint64_t monitor_stop_ipa;    /* IPA of the named stop, if any */
extern int monitor_stop_write;       /* the stopped access was a write */
extern uint64_t monitor_stop_count;  /* unmodelled accesses (stops at 1) */

#endif
