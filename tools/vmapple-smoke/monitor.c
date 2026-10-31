/* SPDX-License-Identifier: MIT */
#include "monitor.h"
#include "storage.h"
#include "../../probes/font5x7.h"
#include "layout.h"

uint64_t monitor_handled;
uint64_t monitor_stop_ipa;
int monitor_stop_write;
uint64_t monitor_stop_count;

static int tracing;
static int baseline_answers;
static uint64_t guest_ram_size;      /* boot_args.memSize, for the config table */
static uint64_t mailbox_requests;    /* stores absorbed at 0x004FFFF8 */
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
 * output is the serial line; on the real panel's 160-row band it is also
 * mirrored onto lines 4 to 7 so the banner and recent output are visible
 * without a UART. FR always reports transmit and receive empty of backlog:
 * the monitor drains every byte at once and has no input. */
static void uart_band_putc(char c);
static void uart_band_flush(void);

static int uart_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    count_uart++;
    if (off == 0x000) {                              /* UARTDR */
        if (write) {
            char c = (char)(*value & 0xff);
            mon_uart_putc(c);
            uart_band_putc(c);
        } else {
            *value = 0;
        }
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

/* PCIe configuration space: mediated by storage.c for VirtIO Modern block
 * storage at BDF 00:01.0; all other slots answer all-ones. */
static int pcie_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    count_pcie++;
    return storage_pcie_cfg_access(off, size, write, value);
}

/* AppleVirtualPlatform fixed addresses: QEMU vmapple defaults (OBSERVED).
 * BDIF at 0x30000000: block mailbox; ROOT 0, AUX 0x10000. Command +0x408 handled
 * by storage.c; status +0 is 1. Other writes ignored. */
static int bdif_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    uint64_t devid = off & 0xffff0000u, reg = off & 0xffffu;
    count_other++;
    if (write) {
        if (reg == 0x408) {
            if (storage_is_active()) return storage_bdif_cmd(devid, *value);
            return 0;
        }
        return 1;
    }
    switch (reg) {
    case 0x000: *value = 1; break;
    case 0x004: *value = 2; break;
    case 0x008: *value = 0x420; break;
    case 0x010: *value = 1; break;
    case 0x400: *value = 1; break;
    case 0x434: *value = 0; break;
    case 0x420:
        if (devid == 0) { *value = 0x8000000; break; }
        if (devid == 0x10000) { *value = 0x10000; break; }
        /* fall through: any other device reads all-ones */
    default: *value = (size == 8) ? ~0ull : ((1ull << (8 * size)) - 1); break;
    }
    return 1;
}

/* Configuration region at 0x400000: host-filled 64 KiB table with QEMU's
 * non-identifying defaults (version 2, serial "1234", model "VM0001", SoC
 * "Apple M1 (Virtual)"). Reads of rnd (+0x38) stop by name until backed. */
static const struct { uint32_t off; const char *text; } cfg_strings[] = {
    { 0x500, "1234" }, { 0x520, "D/A" }, { 0x540, "VM0001" }, { 0x584, "Apple M1 (Virtual)" },
};

static uint8_t cfg_byte(uint64_t off)
{
    /* Exact fields: offset, size in bytes, value. */
    const struct { uint32_t off, size; uint64_t value; } fields[] = {
        { 0x000, 4, 2 }, { 0x004, 4, 1 }, { 0x008, 4, 1 }, { 0x00c, 4, 1 },
        { 0x010, 4, 0x20 }, { 0x020, 8, guest_ram_size }, { 0x02c, 4, 1 },
        { 0x030, 4, 1 }, { 0x580, 4, 1 },
    };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (off >= fields[i].off && off < fields[i].off + fields[i].size)
            return (uint8_t)(fields[i].value >> (8 * (off - fields[i].off)));
    for (unsigned i = 0; i < sizeof(cfg_strings) / sizeof(cfg_strings[0]); i++) {
        uint64_t base = cfg_strings[i].off;
        const char *s = cfg_strings[i].text;
        unsigned n = 0;
        while (s[n]) n++;
        if (off >= base && off < base + n) return (uint8_t)s[off - base];
    }
    return 0;
}

static int cfg_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    count_other++;
    if (write) return 0;
    if (off < 0x3c && off + size > 0x38) return 0;            /* rnd */
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint64_t)cfg_byte(off + i) << (8 * i);
    *value = v;
    return 1;
}

/* The mailbox at 0x004FFFF8: the 26A428 kernel builds a 24-byte request
 * on its stack, stores the request's physical address here, and reads its
 * first halfword back, expecting the host to have answered during the
 * trapped store (local study, INFERRED). Nothing public describes it, so
 * no answer is made: the store is absorbed, logged and shown in the
 * monitor's band, and the guest reads back its own request unchanged, as
 * the Phase 0C smoke's QEMU flash did. Any other access is a named stop. */
static void band_note(unsigned line, const char *s, uint32_t color);
static int mailbox_access(uint64_t off, unsigned size, int write, uint64_t value)
{
    count_other++;
    if (!write || !((off == 0xff8 && (size == 8 || size == 4)) || (off == 0xffc && size == 4)))
        return 0;
    if (off == 0xff8 && mailbox_requests++ == 0) {
        puts_("EL2: mailbox: request at ");
        puthex(value);
        puts_(" absorbed, no host to answer it\n");
        band_note(2, "MAILBOX 4FFFF8: REQUEST ABSORBED, NO HOST", 0x00ffff40u);
    }
    return 1;
}

