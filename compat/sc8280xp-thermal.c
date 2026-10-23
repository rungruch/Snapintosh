/* SPDX-License-Identifier: MIT */
#include "sc8280xp-thermal.h"

_Static_assert(sizeof(struct sc8280xp_thermal_state) == SC8280XP_THERMAL_REPORT_WORDS * 8,
               "thermal report ABI");
_Static_assert(sizeof(struct sc8280xp_thermal_state) <= 4096, "one monitor-only page");

/* tsens-v2 register map: Linux drivers/thermal/qcom/tsens-v2.c's field table,
 * read as a hardware reference. compat/a16-thermal.c reads the same offsets on
 * the X2 Elite, where they are VERIFIED; on the SC8280XP they are OBSERVED (the
 * DT says "qcom,tsens-v2") and not yet read on a gaokun3. */
#define SROT_HW_VER 0x000u
#define SROT_CTRL 0x004u
#define TM_SN_STATUS 0x0a0u               /* one word per sensor */
#define TM_TRDY 0x0e4u
#define CTRL_TSENS_EN (1u << 0)
#define CTRL_SW_RST (1u << 1)
#define CTRL_SENSOR_EN_SHIFT 3            /* bits 18:3, one per sensor */
#define CTRL_CODE_OR_TEMP (1u << 21)      /* 1: LAST_TEMP is a temperature, not an ADC code */
#define STATUS_LAST_TEMP 0xfffu           /* signed 12 bits, 0.1 degC */
#define STATUS_CRITICAL (1u << 19)        /* the firmware-programmed critical threshold */
#define STATUS_VALID (1u << 21)

/* The four controllers and their sensor counts: the gaokun3 EL2 DTB's
 * thermal-sensor nodes (reg = <TM 0x1ff>, <SROT 0x8>; #qcom,sensors), built
 * from upstream arch/arm64/boot/dts/qcom/sc8280xp.dtsi. OBSERVED, not
 * measured; tools/test-sc8280xp-thermal.py checks this table against a DTB. */
#define TSENS0_SENSORS 14u
#define TSENS1_SENSORS 16u
#define TSENS2_SENSORS 11u
#define TSENS3_SENSORS 5u
_Static_assert(TSENS0_SENSORS + TSENS1_SENSORS + TSENS2_SENSORS + TSENS3_SENSORS ==
               SC8280XP_THERMAL_SENSORS, "sensor total");
static const struct { uint64_t tm, srot; unsigned sensors; } tsens[SC8280XP_THERMAL_BLOCKS] = {
    {UINT64_C(0x0c263000), UINT64_C(0x0c222000), TSENS0_SENSORS},
    {UINT64_C(0x0c265000), UINT64_C(0x0c223000), TSENS1_SENSORS},
    {UINT64_C(0x0c251000), UINT64_C(0x0c224000), TSENS2_SENSORS},
    {UINT64_C(0x0c252000), UINT64_C(0x0c225000), TSENS3_SENSORS},
};

int sc8280xp_thermal_address_allowed(uint64_t address) {
    if (address & 3) return 0;
    for (unsigned b = 0; b < SC8280XP_THERMAL_BLOCKS; b++) {
        uint64_t sensors = tsens[b].tm + TM_SN_STATUS;
        if (address == tsens[b].srot + SROT_HW_VER || address == tsens[b].srot + SROT_CTRL ||
            address == tsens[b].tm + TM_TRDY ||
            (address >= sensors && address < sensors + 4 * tsens[b].sensors))
            return 1;
    }
    return 0;
}

static int clock_ok(uint64_t hz) { return hz >= 1000 && hz <= 1000000000; }

uint64_t sc8280xp_thermal_wait(const struct sc8280xp_thermal_state *s, uint64_t now,
                               uint64_t hz, uint64_t limit) {
    if (!s || !s->magic || s->reason || !clock_ok(hz)) return 0;
    uint64_t wait = (int64_t)(s->next_poll - now) > 0 ? s->next_poll - now : 0;
    return wait < limit ? wait : limit;
}

