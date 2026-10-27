/* SPDX-License-Identifier: MIT */
/* el2qual's internal runtime: the state, helpers and assembly entry points
 * that el2qual.c shares with the stage glue it keeps in other files
 * (usbstage.c). It is not an interface beyond el2qual: report.h and the stage
 * modules' own headers (usbprobe.h, pagemap.h, s2walk.h, ...) are those.
 *
 * el2qual.c stays the owner of everything declared here; the other files
 * only read and update it, on the boot CPU, in the order main() calls them. */
#ifndef EL2QUAL_INT_H
#define EL2QUAL_INT_H

#include <stddef.h>
#include <stdint.h>
#include "el2qual.h"
#include "report.h"
#include "uefi.h"

#define RD(reg) ({ uint64_t v_; __asm__ volatile("mrs %0, " reg : "=r"(v_)); v_; })
#define WR(reg, v) __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(v)))
#define ISB() __asm__ volatile("isb" ::: "memory")
#define DSB() __asm__ volatile("dsb sy" ::: "memory")

/* One CPU's fault-recovery context; its layout is the assembly contract's
 * (el2qual.h), checked in el2qual.c. */
struct el2q_ctx {
    uint64_t fixup, fixup_esr;
    uint64_t fatal_esr, fatal_elr, fatal_far, fatal_spsr, fatal_vector;
    uint64_t slot;
};

enum cache_op { CLEAN, CLEAN_INVALIDATE, INVALIDATE };

/* The load options (README.md), each off by default. */
struct el2q_opt {
    int serial, poweroff, nopac, nosmp, emulate_vhe, emulate_rotated, emulate_no_fb, emulate_dt_fb;
    int thermal, exit_vhe, usb, usb_smmu, usb_smmu_map, emulate_map_hole;
    int usb_s2_walk, usb_s2_survey, usb_s2_fixed, usb_s2_bypass_plan, usb_halt;
    int emulate_s2_tables, emulate_s2_odd, emulate_s2_device;
    int emulate_usb_halt, emulate_usb_halt_running;
    int rng;
};

extern struct el2q_opt opt;
extern struct el2q_run run;
extern struct el2q_ctx ctx[EL2Q_MAX_CPUS];
/* The memory map ExitBootServices accepted, and its sizes. */
extern uint8_t memory_map[128 * 1024];
extern uint64_t ebs_map_size, ebs_desc_size;

/* el2qual-asm.S: one load behind the fault fixup; 0, or the ESR it raised. */
uint64_t el2q_try_read32(struct el2q_ctx *ctx, uint64_t *value, uint64_t address);
uint64_t el2q_try_read64(struct el2q_ctx *ctx, uint64_t *value, uint64_t address);

/* el2qual.c. */
void progress(const char *s);
uint64_t current_el(void);
void dcache_range(const volatile void *start, size_t length, enum cache_op op);
int tsens_mapped(void *unused, uint64_t address, uint64_t *par);

/* usbstage.c: the USB stage's allocations before ExitBootServices, and its
 * reads on either side of it. */
void usb_stage_reserve(struct efi_boot_services *bs);
void measure_usb_before(void);
void measure_usb_after(void);

#endif
