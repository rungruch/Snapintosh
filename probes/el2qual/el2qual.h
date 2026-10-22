/* SPDX-License-Identifier: MIT */
/* The contract between el2qual's C and its EL2 assembly (el2qual-asm.S).
 *
 * Two layouts cross that boundary. The per-CPU context is what the exception
 * vectors find through TPIDR_EL2: a probe that may execute an undefined
 * instruction stores a resume PC there first, and the vector absorbs exactly
 * one fault by returning to it. The boot block is what a secondary CPU reads
 * from PSCI CPU_ON entry with its MMU still off, so every load it makes goes
 * to memory, not to a cache: the boot CPU cleans the block to the point of
 * coherency before each CPU_ON. el2qual.c asserts both layouts against its
 * structs. */
#ifndef EL2QUAL_H
#define EL2QUAL_H

/* struct el2q_ctx */
#define CTX_FIXUP        0x00   /* resume PC for one expected fault; 0 = none */
#define CTX_FIXUP_ESR    0x08   /* ESR_EL2 of the fault the fixup absorbed */
#define CTX_FATAL_ESR    0x10
#define CTX_FATAL_ELR    0x18
#define CTX_FATAL_FAR    0x20
#define CTX_FATAL_SPSR   0x28
#define CTX_FATAL_VECTOR 0x30   /* vector index + 1, so 0 means no fault */
#define CTX_SLOT         0x38
#define CTX_SIZE         0x40   /* one cache line per CPU */
#define CTX_SHIFT        6

/* struct el2q_boot */
#define BOOT_HCR         0x00
#define BOOT_MAIR        0x08
#define BOOT_TCR         0x10
#define BOOT_TTBR0       0x18
#define BOOT_SCTLR       0x20
#define BOOT_CPTR        0x28
#define BOOT_VBAR        0x30
#define BOOT_STACKS      0x38   /* base of the secondary stack array */
#define BOOT_CTX         0x40   /* base of the el2q_ctx array */
#define BOOT_EARLY       0x48   /* base of the early-status array */
#define BOOT_SIZE        0x50

#define EL2Q_STACK_SIZE  0x4000
#define EL2Q_EARLY_SHIFT 6      /* one 64-byte line per CPU */

/* A secondary's first store, made with its MMU off: this marker ORed with
 * CurrentEL. It tells the boot CPU the core ran our code at all, and at which
 * level, before anything else could have gone wrong. */
#define EL2Q_EARLY_MAGIC_HI 0xe12e   /* bits 63:48 */

#endif