void monitor_set_guest(uint64_t boot_args_pa)
{
    /* boot_args.memSize (+0x18), pexpert/arm64/boot.h. Guest RAM is mapped
     * IPA = PA, and EL2 reads it with its MMU off. */
    guest_ram_size = boot_args_pa ? *(volatile uint64_t *)(uintptr_t)(boot_args_pa + 0x18) : 0;
}

/* ---- the MSI frame (the gic node's third window) -------------------------
 * The PCIe node's msi-frame-index (2) names this window as its MSI frame.
 * AppleVirtualPlatformPCIEMSIController reads one register, the type at
 * +0x8, and (local study of the 26A428 kernelcache, OBSERVED) requires bit
 * 31 set, then takes bits 12:0 and bits 28:16 as two 13-bit fields of the
 * MSI interrupt range it allocates from. Which field is the first INTID
 * and which the count is not settled by that code (INFERRED either way),
 * so both are 128: INTIDs 128 to 255 under either reading, inside the 288
 * the distributor model reports and clear of every interrupt the tree
 * names (the devices' 37 and 48 to 51, PCIe's 64 to 79). The range is this
 * platform's own choice. It is not Arm's GICv2m: GICv2m's MSI_TYPER has no
 * valid bit and failed the check (OBSERVED). No other register's meaning
 * is known, and Phase 3 has no PCIe functions to raise an MSI, so any
 * other access is a named stop. */
#define MSI_TYPE_VALID   (1u << 31)
#define MSI_RANGE_FIELD  128u

static int msi_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    count_other++;
    if (baseline_answers) {
        if (!write) *value = (size == 8) ? ~0ull : ((1ull << (8 * size)) - 1);
        return 1;
    }
    if (write || off != 0x008 || size != 4) return 0;
    *value = MSI_TYPE_VALID | (MSI_RANGE_FIELD << 16) | MSI_RANGE_FIELD;
    return 1;
}

struct window { uint64_t base, size; int kind; };
enum { K_GICD, K_GICR, K_MSI, K_UART, K_PL031, K_PL061, K_PVPANIC, K_PCIE,
       K_BDIF, K_CFG, K_MAILBOX, K_PCI_BAR };
static const struct window windows[] = {
    { MON_GICD_IPA,    0x10000,    K_GICD },
    { MON_GICR_IPA,    0x400000,   K_GICR },
    { MON_GIC_MSI_IPA, 0x10000,    K_MSI },
    { MON_UART_IPA,    0x10000,    K_UART },
    { MON_PL031_IPA,   0x1000,     K_PL031 },
    { MON_PL061_IPA,   0x1000,     K_PL061 },
    { MON_PVPANIC_IPA, 0x10000,    K_PVPANIC },
    { MON_PCIE_IPA,    MON_PCIE_SIZE, K_PCIE },
    { MON_PCIE_BAR0_BASE, MON_PCIE_BAR0_SIZE, K_PCI_BAR },
    { MON_BDIF_IPA,    0x200000,   K_BDIF },
    { MON_CFG_IPA,     0x10000,    K_CFG },
    { MON_MAILBOX_IPA, 0x1000,     K_MAILBOX },
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
        && ((insn >> 10) & 3) != 2) {                /* unscaled or pre/post-index */
        *size = 1u << (insn >> 30);
        *srt = insn & 0x1f;
        if ((insn >> 22) & 1 && (insn >> 23) & 1) *sign = 1;
        int64_t imm9 = (int32_t)((insn >> 12) & 0x1ff);
        if (imm9 & 0x100) imm9 -= 0x200;
        unsigned rn = (insn >> 5) & 0x1f;
        if (((insn >> 10) & 1) && rn != 31)
            frame[rn] += (uint64_t)imm9;             /* writeback, pre or post only */
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
#define MON_BAND_BG     0x00202020u
#define MON_BAND_TEXT   0x00ffffffu
#define MON_BAND_STOP   0x00ff5050u
#define MON_BAND_BANNER 0x0080ff80u
#define MON_BAND_UART   0x00c0ffffu
#define UART_LINE_CAP   128u
static int band_on;
static uint64_t band_fb_pa;
static unsigned band_width, band_stride, band_guest_rows, band_rows;
static char uart_cur[UART_LINE_CAP];
static unsigned uart_cur_len;
static int uart_first_set;
static char uart_tail[3][UART_LINE_CAP];
static unsigned uart_tail_count;
static int console_paging;           /* the band pages the scrollback now */
static uint32_t count_hvc;
static uint32_t last_hvc_id;
static uint32_t count_mmio;
static uint64_t last_mmio_ipa;
static unsigned timer_snapshot_count;
static unsigned panic_page;          /* scrolls a multi-line panic message */

static void band_pixel(unsigned x, unsigned y, uint32_t color)
{
    if (x >= band_width || y >= band_rows) return;
    volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)
        (band_fb_pa + (uint64_t)(band_guest_rows + y) * band_stride);
    row[x] = color;
}

/* The panel reads memory, not the CPU's caches: what the band drew must
 * be cleaned to the point of coherency before it can be seen. */
static void band_clean(void)
{
    if (band_on)
        mon_clean_range(band_fb_pa + (uint64_t)band_guest_rows * band_stride,
                        (uint64_t)band_rows * band_stride);
}

static void band_text(unsigned line, const char *s, uint32_t color)
{
    unsigned y0 = 6 + line * 20;
    for (unsigned y = y0; y < y0 + 14; y++)
        for (unsigned x = 0; x < band_width; x++)
            band_pixel(x, y, MON_BAND_BG);
    for (unsigned j = 0; s[j]; j++) {
        const uint8_t *g = font5x7_glyph(s[j]);
        unsigned x0 = 8 + j * 12;
        for (unsigned row = 0; row < 7; row++)
            for (unsigned bit = 0; bit < 5; bit++)
                if (g[row] & (0x10 >> bit))
                    for (unsigned d = 0; d < 4; d++)
                        band_pixel(x0 + bit * 2 + (d & 1), y0 + row * 2 + (d >> 1), color);
    }
}

void monitor_set_band(int on, uint64_t fb_pa, unsigned width, unsigned stride,
                      unsigned guest_rows, unsigned rows)
{
    band_on = on;
    band_fb_pa = fb_pa;
    band_width = width;
    band_stride = stride;
    band_guest_rows = guest_rows;
    band_rows = rows;
    uart_cur_len = 0;
    uart_first_set = 0;
    uart_tail_count = 0;
    count_hvc = 0;
    last_hvc_id = 0;
    count_mmio = 0;
    last_mmio_ipa = 0;
    timer_snapshot_count = 0;
    panic_page = 0;
    console_paging = 0;
    console_scroll_reset();
    if (!on) return;
    for (unsigned y = 0; y < band_rows; y++)
        for (unsigned x = 0; x < band_width; x++) band_pixel(x, y, MON_BAND_BG);
    band_text(0, "SNAPINTOSH MONITOR: STAGE 2 DEFAULT-DENY", MON_BAND_TEXT);
    band_clean();
}

static void band_clean_line(unsigned line)
{
    if (!band_on) return;
    unsigned y0 = line * 20;
    mon_clean_range(band_fb_pa + ((uint64_t)band_guest_rows + y0) * band_stride,
                    20ull * band_stride);
}

static void band_note_line(unsigned line, const char *s, uint32_t color)
{
    if (!band_on) return;
    band_text(line, s, color);
    band_clean_line(line);
}

static void band_note(unsigned line, const char *s, uint32_t color)
{
    if (!band_on) return;
    band_text(line, s, color);
    band_clean();
}

static void append_str(char *buf, unsigned *pos, const char *s)
{
    while (*s && *pos < 63) buf[(*pos)++] = *s++;
}

static void append_hex(char *buf, unsigned *pos, uint64_t val, int nibbles)
{
    for (int i = nibbles - 1; i >= 0 && *pos < 63; i--)
        buf[(*pos)++] = "0123456789ABCDEF"[(val >> (4 * i)) & 15];
}

static void append_dec(char *buf, unsigned *pos, uint32_t val)
{
    char tmp[10];
    int n = 0;
    if (val == 0) {
        if (*pos < 63) buf[(*pos)++] = '0';
        return;
    }
    while (val > 0 && n < 10) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (int i = n - 1; i >= 0 && *pos < 63; i--)
        buf[(*pos)++] = tmp[i];
}

static void update_line4_telemetry(void)
{
    if (!band_on || band_rows < 160 || uart_first_set) return;
    char line[64];
    unsigned k = 0;
    append_str(line, &k, "HVC: ");
    append_dec(line, &k, count_hvc);
    append_str(line, &k, " ID ");
    append_hex(line, &k, last_hvc_id, 8);
    append_str(line, &k, "  MMIO: ");
    append_dec(line, &k, count_mmio);
    append_str(line, &k, " IPA ");
    append_hex(line, &k, last_mmio_ipa, 8);
    line[k] = 0;
    band_note_line(4, line, 0x00ffff40u);
}

#ifdef VMAPPLE_GAOKUN3_MONITOR
static void update_line1_storage(void)
{
    if (!band_on || monitor_stop_count) return;
    char sdiag[96];
    if (storage_render_diag(sdiag, sizeof(sdiag)))
        band_note_line(1, sdiag, 0x00c0ffffu);
}
#endif

void monitor_start_telemetry(void)
{
    count_hvc = 0;
    last_hvc_id = 0;
    count_mmio = 0;
    last_mmio_ipa = 0;
    timer_snapshot_count = 0;
    panic_page = 0;
    uint64_t frq = mon_cntfrq();
    if (!frq) frq = 19200000;
    uint32_t ticks = (uint32_t)(frq + frq / 2);
    mon_cnthp_rearm(ticks);
    update_line4_telemetry();
}

void monitor_telemetry_hvc(uint32_t fn_id)
{
    count_hvc++;
    last_hvc_id = fn_id;
    update_line4_telemetry();
}