/* A trip is immutable: the first reason sticks and no later poll reads MMIO,
 * so a sensor that recovers cannot clear a stop already requested. */
static int trip(struct sc8280xp_thermal_state *s, const struct sc8280xp_thermal_io *io,
                unsigned why, uint64_t where, uint64_t now) {
    if (!s->reason) {
        s->reason = why; s->location = where; s->trip_ticks = now; s->ready = 0;
        if (io && io->stop) io->stop(io->ctx, why, where, now);
    }
    return 1;
}

/* read_inflight stays 1 if read32 faults the CPU instead of returning, so a
 * report read after the fault names the address that took it. */
static int read_word(struct sc8280xp_thermal_state *s, const struct sc8280xp_thermal_io *io,
                     uint64_t address, uint32_t *value) {
    s->location = address; s->reads++; s->read_inflight = 1;
    __asm__ volatile("" ::: "memory");
    if (!sc8280xp_thermal_address_allowed(address) || io->read32(io->ctx, address, value)) {
        s->read_failures++;
        return -1;
    }
    __asm__ volatile("" ::: "memory");
    s->read_inflight = 0;
    return 0;
}

int sc8280xp_thermal_poll(struct sc8280xp_thermal_state *s, uint64_t now, uint64_t hz,
                          const struct sc8280xp_thermal_io *io) {
    if (!s->magic) {
        s->magic = SC8280XP_THERMAL_MAGIC; s->abi = SC8280XP_THERMAL_ABI; s->bytes = sizeof(*s);
        s->started = s->last_change = now; s->current_mc = s->peak_mc = INT64_MIN;
    }
    if (s->reason) return 1;
    if (s->magic != SC8280XP_THERMAL_MAGIC || s->abi != SC8280XP_THERMAL_ABI ||
        s->bytes != sizeof(*s) || !io || !io->read32 || !io->counter || !io->stop)
        return trip(s, io, SC8280XP_THERMAL_CONFIG, 0, now);
    if (!clock_ok(hz)) return trip(s, io, SC8280XP_THERMAL_CLOCK, 0, now);
    /* Refused before any MMIO: without a measured version word nothing proves
     * these addresses are the tsens blocks this policy decodes. */
    if (!io->version_pin) return trip(s, io, SC8280XP_THERMAL_UNPINNED, 0, now);
    s->pin = io->version_pin;
    if (s->polls) {
        uint64_t gap = now - s->last_poll;
        if (gap > s->max_gap_ticks) s->max_gap_ticks = gap;
        /* A guard not polled for a second cannot claim the run stayed cool. */
        if (gap >= hz) return trip(s, io, SC8280XP_THERMAL_LATE, 0, now);
        if ((int64_t)(now - s->next_poll) < 0) return 0;
    }
    s->polls++; s->last_poll = now; s->next_poll = now + hz / 4;
    s->valid_count = 0; s->missing_count = 0; s->current_mc = INT64_MIN; s->ready = 0;
    for (unsigned b = 0; b < SC8280XP_THERMAL_BLOCKS; b++) {
        uint32_t v;
        unsigned count = tsens[b].sensors;
        uint64_t mask = (UINT64_C(1) << count) - 1;
        uint64_t srot = tsens[b].srot, tm = tsens[b].tm;
        if (read_word(s, io, srot + SROT_HW_VER, &v))
            return trip(s, io, SC8280XP_THERMAL_READ, s->location, now);
        s->version[b] = v;
        if (v != io->version_pin) return trip(s, io, SC8280XP_THERMAL_CONFIG, srot + SROT_HW_VER, now);
        if (read_word(s, io, srot + SROT_CTRL, &v))
            return trip(s, io, SC8280XP_THERMAL_READ, s->location, now);
        s->control[b] = v;
        /* Every sensor this policy reads must be enabled; the exact enable mask
         * is unmeasured, so extra enabled sensors are allowed, missing ones are not. */
        if (!(v & CTRL_TSENS_EN) || (v & CTRL_SW_RST) || !(v & CTRL_CODE_OR_TEMP) ||
            (((v >> CTRL_SENSOR_EN_SHIFT) & 0xffffu) & mask) != mask)
            return trip(s, io, SC8280XP_THERMAL_CONFIG, srot + SROT_CTRL, now);
        if (read_word(s, io, tm + TM_TRDY, &v))
            return trip(s, io, SC8280XP_THERMAL_READ, s->location, now);
        s->trdy[b] = v; s->valid_mask[b] = 0;
        for (unsigned n = 0; n < count; n++) {
            unsigned slot = b * 16 + n;
            uint64_t address = tm + TM_SN_STATUS + 4 * n, bit = UINT64_C(1) << n;
            if (read_word(s, io, address, &v))
                return trip(s, io, SC8280XP_THERMAL_READ, address, now);
            uint64_t previous = s->raw[slot];
            s->raw[slot] = v;
            if (v & STATUS_CRITICAL) return trip(s, io, SC8280XP_THERMAL_CRITICAL, address, now);
            if (!(v & STATUS_VALID)) {
                /* A brief VALID clear is a conversion in flight; a second without
                 * a valid reading means that sensor is not being watched. */
                s->missing_count++; s->invalid_samples++;
                uint64_t since = s->seen_mask[b] & bit ? s->last_valid[slot] : s->started;
                if (now - since >= hz) return trip(s, io, SC8280XP_THERMAL_INVALID, address, now);
                continue;
            }
            int32_t code = (int32_t)(v & STATUS_LAST_TEMP);
            if (code & 0x800) code -= 4096;
            int64_t mc = (int64_t)code * 100;
            if (mc < -40000 || mc > 120000) return trip(s, io, SC8280XP_THERMAL_RANGE, address, now);
            if (mc > s->current_mc) s->current_mc = mc;
            if (mc > s->peak_mc) s->peak_mc = mc;
            s->valid_count++; s->valid_mask[b] |= bit;
            if (!(s->seen_mask[b] & bit) || (v & STATUS_LAST_TEMP) != (previous & STATUS_LAST_TEMP))
                s->last_change = now;
            s->seen_mask[b] |= bit; s->last_valid[slot] = now;
            if (mc >= SC8280XP_THERMAL_LIMIT_MC) return trip(s, io, SC8280XP_THERMAL_HOT, address, now);
        }
    }
    uint64_t done = io->counter(io->ctx);
    s->poll_ticks = done - now;
    if (s->poll_ticks > s->max_poll_ticks) s->max_poll_ticks = s->poll_ticks;
    if (s->poll_ticks >= hz / 4) return trip(s, io, SC8280XP_THERMAL_SLOW, 0, done);
    /* 46 sensors at 0.1 C that do not change for ten seconds are frozen
     * registers, not a cool chip (INFERRED from the A16 rule; not measured here). */
    s->unchanged_ticks = done - s->last_change;
    if (s->unchanged_ticks >= 10 * hz) return trip(s, io, SC8280XP_THERMAL_UNCHANGED, 0, done);
    s->location = 0;
    s->ready = s->valid_count == SC8280XP_THERMAL_SENSORS;
    return 0;
}

/* Admission resamples a brief VALID clear instead of refusing on it. Each
 * attempt is the full poll, so any trip ends the wait, and sensors are never
 * merged across samples. A counter that never advances ends at the sample bound. */
int sc8280xp_thermal_admit(struct sc8280xp_thermal_state *s, uint64_t hz,
                           const struct sc8280xp_thermal_io *io) {
    if (!io || !io->counter) return sc8280xp_thermal_poll(s, 0, hz, io) ? 1 : -1;
    uint64_t start = io->counter(io->ctx);
    for (unsigned n = 0; n < SC8280XP_THERMAL_ADMIT_SAMPLES; n++) {
        uint64_t now = io->counter(io->ctx);
        if (n) {
            if (now - start >= hz / 2) break;
            s->next_poll = now;
        }
        if (sc8280xp_thermal_poll(s, now, hz, io)) return 1;
        if (s->ready) return 0;
        for (uint64_t spin = 0; spin < hz / 100 && io->counter(io->ctx) - now < hz / 100; spin++);
    }
    return -1;
}
