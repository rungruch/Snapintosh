/* SPDX-License-Identifier: MIT */
#include "monitor.h"
#include "../../probes/font5x7.h"
#include "layout.h"

uint64_t monitor_handled;
uint64_t monitor_stop_ipa;
int monitor_stop_write;
uint64_t monitor_stop_count;

static int absent_fixed;
static int tracing;
static int baseline_answers;
static uint32_t count_pcie, count_uart, count_gic, count_rtc, count_other;

/* ---- small output helpers (monitor console) --------------------------- */
static void puts_(const char *s) { while (*s) mon_uart_putc(*s++); }
static void puthex(uint64_t v)
{
    puts_("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int n = (int)((v >> i) & 0xf);
        mon_uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}
static void putdec(uint64_t v)
{
    char buf[20];
    int i = 0;
    if (!v) { mon_uart_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) mon_uart_putc(buf[--i]);
}

/* ---- model state -------------------------------------------------------- */
#define REGS(name, n) static uint32_t name[(n) / 4]
REGS(gicd_regs, 0x10000);            /* set/clear semantics applied on write */
REGS(gicr_rd, 0x10000);              /* RD_base frame */
REGS(gicr_sgi, 0x10000);             /* SGI_base frame */
REGS(uart_regs, 0x1000);
REGS(pl031_regs, 0x1000);
REGS(pl061_regs, 0x1000);
REGS(pvpanic_regs, 0x1000);
REGS(msi_regs, 0x1000);

static uint32_t load32(const uint32_t *base, uint64_t off, unsigned size)
{
    uint32_t v = base[off / 4];
    if (size == 4) return v;
    unsigned shift = (unsigned)(off & 3) * 8;
    return (v >> shift) & (uint32_t)((1u << (8 * size)) - 1);
}

static void store32(uint32_t *base, uint64_t off, unsigned size, uint64_t value)
{
    if (size == 4) { base[off / 4] = (uint32_t)value; return; }
    unsigned shift = (unsigned)(off & 3) * 8;
    uint32_t mask = (uint32_t)((size == 1 ? 0xffu : 0xffffu) << shift);
    base[off / 4] = (base[off / 4] & ~mask) | ((uint32_t)value << shift & mask);
}

/* ---- GICv3 distributor model ---------------------------------------------
 * A register-state model: enables, priorities, configuration and pending
 * bits are stored with their set/clear write semantics. CTLR's RWP bit
 * always reads 0 (every write completes at once, which is true here).
 * TYPER reports 352 SPIs and one CPU interface, matching the one-vCPU
 * tree the loader hands over. */
static int gicd_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    if (off == 0x0004) {                             /* GICD_TYPER */
        /* QEMU virt's own TYPER, measured at EL2 on this configuration:
         * 288 SPIs and the same feature bits the Phase 0C boot ran with. */
        if (!write) *value = 0x037a0208;
        return 1;
    }
    if (off == 0x0008) {                             /* GICD_IIDR */
        if (!write) *value = 0x0000043b;
        return 1;
    }
    if (off == 0x0000 && !write) {                   /* CTLR: RWP reads 0 */
        *value = load32(gicd_regs, off, size) & ~(1u << 31);
        return 1;
    }
    if (off >= 0xfe00 && off < 0x10000) {            /* PIDR/CIDR block */
        if (!write) *value = (off == 0xfe8) ? 0x3b : 0;
        return 1;
    }
    if (write) {
        if (off >= 0x100 && off < 0x180)             /* ISENABLER: set bits */
            gicd_regs[off / 4] |= (uint32_t)*value;
        else if (off >= 0x180 && off < 0x200)        /* ICENABLER: clear bits */
            gicd_regs[(off - 0x80) / 4] &= ~(uint32_t)*value;
        else if (off >= 0x200 && off < 0x280)        /* ISPENDR */
            gicd_regs[off / 4] |= (uint32_t)*value;
        else if (off >= 0x280 && off < 0x300)        /* ICPENDR */
            gicd_regs[(off - 0x80) / 4] &= ~(uint32_t)*value;
        else if (off == 0xf00) {                     /* SGIR: no other CPU */
            /* A self-targeted SGI is not needed to reach the root wait;
             * a write is accepted and no interrupt is raised. */
        } else
            store32(gicd_regs, off, size, *value);
    } else {
        /* ICENABLER/ICPENDR read back the enable/pending state. */
        if (off >= 0x180 && off < 0x200) off -= 0x80;
        if (off >= 0x280 && off < 0x300) off -= 0x80;
        *value = load32(gicd_regs, off, size);
    }
    return 1;
}

/* Redistributor model. WAKER never reports ChildrenAsleep: there are no
 * children asleep behind this model, and the guest polls that bit.
 * TYPER marks this the last (only) redistributor, processor 0, with the
 * CPU0 affinity the guest reads from its own MPIDR. */
static int gicr_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    uint32_t *frame = gicr_rd;
    if (off >= 0x10000) { frame = gicr_sgi; off -= 0x10000; }
    if (frame == gicr_rd && (off == 0x0008 || off == 0x000c)) {
        if (!write) {                                /* GICR_TYPER (64-bit) */
            uint64_t typer = (1ull << 4) | 1ull;     /* Last, PLPIS; CPU0, affinity 0 */
            unsigned shift = (unsigned)(off & 7) * 8;
            *value = (typer >> shift) & ((size == 8) ? ~0ull : ((1ull << (8 * size)) - 1));
        }
        return 1;
    }
    if (frame == gicr_rd && off == 0x0014) {         /* GICR_WAKER */
        if (write) store32(frame, off, size, *value & ~(1u << 2));
        else *value = load32(frame, off, size) & ~(1u << 2);
        return 1;
    }
    if (frame == gicr_rd && off >= 0xfe00) {
        if (!write) *value = (off == 0xfe8) ? 0x3b : 0;
        return 1;
    }
    if (write) store32(frame, off, size, *value);
    else *value = load32(frame, off, size);
    return 1;
}

/* ---- PL011 model ----------------------------------------------------------
 * The data register goes to the monitor's own UART, so the guest's console
 * output is the serial line. FR always reports transmit and receive empty
 * of backlog: the monitor drains every byte at once and has no input. */
static int uart_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    count_uart++;
    if (off == 0x000) {                              /* UARTDR */
        if (write) mon_uart_putc((char)(*value & 0xff));
        else *value = 0;
        return 1;
    }
    if (off == 0x018 && !write) {                    /* UARTFR: TXFE|RXFE */
        *value = 0x90;
        return 1;
    }
    if (write) store32(uart_regs, off, size, *value);
    else *value = load32(uart_regs, off, size);
    return 1;
}

/* PL031 RTC (published PrimeCell contract): DR is seconds on the counter,
 * everything else is stored state; no interrupt is ever raised. */
static int pl031_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    count_rtc++;
    if (baseline_answers) {
        if (!write) *value = (size == 8) ? ~0ull : ((1ull << (8 * size)) - 1);
        return 1;
    }
    if (off == 0x000 && !write) {                    /* RTCDR */
        uint64_t hz = mon_cntfrq();
        *value = hz ? (uint32_t)(mon_cntpct() / hz) : 0;
        return 1;
    }
    if ((off == 0x014 || off == 0x018) && !write) {  /* RIS/MIS: none pending */
        *value = 0;
        return 1;
    }
    if (write) store32(pl031_regs, off, size, *value);
    else *value = load32(pl031_regs, off, size);
    return 1;
}

static int store_access(uint32_t *regs, uint64_t off, unsigned size, int write,
                        uint64_t *value)
{
    count_other++;
    if (baseline_answers) {
        if (!write) *value = (size == 8) ? ~0ull : ((1ull << (8 * size)) - 1);
        return 1;
    }
    if (write) store32(regs, off, size, *value);
    else *value = load32(regs, off, size);
    return 1;
}

/* PCIe configuration space with no functions: all-ones reads are PCI's
 * defined answer for an absent function. Writes are ignored. */
static int pcie_access(unsigned size, int write, uint64_t *value)
{
    count_pcie++;
    if (!write) *value = (size == 8) ? ~0ull : ((1ull << (8 * size)) - 1);
    return 1;
}

struct window { uint64_t base, size; int kind; };
enum { K_GICD, K_GICR, K_MSI, K_UART, K_PL031, K_PL061, K_PVPANIC, K_PCIE,
       K_FIXED_ID, K_FIXED_REQ };
static const struct window windows[] = {
    { MON_GICD_IPA,    0x10000,    K_GICD },
    { MON_GICR_IPA,    0x400000,   K_GICR },
    { MON_GIC_MSI_IPA, 0x10000,    K_MSI },
    { MON_UART_IPA,    0x10000,    K_UART },
    { MON_PL031_IPA,   0x1000,     K_PL031 },
    { MON_PL061_IPA,   0x1000,     K_PL061 },
    { MON_PVPANIC_IPA, 0x10000,    K_PVPANIC },
    { MON_PCIE_IPA,    MON_PCIE_SIZE, K_PCIE },
    { MON_FIXED_ID_IPA,  0x1000,   K_FIXED_ID },
    { MON_FIXED_REQ_IPA, 0x1000,   K_FIXED_REQ },
    /* A third fixed address P3 found: a computed-pointer read at
     * 0x4006cc during IOKit start, answered by QEMU's flash in the old
     * smoke. Contract UNKNOWN; same policy as the other two. */
    { 0x400000,          0x1000,   K_FIXED_REQ },
};

/* Decode the faulting load/store when the syndrome carries no access
 * information (stage 2 translation faults, ISV 0). Handles the unsigned
 * immediate, register-offset and pre/post-index forms XNU's device
 * drivers use; returns 0 for anything else (a named stop). On success
 * *srt, *size and *sign hold; register writeback is applied to the frame. */
static int decode_access(uint64_t elr, uint64_t *frame, unsigned *srt,
                         unsigned *size, int *sign)
{
    uint64_t par = mon_translate_va(elr);
    if (tracing) { puts_("EL2: decode elr "); puthex(elr); puts_(" par "); puthex(par); puts_("\n"); }
    if (par & 1) return 0;
    uint64_t ipa = (par & 0x0000fffffffff000ull) | (elr & 0xfff);
    uint32_t insn = *(volatile uint32_t *)(uintptr_t)ipa;
    *sign = 0;
    if ((insn & 0x3b000000) == 0x39000000) {         /* unsigned immediate */
        *size = 1u << (insn >> 30);
        *srt = insn & 0x1f;
        if ((insn >> 22) & 1 && (insn >> 23) & 1) *sign = 1;
        return !(insn & (1u << 26));                 /* not a vector access */
    }
    if ((insn & 0x3b200c00) == 0x38200800) {         /* register offset */
        *size = 1u << (insn >> 30);
        *srt = insn & 0x1f;
        if ((insn >> 22) & 1 && (insn >> 23) & 1) *sign = 1;
        return !(insn & (1u << 26));
    }
    if ((insn & 0x3b200000) == 0x38000000
        && ((insn >> 10) & 3) != 2) {                /* pre/post-index */
        *size = 1u << (insn >> 30);
        *srt = insn & 0x1f;
        if ((insn >> 22) & 1 && (insn >> 23) & 1) *sign = 1;
        int64_t imm9 = (int32_t)((insn >> 12) & 0x1ff);
        if (imm9 & 0x100) imm9 -= 0x200;
        unsigned rn = (insn >> 5) & 0x1f;
        if (rn != 31)
            frame[rn] += (uint64_t)imm9;             /* writeback, pre or post */
        return !(insn & (1u << 26));
    }
    return 0;
}

/* ---- the split console's band (Phase 3 P4, decision 3) -------------------
 * The monitor's own rows under the guest's framebuffer. Stage 2 never maps
 * them, so what is here is only ever the monitor's: the harness renders the
 * band it expects with probes/font5x7.h and compares it pixel for pixel.
 * Layout, which tools/vmapple-smoke/smoke.py mirrors: background
 * MON_BAND_BG, glyphs at scale 2 in 12x20-pixel cells, text from x 8,
 * line n at y 6 + 20n within the band. EL2 runs with its MMU off here, so
 * these are Device stores: 32-bit and aligned. */
#define MON_BAND_BG    0x00202020u
#define MON_BAND_TEXT  0x00ffffffu
#define MON_BAND_STOP  0x00ff5050u
static int band_on;

static void band_pixel(unsigned x, unsigned y, uint32_t color)
{
    if (x >= MON_FB_WIDTH || y >= MON_FB_BAND_ROWS) return;
    volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)
        (MON_FB_PA + (uint64_t)(MON_FB_GUEST_ROWS + y) * MON_FB_STRIDE);
    row[x] = color;
}

static void band_text(unsigned line, const char *s, uint32_t color)
{
    for (unsigned j = 0; s[j]; j++) {
        const uint8_t *g = font5x7_glyph(s[j]);
        unsigned x0 = 8 + j * 12, y0 = 6 + line * 20;
        for (unsigned row = 0; row < 7; row++)
            for (unsigned bit = 0; bit < 5; bit++)
                if (g[row] & (0x10 >> bit))
                    for (unsigned d = 0; d < 4; d++)
                        band_pixel(x0 + bit * 2 + (d & 1), y0 + row * 2 + (d >> 1), color);
    }
}

void monitor_set_band(int on)
{
    band_on = on;
    if (!on) return;
    for (unsigned y = 0; y < MON_FB_BAND_ROWS; y++)
        for (unsigned x = 0; x < MON_FB_WIDTH; x++) band_pixel(x, y, MON_BAND_BG);
    band_text(0, "SNAPINTOSH MONITOR: STAGE 2 DEFAULT-DENY", MON_BAND_TEXT);
}

static void band_stop(uint64_t ipa, int write)
{
    if (!band_on) return;
    char line[48] = "STOP: UNMODELLED IPA ";
    unsigned n = 21;
    for (int i = 15; i >= 0; i--) line[n++] = "0123456789ABCDEF"[(ipa >> (4 * i)) & 15];
    const char *what = write ? " WRITE" : " READ";
    for (unsigned i = 0; what[i]; i++) line[n++] = what[i];
    line[n] = 0;
    band_text(1, line, MON_BAND_STOP);
}

int monitor_data_abort(uint64_t esr, uint64_t far, uint64_t elr, uint64_t *frame)
{
    unsigned iss = (unsigned)(esr & 0x1ffffff);
    unsigned sas = (iss >> 22) & 3;
    unsigned size = 1u << sas;
    unsigned srt = (iss >> 16) & 0x1f;
    int write = (iss >> 6) & 1;
    int sign = (iss >> 21) & 1;
    if (!(iss & (1u << 24))
        && !decode_access(elr, frame, &srt, &size, &sign))
        goto stop;
    uint64_t value = write && srt != 31 ? frame[srt] : 0;

    for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        const struct window *w = &windows[i];
        if (far < w->base || far >= w->base + w->size) continue;
        uint64_t off = far - w->base;
        int handled;
        switch (w->kind) {
        case K_GICD: count_gic++; handled = gicd_access(off, size, write, &value); break;
        case K_GICR: count_gic++; handled = gicr_access(off, size, write, &value); break;
        case K_MSI:  handled = store_access(msi_regs, off & 0xfff, size, write, &value); break;
        case K_UART: handled = uart_access(off, size, write, &value); break;
        case K_PL031: handled = pl031_access(off, size, write, &value); break;
        case K_PL061: handled = store_access(pl061_regs, off, size, write, &value); break;
        case K_PVPANIC: handled = store_access(pvpanic_regs, off, size, write, &value); break;
        case K_PCIE: handled = pcie_access(size, write, &value); break;
        case K_FIXED_ID:
            if (!absent_fixed) goto stop;
            if (!write) value = (size == 8) ? ~0ull : ((1ull << (8 * size)) - 1);
            handled = 1; break;
        case K_FIXED_REQ:
            if (!absent_fixed) goto stop;
            if (!write) value = 0;
            handled = 1; break;
        default: handled = 0; break;
        }
        if (!handled) goto stop;
        monitor_handled++;
        if (tracing && w->kind != K_UART) {
            puts_("EL2: mmio ");
            puthex(far);
            puts_(write ? " write\n" : " read\n");
        }
        if (!write && srt != 31) {
            if (sign) {                              /* sign-extend the load */
                if (size == 1) value = (uint64_t)(int64_t)(int8_t)value;
                else if (size == 2) value = (uint64_t)(int64_t)(int16_t)value;
                else if (size == 4) value = (uint64_t)(int64_t)(int32_t)value;
            }
            frame[srt] = value;
        }
        return 1;
    }
stop:
    monitor_stop_count++;
    monitor_stop_ipa = far;
    monitor_stop_write = write;
    puts_("EL2: stop: unmodelled IPA ");
    puthex(far);
    puts_(write ? " write\n" : " read\n");
    band_stop(far, write);
    return 0;
}

/* ---- physical interrupt delivery -----------------------------------------
 * The guest's virtual timer (INTID 27) is delivered through list register
 * 0 with its EOI bit set, so the guest's deactivation raises the GIC
 * maintenance interrupt (PPI 25). While LR0 is occupied the physical timer
 * PPI is disabled at the redistributor, or it would re-fire at EL2 before
 * the guest can run. */
#define GICR_IGROUPR0    (PHYS_GICR_BASE + 0x10000 + 0x0080)
#define GICR_ISENABLER0  (PHYS_GICR_BASE + 0x10000 + 0x0100)
#define GICR_ICENABLER0  (PHYS_GICR_BASE + 0x10000 + 0x0180)
#define GICR_WAKER       (PHYS_GICR_BASE + 0x0014)
/* Group 0: on this platform the timer reaches the guest as an FIQ (the
 * Phase 0C boot took FIQs at EL1, not IRQs), so the virtual timer is
 * delivered in group 0 and the guest acknowledges it at ICC_IAR0_EL1. */
#define LR_PENDING_EOI(vintid) \
    ((1ull << 62) | (0xa0ull << 48) | (1ull << 41) | (uint64_t)(vintid))

void monitor_set_policy(int absent, int trace)
{
    absent_fixed = absent;
    tracing = trace;
}

void monitor_set_baseline_answers(int on) { baseline_answers = on; }

void monitor_init(int absent, int trace)
{
    monitor_set_policy(absent, trace);
    mon_redist_write(GICR_WAKER, 0);                     /* wake redistributor 0 */

    /* QEMU's PPIs reset to Group 0; move the timer and maintenance PPIs
     * to Group 1 or the physical interface never signals EL2 with them. */
    /* The guest's distributor writes go to the model, so QEMU's real
     * distributor would stay at its reset value and forward nothing:
     * enable both groups on it here. PPIs are configured per-CPU at the
     * redistributor below; this only opens the distributor's gate. */
    mon_redist_write(0x08000000, 0x3);                 /* GICD_CTLR: EnGrp0|EnGrp1 */
    mon_redist_write(GICR_IGROUPR0, (1u << MON_INTID_MAINT) | (1u << 26));
    mon_redist_write(GICR_ISENABLER0, (1u << MON_INTID_VTIMER) | (1u << 29) | (1u << 30)
                     | (1u << 26) | (1u << MON_INTID_MAINT));
    /* No self-tick: the guest's own CNTV deadline fires the physical
     * timer PPI, and monitor_fiq injects it. A free-running EL2 tick
     * delivered before the guest arms its timer only disrupts it. */
    puts_("EL2: monitor: stage 2 default-deny, virtual GICv3, PL011 model\n");
}

static void handle_physical(uint64_t iar, int group0)
{
    uint64_t intid = iar & 0xffffff;
    if (intid == 26) {                               /* EL2's own timer: the tick source */
        mon_cnthp_rearm((uint32_t)(mon_cntfrq() / 10));
        if ((mon_lr0_read() >> 62) == 0) {
            mon_lr0_write(LR_PENDING_EOI(MON_INTID_VTIMER));
            /* Re-arm a timer PPI masked while LR0 was busy (the
             * maintenance interrupt is not used for this; UIE would
             * assert it permanently whenever no LR is valid). */
            mon_redist_write(GICR_ISENABLER0, (1u << MON_INTID_VTIMER) | (1u << 29) | (1u << 30));
        }
        mon_eoir1_write(iar);
        return;
    }
    if (intid == MON_INTID_MAINT) {
        (void)mon_misr_read();
        /* The guest has deactivated LR0's interrupt. An LR left invalid
         * with its EOI bit set keeps ICH_EISR_EL2, and so ICH_MISR_EL2.EOI,
         * set, and the level-sensitive maintenance PPI stays asserted: it
         * re-fired on every eret and starved the guest after lockdown_mode.
         * Clearing the spent LR drops the condition. A pending or active
         * LR0 is the guest's and is left alone. */
        if ((mon_lr0_read() >> 62) == 0)
            mon_lr0_write(0);
        mon_redist_write(GICR_ISENABLER0, (1u << MON_INTID_VTIMER) | (1u << 30) | (1u << 29));
    } else if (intid == MON_INTID_VTIMER || intid == 29 || intid == 30) {
        if ((mon_lr0_read() >> 62) == 0) {           /* LR0 free: inject */
            mon_lr0_write(LR_PENDING_EOI(intid));
            mon_redist_write(GICR_ICENABLER0, 1u << intid);
        } else {
            /* LR0 busy: mask the level-sensitive PPI at the redistributor
             * until the maintenance interrupt re-arms it, or it re-fires
             * at EL2 before the guest can run and starves it. */
            mon_redist_write(GICR_ICENABLER0, 1u << intid);
        }
    }
    if (intid != 1023) {
        if (group0) mon_eoir0_write(iar);
        else mon_eoir1_write(iar);
    }
}

void monitor_irq(void) { handle_physical(mon_iar1_read(), 0); }
void monitor_fiq(void) { handle_physical(mon_iar0_read(), 1); }

void monitor_print_stats(void)
{
    puts_("EL2: monitor mmio handled ");
    putdec(monitor_handled);
    puts_(" (gic "); putdec(count_gic);
    puts_(", uart "); putdec(count_uart);
    puts_(", pcie "); putdec(count_pcie);
    puts_(", rtc "); putdec(count_rtc);
    puts_(", other "); putdec(count_other);
    puts_("), unmodelled ");
    putdec(monitor_stop_count);
    puts_("\n");
}
