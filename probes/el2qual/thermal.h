/* SPDX-License-Identifier: MIT */
/* el2qual's tsens read (load option "thermal"): the Phase 1 thermal guard run
 * against live controllers, read only.
 *
 * thermal.c holds no hardware access of its own. el2qual.c supplies the four
 * operations below at EL2; tools/test-el2qual.c supplies synthetic ones, so
 * every outcome, including the ones that must read nothing, runs on the Mac
 * first. */
#ifndef EL2QUAL_THERMAL_H
#define EL2QUAL_THERMAL_H

#include <stdint.h>
#include "report.h"

struct el2q_tsens_hw {
    void *ctx;
    /* 0 when the address translates, for a read at the current EL, to the
     * same physical address as Device memory; nonzero otherwise. *par gets
     * the translation's raw result (PAR_EL1) either way. Must not fault. */
    int (*mapped)(void *ctx, uint64_t address, uint64_t *par);
    /* One 32-bit read behind a fault fixup: 0 and *value set, or the ESR of
     * the exception the read raised. */
    uint64_t (*read32)(void *ctx, uint64_t address, uint64_t *value);
    uint64_t (*counter)(void *ctx);
    uint64_t (*isr)(void *ctx);     /* ISR_EL1, for an SError the reads left pending */
    uint64_t hz;                    /* the counter's rate (CNTFRQ_EL0) */
    uint64_t window;                /* ticks to keep polling after admission */
};

/* Fills *out. Every word the guard may read is checked with hw->mapped
 * first; if any fails, nothing is read. Otherwise the guard admits with
 * EL2Q_TSENS_PIN as its version pin and, if it admits, polls at its own
 * quarter-second rate until hw->window has passed or it trips. A read that
 * faults trips the guard (READ) and stops all further reads. */
void el2q_tsens_run(struct el2q_tsens *out, const struct el2q_tsens_hw *hw);

#endif
