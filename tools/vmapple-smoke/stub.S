/* SPDX-License-Identifier: MIT */
/*
 * A minimal EL2 stub for the Route B QEMU smoke test (Phase 0C). It stands in
 * for the parts of a Snapintosh VirtualMac2,1 monitor that the stock kernel
 * needs before it can print: it enters the guest at EL1, serves the Apple PAC
 * hypercalls (with collapsed key domains, the only thing standard PAuth can
 * do -- see docs/GAOKUN3-PHASE0B.md B6), answers the OEM and nested services
 * "not supported", emulates Apple IMPDEF system registers trapped by
 * HCR_EL2.TIDCP as RAZ/WI, and records any unexpected EL2 exception so the
 * harness can say where the guest died.
 *
 * It is NOT a hypervisor: no stage 2, no second CPU, no device models beyond a
 * direct PL011 for breadcrumbs. It exists to answer two Phase 0C questions on
 * TCG -- can the PAC calls be served on standard PAuth, and does the kernel hit
 * Apple CPU behaviour EL2 cannot supply -- as far as early boot reaches.
 *
 * Entry is at EL2 (QEMU virt, virtualization=on, no EL3). x0..x4 are whatever
 * QEMU's loader left; the stub reads everything it needs from PARAMS_PA.
 */
#include "layout.h"

.macro PUTC reg, tmp, ch
	mov	\tmp, #\ch
	str	\tmp, [\reg]
.endm

.section .text.start, "ax"
.global _start
_start:
	mov	x23, x0			// candidate parameter page (q1n1's hand-off)
	// llvm-objcopy -O binary omits NOBITS (.bss); zero it before any
	// state, stage-2 table, or stack slot is touched.
	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
0:	cmp	x0, x1
	b.hs	0f
	stp	xzr, xzr, [x0], #16
	b	0b
0:
	// EL2 stack: top of the 16 KiB region just below PARAMS_PA scratch.
	ldr	x0, =stack_top
	mov	sp, x0

	// Vectors.
	ldr	x0, =el2_vectors
	msr	VBAR_EL2, x0
	isb

	// HCR_EL2: RW (bit 31, EL1 is AArch64), API (41) | APK (40) so PAuth
	// runs natively at EL1, TIDCP (bit 52) so Apple IMPDEF sysreg use at
	// EL1 traps to EL2. Bit 20/21 (TWE/TWI) stay clear: WFI is the guest's.
	ldr	x0, =0x30080100000	// HCR.RW | API | APK | TIDCP
	msr	HCR_EL2, x0

	// CPTR_EL2: do not trap FP/SIMD (FPEN 0b11), SVE (ZEN 0b11), or SME
	// (SMEN 0b11). QEMU -cpu max advertises SVE/SME and the VirtualMac
	// kernel initialises SMCR_EL1 before serial. This is a QEMU positive
	// control only: the gaokun3 reports both absent, so its profile must
	// make that kernel path unreachable or reject it explicitly.
#ifdef VMAPPLE_GAOKUN3_MONITOR
	// Non-VHE CPTR_EL2 on Cortex-A78C (HCR_EL2.E2H is 0 after q1n1's
	// VHE exit): RES1 bits [13:12, 9:0], TFP (bit 10) clear so FP/SIMD
	// does not trap. Verified on the gaokun3 in runs 11 to 15.
	ldr	x0, =0x33ff
#else
	ldr	x0, =0x03330000
#endif
	msr	CPTR_EL2, x0
	msr	MDCR_EL2, xzr

	// Let EL1 read the physical counter and timer; no virtual offset.
	mov	x0, #3
	msr	CNTHCTL_EL2, x0
	msr	CNTVOFF_EL2, xzr

	// GICv3 system registers usable at EL1 (SRE|Enable|DFB|DIB).
	mrs	x0, ICC_SRE_EL2
	orr	x0, x0, #0xf
	msr	ICC_SRE_EL2, x0
	isb

	// EL1 starts with the MMU off; leave SCTLR_EL1 at its reset M=0.

	// The parameter page: x0 from q1n1's hand-off when it carries the
	// magic, else the harness's fixed page in QEMU. On the gaokun3 there
	// is no fixed page at 0x43000000, so a missing hand-off halts cleanly
	// instead of reading unmapped memory.
	ldr	x0, =params_base
#ifdef VMAPPLE_GAOKUN3_MONITOR
	mov	x1, xzr
#else
	ldr	x1, =PARAMS_PA
