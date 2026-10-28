/* SPDX-License-Identifier: MIT */
/* Fixed guest-physical addresses shared by the EL2 stub, the self-test
 * mini-kernel and the Python harness (tools/vmapple-smoke/smoke.py). QEMU's
 * `virt` machine puts RAM at 0x40000000 and auto-generates a DTB in its first
 * MiB, so the stub sits above that. The VirtualMac kernelcache's header VA
 * ends in 0x1004000 within its 32 MiB bootstrap-map block; its physical base
 * must have that same full offset, because XNU's early block maps preserve it.
 * These are smoke-test addresses only, not a Snapintosh runtime map. */
#ifndef VMAPPLE_SMOKE_LAYOUT_H
#define VMAPPLE_SMOKE_LAYOUT_H

#define RAM_BASE     0x40000000ull

/* Above the DTB QEMU's virt machine auto-generates at the base of RAM. */
#define STUB_PA      0x40200000ull   /* EL2 stub image and its stack/vectors */
#define PARAMS_PA    0x43000000ull   /* struct smoke_params the harness fills */
#define FAULT_PA     0x43001000ull   /* struct smoke_fault the stub writes */

#define PL011_BASE   0x09000000ull   /* QEMU virt UART0 */

/* smoke_params: what the stub needs to drop into the guest at EL1.
 *   +0x00 entry   guest-physical entry point
 *   +0x08 x0      value handed to the guest in x0 (boot_args PA, or 0)
 *   +0x10 flags   bit0: print EL2 hypercall breadcrumbs to the UART
 *   +0x18 magic   SMOKE_PARAMS_MAGIC, so the stub refuses a blank page
 *   +0x20 s2_lo, +0x28 s2_hi  the guest RAM the monitor's stage 2 maps,
 *                 2 MiB aligned, inside 0x40000000..0xc0000000 */
#define SMOKE_PARAMS_MAGIC 0x564d41504c450002ull  /* "VMAPLE" + version 2 */
#define PARAM_ENTRY  0x00
#define PARAM_X0     0x08
#define PARAM_FLAGS  0x10
#define PARAM_MAGIC  0x18
#define PARAM_S2_LO  0x20
#define PARAM_S2_HI  0x28
/* Parameter-page extension (version 2 page, words past the 0x30-byte
 * blob): the screen split, written by q1n1's q1n1_vmb_fill_params and
 * read by the gaokun3 monitor build, whose framebuffer is the real
 * panel and cannot be a compile-time constant of the QEMU build.
 * Zero when no framebuffer is described. */
#define PARAM_FB_PA         0x30
#define PARAM_FB_WIDTH      0x38
#define PARAM_FB_STRIDE     0x40
#define PARAM_FB_GUEST_ROWS 0x48
#define PARAM_FB_BAND_ROWS  0x50
#define PARAM_FLAG_TRACE 1
/* bit1: run as the Route B monitor (Phase 3 P3): stage 2 default-deny,
 * virtual GICv3, device models in monitor.c. bit2 is unused: it was the
 * absent-fixed diagnostic, superseded by Phase 3 decision 5. */
#define PARAM_FLAG_MONITOR 2
/* bit3: debug only -- stage 2 also maps QEMU's GIC and PL011 at the
 * classic layout's IPAs, bypassing the models (bisection aid). */
#define PARAM_FLAG_PASSTHRU 8
/* bit4: debug only -- stage 2 translates the template GIC IPAs to QEMU's
 * real GIC, bypassing the GIC models (bisection aid). */
#define PARAM_FLAG_GICPT 16
/* bit5: debug only -- stage 2 maps QEMU's PL011 at the template UART IPA,
 * bypassing the PL011 model (bisection aid). */
#define PARAM_FLAG_UARTPT 32
#define PARAM_FLAG_BASELINE 64 /* debug: secondary windows as in Phase 0C */
#define PARAM_FLAG_HALT_ON_OFF 128 /* debug: halt instead of exiting at PSCI off */
/* bit8: the split console (Phase 3 P4, decision 3). Stage 2 maps only the
 * guest's rows of the framebuffer; the monitor draws its band below them,
 * where a guest store is a stage 2 fault. On a named stop the stub halts
 * instead of exiting, so the harness can read the screen. */
#define PARAM_FLAG_FRAMEBUFFER 256
/* bit9: the monitor halts in its band before the guest is entered
 * (Phase 3 step H2 proper). Without it the stub enters the guest. */
#define PARAM_FLAG_NO_GUEST 512
/* bit10: diagnostic hold: the monitor halts after setup, before any
 * GIC access (runs 30/31 restarted in that window). */
#define PARAM_FLAG_GIC_HOLD 1024

/* The QEMU framebuffer for the split console: above the panic chunk
 * (0x9d104000) and below the monitor run's 1536 MiB end (0xa0000000), so
 * it is neither reported DRAM nor firmware-carved memory. A 1024-pixel row is
 * exactly one 4 KiB page, and the guest's 512 rows are one 2 MiB stage 2
 * block; the 88 rows below it are the monitor's. No suffix: stub.S uses
 * these too. probes/classicload.py's QEMU_SPLIT_SCREEN must agree. */
#define MON_FB_PA         0x9e000000
#define MON_FB_WIDTH      1024
#define MON_FB_STRIDE     4096
#define MON_FB_GUEST_ROWS 512
#define MON_FB_BAND_ROWS  88

/* smoke_fault: the stub records an unexpected EL2 exception here and halts,
 * so the harness can report where the guest died rather than just time out.
 *   +0x00 magic (SMOKE_FAULT_MAGIC once written)
 *   +0x08 esr   +0x10 elr   +0x18 far   +0x20 vector-offset   +0x28 hcr */
#define SMOKE_FAULT_MAGIC 0x5641554c54ull   /* "FAULT" */
#define FAULT_MAGIC   0x00
#define FAULT_ESR     0x08
#define FAULT_ELR     0x10
#define FAULT_FAR     0x18
#define FAULT_VEC     0x20
#define FAULT_HCR     0x28

/* The SMCCC function IDs the stub recognises (Apple VMAPPLE, from published
 * XNU osfmk/arm64/hv_hvc.h, read as facts). Range tests use the service mask. */
#define HVC_SERVICE_MASK          0xffff0000u
#define HVC_CPU_SERVICE           0xc1000000u   /* Apple PAC range */
#define HVC_OEM_SERVICE           0xc3000000u
#define HVC_NESTED_SERVICE        0xc6000000u
#define VMAPPLE_PAC_GET_DEFAULT_KEYS 0xc1000001u
#define VMAPPLE_PAC_NOP           0xc10000f0u
#define PSCI_SERVICE_32           0x84000000u
#define PSCI_SERVICE_64           0xc4000000u
#define PSCI_VERSION_FN           0x84000000u
#define PSCI_SYSTEM_OFF           0x84000008u
#define PSCI_SYSTEM_RESET         0x84000009u

/* A smoke-test-private call the mini-kernel uses to end the run cleanly:
 * the stub turns it into an Angel semihosting SYS_EXIT. x1 is the exit code. */
#define SMOKE_HVC_EXIT            0x4b000000u    /* 'K' */

/* Opaque non-zero cookies the stub hands back for GET_DEFAULT_KEYS. The guest
 * treats these as opaque and only passes them back to SET_* calls, which the
 * stub ignores (collapsed key domains; see docs/GAOKUN3-PHASE0B.md B6). */
#define SMOKE_DEFAULT_ROP_COOKIE  0x524f50abcdef0001ull
#define SMOKE_DEFAULT_JOP_COOKIE  0x4a4f50abcdef0001ull

/* The TIDCP access log (Phase 0B task B5). HCR_EL2.TIDCP traps the guest's
 * implementation-defined system-register accesses to EL2; the stub RAZ/WIs
 * them, which is a diagnostic stand-in and not a monitor contract. To learn
 * which registers the stock kernel actually touches, every trap is recorded
 * here, deduplicated by register and direction so a register read in a loop
 * takes one slot. Rt is masked out of the key because it is incidental.
 *   +0x00 magic  +0x08 entries used  +0x10 total traps  +0x18 dropped
 *   +0x20 entries[]: key (ESR ISS without Rt), count, first ELR, reserved */
#define TIDCP_LOG_PA     0x43002000ull
#define TIDCP_LOG_MAGIC  0x54494443504c4f47ull   /* "TIDCPLOG" */
#define TIDCP_MAGIC      0x00
#define TIDCP_USED       0x08
#define TIDCP_TOTAL      0x10
#define TIDCP_DROPPED    0x18
#define TIDCP_ENTRIES    0x20
#define TIDCP_ENTRY_SIZE 32
#define TIDCP_MAX        64
#define TIDCP_ISS_MASK   0x3fffff                /* ESR ISS for EC 0x18 */
#define TIDCP_RT_MASK    0x3e0                   /* ISS bits 9:5, the Rt field */

/* The monitor's guest IPA map (Phase 3 P3): the VirtualMac template's own
 * physical addresses (arm-io parent 0x10000000 + the child's reg offset).
 * Stage 2 maps only guest RAM; every address below faults to monitor.c. */
#define MON_GICD_IPA     0x10000000ull   /* 64 KiB distributor model */
#define MON_GICR_IPA     0x10010000ull   /* redistributors (0x20000 per CPU) */
#define MON_GIC_MSI_IPA  0x1fff0000ull   /* the gic node's third window */
#define MON_UART_IPA     0x20010000ull   /* PL011 model */
#define MON_PL031_IPA    0x20050000ull   /* PL031 RTC model */
#define MON_PL061_IPA    0x20060000ull   /* PL061 GPIO model */
#define MON_PVPANIC_IPA  0x20070000ull   /* pvpanic model */
#define MON_PCIE_IPA     0x40000000ull   /* PCIe config, absent functions */
#define MON_PCIE_SIZE    0x10000000ull
/* AppleVirtualPlatform's fixed addresses, which have no tree node; their
 * answers are Phase 3 decision 5's (monitor.c). */
#define MON_BDIF_IPA     0x30000000ull   /* backdoor interface, 2 MiB */
#define MON_CFG_IPA      0x00400000ull   /* configuration region, 64 KiB */
#define MON_MAILBOX_IPA  0x004ff000ull   /* page of the 0x004ffff8 mailbox */

/* Physical (EL2) addresses of the real devices the monitor drives. The
 * guest never sees these IPAs: stage 2 does not map them. The gaokun3
 * values are OBSERVED in the machine's EL2 device tree (arm,gic-v3;
 * docs/GAOKUN3-PHASE3.md): distributor 64 KiB at 0x17a00000,
 * redistributors 1 MiB at 0x17a60000 with a 0x20000 stride. */
#ifdef VMAPPLE_GAOKUN3_MONITOR
#define PHYS_GICD_BASE   0x17a00000ull
#define PHYS_GICR_BASE   0x17a60000ull
#else
#define PHYS_GICD_BASE   0x08000000ull   /* QEMU virt distributor */
#define PHYS_GICR_BASE   0x080a0000ull   /* QEMU virt redistributor 0 */
#endif
#define MON_INTID_VTIMER 27
#define MON_INTID_MAINT  25

#endif
