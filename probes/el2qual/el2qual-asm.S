/* SPDX-License-Identifier: MIT */
/* el2qual's EL2 code: exception vectors, the PSCI secondary entry, and every
 * instruction whose existence is the question being asked.
 *
 * Each probe instruction runs behind a fixup (el2qual.h): the routine stores
 * a resume PC in its CPU's context, executes the instruction, and clears the
 * fixup. If the instruction is undefined, the vector records ESR_EL2 and
 * resumes there instead of treating it as fatal. A routine therefore returns
 * 0 when the instruction executed and the ESR otherwise.
 *
 * Only exceptions taken to EL2 can be absorbed. A PAuth instruction or key
 * access that SCR_EL3.{API,APK} traps goes to EL3 instead, and what the
 * firmware does then is unknown; el2qual.c says so on screen before trying. */
#include "el2qual.h"

/* The PAuth mnemonics below need the assembler to accept Armv8.3. Nothing in
 * the C build assumes the extension; it is only executed behind a fixup. */
.arch armv8.3-a

.text

/* ---- Probes ------------------------------------------------------------
 * x0 = struct el2q_ctx * of the running CPU, x1 = where the result goes.
 * x2 and x3 are inputs for the instructions that take any. */
.macro TRY name, insn:vararg
.global \name
.p2align 2
\name:
    adr     x9, 1f
    str     xzr, [x0, #CTX_FIXUP_ESR]
    str     x9, [x0, #CTX_FIXUP]
    \insn
    str     x10, [x1]
1:  str     xzr, [x0, #CTX_FIXUP]
    ldr     x0, [x0, #CTX_FIXUP_ESR]
    ret
.endm

TRY el2q_try_rndr,    mrs x10, s3_3_c2_c4_0     /* RNDR: FEAT_RNG */
TRY el2q_try_vncr,    mrs x10, s3_4_c2_c2_0     /* VNCR_EL2: FEAT_NV2 */
TRY el2q_try_cntpoff, mrs x10, s3_4_c14_c0_6    /* CNTPOFF_EL2: FEAT_ECV */
TRY el2q_try_hfgrtr,  mrs x10, s3_4_c1_c1_4     /* HFGRTR_EL2: FEAT_FGT */
TRY el2q_try_apiakey, mrs x10, s3_0_c2_c1_0     /* APIAKeyLo_EL1 */
TRY el2q_try_icc_sre, mrs x10, s3_4_c12_c9_5    /* ICC_SRE_EL2 */
TRY el2q_try_ich_vtr, mrs x10, s3_4_c12_c11_1   /* ICH_VTR_EL2 */
TRY el2q_try_ich_hcr, mrs x10, s3_4_c12_c11_0   /* ICH_HCR_EL2 */
TRY el2q_try_pacga,   pacga x10, x2, x3

/* One 32-bit device read for the tsens stage (thermal.c): x2 = the address.
 * A translation fault or a synchronous external abort on it is absorbed like
 * an undefined instruction above, and the ESR returned. An asynchronous
 * SError is not; DAIF keeps it pending, and el2qual.c reads ISR_EL1 after.
 * Only addresses whose translation el2qual.c has checked come here. */
TRY el2q_try_read32,  ldr w10, [x2]

/* x0 ctx, x1 out[2], x2 the pattern (EL2Q_LR_PATTERN).
 *
 * ICH_LR0_EL2 is saved, written with the pattern, read back and restored;
 * out[0] is the saved value and out[1] what the write left. The caller runs
 * this only with ICH_HCR_EL2.En clear, so the virtual CPU interface is off,
 * and the pattern's state is invalid, so it could not signal even if it were
 * on. If the first read faults, nothing has been written. */
.global el2q_try_ich_lr0
.p2align 2
el2q_try_ich_lr0:
    adr     x9, 1f
    str     xzr, [x0, #CTX_FIXUP_ESR]
    str     x9, [x0, #CTX_FIXUP]
    mrs     x11, s3_4_c12_c12_0
    msr     s3_4_c12_c12_0, x2
    isb
    mrs     x10, s3_4_c12_c12_0
    msr     s3_4_c12_c12_0, x11
    isb
    stp     x11, x10, [x1]
1:  str     xzr, [x0, #CTX_FIXUP]
    ldr     x0, [x0, #CTX_FIXUP_ESR]
    ret

/* x0 ctx, x1 in[n], x2 modifier[n], x3 out[n], x4 rt[3], x5 n.
 *
 * PACIA1716 and AUTIA1716 are in the hint space: without PAuth they are
 * NOPs, so an unchanged pointer is the "absent" answer, not a fault. XPACI is
 * not a hint and is undefined without PAuth, which is what the returned ESR
 * reports. rt[] is a sign, an authenticate of the signed pointer, and a strip
 * of it; a working implementation gives back in[0] from both. */
.global el2q_try_pac
.p2align 2
el2q_try_pac:
    adr     x9, 2f
    str     xzr, [x0, #CTX_FIXUP_ESR]
    str     x9, [x0, #CTX_FIXUP]
    mov     x6, #0
1:  ldr     x17, [x1, x6, lsl #3]
    ldr     x16, [x2, x6, lsl #3]
    pacia1716
    str     x17, [x3, x6, lsl #3]
    add     x6, x6, #1
    cmp     x6, x5
    b.lo    1b
    ldr     x17, [x1]
    ldr     x16, [x2]
    pacia1716
    str     x17, [x4]
    autia1716
    str     x17, [x4, #8]
    ldr     x17, [x4]
    xpaci   x17
    str     x17, [x4, #16]
2:  str     xzr, [x0, #CTX_FIXUP]
    ldr     x0, [x0, #CTX_FIXUP_ESR]
    ret

/* Only called once el2q_try_apiakey has shown the key registers exist. */
.global el2q_apia_get
.p2align 2
el2q_apia_get:
    mrs     x1, s3_0_c2_c1_0                    /* APIAKeyLo_EL1 */
    mrs     x2, s3_0_c2_c1_1                    /* APIAKeyHi_EL1 */
    stp     x1, x2, [x0]
    ret

.global el2q_apia_set
.p2align 2
el2q_apia_set:
    msr     s3_0_c2_c1_0, x0
    msr     s3_0_c2_c1_1, x1
    isb
    ret

/* ---- Platform ------------------------------------------------------------ */

/* SMCCC: x0 function, x1-x3 arguments, result in x0. x4-x17 may be
 * clobbered by an SMCCC 1.0 implementation; all are caller-saved here. */
.global el2q_smc
.p2align 2
el2q_smc:
    smc     #0
    ret

/* Make SP_EL2 the stack in use. Firmware may run EL2 code on SP_EL0, and an
 * exception always switches to SP_ELx; without this the vector would push to
 * whatever SP_EL2 last held. The value of sp is the same before and after. */
.global el2q_use_spx
.p2align 2
el2q_use_spx:
    mov     x0, sp
    msr     spsel, #1
    mov     sp, x0
    ret

/* Recreate the state slbounce hands over (HCR_EL2 = E2H | TGE, the same
 * tables now under the EL2&0 regime) on a machine that started at EL2
 * without it. Test only: it lets QEMU exercise the VHE paths the real
 * gaokun3 will take. x0 is the caller's TCR_EL2 already converted to the
 * VHE layout, since E2H changes which bits mean what.
 *
 * The regime changes underneath the running code, so it happens with the
 * MMU off and without a memory access; UEFI maps VA == PA, so execution
 * continues at the same addresses either way. */
.global el2q_enter_vhe
.p2align 2
el2q_enter_vhe:
    mrs     x1, sctlr_el2
    bic     x2, x1, #1
    dsb     sy
    msr     sctlr_el2, x2
    isb
    mov     x3, #(1 << 27)                      /* TGE */
    movk    x3, #4, lsl #32                     /* E2H, bit 34 */
    msr     hcr_el2, x3
    isb
    msr     tcr_el2, x0
    isb
    tlbi    alle2
    dsb     sy
    isb
    msr     sctlr_el2, x1
    isb
    ret

/* ---- Secondary entry -------------------------------------------------------
 * PSCI CPU_ON enters here with the MMU and caches off and x0 = the context ID
 * el2qual.c passed, the CPU's slot. Every load before SCTLR_EL2 is written
 * reads memory directly; the boot CPU cleaned el2q_boot and this image to
 * the point of coherency before CPU_ON. */
.global el2q_secondary_entry
.p2align 2
el2q_secondary_entry:
    msr     daifset, #15
    adrp    x19, el2q_boot
    add     x19, x19, :lo12:el2q_boot
    /* First, before anything that could go wrong: say we are here, and at
     * which level. A core that PSCI starts below EL2 stops right after. */
    ldr     x1, [x19, #BOOT_EARLY]
    add     x1, x1, x0, lsl #EL2Q_EARLY_SHIFT
    mrs     x2, CurrentEL
    movz    x3, #EL2Q_EARLY_MAGIC_HI, lsl #48
    orr     x2, x2, x3
    str     x2, [x1]
    dsb     sy
    mrs     x2, CurrentEL
    cmp     x2, #8
    b.ne    9f
    ldr     x2, [x19, #BOOT_VBAR]
    msr     vbar_el2, x2
    /* HCR_EL2 first: E2H decides how TCR_EL2 and SCTLR_EL2 are read. */
    ldr     x2, [x19, #BOOT_HCR]
    msr     hcr_el2, x2
    isb
    ldr     x2, [x19, #BOOT_MAIR]
    msr     mair_el2, x2
    ldr     x2, [x19, #BOOT_TCR]
    msr     tcr_el2, x2
    ldr     x2, [x19, #BOOT_TTBR0]
    msr     ttbr0_el2, x2
    ldr     x2, [x19, #BOOT_CPTR]
    msr     cptr_el2, x2
    dsb     sy
    tlbi    alle2
    ic      iallu
    dsb     sy
    isb
    ldr     x2, [x19, #BOOT_SCTLR]
    msr     sctlr_el2, x2
    isb
    msr     spsel, #1
    ldr     x2, [x19, #BOOT_STACKS]
    mov     x3, #EL2Q_STACK_SIZE
    madd    x2, x0, x3, x2
    add     sp, x2, #EL2Q_STACK_SIZE
    ldr     x2, [x19, #BOOT_CTX]
    add     x2, x2, x0, lsl #CTX_SHIFT
    msr     tpidr_el2, x2
    bl      el2q_secondary_main                 /* does not return */
9:  wfe
    b       9b

/* ---- Vectors ----------------------------------------------------------------
 * All sixteen entries share one handler. A synchronous exception with a
 * fixup armed resumes at the fixup with ESR_EL2 recorded; anything else is
 * recorded as fatal and handed to el2q_fatal(), which reports and stops. */
.p2align 11
.global el2q_vectors
el2q_vectors:
.irp index, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    .p2align 7
    stp     x0, x1, [sp, #-32]!
    str     x2, [sp, #16]
    mov     x0, #\index
    b       el2q_trap
.endr

.p2align 2
el2q_trap:
    mrs     x1, tpidr_el2
    tst     x0, #3                              /* 0, 4, 8, 12: synchronous */
    b.ne    2f
    ldr     x2, [x1, #CTX_FIXUP]
    cbz     x2, 2f
    msr     elr_el2, x2
    str     xzr, [x1, #CTX_FIXUP]
    mrs     x2, esr_el2
    str     x2, [x1, #CTX_FIXUP_ESR]
    ldr     x2, [sp, #16]
    ldp     x0, x1, [sp], #32
    eret
2:  ldr     x2, [x1, #CTX_FATAL_VECTOR]
    cbnz    x2, 3f                              /* faulted while reporting a fault */
    add     x2, x0, #1
    str     x2, [x1, #CTX_FATAL_VECTOR]
    mrs     x2, esr_el2
    str     x2, [x1, #CTX_FATAL_ESR]
    mrs     x2, elr_el2
    str     x2, [x1, #CTX_FATAL_ELR]
    mrs     x2, far_el2
    str     x2, [x1, #CTX_FATAL_FAR]
    mrs     x2, spsr_el2
    str     x2, [x1, #CTX_FATAL_SPSR]
    mov     x0, x1
    bl      el2q_fatal                          /* does not return */
3:  msr     daifset, #15
    wfe
    b       3b