void monitor_timer_snapshot(void)
{
    timer_snapshot_count++;

    uint64_t elr_el2 = mon_elr_el2();
    uint64_t spsr_el2 = mon_spsr_el2();
    uint64_t esr_el1 = mon_esr_el1();
    uint64_t far_el1 = mon_far_el1();
    uint64_t elr_el1 = mon_elr_el1();
    uint64_t sctlr_el1 = mon_sctlr_el1();

    uint64_t params = mon_params_base();
    uint64_t phys_base = 0;
    uint32_t panic_reason = 0;
    uint64_t panic_str_va = 0;
    uint64_t panic_args_va = 0;
    uint64_t panic_caller_va = 0;
    int is_panic = 0;

    /* XNU debugger_state (+0x08 op, +0x18 str, +0x20 args, +0x30 caller).
     * phys_base derives from kernelcache entry fixed offset 0x2eb0480. */
    if (params) {
        uint64_t entry_pa = *(const volatile uint64_t *)(params + PARAM_ENTRY);
        if (entry_pa > 0x2eb0480ull) {
            phys_base = entry_pa - 0x2eb0480ull;
            uint64_t dbg_state_pa = phys_base + 0x3b84140ull;
            mon_clean_range(dbg_state_pa, 64);
            panic_reason = *(const volatile uint32_t *)(dbg_state_pa + 0x08);
            panic_str_va = *(const volatile uint64_t *)(dbg_state_pa + 0x18);
            panic_args_va = *(const volatile uint64_t *)(dbg_state_pa + 0x20);
            panic_caller_va = *(const volatile uint64_t *)(dbg_state_pa + 0x30);
        }
    }

    if (panic_reason != 0 || panic_str_va != 0 ||
        elr_el1 == 0xfffffe0009f0abc8ull || elr_el2 == 0xfffffe0009eb2758ull) {
        is_panic = 1;
    }

    if (tracing && (timer_snapshot_count == 1 || (timer_snapshot_count % 10) == 0)) {
        puts_("EL2: snap ");
        putdec(timer_snapshot_count);
        puts_(" PC ");
        puthex(elr_el2);
        puts_(" SPSR ");
        puthex(spsr_el2);
        puts_(" ESR_EL1 ");
        puthex(esr_el1);
        puts_(" ELR_EL1 ");
        puthex(elr_el1);
        if (is_panic) {
            puts_(" PANIC caller ");
            puthex(panic_caller_va);
            puts_(" str ");
            puthex(panic_str_va);
        }
        puts_("\n");
    }

    if (!band_on || band_rows < 160) return;

#ifdef VMAPPLE_GAOKUN3_MONITOR
    update_line1_storage();
    struct storage_diag sd;
    storage_get_diag(&sd);
    int is_user = ((spsr_el2 & 0x0fu) == 0 ||
                   (elr_el2 >= 0x100000000ull && elr_el2 < 0xffff000000000000ull) ||
                   (elr_el1 >= 0x100000000ull && elr_el1 < 0xffff000000000000ull));
    if (is_user || sd.vq_requests >= 65) {
        band_note_line(2, "USERSPACE: PID 1 RUNNING (/sbin/launchd)", 0x00ffff40u);
        char l3[64];
        unsigned k = 0;
        if (is_user) {
            append_str(l3, &k, "EL0 PC ");
            uint64_t user_pc = ((spsr_el2 & 0x0fu) == 0) ? elr_el2 : elr_el1;
            append_hex(l3, &k, user_pc, 16);
            append_str(l3, &k, " SPSR ");
            append_hex(l3, &k, spsr_el2, 8);
        } else {
            append_str(l3, &k, "INIT LOADED: VQ=");
            append_dec(l3, &k, sd.vq_requests);
            append_str(l3, &k, " BAR0=");
            append_dec(l3, &k, sd.bar0_accesses);
        }
        l3[k] = 0;
        band_note_line(3, l3, 0x00ffff40u);
    }
#endif

    /* Once the guest's console prints, the band is its mirror and the
     * register snapshot stays off it; a panic then pages the console's own
     * panic log back (run 42 lost it to the three-line window). */
    if (uart_first_set) {
        if (is_panic) {
            unsigned pages = monitor_console_page(panic_page, "GUEST PANICKED: CONSOLE");
            panic_page = pages ? (panic_page + 1) % pages : 0;
        }
        return;
    }

    if (is_panic) {
        char l4[64], l5[64];
        unsigned k;

        k = 0;
        append_str(l4, &k, "PANIC C ");
        append_hex(l4, &k, panic_caller_va, 16);
        append_str(l4, &k, " FAR ");
        append_hex(l4, &k, far_el1, 16);
        l4[k] = 0;
        band_text(4, l4, 0x0080ff80u);

        k = 0;
        append_str(l5, &k, "ESR ");
        append_hex(l5, &k, esr_el1, 8);
        append_str(l5, &k, " R ");
        append_dec(l5, &k, panic_reason);
        append_str(l5, &k, " PC ");
        append_hex(l5, &k, elr_el2, 16);
        l5[k] = 0;
        band_text(5, l5, 0x0080ff80u);

        /* The message itself: format the guest's panic string and its
         * varargs, two band lines at a time, scrolling one page per
         * snapshot so a message longer than two lines can be read. */
        char lines[2][PANIC_LINE_CAP];
        unsigned total = 0;
        const unsigned width = 44;      /* chars per band line at this scale */
        if (phys_base != 0 && panic_str_va != 0)
            total = panic_render(lines, 2, width, panic_page, panic_str_va, panic_args_va);
        if (!total) {
            /* Nothing formatted (no guest RAM, or unreadable): show the
             * raw format-string pointer rather than inventing text. */
            k = 0;
            append_str(lines[0], &k, "STR VA ");
            append_hex(lines[0], &k, panic_str_va, 16);
            lines[0][k] = 0;
            lines[1][0] = 0;
        }
        band_text(6, lines[0], 0x0080ff80u);
        band_text(7, lines[1], 0x0080ff80u);

        unsigned per_page = 2 * width;
        unsigned pages = total ? (total + per_page - 1) / per_page : 1;
        panic_page = (panic_page + 1) % pages;

        mon_clean_range(band_fb_pa + ((uint64_t)band_guest_rows + 4 * 20) * band_stride,
                        4ull * 20 * band_stride);
    } else {
        char l5[64], l6[64], l7[64];
        unsigned k;

        k = 0;
        append_str(l5, &k, "SNAP ");
        append_dec(l5, &k, timer_snapshot_count);
        append_str(l5, &k, " PC ");
        append_hex(l5, &k, elr_el2, 16);
        append_str(l5, &k, " SPSR ");
        append_hex(l5, &k, spsr_el2, 8);
        l5[k] = 0;
        band_text(5, l5, 0x00ffa040u);

        k = 0;
        append_str(l6, &k, "ESR_EL1 ");
        append_hex(l6, &k, esr_el1, 8);
        append_str(l6, &k, " FAR_EL1 ");
        append_hex(l6, &k, far_el1, 16);
        l6[k] = 0;
        band_text(6, l6, 0x00ffa040u);

        k = 0;
        append_str(l7, &k, "ELR_EL1 ");
        append_hex(l7, &k, elr_el1, 16);
        append_str(l7, &k, " SCTLR ");
        append_hex(l7, &k, sctlr_el1, 8);
        l7[k] = 0;
        band_text(7, l7, 0x00ffa040u);

        mon_clean_range(band_fb_pa + ((uint64_t)band_guest_rows + 5 * 20) * band_stride,
                        3ull * 20 * band_stride);
    }
}

/* On the real panel's 160-row band (8 text rows 0..7), mirror guest PL011
 * output onto lines 4..7: line 4 pins the first non-empty line (the kernel
 * banner), and lines 5..7 show a rolling 3-line window of subsequent output.
 * QEMU's 88-row band has only 4 lines (0..3) and is untouched. */
static void uart_copy_line(char *dst, const char *src)
{
    unsigned i = 0;
    while (src[i] && i < UART_LINE_CAP - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void uart_band_commit(const char *s)
{
    if (!s[0]) return;
    console_scroll_add(s);
#ifdef VMAPPLE_GAOKUN3_MONITOR
    for (const char *p = s; *p; p++) {
        if (p[0] == 'm' && p[1] == 'o' && p[2] == 'u' && p[3] == 'n' &&
            p[4] == 't' && p[5] == '-' && p[6] == 'c') {
            band_note_line(2, "APFS MOUNT: COMPLETE (volume TestRoot)", 0x0080ff80u);
            break;
        }
        if (p[0] == 'U' && p[1] == 'S' && p[2] == 'E' && p[3] == 'R' &&
            p[4] == 'S' && p[5] == 'P' && p[6] == 'A' && p[7] == 'C' && p[8] == 'E') {
            band_note_line(2, "USERSPACE: PID 1 RUNNING (/sbin/launchd)", 0x00ffff40u);
            break;
        }
    }
#endif
    if (console_paging) return;
    if (!uart_first_set) {
        uart_first_set = 1;
        band_note(4, s, MON_BAND_BANNER);
        return;
    }
    if (uart_tail_count < 3) {
        unsigned idx = uart_tail_count++;
        uart_copy_line(uart_tail[idx], s);
        band_note(5 + idx, uart_tail[idx], MON_BAND_UART);
        return;
    }
    uart_copy_line(uart_tail[0], uart_tail[1]);
    uart_copy_line(uart_tail[1], uart_tail[2]);
    uart_copy_line(uart_tail[2], s);
    band_text(5, uart_tail[0], MON_BAND_UART);
    band_text(6, uart_tail[1], MON_BAND_UART);
    band_text(7, uart_tail[2], MON_BAND_UART);
    band_clean();
}

static void uart_band_flush(void)
{
    if (!band_on || band_rows < 160 || !uart_cur_len) return;
    uart_cur[uart_cur_len] = 0;
    uart_cur_len = 0;
    uart_band_commit(uart_cur);
}

static void uart_band_putc(char c)
{
    if (!band_on || band_rows < 160 || c == '\r') return;
    if (c == '\n') {
        uart_band_flush();
        return;
    }
    if (c == '\t') c = ' ';
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
        if (uart_cur_len == UART_LINE_CAP - 1) uart_band_flush();   /* wrap, not cut */
        uart_cur[uart_cur_len++] = c;
    }
}

unsigned monitor_console_page(unsigned page, const char *what)
{
    if (!band_on || band_rows < 160) return 0;
    uart_band_flush();
    const char *rows[4];
    unsigned pages = console_scroll_page(rows, 4, page);
    char head[64];
    unsigned k = 0;
    append_str(head, &k, what);
    append_str(head, &k, " PAGE ");
    append_dec(head, &k, pages ? page % pages + 1 : 0);
    append_str(head, &k, "/");
    append_dec(head, &k, pages);
    head[k] = 0;
    console_paging = 1;
    band_text(2, head, 0x00ffff40u);
    for (unsigned r = 0; r < 4; r++) band_text(4 + r, rows[r], MON_BAND_UART);
    band_clean();
    return pages;
}

void monitor_halt_report(uint32_t psci_fn)
{
    if (!band_on || band_rows < 160) return;
    const char *what = psci_fn == PSCI_SYSTEM_RESET ? "PSCI SYSTEM_RESET: CONSOLE"
                       : psci_fn == PSCI_SYSTEM_OFF ? "PSCI SYSTEM_OFF: CONSOLE"
                       : "GUEST STOPPED: CONSOLE";
    uint64_t frq = mon_cntfrq();
    if (!frq) frq = 19200000;
    for (unsigned page = 0;; page++) {
        monitor_console_page(page, what);
        uint64_t t0 = mon_cntpct();
        while (mon_cntpct() - t0 < frq) { }
    }
}

static void band_stop(uint64_t ipa, int write)
{
    if (!band_on) return;
    uart_band_flush();
    char line[48] = "STOP: UNMODELLED IPA ";
    unsigned n = 21;
    for (int i = 15; i >= 0; i--) line[n++] = "0123456789ABCDEF"[(ipa >> (4 * i)) & 15];
    const char *what = write ? " WRITE" : " READ";
    for (unsigned i = 0; what[i]; i++) line[n++] = what[i];
    line[n] = 0;
    band_text(1, line, MON_BAND_STOP);
    band_clean();
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
        case K_MSI:  handled = msi_access(off, size, write, &value); break;
        case K_UART: handled = uart_access(off, size, write, &value); break;
        case K_PL031: handled = pl031_access(off, size, write, &value); break;
        case K_PL061: handled = store_access(pl061_regs, off, size, write, &value); break;
        case K_PVPANIC: handled = store_access(pvpanic_regs, off, size, write, &value); break;
        case K_PCIE: handled = pcie_access(off, size, write, &value); break;
        case K_PCI_BAR: handled = storage_bar0_access(off, size, write, &value); break;
        case K_BDIF: handled = bdif_access(off, size, write, &value); break;
        case K_CFG: handled = cfg_access(off, size, write, &value); break;
        case K_MAILBOX: handled = mailbox_access(off, size, write, value); break;
        default: handled = 0; break;
        }
        if (!handled) goto stop;
        monitor_handled++;
        count_mmio++;
        last_mmio_ipa = far;
        update_line4_telemetry();
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
    count_mmio++;
    last_mmio_ipa = far;
    update_line4_telemetry();
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
/* The boot CPU's redistributor RD_base, found by monitor_init. */
static uint64_t phys_gicr_rd = PHYS_GICR_BASE;
#define GICR_WAKER       (phys_gicr_rd + 0x0014)
#define GICR_IGROUPR0    (phys_gicr_rd + 0x10000 + 0x0080)
#define GICR_ISENABLER0  (phys_gicr_rd + 0x10000 + 0x0100)
#define GICR_ICENABLER0  (phys_gicr_rd + 0x10000 + 0x0180)
/* The physical PPIs the monitor enables on the real redistributor, and
 * nothing else (Phase 3 decision 4's write list, docs/GAOKUN3-PHASE3.md):
 *   25 (maintenance) re-arms the guest timer after the guest deactivates it;
 *   27 (virtual timer) is the guest's own CNTV deadline, injected to EL1;
 *   26 (EL2 physical timer, CNTHP) fires the ~1 s diagnostic snapshot that
 *      reads EL1 state and the panic message into the band. PPI 26 was added
 *      to decision 4's list when the snapshot was introduced (run 36). */
#define GUEST_TIMER_PPIS (1u << MON_INTID_VTIMER)
#define MONITOR_PPIS     ((1u << MON_INTID_MAINT) | (1u << MON_INTID_HYPTIMER) | GUEST_TIMER_PPIS)
/* GICv3 (IHI 0069) GICD_CTLR, as EL2 sees it: DS (bit 6) reads one only
 * with a single security state, as QEMU's GIC has; there bit 0 enables
 * Group 0 and bit 1 Group 1. With two security states the Non-secure view
 * has ARE_NS at bit 4 (set by the firmware; clearing it is UNPREDICTABLE)
 * and EnableGrp1A at bit 1. RWP (bit 31) is read-only: write pending. */
#define GICD_CTLR_DS     (1u << 6)
#define GICD_CTLR_ARE_NS (1u << 4)
#define GICD_CTLR_RWP    (1u << 31)
#define GICR_LAST        (1ull << 4)        /* GICR_TYPER: last frame */
#define GICR_FRAME       0x20000ull
#define GICR_MAX_FRAMES  8                  /* the gaokun3's region: 1 MiB */
static struct {
    int stopped;                            /* monitor_init stopped by name */
    const char *why;
    uint32_t ctlr_before, ctlr_after;
    unsigned frame;
    int ds;
} gic_state;
/* Group 0: on this platform the timer reaches the guest as an FIQ (the
 * Phase 0C boot took FIQs at EL1, not IRQs), so the virtual timer is
 * delivered in group 0 and the guest acknowledges it at ICC_IAR0_EL1. */
#define LR_PENDING_EOI(vintid) \
    ((1ull << 62) | (0xa0ull << 48) | (1ull << 41) | (uint64_t)(vintid))

void monitor_set_policy(int trace)
{
    tracing = trace;
}

void monitor_set_baseline_answers(int on) { baseline_answers = on; }

static int gic_stop(const char *why)
{
    gic_state.stopped = 1;
    gic_state.why = why;
    puts_("EL2: stop: ");
    puts_(why);
    puts_("\n");
    return 0;
}

/* Bounded wait for a register bit to clear (RWP, ChildrenAsleep). */
static int gic_wait_clear(uint64_t addr, uint32_t bit)
{
    for (unsigned i = 0; i < 1000000; i++)
        if (!(mon_mmio_read32(addr) & bit)) return 1;
    return 0;
}

/* Every write here is read-modify-write or write-1-to-set, and nothing is
 * written until the boot CPU's redistributor is found and the distributor
 * is in the state the firmware should have left it in. Returns 0 for a
 * named stop; monitor_gic_report shows the outcome in the band. */
int monitor_init(int trace)
{
    monitor_set_policy(trace);
    gic_state.stopped = 0;
    storage_init_params(mon_params_base());

    /* The boot CPU's redistributor: the frame whose GICR_TYPER affinity
     * matches MPIDR_EL1 (both read only), not an assumed first frame. */
    uint64_t mpidr = mon_mpidr();
    uint64_t aff = (mpidr & 0xffffffull) | ((mpidr >> 32 & 0xff) << 24);
    unsigned frame = 0;
    for (;; frame++) {
        if (frame == GICR_MAX_FRAMES) return gic_stop("no redistributor matches the boot CPU");
        uint64_t typer = mon_mmio_read64(PHYS_GICR_BASE + frame * GICR_FRAME + 0x8);
        if ((typer >> 32) == aff) break;
        if (typer & GICR_LAST) return gic_stop("no redistributor matches the boot CPU");
    }
    phys_gicr_rd = PHYS_GICR_BASE + frame * GICR_FRAME;
    gic_state.frame = frame;

    /* The distributor: keep every bit the firmware set, ARE_NS above all,
     * and add only the group enable this side of it controls. The guest's
     * own distributor writes go to the model, so without this a QEMU GIC
     * at reset forwards nothing. */
    uint32_t ctlr = mon_mmio_read32(PHYS_GICD_BASE);
    gic_state.ctlr_before = ctlr;
    gic_state.ds = (ctlr & GICD_CTLR_DS) != 0;
    uint32_t want;
    if (gic_state.ds) {
        want = ctlr | 0x3;                           /* EnableGrp0 | EnableGrp1 */
    } else {
        if (!(ctlr & GICD_CTLR_ARE_NS)) return gic_stop("GICD_CTLR ARE_NS is clear");
        want = ctlr | (1u << 1);                     /* EnableGrp1A */
    }
    want &= ~GICD_CTLR_RWP;
    if (want != (ctlr & ~GICD_CTLR_RWP)) {
        mon_redist_write(PHYS_GICD_BASE, want);
        if (!gic_wait_clear(PHYS_GICD_BASE, GICD_CTLR_RWP))
            return gic_stop("GICD_CTLR write still pending");
    }
    gic_state.ctlr_after = mon_mmio_read32(PHYS_GICD_BASE);

    /* Wake the redistributor only if it sleeps (ProcessorSleep, bit 1),
     * and wait for ChildrenAsleep (bit 2) to clear. With two security
     * states the Non-secure view reads zero and nothing is written. */
    uint32_t waker = mon_mmio_read32(GICR_WAKER);
    if (waker & 2u) {
        mon_redist_write(GICR_WAKER, waker & ~2u);
        if (!gic_wait_clear(GICR_WAKER, 4u)) return gic_stop("redistributor did not wake");
    }

    /* QEMU's PPIs reset to Group 0 and its GIC has one security state:
     * move the maintenance PPI to Group 1, or the physical interface never
     * signals EL2 with it. With two security states the groups are the
     * secure firmware's, not EL2's, and this register is not touched. */
    if (gic_state.ds) {
        uint32_t groups = mon_mmio_read32(GICR_IGROUPR0);
        uint32_t want = (1u << MON_INTID_MAINT) | (1u << MON_INTID_HYPTIMER);
        if ((groups & want) != want)
            mon_redist_write(GICR_IGROUPR0, groups | want);
    }
    mon_redist_write(GICR_ISENABLER0, MONITOR_PPIS);  /* write-1-to-set */
    /* No self-tick: the guest's own CNTV deadline fires the physical
     * timer PPI, and the handler injects it. A free-running EL2 tick
     * delivered before the guest arms its timer only disrupts it. */
    puts_("EL2: gic: ctlr ");
    puthex(gic_state.ctlr_before);
    puts_(" -> ");
    puthex(gic_state.ctlr_after);
    puts_(", redistributor frame ");
    putdec(frame);
    puts_("\n");
    puts_("EL2: monitor: stage 2 default-deny, virtual GICv3, PL011 model\n");
    return 1;
}

/* After the band is drawn: show what monitor_init read of the real GIC,
 * or why it stopped. Returns 0 when monitor_init stopped (the caller
 * halts there). Line 3: "GIC CTLR XXXXXXXX -> XXXXXXXX  RD FRAME n". */
void monitor_note(const char *s)
{
    band_note(2, s, 0x00ffff40u);
}

void monitor_fault(uint64_t vec, uint64_t esr, uint64_t elr)
{
    if (!band_on || monitor_stop_count) return;
    uart_band_flush();
    char line[64] = "FAULT ";
    unsigned k = 6;
    for (int i = 2; i >= 0; i--) line[k++] = "0123456789ABCDEF"[(vec >> (4 * i)) & 15];
    const char *s1 = " ESR ";
    for (unsigned i = 0; s1[i]; i++) line[k++] = s1[i];
    for (int i = 7; i >= 0; i--) line[k++] = "0123456789ABCDEF"[(esr >> (4 * i)) & 15];
    const char *s2 = " ELR ";
    for (unsigned i = 0; s2[i]; i++) line[k++] = s2[i];
    for (int i = 15; i >= 0; i--) line[k++] = "0123456789ABCDEF"[(elr >> (4 * i)) & 15];
    line[k] = 0;
    band_note(1, line, MON_BAND_STOP);
}

int monitor_gic_report(void)
{
    if (gic_state.stopped) {
        char line[64] = "STOP: ";
        unsigned k = 6;
        for (unsigned i = 0; gic_state.why[i] && k < 63; i++) line[k++] = gic_state.why[i];
        line[k] = 0;
        band_note(1, line, 0x00ff5050u);
        return 0;
    }
    char line[48] = "GIC CTLR ";
    unsigned k = 9;
    for (int i = 7; i >= 0; i--) line[k++] = "0123456789ABCDEF"[(gic_state.ctlr_before >> (4 * i)) & 15];
    const char *arrow = " -> ";
    for (unsigned i = 0; arrow[i]; i++) line[k++] = arrow[i];
    for (int i = 7; i >= 0; i--) line[k++] = "0123456789ABCDEF"[(gic_state.ctlr_after >> (4 * i)) & 15];
    const char *rd = "  RD FRAME ";
    for (unsigned i = 0; rd[i]; i++) line[k++] = rd[i];
    line[k++] = (char)('0' + gic_state.frame % 10);
    line[k] = 0;
    band_note(3, line, 0x00ffffffu);
#ifdef VMAPPLE_GAOKUN3_MONITOR
    update_line1_storage();
#endif
    return 1;
}

static void handle_physical(uint64_t iar, int group0)
{
    uint64_t intid = iar & 0xffffff;
    if (intid == MON_INTID_HYPTIMER) {               /* EL2 physical timer snapshot */
        /* Kept running after the console starts: the snapshot then draws
         * only when the guest has panicked. */
        uint64_t frq = mon_cntfrq();
        if (!frq) frq = 19200000;
        mon_cnthp_rearm((uint32_t)frq);
        monitor_timer_snapshot();
        if (group0) mon_eoir0_write(iar);
        else mon_eoir1_write(iar);
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
        mon_redist_write(GICR_ISENABLER0, GUEST_TIMER_PPIS);
    } else if (intid == MON_INTID_VTIMER) {
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
/* ---- trapped system registers ---------------------------------------------
 * With HCR_EL2.IMO and FMO set, the guest's writes to the GIC's SGI
 * registers trap to EL2 (exception class 0x18), alongside the Apple
 * implementation-defined registers HCR_EL2.TIDCP traps. Only the
 * implementation-defined space (op0 3, CRn 11 or 15) is the stub's
 * read-as-zero, write-ignored; an architectural register silently ignored
 * would be a fabricated success. ICC_SGI1R_EL1 is served; anything else is
 * a named stop. */

/* A Group 1 software interrupt for the guest's one CPU, in list registers
 * 1 to 3 (LR0 carries the timer). A purely virtual interrupt needs no EOI
 * maintenance: the LR goes invalid when the guest deactivates it. */
#define LR_PENDING_G1(vintid) \
    ((1ull << 62) | (1ull << 60) | (0xa0ull << 48) | (uint64_t)(vintid))

int monitor_inject_virq(unsigned intid)
{
    unsigned free = 0;
    for (unsigned n = 1; n <= 3; n++) {
        uint64_t lr = mon_lr_read(n);
        if ((lr >> 62) == 0) { if (!free) free = n; continue; }
        if ((lr & 0xffffffffull) == intid && (lr >> 62) == 1) return 1;  /* already pending */
    }
    if (!free) return 0;
    mon_lr_write(free, LR_PENDING_G1(intid));
    return 1;
}

static int inject_sgi(unsigned intid)
{
    if (monitor_inject_virq(intid)) return 1;
    char line[48] = "STOP: NO FREE LIST REGISTER FOR SGI ";
    line[36] = (char)('0' + intid / 10);
    line[37] = (char)('0' + intid % 10);
    line[38] = 0;
    puts_("EL2: stop: no free list register for software interrupt ");
    putdec(intid);
    puts_("\n");
    band_note(1, line, 0x00ff5050u);
    return 0;
}

/* ICC_SGI1R_EL1 (GICv3 architecture): TargetList [15:0], Aff1 [23:16],
 * INTID [27:24], Aff2 [39:32], IRM [40], RS [47:44], Aff3 [55:48]. The
 * guest has one CPU at affinity 0.0.0.0. A software interrupt for a CPU
 * that does not exist is discarded, as the architecture discards one for
 * an absent PE; IRM 1 means every CPU but the sender, of which there is
 * none. */
static int sgi1r_write(uint64_t value)
{
    unsigned intid = (unsigned)(value >> 24) & 15;
    int irm = (int)(value >> 40) & 1;
    int self = !irm && !(value & 0x00ff00ff00ff0000ull) && !((value >> 44) & 15) && (value & 1);
    return self ? inject_sgi(intid) : 1;
}

int monitor_sysreg(uint64_t esr, uint64_t *frame)
{
    unsigned op0 = (unsigned)(esr >> 20) & 3, op2 = (unsigned)(esr >> 17) & 7;
    unsigned op1 = (unsigned)(esr >> 14) & 7, crn = (unsigned)(esr >> 10) & 15;
    unsigned rt = (unsigned)(esr >> 5) & 31, crm = (unsigned)(esr >> 1) & 15;
    int read = (int)(esr & 1);
    if ((op0 == 1 || op0 == 3) && (crn == 11 || crn == 15)) return 2;   /* IMPDEF space */
    if (op0 == 3 && op1 == 0 && crn == 12 && crm == 11 && op2 == 5 && !read)
        return sgi1r_write(rt == 31 ? 0 : frame[rt]);           /* ICC_SGI1R_EL1 */
    if (op0 == 1 && crn == 8) {
        /* TLBI maintenance instruction trapped via HCR_EL2.TTLB. Cortex-A78C
         * implements FEAT_TLBIOS (TLB=1) but lacks FEAT_TLBIRANGE (TLB=2), so
         * range invalidations like TLBI RVALE1IS (D50882A8) would otherwise
         * fault as Undefined Instruction. Satisfy any EL1 TLBI by invalidating
         * all EL1 stage-1 translations. */
        mon_tlbi_vmalle1is();
        return 1;
    }
    /* "s<op0>_<op1>_c<crn>_c<crm>_<op2>", the assembler's generic name. */
    char name[24];
    unsigned n = 0;
    const unsigned field[5] = { op0, op1, crn, crm, op2 };
    for (unsigned i = 0; i < 5; i++) {
        name[n++] = i == 0 ? 's' : '_';
        if (i == 2 || i == 3) name[n++] = 'c';
        if (field[i] >= 10) name[n++] = (char)('0' + field[i] / 10);
        name[n++] = (char)('0' + field[i] % 10);
    }
    name[n] = 0;
    puts_("EL2: stop: unmodelled system register ");
    puts_(name);
    puts_(read ? " read\n" : " write\n");
    char line[64] = "STOP: SYSTEM REGISTER ";
    unsigned k = 22;
    for (unsigned i = 0; name[i]; i++) line[k++] = name[i];
    const char *dir = read ? " READ" : " WRITE";
    for (unsigned i = 0; dir[i]; i++) line[k++] = dir[i];
    line[k] = 0;
    band_note(1, line, 0x00ff5050u);
    return 0;
}

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
