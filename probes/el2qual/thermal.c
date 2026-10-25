/* SPDX-License-Identifier: MIT */
/* See thermal.h. The policy is the guard's; this file only checks the words
 * are mapped, connects the guard to el2qual's fault-catching read, drives the
 * polling window and copies the guard's state into the report record. */
#include <stddef.h>
#include "thermal.h"
#include "../../compat/sc8280xp-thermal.h"

_Static_assert(EL2Q_TSENS_BLOCKS == SC8280XP_THERMAL_BLOCKS, "controller count");
_Static_assert(EL2Q_TSENS_PIN == SC8280XP_THERMAL_VERSION_PIN, "the stage tests the guard's pin");

/* A counter that stops would keep the window loop from ever ending. The
 * architected counter ticks every ~52 ns at 19.2 MHz; four million reads in
 * a row returning one value is milliseconds of no tick, which a running
 * counter cannot produce. Those reads end the run as STUCK instead. */
#define STUCK_READS (UINT64_C(1) << 22)

struct bridge {
    const struct el2q_tsens_hw *hw;
    struct el2q_tsens *out;
};

/* The guard treats a nonzero return as a failed read, trips READ at this
 * address and makes no further read; the first fault is the one recorded. */
static int bridge_read(void *ctx, uint64_t address, uint32_t *value)
{
    struct bridge *b = ctx;
    uint64_t word = 0;
    uint64_t esr = b->hw->read32(b->hw->ctx, address, &word);
    if (esr) {
        if (!b->out->abort_esr) {
            b->out->abort_esr = esr;
            b->out->abort_at = address;
        }
        return -1;
    }
    *value = (uint32_t)word;
    return 0;
}

static uint64_t bridge_counter(void *ctx)
{
    struct bridge *b = ctx;
    return b->hw->counter(b->hw->ctx);
}

/* Nothing runs that a trip would have to stop: the guard records its own
 * reason and location, and does no MMIO after it. */
static void bridge_stop(void *ctx, unsigned reason, uint64_t location, uint64_t now)
{
    (void)ctx; (void)reason; (void)location; (void)now;
}

static struct sc8280xp_thermal_state guard;

static void copy_out(struct el2q_tsens *out)
{
    out->reason = guard.reason;
    out->location = guard.location;
    out->polls = guard.polls;
    out->reads = guard.reads;
    out->valid = guard.valid_count;
    out->peak_mc = guard.peak_mc;
    out->current_mc = guard.current_mc;
    out->max_poll_ticks = guard.max_poll_ticks;
    out->max_gap_ticks = guard.max_gap_ticks;
    for (unsigned b = 0; b < EL2Q_TSENS_BLOCKS; b++) {
        out->version[b] = guard.version[b];
        out->control[b] = guard.control[b];
        out->trdy[b] = guard.trdy[b];
        out->valid_mask[b] = guard.valid_mask[b];
    }
}

void el2q_tsens_run(struct el2q_tsens *out, const struct el2q_tsens_hw *hw)
{
    *out = (struct el2q_tsens){.state = EL2Q_TSENS_NOT_RUN, .peak_mc = INT64_MIN,
                               .current_mc = INT64_MIN};
    if (!hw || !hw->mapped || !hw->read32 || !hw->counter || !hw->isr) return;
    out->hz = hw->hz;
    out->window_ticks = hw->window;

    /* Every word first: one unmapped word means the firmware's tables do not
     * describe these blocks as this probe expects, so none is read. */
    uint64_t words[SC8280XP_THERMAL_WORDS];
    unsigned count = sc8280xp_thermal_words(words, SC8280XP_THERMAL_WORDS);
    for (unsigned i = 0; i < count && i < SC8280XP_THERMAL_WORDS; i++) {
        uint64_t par = 0;
        out->checked++;
        if (hw->mapped(hw->ctx, words[i], &par)) {
            if (!out->unmapped++) {
                out->unmapped_at = words[i];
                out->par = par;
            }
        }
    }
    if (out->unmapped || out->checked != SC8280XP_THERMAL_WORDS) {
        out->state = EL2Q_TSENS_UNMAPPED;
        out->isr = hw->isr(hw->ctx);
        return;
    }

    guard = (struct sc8280xp_thermal_state){0};
    struct bridge b = {hw, out};
    const struct sc8280xp_thermal_io io = {&b, bridge_read, bridge_counter, bridge_stop,
                                           EL2Q_TSENS_PIN};
    out->admit = sc8280xp_thermal_admit(&guard, hw->hz, &io);
    if (out->admit == 1) out->state = EL2Q_TSENS_TRIPPED;
    else if (out->admit != 0) out->state = EL2Q_TSENS_NO_SAMPLE;
    else {
        uint64_t start = hw->counter(hw->ctx), now = start, last = start, same = 0;
        for (;;) {
            now = hw->counter(hw->ctx);
            if (now - start >= hw->window) {
                out->state = EL2Q_TSENS_PASSED;
                break;
            }
            same = now == last ? same + 1 : 0;
            last = now;
            if (same >= STUCK_READS) {
                out->state = EL2Q_TSENS_STUCK;
                break;
            }
            /* A poll not yet due returns at once without reading. */
            if (sc8280xp_thermal_poll(&guard, now, hw->hz, &io)) {
                out->state = EL2Q_TSENS_TRIPPED;
                break;
            }
        }
        out->elapsed_ticks = now - start;
    }
    copy_out(out);
    out->isr = hw->isr(hw->ctx);
}