#endif
	str	x1, [x0]
	cbz	x23, 9f
	tst	x23, #0xfff
	b.ne	9f
	ldr	x1, [x23, #PARAM_MAGIC]
	ldr	x2, =SMOKE_PARAMS_MAGIC
	cmp	x1, x2
	b.ne	9f
	str	x23, [x0]
9:
	// Read the parameters the harness placed for us.
	ldr	x19, =params_base
	ldr	x19, [x19]
	cbz	x19, param_bad
	ldr	x0, [x19, #PARAM_MAGIC]
	ldr	x1, =SMOKE_PARAMS_MAGIC
	cmp	x0, x1
	b.ne	param_bad
	ldr	x20, [x19, #PARAM_ENTRY]
	ldr	x21, [x19, #PARAM_X0]
	ldr	x22, [x19, #PARAM_FLAGS]

	// Route B monitor (Phase 3 P3), only with PARAM_FLAG_MONITOR: stage 2
	// default-deny, the virtual GICv3 interface, and physical IRQ routing
	// to EL2. Without the flag this stub is exactly the Phase 0C smoke.
	tbz	x22, #1, 1f
	// The guest RAM stage 2 maps: 2 MiB aligned, non-empty, inside GiB 1
	// and 2, the only ones monitor_setup has L2 tables for.
	ldr	x0, [x19, #PARAM_S2_LO]
	ldr	x1, [x19, #PARAM_S2_HI]
	orr	x2, x0, x1
	tst	x2, #0x1fffff
	b.ne	param_bad
	cmp	x0, x1
	b.hs	param_bad
	mov	x2, #0x40000000
	cmp	x0, x2
	b.lo	param_bad
#ifdef VMAPPLE_GAOKUN3_MONITOR
	mov	x2, #0x180000000	// the gaokun3 guest sits in GiB 4 to 5
#else
	mov	x2, #0xc0000000
#endif
	cmp	x1, x2
	b.hi	param_bad
	ldr	x0, =pt_flag
	str	x22, [x0]
#ifdef VMAPPLE_GAOKUN3_MONITOR
	// Draw the band before anything risky: after runs 30/31 restarted
	// in this window, the band is the only evidence channel, and which
	// step line it shows names where execution reached.
	mov	x0, x22
	lsr	x0, x0, #8
	and	x0, x0, #1
	ldr	x1, [x19, #PARAM_FB_PA]
	ldr	x2, [x19, #PARAM_FB_WIDTH]
	ldr	x3, [x19, #PARAM_FB_STRIDE]
	ldr	x4, [x19, #PARAM_FB_GUEST_ROWS]
	ldr	x5, [x19, #PARAM_FB_BAND_ROWS]
	bl	monitor_set_band
#endif
	bl	monitor_setup
#ifdef VMAPPLE_GAOKUN3_MONITOR
	ldr	x0, =msg_setup_done
	bl	monitor_note
	ldr	x0, =pt_flag
	ldr	x0, [x0]
	tbz	x0, #10, 8f
	// Diagnostic hold, interrupts masked: monitor_setup has enabled
	// the EL2 interrupt interface, and an unmasked halt could take a
	// firmware timer interrupt into the IRQ path (and its GIC writes)
	// that this build exists to exclude (run 32).
	msr	daifset, #0xf
7:	wfi
	b	7b
8:
#endif
	and	x0, x22, #1		// tracing
	bl	monitor_init
	mov	x0, x21			// boot_args, for the config region's RAM size
	bl	monitor_set_guest
	mov	x0, x22
	lsr	x0, x0, #6		// baseline-answers debug flag
	and	x0, x0, #1
	bl	monitor_set_baseline_answers
#ifndef VMAPPLE_GAOKUN3_MONITOR
	mov	x0, x22
	lsr	x0, x0, #8		// split console
	and	x0, x0, #1
	ldr	x1, =MON_FB_PA
	mov	x2, #MON_FB_WIDTH
	mov	x3, #MON_FB_STRIDE
	mov	x4, #MON_FB_GUEST_ROWS
	mov	x5, #MON_FB_BAND_ROWS
	bl	monitor_set_band
#endif
	// The GIC line, or monitor_init's named stop, now that the band is
	// drawn. A stop halts here: nothing of the guest has run.
	bl	monitor_gic_report
	cbnz	w0, 1f
	b	halt
1:	ldr	x0, =pt_flag
	ldr	x0, [x0]
	tbnz	x0, #9, halt		// H2: monitor up, guest not entered

	// Start the TIDCP log empty, so a stale page cannot look like data.
	ldr	x0, =params_base
	ldr	x0, [x0]
	add	x0, x0, #0x2000			// the TIDCP log page
	ldr	x1, =TIDCP_LOG_MAGIC
	str	x1, [x0, #TIDCP_MAGIC]
	str	xzr, [x0, #TIDCP_USED]
	str	xzr, [x0, #TIDCP_TOTAL]
	str	xzr, [x0, #TIDCP_DROPPED]

	ldr	x0, =msg_enter
	bl	uart_puts
	// The harness checks the device tree's timebase against this.
	ldr	x0, =msg_cntfrq
	bl	uart_puts
	mrs	x0, CNTFRQ_EL0
	bl	uart_hex
#ifdef VMAPPLE_GAOKUN3_MONITOR
	ldr	x0, =msg_enter_band
	bl	monitor_note
#endif
	bl	monitor_start_telemetry

#ifdef VMAPPLE_GAOKUN3_MONITOR
	// gaokun3 only. Bypass the stock 26A428 kernel's ARMv8.4 Range TLBI
	// instructions by flipping two conditional branches in its in-memory
	// image to unconditional, so the CPU never decodes an unallocated
	// opcode. The Cortex-A78C implements ARMv8.2 (ID_AA64ISAR0_EL1.TLB=1,
	// FEAT_TLBIOS) and lacks FEAT_TLBIRANGE (TLB=2); TLBI RVALE1IS then
	// takes an Undefined Instruction exception the hypervisor cannot trap
	// (HCR_EL2.TTLB does not see unallocated encodings -- VERIFIED runs 38,
	// 39). QEMU's -cpu max implements FEAT_TLBIRANGE, so its build takes
	// the kernel's native range path and this rewrite is both unnecessary
	// and absent, keeping the QEMU smoke a faithful stock-kernel baseline.
	//
	// This is a kernel patch (it changes guest instructions, not the file
	// on disk), admitted only for the reviewed kernel. Its provenance:
	//   - the vma2macosap-26A428 profile pins the kernelcache SHA-256
	//     (95f42a48..., probes/guestprofile.py), and q1n1 verifies every
	//     bundle chunk's hash in place before entry (run 28 onward);
	//   - each site is rewritten only if it still holds the exact stock
	//     branch word (the cmp/b.ne guard below), so a changed kernel is
	//     left untouched rather than mis-patched.
	// Site 1: pmap_flush_tlb_range (0xfffffe000a065870): b.lo +0x90 -> b +0x90
	//         (bypasses tlbi rvale1is / rvaale1is, uses the single-page loop)
	// Site 2: pmap global flush (0xfffffe000a06e014): b.hi +0x28 -> b +0x28
	//         (bypasses tlbi rvaae1, uses tlbi vmalle1; one CPU, so a
	//          non-broadcast VMALLE1 suffices -- revisit for more vCPUs)
	ldr	x1, =params_base
	ldr	x1, [x1]
	ldr	x0, [x1, #PARAM_ENTRY]
	ldr	x2, =0x2eb0480		// entry_va - mh_va
	sub	x0, x0, x2		// x0 = phys_base
	ldr	x2, =0x3061870		// offset to site 1
	add	x3, x0, x2
	ldr	w4, [x3]
	ldr	w5, =0x54000483		// b.lo +0x90
	cmp	w4, w5
	b.ne	1f
	ldr	w5, =0x14000024		// b +0x90
	str	w5, [x3]
	dc	civac, x3
1:
	ldr	x2, =0x306a014		// offset to site 2
	add	x3, x0, x2
	ldr	w4, [x3]
	ldr	w5, =0x54000148		// b.hi +0x28
	cmp	w4, w5
	b.ne	2f
	ldr	w5, =0x1400000a		// b +0x28
	str	w5, [x3]
	dc	civac, x3
2:
	dsb	sy
	isb
#endif

	// Reset EL1 state: on the gaokun3 UEFI ran at EL1 with M=1, C=1, I=1
	// before slbounce entered EL2, so SCTLR_EL1 and its companions still
	// hold the firmware's values unless cleared here.
	mrs	x0, MIDR_EL1
	msr	VPIDR_EL2, x0
	mov	x0, #0x80000000		// uniprocessor-style affinity 0.0.0.0
	msr	VMPIDR_EL2, x0
	ldr	x0, =0x30d00800		// architectural RES1 bits; M, C, I all 0
	msr	SCTLR_EL1, x0
	msr	TCR_EL1, xzr
	msr	TTBR0_EL1, xzr
	msr	TTBR1_EL1, xzr
	msr	MAIR_EL1, xzr
	msr	AMAIR_EL1, xzr
	msr	VBAR_EL1, xzr
	ldr	x0, =0x03330000		// FPEN 0b11 (no trap FP/SIMD), ZEN 0b11, SMEN 0b11
	msr	CPACR_EL1, x0
	msr	CONTEXTIDR_EL1, xzr
	msr	MDSCR_EL1, xzr
	msr	SP_EL1, xzr
	msr	SP_EL0, xzr
	msr	TPIDR_EL1, xzr
	msr	TPIDR_EL0, xzr
	msr	TPIDRRO_EL0, xzr
	msr	CNTV_CTL_EL0, xzr
	msr	CNTKCTL_EL1, xzr
	tlbi	vmalls12e1is
	ic	iallu
	dsb	sy
	isb

	// Drop to EL1h with DAIF masked, at the guest entry, x0 = handoff.
	msr	ELR_EL2, x20
	mov	x0, #0x3c5
	msr	SPSR_EL2, x0
	mov	x0, x21
	// Clear the rest so the guest sees a clean register file.
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	mov	x8, xzr
	mov	x9, xzr
	mov	x10, xzr
	mov	x11, xzr
	mov	x12, xzr
	mov	x13, xzr
	mov	x14, xzr
	mov	x15, xzr
	mov	x16, xzr
	mov	x17, xzr
	mov	x18, xzr
	mov	x19, xzr
	mov	x20, xzr
	mov	x21, xzr
	mov	x22, xzr
	mov	x23, xzr
	mov	x24, xzr
	mov	x25, xzr
	mov	x26, xzr
	mov	x27, xzr
	mov	x28, xzr
	mov	x29, xzr
	mov	x30, xzr
	eret

param_bad:
	ldr	x0, =msg_noparam
	bl	uart_puts
	b	halt

/* ---- UART (PL011 data register; TCG accepts a bare store) --------------- */
uart_puts:
#ifdef VMAPPLE_GAOKUN3_MONITOR
	ret				// no PL011: the monitor's band is the console
#endif
	ldr	x1, =PL011_BASE
1:	ldrb	w2, [x0], #1
	cbz	w2, 2f
	str	w2, [x1]
	b	1b
2:	ret

// x0 = 64-bit value to print as 16 hex digits.
uart_hex:
#ifdef VMAPPLE_GAOKUN3_MONITOR
	ret
#endif
	ldr	x1, =PL011_BASE
	mov	x3, #60
1:	lsr	x2, x0, x3
	and	x2, x2, #0xf
	cmp	x2, #10
	b.lo	2f
	add	x2, x2, #('a' - 10)
	b	3f
2:	add	x2, x2, #'0'
3:	str	w2, [x1]
	subs	x3, x3, #4
	b.ge	1b
	mov	w2, #'\n'
	str	w2, [x1]
	ret

/* ---- Exception vectors -------------------------------------------------- */
.align 11
el2_vectors:
	.align 7			// 0x000 Current EL, SP0: sync
	b	fault_cur_sp0
	.align 7			// 0x080 IRQ (current EL: the monitor's own tick)
	b	el2_irq_current
	.align 7			// 0x100 FIQ
	b	el2_fiq_current
	.align 7			// 0x180 SError
	b	fault_cur_sp0
	.align 7			// 0x200 Current EL, SPx: sync
	b	fault_cur_sp0
	.align 7			// 0x280 IRQ
	b	el2_irq_current
	.align 7			// 0x300 FIQ
	b	el2_fiq_current
	.align 7			// 0x380 SError
	b	fault_cur_sp0
	.align 7			// 0x400 Lower EL, AArch64: sync (hvc, sysreg)
	b	el2_sync_lower
	.align 7			// 0x480 IRQ
	b	el2_irq_lower
	.align 7			// 0x500 FIQ
	b	el2_fiq_lower
	.align 7			// 0x580 SError
	b	fault_lower_irq
	.align 7			// 0x600 Lower EL, AArch32: sync, not expected
	b	fault_lower32
	.align 7			// 0x680 IRQ
	b	fault_lower32
	.align 7			// 0x700 FIQ
	b	fault_lower32
	.align 7			// 0x780 SError
	b	fault_lower32

/* Save x0..x30 to the EL2 stack; frame base returned in x29. */
.macro SAVE_FRAME
	sub	sp, sp, #256
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	mov	x29, sp
.endm

.macro RESTORE_FRAME
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #256
.endm

el2_sync_lower:
	SAVE_FRAME
	mrs	x25, ESR_EL2
	lsr	x24, x25, #26		// EC
	cmp	x24, #0x16		// HVC64
	b.eq	do_hvc
	cmp	x24, #0x18		// trapped MSR/MRS/system (TIDCP et al.)
	b.eq	do_sysreg
	cmp	x24, #0x24		// data abort from EL1 (a device IPA, in monitor mode)
	b.eq	do_abort
	cmp	x24, #0x20		// instruction abort from EL1
	b.eq	do_abort
	// Anything else from EL1 is unexpected: record and halt.
	mov	x0, #0x400
	b	record_and_halt

/* Stage-2 fault: a device model in monitor.c answers it, or the run
 * stops by name (default-deny). Only in monitor mode. */
do_abort:
	ldr	x0, =monitor_enabled
	ldr	x0, [x0]
	cbz	x0, 1f
	mov	x0, x25
	// For a stage 2 fault FAR_EL2 holds the guest VA; the IPA is in
	// HPFAR_EL2 as bits [47:12] in its [39:4].
	mrs	x1, HPFAR_EL2
	lsr	x1, x1, #4
	lsl	x1, x1, #12
	mrs	x3, FAR_EL2
	and	x3, x3, #0xfff
	orr	x1, x1, x3
	mrs	x2, ELR_EL2		// faulting guest VA, for instruction decode
	mov	x3, x29			// the saved guest frame
	bl	monitor_data_abort
	cbnz	x0, 2f
	mov	x0, #0x400		// named stop: record and end the run
	b	record_and_halt
2:	mrs	x0, ELR_EL2
	add	x0, x0, #4
	msr	ELR_EL2, x0
	RESTORE_FRAME
	eret
1:	mov	x0, #0x400
	b	record_and_halt

el2_fiq_lower:
	SAVE_FRAME
	ldr	x0, =monitor_enabled
	ldr	x0, [x0]
	cbz	x0, fault_lower_fiq
	bl	monitor_fiq
	RESTORE_FRAME
	eret

/* An IRQ/FIQ taken while at EL2 (the monitor's tick can land inside a
 * fault handler): serve it and resume EL2 code. SPSR_EL2 is saved by the
 * entry and restored by eret; ELR_EL2 is untouched by async exceptions. */
el2_irq_current:
	SAVE_FRAME
	ldr	x0, =monitor_enabled
	ldr	x0, [x0]
	cbz	x0, fault_cur_sp0
	bl	monitor_irq
	RESTORE_FRAME
	eret
el2_fiq_current:
	SAVE_FRAME
	ldr	x0, =monitor_enabled
	ldr	x0, [x0]
	cbz	x0, fault_cur_sp0
	bl	monitor_fiq
	RESTORE_FRAME
	eret

el2_irq_lower:
	SAVE_FRAME
	ldr	x0, =monitor_enabled
	ldr	x0, [x0]
	cbz	x0, fault_lower_irq
	bl	monitor_irq
	RESTORE_FRAME
	eret

/* HVC: the function id is the guest's x0, saved at frame+0. */
do_hvc:
	ldr	x0, [x29, #0]
	mov	w26, w0			// low 32 bits = function id
	mov	w0, w26
	bl	monitor_telemetry_hvc

	ldr	x1, =params_base
	ldr	x1, [x1]
	ldr	x1, [x1, #PARAM_FLAGS]
	tbz	x1, #0, 1f
	ldr	x0, =msg_hvc
	bl	uart_puts
	mov	x0, x26
	bl	uart_hex
	ldr	x0, =msg_spsr
	bl	uart_puts
	mrs	x0, SPSR_EL2
	bl	uart_hex
1:
	// SMOKE_HVC_EXIT: end the run (Angel SYS_EXIT with the guest's x1).
	ldr	w0, =SMOKE_HVC_EXIT
	cmp	w26, w0
	b.eq	smoke_exit

	// PAC_NOP: answer -1 so the guest disables the whole OEM service.
	ldr	w0, =VMAPPLE_PAC_NOP
	cmp	w26, w0
	b.eq	hvc_fail

	// PAC CPU service (0xc100xxxx).
	mov	w0, w26
	and	w0, w0, #HVC_SERVICE_MASK
	ldr	w1, =HVC_CPU_SERVICE
	cmp	w0, w1
	b.eq	hvc_pac

	// PSCI (0x8400xxxx or 0xc400xxxx).
	ldr	w1, =PSCI_SERVICE_32
	cmp	w0, w1
	b.eq	hvc_psci
	ldr	w1, =PSCI_SERVICE_64
	cmp	w0, w1
	b.eq	hvc_psci

	// OEM (0xc300xxxx) and nested (0xc600xxxx) and everything else: not
	// supported. The guest treats a negative x0 as "absent" and copes.
hvc_fail:
	mov	x0, #-1
	str	x0, [x29, #0]
	b	hvc_done

hvc_pac:
	// SET_INITIAL_STATE (0xc1000000): install the one boot-wide key set.
	// Collapsed domains need the same keys on every CPU, because kernel
	// pointers signed on one CPU are authenticated on another, so the keys
	// are generated once and reused for each CPU's call.
	ldr	w1, =HVC_CPU_SERVICE
	cmp	w26, w1
	b.ne	1f
	bl	pac_install_keys
#ifdef VMAPPLE_GAOKUN3_MONITOR
	ldr	x0, =msg_pac_done
	bl	monitor_note
#endif
	b	hvc_pac_ok
1:	ldr	w1, =VMAPPLE_PAC_GET_DEFAULT_KEYS
	cmp	w26, w1
	b.ne	hvc_pac_ok
	// GET_DEFAULT_KEYS: opaque non-zero cookies in x2 (ROP) and x3 (JOP).
	ldr	x0, =SMOKE_DEFAULT_ROP_COOKIE
	str	x0, [x29, #16]
	ldr	x0, =SMOKE_DEFAULT_JOP_COOKIE
	str	x0, [x29, #24]
hvc_pac_ok:
	// Every PAC call (SET_INITIAL_STATE, SET_*_KEYS, SET_EL0_DIVERSIFIER*)
	// succeeds without touching a key: collapsed key domains.
	str	xzr, [x29, #0]
	b	hvc_done

hvc_psci:
	ldr	w1, =PSCI_VERSION_FN
	cmp	w26, w1
	b.ne	1f
	movz	x0, #1, lsl #16		// PSCI 1.1 (0x00010001)
	movk	x0, #1
	str	x0, [x29, #0]
	b	hvc_done
1:	ldr	w1, =PSCI_SYSTEM_OFF
	cmp	w26, w1
	b.eq	psci_off
	ldr	w1, =PSCI_SYSTEM_RESET
	cmp	w26, w1
	b.eq	psci_off
	mov	x0, #-1			// CPU_ON etc.: not supported here
	str	x0, [x29, #0]
	b	hvc_done

hvc_done:
	RESTORE_FRAME
	eret				// ELR_EL2 already points past the hvc

/* Fill pac_keys once (ten 64-bit words: IA, IB, DA, DB, GA, lo and hi) and
 * write them to this CPU's key registers. The source is RNDR when the CPU has
 * FEAT_RNG (QEMU -cpu max does); otherwise a counter-mixed fallback that is
 * NOT a secure source and exists only so the smoke test runs on CPUs without
 * RNG. A real monitor on the gaokun3, which has no RNDR, needs a seed from the
 * host or an SoC random source (docs/GAOKUN3-PHASE0B.md). Clobbers x0-x7. */
pac_install_keys:
	ldr	x4, =pac_keys
	ldr	x5, =pac_keys_ready
	ldr	x0, [x5]
	cbnz	x0, 4f
	mrs	x6, ID_AA64ISAR0_EL1
	ubfx	x6, x6, #60, #4		// RNDR field
	mov	x7, #0
1:	cbz	x6, 2f
	mrs	x0, s3_3_c2_c4_0	// RNDR (encoded: the assembler needs +rng)
	b.eq	1b			// NZCV.Z set: no entropy yet, retry
	b	3f
2:	mrs	x0, CNTPCT_EL0		// fallback: not a secure source
	ldr	x1, =0x9e3779b97f4a7c15
	mul	x0, x0, x1
	eor	x0, x0, x7, lsl #32
	eor	x0, x0, x0, lsr #29
3:	str	x0, [x4, x7, lsl #3]
	add	x7, x7, #1
	cmp	x7, #10
	b.lo	1b
	mov	x0, #1
	str	x0, [x5]
4:	ldp	x0, x1, [x4, #0]
	msr	APIAKeyLo_EL1, x0
	msr	APIAKeyHi_EL1, x1
	ldp	x0, x1, [x4, #16]
	msr	APIBKeyLo_EL1, x0
	msr	APIBKeyHi_EL1, x1
	ldp	x0, x1, [x4, #32]
	msr	APDAKeyLo_EL1, x0
	msr	APDAKeyHi_EL1, x1
	ldp	x0, x1, [x4, #48]
	msr	APDBKeyLo_EL1, x0
	msr	APDBKeyHi_EL1, x1
	ldp	x0, x1, [x4, #64]
	msr	APGAKeyLo_EL1, x0
	msr	APGAKeyHi_EL1, x1
	isb
	ret

/* Trapped system register (EC 0x18): Apple IMPDEF via TIDCP, and with the
 * monitor the GIC's SGI registers too. Without the monitor every one reads
 * as zero and ignores writes, exactly the Phase 0C smoke. With it, only the
 * implementation-defined space does: monitor_sysreg serves ICC_SGI1R_EL1
 * and makes anything else a named stop. */
do_sysreg:
	ldr	x0, =monitor_enabled
	ldr	x0, [x0]
	cbz	x0, 3f
	mov	x0, x25			// ESR_EL2
	mov	x1, x29			// the guest's saved x0..x30
	bl	monitor_sysreg
	cmp	x0, #1
	b.eq	1f			// served: skip the instruction
	cbnz	x0, 3f			// implementation-defined: RAZ/WI below
	mov	x0, #0x400		// named stop, already printed
	b	record_and_halt
3:	bl	tidcp_record		// B5: note the register, then RAZ/WI below
	tbz	x25, #0, 1f		// ISS bit0: 0 = write (MSR), 1 = read (MRS)
	ubfx	x0, x25, #5, #5		// Rt
	cmp	x0, #31			// xzr target: nothing to write
	b.eq	1f
	add	x1, x29, x0, lsl #3
	str	xzr, [x1]		// RAZ into the saved Rt slot
1:	mrs	x0, ELR_EL2
	add	x0, x0, #4		// skip the trapped instruction
	msr	ELR_EL2, x0
	RESTORE_FRAME
	eret

/* Record one TIDCP-trapped system-register access, deduplicated by register
 * and direction, and print each register the first time it is seen (with
 * tracing on) so the serial log carries the set in first-seen order even if
 * the guest later wedges. x25 holds ESR_EL2. Everything except x24, x25 and
 * x29 is dead here: the guest's copy is in the frame. */
tidcp_record:
	mov	x28, x30

	and	x19, x25, #TIDCP_ISS_MASK
	mov	x0, #TIDCP_RT_MASK
	bic	x19, x19, x0			// key: the register and direction

	ldr	x20, =params_base
	ldr	x20, [x20]
	add	x20, x20, #0x2000		// the TIDCP log page
	ldr	x0, [x20, #TIDCP_TOTAL]
	add	x0, x0, #1
	str	x0, [x20, #TIDCP_TOTAL]

	ldr	x21, [x20, #TIDCP_USED]
	add	x22, x20, #TIDCP_ENTRIES
	mov	x23, xzr
1:	cmp	x23, x21
	b.hs	2f
	ldr	x0, [x22]
	cmp	x0, x19
	b.eq	4f
	add	x22, x22, #TIDCP_ENTRY_SIZE
	add	x23, x23, #1
	b	1b

2:	cmp	x21, #TIDCP_MAX
	b.hs	5f				// full: keep counting, stop listing
	str	x19, [x22]
	mov	x0, #1
	str	x0, [x22, #8]
	mrs	x0, ELR_EL2
	str	x0, [x22, #16]
	add	x21, x21, #1
	str	x21, [x20, #TIDCP_USED]

	ldr	x0, =params_base
	ldr	x0, [x0]
	ldr	x0, [x0, #PARAM_FLAGS]
	tbz	x0, #0, 3f
	ldr	x0, =msg_tidcp
	bl	uart_puts
	mov	x0, x25
	bl	uart_hex
3:	mov	x30, x28
	ret

4:	ldr	x0, [x22, #8]			// seen before: just count it
	add	x0, x0, #1
	str	x0, [x22, #8]
	mov	x30, x28
	ret

5:	ldr	x0, [x20, #TIDCP_DROPPED]
	add	x0, x0, #1
	str	x0, [x20, #TIDCP_DROPPED]
	mov	x30, x28
	ret

psci_off:
	ldr	x0, =monitor_enabled
	ldr	x0, [x0]
	cbz	x0, smoke_exit
	bl	monitor_print_stats
	ldr	x0, =pt_flag
	ldr	x0, [x0]
	tbz	x0, #7, smoke_exit
	b	halt				// debug: stay alive for inspection


/* Angel semihosting SYS_EXIT (ApplicationExit). x1 from the guest is the
 * exit code; QEMU with -semihosting stops cleanly. */
smoke_exit:
#ifdef VMAPPLE_GAOKUN3_MONITOR
	b	halt			// no semihosting on the machine
#endif
	ldr	x1, [x29, #8]
	mov	x0, #0x18		// SYS_EXIT
	mov	x2, x1
	ldr	x1, =0x20026		// ADP_Stopped_ApplicationExit
	hlt	#0xf000
	b	halt

/* ---- Route B monitor setup (Phase 3 P3) ----------------------------------
 * Stage 2, 4 KiB granule, three levels. Only guest RAM is mapped, as 2 MiB
 * blocks from 0x44000000 to 0x61200000 (the payload, the reported DRAM and
 * the panic chunk); every other IPA is absent and faults to monitor.c.
 * That is default-deny: a device the monitor does not model is a named
 * stop, and QEMU's own devices are unreachable at their physical
 * addresses because the guest's IPAs are the VirtualMac template's. */
/* The L2 table for IPA x1 in GiB 1 or 2 (x1 checked by _start); x0 out,
 * x4 and x5 clobbered, x30 kept for the caller's own return. */
s2_l2_for:
#ifdef VMAPPLE_GAOKUN3_MONITOR
	// The L2 tables are contiguous in .bss, one per GiB from GiB 1.
	lsr	x0, x1, #30
	sub	x0, x0, #1
	lsl	x0, x0, #12
	ldr	x4, =s2_l2
	add	x0, x0, x4
	ret
#else
	ldr	x0, =s2_l2
	ldr	x5, =s2_l2b
	mov	x4, #0x80000000
	cmp	x1, x4
	csel	x0, x5, x0, hs
	ret
#endif

monitor_setup:
	mov	x8, x30				// s2_l2_for is a leaf; keep our return
	ldr	x0, =s2_l0
	ldr	x1, =s2_l1
	orr	x1, x1, #3			// table descriptor
	str	x1, [x0]
	ldr	x0, =s2_l1
	ldr	x1, =s2_l2
	orr	x1, x1, #3
	str	x1, [x0, #8]			// L1 entry 1: 0x40000000..0x7fffffff
	ldr	x1, =s2_l2b
	orr	x1, x1, #3
	str	x1, [x0, #16]			// L1 entry 2: 0x80000000..0xbfffffff
#ifdef VMAPPLE_GAOKUN3_MONITOR
	ldr	x1, =s2_l2fb
	orr	x1, x1, #3
	str	x1, [x0, #24]			// L1 entry 3: the framebuffer's GiB
	ldr	x1, =s2_l2c
	orr	x1, x1, #3
	str	x1, [x0, #32]			// L1 entry 4: guest RAM, GiB 4
	ldr	x1, =s2_l2d
	orr	x1, x1, #3
	str	x1, [x0, #40]			// L1 entry 5: guest RAM, GiB 5
#endif
	// Debug passthrough (PARAM_FLAG_PASSTHRU in the setup word the caller
	// leaves in x22, saved to pt_flag by _start): map QEMU's GIC and PL011
	// blocks as device memory in GiB 0, at their physical addresses.
	ldr	x0, =pt_flag
	ldr	x0, [x0]
	tbz	x0, #3, 2f
	ldr	x0, =s2_l1
	ldr	x1, =s2_l2dev
	orr	x1, x1, #3
	str	x1, [x0]			// L1 entry 0: 0x00000000..0x3fffffff
	ldr	x0, =s2_l2dev
	mov	x1, #0x08000000			// GICD+GICR block
	mov	x2, #0x7c1			// block, AF, device
	movk	x2, #0x40, lsl #48		// XN (bit 54)
	orr	x2, x1, x2
	str	x2, [x0, #64*8]
	mov	x1, #0x09000000			// PL011 block
	mov	x2, #0x7c1
	movk	x2, #0x40, lsl #48
	orr	x2, x1, x2
	str	x2, [x0, #72*8]
	// The classic layout's remaining device IPAs land on QEMU's PCIe
	// window, as in the Phase 0C smoke: map them identity as device.
	mov	x1, #0x10000000
3:	mov	x2, #0x7c1
	movk	x2, #0x40, lsl #48
	orr	x2, x1, x2
	lsr	x3, x1, #21
	and	x3, x3, #0x1ff
	str	x2, [x0, x3, lsl #3]
	add	x1, x1, #0x200000
	ldr	x2, =0x3a000000
	cmp	x1, x2
	b.lo	3b
	b	2f
	// Debug GIC passthrough (PARAM_FLAG_GICPT): the template's GICD and
	// GICR IPAs translate to QEMU's real distributor and redistributor.
2:	ldr	x0, =pt_flag
	ldr	x0, [x0]
	tbz	x0, #4, 2f
	ldr	x0, =s2_l1
	ldr	x1, =s2_l2dev
	orr	x1, x1, #3
	str	x1, [x0]
	ldr	x0, =s2_l2dev
	ldr	x1, =s2_l3gic
	orr	x1, x1, #3
	str	x1, [x0, #128*8]
	ldr	x0, =s2_l3gic
	mov	x1, #0x08000000			// QEMU GICD pages
	mov	x4, #16
4:	mov	x2, #0x7ff			// page, AF, WB, RW, executable
	orr	x2, x1, x2
	lsr	x3, x1, #12
	and	x3, x3, #0x1ff
	str	x2, [x0, x3, lsl #3]
	add	x1, x1, #0x1000
	subs	x4, x4, #1
	b.ne	4b
	mov	x1, #0x080a0000			// QEMU GICR first frame
	mov	x4, #32
	mov	x6, #16				// IPA 0x10010000 is L3 index 16
5:	mov	x2, #0x7ff
	orr	x2, x1, x2
	str	x2, [x0, x6, lsl #3]
	add	x1, x1, #0x1000
	add	x6, x6, #1
	subs	x4, x4, #1
	b.ne	5b
	// Guest RAM, [PARAM_S2_LO, PARAM_S2_HI) as the harness computed it from
	// the layout (the payload, the reported DRAM and the panic chunk),
	// 2 MiB blocks in GiB 1 (s2_l2) or GiB 2 (s2_l2b). _start checked the
	// range lies inside those two GiB.
2:	ldr	x9, =params_base
	ldr	x9, [x9]
	ldr	x1, [x9, #PARAM_S2_LO]
	ldr	x10, [x9, #PARAM_S2_HI]
1:	bl	s2_l2_for			// x0: the L2 table holding x1
	mov	x2, #0x7fd			// block, AF, SH, WB, S2AP RW
	orr	x2, x1, x2
	lsr	x3, x1, #21
	and	x3, x3, #0x1ff
	str	x2, [x0, x3, lsl #3]
	add	x1, x1, #0x200000
	cmp	x1, x10
	b.lo	1b
	// The split console (PARAM_FLAG_FRAMEBUFFER): one 2 MiB block maps the
	// guest's 512 framebuffer rows; the monitor's band below stays absent,
	// so a guest store there is a stage 2 fault, not a drawn pixel.
	ldr	x0, =pt_flag
	ldr	x0, [x0]
	tbz	x0, #8, 6f
#ifdef VMAPPLE_GAOKUN3_MONITOR
	// The real panel: 6400-byte rows, so the guest's last row ends inside
	// a 2 MiB block. Map whole blocks where they fit and finish the last
	// one with 4 KiB pages from the L3 pool; the band stays absent.
	// Use Normal Outer/Inner Non-cacheable (MemAttr 0b0101: 0x7d5 block,
	// 0x7d7 page) so guest stores to boot_args.Video reach the display
	// scanout buffer in DRAM without needing explicit cache maintenance,
	// while still permitting Normal unaligned and pair stores.
	ldr	x9, =params_base
	ldr	x9, [x9]
	ldr	x1, [x9, #PARAM_FB_PA]
	cbz	x1, 6f
	ldr	x4, =s2_l3_next
	ldr	x5, =s2_l3pool
	str	x5, [x4]
	ldr	x10, [x9, #PARAM_FB_STRIDE]
	ldr	x11, [x9, #PARAM_FB_GUEST_ROWS]
	mul	x10, x10, x11
	add	x10, x10, x1			// guest framebuffer end (page aligned)
7:	bl	s2_l2_for			// x0: the L2 table holding x1
	mov	x4, x1
	add	x4, x4, #0x200000
	cmp	x4, x10
	b.hi	8f				// partial block: pages below
	mov	x2, #0x7d5			// block, AF, SH, Normal NC, S2AP RW
	orr	x2, x1, x2
	lsr	x3, x1, #21
	and	x3, x3, #0x1ff
	str	x2, [x0, x3, lsl #3]
	mov	x1, x4
	cmp	x1, x10
	b.lo	7b
	b	6f
8:	ldr	x4, =s2_l3_next			// a fresh L3 table from the pool
	ldr	x5, [x4]
	mov	x6, x5
	add	x6, x6, #4096
	str	x6, [x4]
	orr	x5, x5, #3			// table descriptor
	lsr	x3, x1, #21
	and	x3, x3, #0x1ff
	str	x5, [x0, x3, lsl #3]
	sub	x5, x5, #3
9:	mov	x2, #0x7d7			// page, AF, SH, Normal NC, S2AP RW
	orr	x2, x1, x2
	lsr	x3, x1, #12
	and	x3, x3, #0x1ff
	str	x2, [x5, x3, lsl #3]
	add	x1, x1, #0x1000
	cmp	x1, x10
	b.lo	9b
	b	6f
#else
	ldr	x1, =MON_FB_PA
	bl	s2_l2_for
#endif
	mov	x2, #0x7fd			// block, AF, SH, WB, S2AP RW
	orr	x2, x1, x2
	lsr	x3, x1, #21
	and	x3, x3, #0x1ff
	str	x2, [x0, x3, lsl #3]
6:
#ifdef VMAPPLE_GAOKUN3_MONITOR
	// Clean the stage-2 tables and the guest RAM window to the point
	// of coherency before enabling stage 2 (the guest starts with M=0,
	// C=0, so any dirty or stale UEFI cache line in [s2_lo, s2_hi)
	// must be flushed and invalidated first), and use a 39-bit IPA /
	// 40-bit PA VTCR_EL2 starting at s2_l1 (Cortex-A78C PARange is
	// 40 bits; T0SZ=16 / PS=5 exceeds PARange).
	ldr	x0, =s2_l0
	ldr	x1, =s2_l3_next
	sub	x1, x1, x0
	bl	mon_clean_range
	ldr	x9, =params_base
	ldr	x9, [x9]
	ldr	x0, [x9, #PARAM_S2_LO]
	ldr	x1, [x9, #PARAM_S2_HI]
	sub	x1, x1, x0
	bl	mon_clean_range
	mov	x30, x8
	dsb	sy
	ldr	x0, =0x80023559			// VTCR: RES1, 40b PA, 4K, WB, SL0=1 (L1), T0SZ=25 (39b IPA)
	msr	VTCR_EL2, x0
	ldr	x0, =s2_l1
	msr	VTTBR_EL2, x0
	isb
	tlbi	vmalls12e1is
	dsb	sy
	isb
#else
	mov	x30, x8
	dsb	sy
	ldr	x0, =0x53490			// VTCR: 48-bit IPA/PA, L0 start, WB
	msr	VTCR_EL2, x0
	ldr	x0, =s2_l0
	msr	VTTBR_EL2, x0
#endif
	msr	ICH_VMCR_EL2, xzr
	msr	ICH_LR0_EL2, xzr
	msr	ICH_LR1_EL2, xzr
	msr	ICH_LR2_EL2, xzr
	msr	ICH_LR3_EL2, xzr
	mov	x0, #1				// ICH_HCR: En only. UIE would assert the
	msr	ICH_HCR_EL2, x0			// maintenance PPI permanently (no valid LRs)
	// The architected event stream: the guest's lock and idle loops wait
	// in WFE, which no interrupt reaches on this path. Events on counter
	// bit 8 wake them, as a VMM is expected to provide.
	mrs	x0, CNTHCTL_EL2
	mov	x1, #0x84			// EVNTEN | EVNTI=8 (counter bit 8)
	orr	x0, x0, x1
	msr	CNTHCTL_EL2, x0
	// The physical CPU interface at EL2: let every priority through and
	// enable Group 1, or no physical PPI ever signals this EL. With two
	// security states (the gaokun3, GICD_CTLR.DS == 0), Group 0 belongs
	// to EL3 and writing ICC_IGRPEN0_EL1 at Non-secure EL2 traps to EL3.
	mov	x0, #0xff
	msr	ICC_PMR_EL1, x0
	mov	x0, #1
	msr	ICC_IGRPEN1_EL1, x0
#ifndef VMAPPLE_GAOKUN3_MONITOR
	msr	ICC_IGRPEN0_EL1, x0
#endif
	isb
	mrs	x0, HCR_EL2
	mov	x1, #0x19			// VM | FMO | IMO
	orr	x0, x0, x1
	msr	HCR_EL2, x0
	ldr	x0, =monitor_enabled
	mov	x1, #1
	str	x1, [x0]
	isb
	ret

/* Helpers monitor.c calls. The physical CPU interface is ICC_*_EL1 at
 * EL2; the virtual one is ICH_*_EL2. */
.global mon_iar1_read
mon_iar1_read:
	mrs	x0, ICC_IAR1_EL1
	ret
.global mon_iar0_read
mon_iar0_read:
	mrs	x0, ICC_IAR0_EL1
	ret
.global mon_eoir0_write
mon_eoir0_write:
	msr	ICC_EOIR0_EL1, x0
	ret
.global mon_eoir1_write
mon_eoir1_write:
	msr	ICC_EOIR1_EL1, x0
	ret
.global mon_lr0_read
mon_lr0_read:
	mrs	x0, ICH_LR0_EL2
	ret
.global mon_lr0_write
mon_lr0_write:
	msr	ICH_LR0_EL2, x0
	isb
	ret
// List registers 1 to 3 (n in x0): the guest's software interrupts.
.global mon_lr_read
mon_lr_read:
	cmp	x0, #1
	b.ne	2f
	mrs	x0, ICH_LR1_EL2
	ret
2:	cmp	x0, #2
	b.ne	3f
	mrs	x0, ICH_LR2_EL2
	ret
3:	mrs	x0, ICH_LR3_EL2
	ret
.global mon_lr_write
mon_lr_write:
	cmp	x0, #1
	b.ne	2f
	msr	ICH_LR1_EL2, x1
	b	4f
2:	cmp	x0, #2
	b.ne	3f
	msr	ICH_LR2_EL2, x1
	b	4f
3:	msr	ICH_LR3_EL2, x1
4:	isb
	ret
.global mon_misr_read
mon_misr_read:
	mrs	x0, ICH_MISR_EL2
	ret
.global mon_redist_write
mon_redist_write:
	str	w1, [x0]
	ret
// Reads of the platform GIC: monitor_init reads before it writes.
.global mon_mmio_read32
mon_mmio_read32:
	ldr	w0, [x0]
	ret
.global mon_mmio_read64
mon_mmio_read64:
	ldr	x0, [x0]
	ret
.global mon_mpidr
mon_mpidr:
	mrs	x0, MPIDR_EL1
	ret
.global mon_uart_putc
mon_uart_putc:
#ifdef VMAPPLE_GAOKUN3_MONITOR
	ret				// guest PL011 output has no UART to go to
#else
	ldr	x1, =PL011_BASE
	str	w0, [x1]
#endif
	ret
.global mon_clean_range
mon_clean_range:
#ifdef VMAPPLE_GAOKUN3_MONITOR
	add	x1, x0, x1		// the panel and uncached EL1 read memory, not caches
	bic	x0, x0, #63
1:	dc	civac, x0
	add	x0, x0, #64
	cmp	x0, x1
	b.lo	1b
	dsb	sy
#endif
	ret
.global mon_cntpct
mon_cntpct:
	mrs	x0, CNTPCT_EL0
	ret
.global mon_cntfrq
mon_cntfrq:
	mrs	x0, CNTFRQ_EL0
	ret
.global mon_cnthp_rearm
mon_cnthp_rearm:
	msr	CNTHP_TVAL_EL2, x0
	mov	x0, #1
	msr	CNTHP_CTL_EL2, x0
	ret
.global mon_translate_va
mon_translate_va:
	at	s1e1r, x0
	isb
	mrs	x0, PAR_EL1
	ret
.global mon_elr_el2
mon_elr_el2:
	mrs	x0, ELR_EL2
	ret
.global mon_spsr_el2
mon_spsr_el2:
	mrs	x0, SPSR_EL2
	ret
.global mon_esr_el1
mon_esr_el1:
	mrs	x0, ESR_EL1
	ret
.global mon_far_el1
mon_far_el1:
	mrs	x0, FAR_EL1
	ret
.global mon_elr_el1
mon_elr_el1:
	mrs	x0, ELR_EL1
	ret
.global mon_sctlr_el1
mon_sctlr_el1:
	mrs	x0, SCTLR_EL1
	ret
.global mon_tlbi_vmalle1is
mon_tlbi_vmalle1is:
	dsb	ish
	tlbi	vmalle1is
	dsb	ish
	isb
	ret
.global mon_params_base
mon_params_base:
	ldr	x0, =params_base
	ldr	x0, [x0]
	ret

/* Freestanding C ABI helpers for -mstrict-align (EL2 runs with M=0 in
 * the QEMU smoke, so aggregate initialisers must copy byte by byte). */
.global memcpy
memcpy:
	mov	x3, x0
1:	cbz	x2, 2f
	ldrb	w4, [x1], #1
	strb	w4, [x3], #1
	sub	x2, x2, #1
	b	1b
2:	ret
.global memset
memset:
	mov	x3, x0
1:	cbz	x2, 2f
	strb	w1, [x3], #1
	sub	x2, x2, #1
	b	1b
2:	ret

/* ---- Fault records ------------------------------------------------------ */
fault_cur_sp0:
	SAVE_FRAME
	mov	x0, #0x200
	b	record_and_halt
fault_lower_irq:
	SAVE_FRAME
	mov	x0, #0x480
	b	record_and_halt
fault_lower_fiq:
	SAVE_FRAME
	mov	x0, #0x500
	b	record_and_halt
fault_lower32:
	SAVE_FRAME
	mov	x0, #0x600
	b	record_and_halt

// x0 = vector offset. Writes a fault record at FAULT_PA and halts.
record_and_halt:
	mov	x28, x0
	// Guard against a recursive fault if params_base or UART is unmapped.
	ldr	x0, =in_fault
	ldr	x1, [x0]
	cbnz	x1, halt
	mov	x1, #1
	str	x1, [x0]
#ifdef VMAPPLE_GAOKUN3_MONITOR
	mov	x0, x28
	mrs	x1, ESR_EL2
	mrs	x2, ELR_EL2
	bl	monitor_fault
#endif
	ldr	x0, =msg_fault
	bl	uart_puts
	mrs	x0, ESR_EL2
	bl	uart_hex
	mrs	x0, ELR_EL2
	bl	uart_hex
	ldr	x19, =params_base
	ldr	x19, [x19]
	cbz	x19, halt
	add	x19, x19, #0x1000		// the fault record page
	ldr	x0, =SMOKE_FAULT_MAGIC
	str	x0, [x19, #FAULT_MAGIC]
	mrs	x0, ESR_EL2
	str	x0, [x19, #FAULT_ESR]
	mrs	x0, ELR_EL2
	str	x0, [x19, #FAULT_ELR]
	mrs	x0, FAR_EL2
	str	x0, [x19, #FAULT_FAR]
	str	x28, [x19, #FAULT_VEC]
	mrs	x0, HCR_EL2
	str	x0, [x19, #FAULT_HCR]
	dsb	sy
	// With the split console, stay up: the harness reads the screen
	// through QEMU's monitor once this record appears, then ends the run.
	ldr	x0, =pt_flag
	ldr	x0, [x0]
	tbnz	x0, #8, halt
	// Stop QEMU so the harness does not wait out its timeout.
#ifdef VMAPPLE_GAOKUN3_MONITOR
	b	halt
#endif
	mov	x0, #0x18
	ldr	x1, =0x20023		// ADP_Stopped_RunTimeError
	hlt	#0xf000
halt:
	wfi
	b	halt

.section .rodata
msg_setup_done:	.asciz "SETUP DONE: STAGE 2 READY, GIC NOT TOUCHED"
msg_enter_band:	.asciz "ENTERING GUEST AT EL1"
msg_pac_done:	.asciz "PAC KEYS INSTALLED (QARMA5), HVC OK"
msg_enter:	.asciz "EL2: entering guest at EL1\n"
msg_cntfrq:	.asciz "EL2: cntfrq "
msg_noparam:	.asciz "EL2: bad params magic\n"
msg_hvc:	.asciz "EL2: hvc "
msg_spsr:	.asciz "EL2: guest spsr "
msg_tidcp:	.asciz "EL2: tidcp "
msg_fault:	.asciz "EL2: unexpected exception esr/elr\n"

.section .bss
.align 4
monitor_enabled:
	.space 8
params_base:
	.space 8
in_fault:
	.space 8
.align 12
s2_l0:
	.space 4096
s2_l1:
	.space 4096
s2_l2:
	.space 4096
s2_l2b:
	.space 4096
#ifdef VMAPPLE_GAOKUN3_MONITOR
s2_l2fb:
	.space 4096
s2_l2c:
	.space 4096
s2_l2d:
	.space 4096
s2_l3pool:
	.space 8 * 4096
s2_l3_next:
	.space 8
.align 12
#endif
s2_l2dev:
	.space 4096
s2_l3gic:
	.space 4096
s2_l3uart:
	.space 4096
pt_flag:
	.space 8
.align 4
pac_keys:
	.space 80
pac_keys_ready:
	.space 8
.align 4
stack_bot:
	.space 16384
stack_top:
