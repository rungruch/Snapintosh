/* SPDX-License-Identifier: MIT */
/*
 * A synthetic EL1 "guest" for the smoke test's self-test. It is not Apple
 * code and carries no Apple data; it exercises exactly the EL2 services the
 * stub provides, so the self-test can prove them on any host with QEMU,
 * without the VirtualMac kernelcache. It:
 *   1. confirms it is at EL1,
 *   2. issues the PAC hypercalls the stock kernel issues at boot
 *      (SET_INITIAL_STATE, GET_DEFAULT_KEYS, SET_B_KEYS, SET_EL0_DIVERSIFIER),
 *   3. signs and authenticates a pointer at EL1 to prove PAuth runs natively
 *      and is not trapped away,
 *   4. reads an Apple IMPDEF system register, which HCR_EL2.TIDCP traps to the
 *      stub and which must read back as zero,
 *   5. ends the run through the stub's exit hypercall.
 * Each step prints a line to the UART so a hang leaves its place on screen.
 */
#include "layout.h"

.section .text.start, "ax"
.global _start
_start:
	ldr	x0, =msg_el1
	bl	puts

	// The EL2 entry must have left all four DAIF masks set. The kernel later
	// clears only A/I/F through DAIFClr #7 and asserts that D stays masked.
	mrs	x14, DAIF
	tbz	x14, #9, fail
	msr	DAIFClr, #7
	mrs	x15, DAIF
	tbz	x15, #9, fail
	msr	DAIFSet, #7
	mrs	x16, DAIF
	tbz	x16, #9, fail
	ldr	x0, =msg_daif
	bl	puts

	// CurrentEL must be EL1 (0x4).
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #1
	b.ne	fail

	// SET_INITIAL_STATE, as common_start does before enabling the keys.
	mov	x0, #HVC_CPU_SERVICE		// 0xc1000000 == SET_INITIAL_STATE
	hvc	#0
	cbnz	x0, fail

	// The keys must now be installed (non-zero) and readable at EL1.
	mrs	x12, APIBKeyLo_EL1
	cbz	x12, fail
	mrs	x13, APIAKeyLo_EL1
	cbz	x13, fail

	// GET_DEFAULT_KEYS: both cookies must come back non-zero.
	ldr	x0, =VMAPPLE_PAC_GET_DEFAULT_KEYS
	hvc	#0
	cbnz	x0, fail
	cbz	x2, fail
	cbz	x3, fail

	// SET_B_KEYS and SET_EL0_DIVERSIFIER with those cookies, as a context
	// switch would. Collapsed domains: both must still report success.
	mov	x1, x2
	movz	x0, #0x0000
	movk	x0, #0xc100, lsl #16		// 0xc1000003 SET_B_KEYS
	add	x0, x0, #3
	hvc	#0
	cbnz	x0, fail
	movz	x0, #0x0000
	movk	x0, #0xc100, lsl #16
	add	x0, x0, #4			// 0xc1000004 SET_EL0_DIVERSIFIER
	hvc	#0
	cbnz	x0, fail

	// Collapsed key domains: neither call may have changed a key.
	mrs	x0, APIBKeyLo_EL1
	cmp	x0, x12
	b.ne	fail
	mrs	x0, APIAKeyLo_EL1
	cmp	x0, x13
	b.ne	fail

	// A second SET_INITIAL_STATE (another CPU, in the real kernel) must
	// install the same keys, not new ones.
	mov	x0, #HVC_CPU_SERVICE
	hvc	#0
	cbnz	x0, fail
	mrs	x0, APIAKeyLo_EL1
	cmp	x0, x13
	b.ne	fail

	ldr	x0, =msg_pac
	bl	puts

	// Enable the four keys at EL1, as the kernel does with
	// SCTLR_EL1_DEFAULT after PAC_INIT_KEY_STATE (EnIA 31, EnIB 30,
	// EnDA 27, EnDB 13).
	mrs	x0, SCTLR_EL1
	movz	x1, #0xc800, lsl #16
	orr	x1, x1, #0x2000
	orr	x0, x0, x1
	msr	SCTLR_EL1, x0
	isb

	// PAuth must run natively at EL1 (HCR_EL2.API/APK let it through):
	// sign a pointer, authenticate it, and get the original back.
	ldr	x9, =0x0000fe00deadbeef
	mov	x10, x9
	paciza	x9
	cmp	x9, x10
	b.eq	fail				// signing changed nothing: PAC absent
	autiza	x9
	cmp	x9, x10
	b.ne	fail				// round trip failed

	ldr	x0, =msg_pacok
	bl	puts

	// An Apple IMPDEF register. HCR_EL2.TIDCP traps CRn 11/15 to the stub,
	// which reads it as zero and skips the instruction. Reaching the next
	// line at all proves the trap path works.
	mrs	x11, S3_4_C15_C0_4
	cbnz	x11, fail			// stub must RAZ it

	ldr	x0, =msg_impdef
	bl	puts

	// End the run: the stub turns this into a clean QEMU exit, code 0.
	ldr	x0, =msg_done
	bl	puts
	ldr	w0, =SMOKE_HVC_EXIT
	mov	x1, #0
	hvc	#0
	b	spin

fail:
	ldr	x0, =msg_fail
	bl	puts
	ldr	w0, =SMOKE_HVC_EXIT
	mov	x1, #1
	hvc	#0
spin:
	wfi
	b	spin

puts:
	ldr	x1, =PL011_BASE
1:	ldrb	w2, [x0], #1
	cbz	w2, 2f
	str	w2, [x1]
	b	1b
2:	ret

.section .rodata
msg_el1:	.asciz "MK: running at EL1\n"
msg_daif:	.asciz "MK: DAIF debug remains masked\n"
msg_pac:	.asciz "MK: pac hypercalls ok\n"
msg_pacok:	.asciz "MK: pauth sign/auth ok\n"
msg_impdef:	.asciz "MK: impdef sysreg trapped and raz\n"
msg_done:	.asciz "MK: done ok\n"
msg_fail:	.asciz "MK: FAIL\n"
