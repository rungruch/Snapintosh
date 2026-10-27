/* SPDX-License-Identifier: MIT */
/* See usbprobe.h. */
#include "usbprobe.h"

#define GCC_BASE      0x00100000ull
#define USB0_CORE     0x0a600000ull
#define USB0_QSCRATCH 0x0a6f8800ull

const uint64_t el2q_usb_gcc_address[EL2Q_USB_GCC_COUNT] = {
    [EL2Q_USB_GDSC] = GCC_BASE + 0xf004,
    [EL2Q_USB_MASTER] = GCC_BASE + 0xf010,
    [EL2Q_USB_SLEEP] = GCC_BASE + 0xf018,
    [EL2Q_USB_UTMI] = GCC_BASE + 0xf01c,
    [EL2Q_USB_AXI] = GCC_BASE + 0xf080,
    [EL2Q_USB_CNOC] = GCC_BASE + 0xf084,
    [EL2Q_USB1_GDSC] = GCC_BASE + 0x10004,
    [EL2Q_USB1_MASTER] = GCC_BASE + 0x10010,
};

const uint64_t el2q_usb_word_address[EL2Q_USB_WORD_COUNT] = {
    [EL2Q_USB_XCAP] = USB0_CORE + 0x0,
    [EL2Q_USB_XHCS] = USB0_CORE + 0x4,
    [EL2Q_USB_SNPSID] = USB0_CORE + 0xc120,
    [EL2Q_USB_VER] = USB0_CORE + 0xc1a0,
    [EL2Q_USB_GCTL] = USB0_CORE + 0xc110,
    [EL2Q_USB_U2PHY] = USB0_CORE + 0xc200,
    [EL2Q_USB_U3PIPE] = USB0_CORE + 0xc2c0,
    [EL2Q_USB_DCFG] = USB0_CORE + 0xc700,
    [EL2Q_USB_DCTL] = USB0_CORE + 0xc704,
    [EL2Q_USB_EVNT] = USB0_CORE + 0xc400,
    [EL2Q_USB_QS_HS] = USB0_QSCRATCH + 0x10,
    [EL2Q_USB_QS_SS] = USB0_QSCRATCH + 0x30,
};

#define PWR_ON  (1u << 31)    /* GDSC register */
#define CLK_OFF (1u << 31)    /* clock branch (CBCR) register */

/* One word: mapping first, then the read. The first refusal or fault is
 * the one recorded. */
static uint32_t read_word(const struct el2q_usb_hw *hw, uint64_t address, uint32_t *value,
                          uint64_t *esr, uint64_t *at)
{
    if (hw->mapped(hw->ctx, address)) {
        if (!*at) *at = address;
        return EL2Q_USB_UNMAPPED;
    }
    uint64_t word = 0, fault = hw->read32(hw->ctx, address, &word);
    if (fault) {
        if (!*esr) { *esr = fault; *at = address; }
        return EL2Q_USB_FAULT;
    }
    *value = (uint32_t)word;
    return EL2Q_USB_READ_OK;
}

void el2q_usb_gcc(struct el2q_usb_phase *p, const struct el2q_usb_hw *hw)
{
    *p = (struct el2q_usb_phase){.ran = 1};
    if (!hw || !hw->mapped || !hw->read32) return;
    for (unsigned i = 0; i < EL2Q_USB_GCC_COUNT; i++)
        p->gcc_read[i] = read_word(hw, el2q_usb_gcc_address[i], &p->gcc[i], &p->esr, &p->at);
    /* The register port of the core needs the power domain, the master clock
     * and the two AXI clocks it is reached through. Sleep and mock UTMI feed
     * the link and PHY side, and are reported but not required to read. */
    static const unsigned needed[] = {EL2Q_USB_GDSC, EL2Q_USB_MASTER, EL2Q_USB_AXI, EL2Q_USB_CNOC};
    for (unsigned i = 0; i < sizeof needed / sizeof needed[0]; i++)
        if (p->gcc_read[needed[i]] != EL2Q_USB_READ_OK) return;
    if (!(p->gcc[EL2Q_USB_GDSC] & PWR_ON)) p->power = EL2Q_USB_POWER_GDSC_OFF;
    else if ((p->gcc[EL2Q_USB_MASTER] | p->gcc[EL2Q_USB_AXI] | p->gcc[EL2Q_USB_CNOC]) & CLK_OFF)
        p->power = EL2Q_USB_POWER_CLOCKS_OFF;
    else
        p->power = EL2Q_USB_POWER_ON;
}

void el2q_usb_core(struct el2q_usb_phase *p, const struct el2q_usb_hw *hw)
{
    p->core = EL2Q_USB_CORE_NOT_TRIED;
    if (!hw || !hw->mapped || !hw->read32 || p->power != EL2Q_USB_POWER_ON) return;
    /* Every word first: one unmapped word means the firmware's tables do not
     * describe the controller as expected, so none is read. */
    for (unsigned i = 0; i < EL2Q_USB_WORD_COUNT; i++)
        if (hw->mapped(hw->ctx, el2q_usb_word_address[i])) {
            p->core = EL2Q_USB_CORE_UNMAPPED;
            if (!p->at) p->at = el2q_usb_word_address[i];
            return;
        }
    for (unsigned i = 0; i < EL2Q_USB_WORD_COUNT; i++) {
        uint64_t word = 0, fault = hw->read32(hw->ctx, el2q_usb_word_address[i], &word);
        if (fault) {
            p->word_read[i] = EL2Q_USB_FAULT;
            p->core = EL2Q_USB_CORE_FAULT;
            if (!p->esr) { p->esr = fault; p->at = el2q_usb_word_address[i]; }
            return;
        }
        p->word_read[i] = EL2Q_USB_READ_OK;
        p->word[i] = (uint32_t)word;
    }
    p->core = EL2Q_USB_CORE_READ;
}

#define SMMU_sCR0     0x0
#define SMMU_IDR0     0x20
#define SMMU_SMR(n)   (0x800 + 4 * (n))
#define SMMU_S2CR(n)  (0xc00 + 4 * (n))
#define SMR_VALID     (1u << 31)
#define sCR0_EXIDENABLE (1u << 3)
#define S2CR_EXIDVALID  (1u << 10)
#define SMMU_IDR1     0x24
#define IDR1_PAGESIZE (1u << 31)
#define SMMU_GR1_CBAR(n)  (0x0 + 4 * (n))
#define SMMU_GR1_CBA2R(n) (0x800 + 4 * (n))
#define CB_SCTLR 0x0
#define CB_TCR2  0x10
#define CB_TTBR0 0x20
#define CB_TCR   0x30
#define CB_MAIR0 0x38
#define CB_MAIR1 0x3c

/* The context bank the matched S2CR names; see usbprobe.h. */
static void read_bank(struct el2q_usb *u, const struct el2q_usb_hw *hw)
{
    if ((u->s2cr >> 16 & 3) != 0) {                      /* S2CR.TYPE: 0 translate */
        u->cb = EL2Q_USB_CB_NOT_TRANSLATED;
        return;
    }
    u->cb_index = u->s2cr & 0xff;                        /* CBNDX */
    uint64_t word = 0, f = hw->read32(hw->ctx, EL2Q_USB_SMMU_BASE + SMMU_IDR1, &word);
    if (f) {
        u->cb = EL2Q_USB_CB_FAULT;
        u->cb_esr = f;
        u->cb_at = EL2Q_USB_SMMU_BASE + SMMU_IDR1;
        return;
    }
    u->id1 = (uint32_t)word;
    unsigned shift = (u->id1 & IDR1_PAGESIZE) ? 16 : 12;
    uint64_t numpage = 1ull << ((u->id1 >> 28 & 7) + 1);
    uint64_t gr1 = EL2Q_USB_SMMU_BASE + (1ull << shift);
    uint64_t page = (numpage + u->cb_index) << shift;
    u->cb_page = EL2Q_USB_SMMU_BASE + page;
    if (u->cb_index >= (u->id1 & 0xff) || page + (1ull << shift) > EL2Q_USB_SMMU_SIZE) {
        u->cb = EL2Q_USB_CB_OUT_OF_RANGE;
        return;
    }
    if (hw->mapped(hw->ctx, gr1) || hw->mapped(hw->ctx, gr1 + SMMU_GR1_CBA2R(u->cb_index)) ||
        hw->mapped(hw->ctx, u->cb_page)) {
        u->cb = EL2Q_USB_CB_UNMAPPED;
        return;
    }
    const uint64_t at[] = {gr1 + SMMU_GR1_CBAR(u->cb_index), gr1 + SMMU_GR1_CBA2R(u->cb_index),
                           u->cb_page + CB_SCTLR, u->cb_page + CB_TCR, u->cb_page + CB_TCR2,
                           u->cb_page + CB_TTBR0, u->cb_page + CB_TTBR0 + 4,
                           u->cb_page + CB_MAIR0, u->cb_page + CB_MAIR1};
    uint32_t value[sizeof at / sizeof at[0]] = {0};
    for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
        f = hw->read32(hw->ctx, at[i], &word);
        if (f) {
            u->cb = EL2Q_USB_CB_FAULT;
            u->cb_esr = f;
            u->cb_at = at[i];
            break;
        }
        value[i] = (uint32_t)word;
        u->cb_words = i + 1;
    }
    u->cbar = value[0];
    u->cba2r = value[1];
    u->cb_sctlr = value[2];
    u->cb_tcr = value[3];
    u->cb_tcr2 = value[4];
    u->cb_ttbr0 = value[5] | (uint64_t)value[6] << 32;
    u->cb_mair0 = value[7];
    u->cb_mair1 = value[8];
    if (u->cb != EL2Q_USB_CB_FAULT) u->cb = EL2Q_USB_CB_READ;
}

/* The stage-2 bank a nested bank names; see usbprobe.h. Only after the
 * stage-1 bank was read in full and found nested. */
static void read_stage2(struct el2q_usb *u, const struct el2q_usb_hw *hw)
{
    if (u->cb != EL2Q_USB_CB_READ || (u->cbar >> 16 & 3) != 3) return;
    u->s2_index = u->cbar >> 8 & 0xff;
    unsigned shift = (u->id1 & IDR1_PAGESIZE) ? 16 : 12;
    uint64_t numpage = 1ull << ((u->id1 >> 28 & 7) + 1);
    uint64_t gr1 = EL2Q_USB_SMMU_BASE + (1ull << shift);
    uint64_t page = (numpage + u->s2_index) << shift;
    u->s2_page = EL2Q_USB_SMMU_BASE + page;
    if (u->s2_index >= (u->id1 & 0xff) || page + (1ull << shift) > EL2Q_USB_SMMU_SIZE) {
        u->s2 = EL2Q_USB_CB_OUT_OF_RANGE;
        return;
    }
    if (hw->mapped(hw->ctx, gr1 + SMMU_GR1_CBAR(u->s2_index)) ||
        hw->mapped(hw->ctx, gr1 + SMMU_GR1_CBA2R(u->s2_index)) || hw->mapped(hw->ctx, u->s2_page)) {
        u->s2 = EL2Q_USB_CB_UNMAPPED;
        return;
    }
    const uint64_t at[] = {gr1 + SMMU_GR1_CBAR(u->s2_index), gr1 + SMMU_GR1_CBA2R(u->s2_index),
                           u->s2_page + CB_SCTLR, u->s2_page + CB_TCR,
                           u->s2_page + CB_TTBR0, u->s2_page + CB_TTBR0 + 4};
    uint32_t value[sizeof at / sizeof at[0]] = {0};
    u->s2 = EL2Q_USB_CB_READ;
    for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
        uint64_t word = 0, f = hw->read32(hw->ctx, at[i], &word);
        if (f) {
            u->s2 = EL2Q_USB_CB_FAULT;
            u->s2_esr = f;
            u->s2_at = at[i];
            break;
        }
        value[i] = (uint32_t)word;
        u->s2_words = i + 1;
        /* A bank whose own CBAR is not a stage-2 one is not what the nested
         * bank's index was taken to mean: interpret nothing more of it. */
        if (i == 0 && (value[0] >> 16 & 3) != 0) {
            u->s2_cbar = value[0];
            u->s2 = EL2Q_USB_CB_NOT_STAGE2;
            return;
        }
    }
    u->s2_cbar = value[0];
    u->s2_cba2r = value[1];
    u->s2_sctlr = value[2];
    u->s2_tcr = value[3];
    u->s2_ttbr0 = value[4] | (uint64_t)value[5] << 32;
}

void el2q_usb_smmu(struct el2q_usb *u, const struct el2q_usb_hw *hw)
{
    u->smmu = EL2Q_USB_SMMU_UNMAPPED;
    u->scr0 = u->idr0 = u->smr = u->s2cr = u->group = u->smmu_words = 0;
    u->smmu_esr = 0;
    u->cb = EL2Q_USB_CB_NOT_TRIED;
    u->cb_index = u->id1 = u->cbar = u->cba2r = u->cb_words = 0;
    u->cb_sctlr = u->cb_tcr = u->cb_tcr2 = u->cb_mair0 = u->cb_mair1 = 0;
    u->cb_page = u->cb_ttbr0 = u->cb_esr = u->cb_at = 0;
    u->s2 = EL2Q_USB_CB_NOT_TRIED;
    u->s2_index = u->s2_cbar = u->s2_cba2r = u->s2_sctlr = u->s2_tcr = u->s2_words = 0;
    u->s2_page = u->s2_ttbr0 = u->s2_esr = u->s2_at = 0;
    if (!hw || !hw->mapped || !hw->read32) return;
    /* sCR0, IDR0, the SMRs and the S2CRs all lie in the first 4 KiB of the
     * global register space: S2CR(254), the last there can be, ends it. */
    if (hw->mapped(hw->ctx, EL2Q_USB_SMMU_BASE) ||
        hw->mapped(hw->ctx, EL2Q_USB_SMMU_BASE + SMMU_S2CR(EL2Q_USB_SMR_LIMIT - 1)))
        return;
    uint64_t word = 0;
#define SMMU_READ(offset, dest) do { \
        uint64_t f_ = hw->read32(hw->ctx, EL2Q_USB_SMMU_BASE + (offset), &word); \
        if (f_) { u->smmu = EL2Q_USB_SMMU_FAULT; u->smmu_esr = f_; return; } \
        (dest) = (uint32_t)word; \
    } while (0)
    SMMU_READ(SMMU_sCR0, u->scr0);
    u->smmu_words = 1;
    SMMU_READ(SMMU_IDR0, u->idr0);
    u->smmu_words = 2;
    /* All ones is what an address nothing answers at returns (an empty PCIe
     * window on QEMU's virt, a block that is not there); IDR0 has reserved
     * bits, so no SMMU reads that. Searching such "SMRs" would report a match
     * on noise. */
    if (u->scr0 == 0xffffffffu || u->idr0 == 0xffffffffu) {
        u->smmu = EL2Q_USB_SMMU_ALL_ONES;
        return;
    }
    int exids = (u->scr0 & sCR0_EXIDENABLE) != 0;
    unsigned groups = u->idr0 & 0xff;                    /* NUMSMRG */
    if (groups > EL2Q_USB_SMR_LIMIT) groups = EL2Q_USB_SMR_LIMIT;
    for (unsigned n = 0; n < groups; n++) {
        uint32_t smr = 0, s2cr = 0;
        SMMU_READ(SMMU_SMR(n), smr);
        uint32_t mask = exids ? smr >> 16 : smr >> 16 & 0x7fff, id = smr & 0xffff;
        /* Bits set in the mask are "don't care". */
        if (((id ^ EL2Q_USB_STREAM) & ~mask & 0xffff) != 0) continue;
        if (!exids && !(smr & SMR_VALID)) continue;
        SMMU_READ(SMMU_S2CR(n), s2cr);
        if (exids && !(s2cr & S2CR_EXIDVALID)) continue;
        u->smr = smr;
        u->s2cr = s2cr;
        u->group = n;
        u->smmu = EL2Q_USB_SMMU_MATCH;
        read_bank(u, hw);
        read_stage2(u, hw);
        return;
    }
#undef SMMU_READ
    u->smmu = EL2Q_USB_SMMU_NO_MATCH;
}

/* ---- Report lines -----------------------------------------------------------
 * The gaokun3's screen is the only record of these (its SPCR UART is not one
 * el2qual drives), so every value is on a line of its own kind:
 *   USB0 GCC <B|A> GDSC <w> MASTER <w> SLEEP <w>
 *   USB0 GCC <B|A> UTMI <w> AXI <w> CNOC <w>
 *   USB1 GCC <B|A> GDSC <w> MASTER <w>
 *   USB0 POWER <B|A> <ON|GDSC OFF|CLOCKS OFF|UNREAD>
 * per phase that ran (B before ExitBootServices, A after), then after
 * ExitBootServices
 *   USB0 CORE <READ|NOT READ: POWER NOT ON|NOT READ: UNMAPPED AT <a>|FAULT AT <a> ESR <e>>
 *   USB0 XHCI CAP <w> HCS <w>
 *   USB0 DWC3 SNPSID <w> VER <w> GCTL <w>
 *   USB0 DWC3 U2PHY <w> U3PIPE <w> DCFG <w>
 *   USB0 DWC3 DCTL <w> EVNT <w>
 *   USB0 QSCRATCH HS <w> SS <w>
 * (the last five only when a read was attempted), then with "usb-smmu-map"
 *   SMMU MAP <OK <CLONE|NEW TABLE> KEPT <n>/256|NOT NEEDED|
 *            REFUSED: <REGIME|DEVICE|ALLOC|STRUCTURE <16 hex>|VERIFY <n>/256>>
 * ending " (TEST WINDOW)" when "emulate-map-hole" mapped QEMU's test window
 * and with "usb-smmu"
 *   SMMU SCR0 <w> IDR0 <w>
 *   SMMU STREAM 820 <MATCH SMR <n> <w> S2CR <w>|NO MATCH|UNMAPPED|NOT READ: ALL ONES|FAULT ESR <e>>
 * and after a match, its context bank:
 *   SMMU CB <NOT TRANSLATED|NOT READ: OUT OF RANGE|NOT READ: UNMAPPED|FAULT AT <a> ESR <e>>
 * or, when its words were read (FAULT also prints the ones read before it):
 *   SMMU ID1 <w> CB <nn> AT <a>
 *   SMMU CB<nn> CBAR <w> CBA2R <w> SCTLR <w>
 *   SMMU CB<nn> TCR <w> TCR2 <w>
 *   SMMU CB<nn> TTBR0 <16 hex> MAIR0 <w> MAIR1 <w>
 * and when that bank is nested, its stage-2 bank:
 *   SMMU S2 <NOT READ: OUT OF RANGE|NOT READ: UNMAPPED|FAULT AT <a> ESR <e>|NOT A STAGE-2 BANK, CBAR <w>>
 * or, when its words were read (FAULT also prints the ones read before it):
 *   SMMU S2 CB<nn> AT <a> CBAR <w> CBA2R <w>
 *   SMMU S2 CB<nn> SCTLR <w> TCR <w>
 *   SMMU S2 CB<nn> TTBR0 <16 hex>
 * then with "usb-s2-walk" the walk's lines, with "usb-s2-survey" the
 * survey's and with "usb-s2-fixed" the memory map's (s2walk.c), and last
 *   USB SERROR <PENDING|NONE>
 * <w> is eight hex digits, or UNMAPPED, FAULT or NOT READ for a word that
 * has none. */

struct lbuf { char *s; unsigned n; };

static void put_s(struct lbuf *b, const char *s)
{
    while (*s && b->n < EL2Q_USB_LINE_COLS) b->s[b->n++] = *s++;
    b->s[b->n] = 0;
}

static void put_hex(struct lbuf *b, uint64_t v, unsigned digits)
{
    char t[17];
    for (unsigned i = 0; i < digits; i++) t[i] = "0123456789ABCDEF"[(v >> (4 * (digits - 1 - i))) & 15];
    t[digits] = 0;
    put_s(b, t);
}

static void put_dec(struct lbuf *b, uint64_t v)
{
    char d[20];
    unsigned n = 0;
    do d[n++] = (char)('0' + v % 10); while ((v /= 10) && n < sizeof d);
    while (n) {
        char c[2] = {d[--n], 0};
        put_s(b, c);
    }
}

static void put_word(struct lbuf *b, const char *label, uint32_t state, uint32_t value)
{
    put_s(b, label);
    if (state == EL2Q_USB_READ_OK) put_hex(b, value, 8);
    else put_s(b, state == EL2Q_USB_UNMAPPED ? "UNMAPPED" : state == EL2Q_USB_FAULT ? "FAULT" : "NOT READ");
}

static const char *const power_words[] = {"UNREAD", "GDSC OFF", "CLOCKS OFF", "ON"};

static unsigned phase_lines(const struct el2q_usb_phase *p, char tag, unsigned index, struct lbuf *b)
{
    if (!p->ran) return 0;
    const char t[] = {' ', tag, 0};
    if (index == 0) {
        put_s(b, "USB0 GCC"); put_s(b, t);
        put_word(b, " GDSC ", p->gcc_read[EL2Q_USB_GDSC], p->gcc[EL2Q_USB_GDSC]);
        put_word(b, " MASTER ", p->gcc_read[EL2Q_USB_MASTER], p->gcc[EL2Q_USB_MASTER]);
        put_word(b, " SLEEP ", p->gcc_read[EL2Q_USB_SLEEP], p->gcc[EL2Q_USB_SLEEP]);
    } else if (index == 1) {
        put_s(b, "USB0 GCC"); put_s(b, t);
        put_word(b, " UTMI ", p->gcc_read[EL2Q_USB_UTMI], p->gcc[EL2Q_USB_UTMI]);
        put_word(b, " AXI ", p->gcc_read[EL2Q_USB_AXI], p->gcc[EL2Q_USB_AXI]);
        put_word(b, " CNOC ", p->gcc_read[EL2Q_USB_CNOC], p->gcc[EL2Q_USB_CNOC]);
    } else if (index == 2) {
        put_s(b, "USB1 GCC"); put_s(b, t);
        put_word(b, " GDSC ", p->gcc_read[EL2Q_USB1_GDSC], p->gcc[EL2Q_USB1_GDSC]);
        put_word(b, " MASTER ", p->gcc_read[EL2Q_USB1_MASTER], p->gcc[EL2Q_USB1_MASTER]);
    } else if (index == 3) {
        put_s(b, "USB0 POWER"); put_s(b, t); put_s(b, " ");
        put_s(b, p->power < 4 ? power_words[p->power] : "?");
    }
    return 4;
}

static unsigned core_lines(const struct el2q_usb_phase *p, unsigned index, struct lbuf *b)
{
    if (!p->ran) return 0;
    const uint32_t *r = p->word_read, *w = p->word;
    unsigned count = p->core == EL2Q_USB_CORE_READ || p->core == EL2Q_USB_CORE_FAULT ? 6 : 1;
    if (index == 0) {
        put_s(b, "USB0 CORE ");
        if (p->core == EL2Q_USB_CORE_READ) put_s(b, "READ");
        else if (p->core == EL2Q_USB_CORE_NOT_TRIED) put_s(b, "NOT READ: POWER NOT ON");
        else if (p->core == EL2Q_USB_CORE_UNMAPPED) { put_s(b, "NOT READ: UNMAPPED AT "); put_hex(b, p->at, 8); }
        else { put_s(b, "FAULT AT "); put_hex(b, p->at, 8); put_s(b, " ESR "); put_hex(b, p->esr, 8); }
    } else if (index == 1 && count > 1) {
        put_word(b, "USB0 XHCI CAP ", r[EL2Q_USB_XCAP], w[EL2Q_USB_XCAP]);
        put_word(b, " HCS ", r[EL2Q_USB_XHCS], w[EL2Q_USB_XHCS]);
    } else if (index == 2 && count > 1) {
        put_word(b, "USB0 DWC3 SNPSID ", r[EL2Q_USB_SNPSID], w[EL2Q_USB_SNPSID]);
        put_word(b, " VER ", r[EL2Q_USB_VER], w[EL2Q_USB_VER]);
        put_word(b, " GCTL ", r[EL2Q_USB_GCTL], w[EL2Q_USB_GCTL]);
    } else if (index == 3 && count > 1) {
        put_word(b, "USB0 DWC3 U2PHY ", r[EL2Q_USB_U2PHY], w[EL2Q_USB_U2PHY]);
        put_word(b, " U3PIPE ", r[EL2Q_USB_U3PIPE], w[EL2Q_USB_U3PIPE]);
        put_word(b, " DCFG ", r[EL2Q_USB_DCFG], w[EL2Q_USB_DCFG]);
    } else if (index == 4 && count > 1) {
        put_word(b, "USB0 DWC3 DCTL ", r[EL2Q_USB_DCTL], w[EL2Q_USB_DCTL]);
        put_word(b, " EVNT ", r[EL2Q_USB_EVNT], w[EL2Q_USB_EVNT]);
    } else if (index == 5 && count > 1) {
        put_word(b, "USB0 QSCRATCH HS ", r[EL2Q_USB_QS_HS], w[EL2Q_USB_QS_HS]);
        put_word(b, " SS ", r[EL2Q_USB_QS_SS], w[EL2Q_USB_QS_SS]);
    }
    return count;
}

static unsigned cb_lines(const struct el2q_usb *u, unsigned index, struct lbuf *b)
{
    if (u->cb == EL2Q_USB_CB_NOT_TRIED) return 0;
    int words = u->cb == EL2Q_USB_CB_READ || (u->cb == EL2Q_USB_CB_FAULT && u->cb_words);
    unsigned count = 1 + (words ? 4 : 0) - (u->cb == EL2Q_USB_CB_READ);
    unsigned w = u->cb_words;
    char cb[] = {'C', 'B', "0123456789ABCDEF"[u->cb_index >> 4 & 15], "0123456789ABCDEF"[u->cb_index & 15], 0};
    if (u->cb != EL2Q_USB_CB_READ) {
        if (index == 0) {
            put_s(b, "SMMU CB ");
            if (u->cb == EL2Q_USB_CB_NOT_TRANSLATED) put_s(b, "NOT TRANSLATED");
            else if (u->cb == EL2Q_USB_CB_OUT_OF_RANGE) put_s(b, "NOT READ: OUT OF RANGE");
            else if (u->cb == EL2Q_USB_CB_UNMAPPED) put_s(b, "NOT READ: UNMAPPED");
            else { put_s(b, "FAULT AT "); put_hex(b, u->cb_at, 8); put_s(b, " ESR "); put_hex(b, u->cb_esr, 8); }
            return count;
        }
        index--;
    }
    if (!words) return count;
    if (index == 0) {
        put_s(b, "SMMU ID1 "); put_hex(b, u->id1, 8);
        put_s(b, " "); put_s(b, cb); put_s(b, " AT "); put_hex(b, u->cb_page, 8);
    } else if (index == 1) {
        put_s(b, "SMMU "); put_s(b, cb);
        put_word(b, " CBAR ", w > 0 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->cbar);
        put_word(b, " CBA2R ", w > 1 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->cba2r);
        put_word(b, " SCTLR ", w > 2 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->cb_sctlr);
    } else if (index == 2) {
        put_s(b, "SMMU "); put_s(b, cb);
        put_word(b, " TCR ", w > 3 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->cb_tcr);
        put_word(b, " TCR2 ", w > 4 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->cb_tcr2);
    } else if (index == 3) {
        put_s(b, "SMMU "); put_s(b, cb); put_s(b, " TTBR0 ");
        if (w > 6) put_hex(b, u->cb_ttbr0, 16);
        else put_s(b, "NOT READ");
        put_word(b, " MAIR0 ", w > 7 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->cb_mair0);
        put_word(b, " MAIR1 ", w > 8 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->cb_mair1);
    }
    return count;
}

static unsigned s2_lines(const struct el2q_usb *u, unsigned index, struct lbuf *b)
{
    if (u->s2 == EL2Q_USB_CB_NOT_TRIED) return 0;
    char cb[] = {'C', 'B', "0123456789ABCDEF"[u->s2_index >> 4 & 15], "0123456789ABCDEF"[u->s2_index & 15], 0};
    unsigned w = u->s2_words;
    int words = u->s2 == EL2Q_USB_CB_READ || (u->s2 == EL2Q_USB_CB_FAULT && w);
    unsigned status = u->s2 != EL2Q_USB_CB_READ;
    unsigned count = status + (words ? 3 : 0);
    if (status && index == 0) {
        put_s(b, "SMMU S2 ");
        if (u->s2 == EL2Q_USB_CB_OUT_OF_RANGE) put_s(b, "NOT READ: OUT OF RANGE");
        else if (u->s2 == EL2Q_USB_CB_UNMAPPED) put_s(b, "NOT READ: UNMAPPED");
        else if (u->s2 == EL2Q_USB_CB_NOT_STAGE2) { put_s(b, "NOT A STAGE-2 BANK, CBAR "); put_hex(b, u->s2_cbar, 8); }
        else { put_s(b, "FAULT AT "); put_hex(b, u->s2_at, 8); put_s(b, " ESR "); put_hex(b, u->s2_esr, 8); }
        return count;
    }
    index -= status;
    if (!words || index > 2) return count;
    put_s(b, "SMMU S2 "); put_s(b, cb);
    if (index == 0) {
        put_s(b, " AT "); put_hex(b, u->s2_page, 8);
        put_word(b, " CBAR ", w > 0 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->s2_cbar);
        put_word(b, " CBA2R ", w > 1 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->s2_cba2r);
    } else if (index == 1) {
        put_word(b, " SCTLR ", w > 2 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->s2_sctlr);
        put_word(b, " TCR ", w > 3 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->s2_tcr);
    } else if (index == 2) {
        put_s(b, " TTBR0 ");
        if (w > 5) put_hex(b, u->s2_ttbr0, 16);
        else put_s(b, "NOT READ");
    }
    return count;
}

static unsigned map_lines(const struct el2q_usb *u, unsigned index, struct lbuf *b)
{
    if (u->map == EL2Q_PM_NOT_ASKED) return 0;
    if (index == 0) {
        put_s(b, "SMMU MAP ");
        if (u->map == EL2Q_PM_REFUSED_STRUCT) {
            put_s(b, "REFUSED: STRUCTURE ");
            put_hex(b, u->map_arg, 16);
        } else if (u->map == EL2Q_PM_REFUSED_VERIFY) {
            put_s(b, "REFUSED: VERIFY ");
            put_dec(b, u->map_arg);
            put_s(b, "/");
            put_dec(b, EL2Q_USB_SMMU_SIZE >> 12);
        } else if (u->map == EL2Q_PM_OK) {
            put_s(b, u->map_cloned ? "OK CLONE KEPT " : "OK NEW TABLE KEPT ");
            put_dec(b, u->map_kept);
            put_s(b, "/");
            put_dec(b, EL2Q_USB_SMMU_SIZE >> 12);
        } else {
            if (u->map == EL2Q_PM_REFUSED_REGIME || u->map == EL2Q_PM_REFUSED_DEVICE ||
                u->map == EL2Q_PM_REFUSED_ALLOC)
                put_s(b, "REFUSED: ");
            put_s(b, el2q_pm_word(u->map));
        }
        if (u->map_test) put_s(b, " (TEST WINDOW)");
    }
    return 1;
}

static unsigned smmu_lines(const struct el2q_usb *u, unsigned index, struct lbuf *b)
{
    if (u->smmu == EL2Q_USB_SMMU_NOT_ASKED) return 0;
    if (index == 0) {
        put_word(b, "SMMU SCR0 ", u->smmu_words > 0 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->scr0);
        put_word(b, " IDR0 ", u->smmu_words > 1 ? EL2Q_USB_READ_OK : EL2Q_USB_NOT_READ, u->idr0);
    } else if (index == 1) {
        put_s(b, "SMMU STREAM 820 ");
        if (u->smmu == EL2Q_USB_SMMU_MATCH) {
            put_s(b, "MATCH SMR "); put_hex(b, u->group, 2); put_s(b, " "); put_hex(b, u->smr, 8);
            put_s(b, " S2CR "); put_hex(b, u->s2cr, 8);
        } else if (u->smmu == EL2Q_USB_SMMU_NO_MATCH) put_s(b, "NO MATCH");
        else if (u->smmu == EL2Q_USB_SMMU_UNMAPPED) put_s(b, "UNMAPPED");
        else if (u->smmu == EL2Q_USB_SMMU_ALL_ONES) put_s(b, "NOT READ: ALL ONES");
        else { put_s(b, "FAULT ESR "); put_hex(b, u->smmu_esr, 8); }
    }
    return 2;
}

void el2q_usb_line(const struct el2q_usb *u, unsigned index, char out[EL2Q_USB_LINE_COLS + 1])
{
    struct lbuf b = {out, 0};
    out[0] = 0;
    if (!u->before.ran && !u->after.ran) return;
    unsigned n;
    if (index < (n = phase_lines(&u->before, 'B', index, &b))) return;
    index -= n;
    if (index < (n = phase_lines(&u->after, 'A', index, &b))) return;
    index -= n;
    if (index < (n = core_lines(&u->after, index, &b))) return;
    index -= n;
    if (index < (n = map_lines(u, index, &b))) return;
    index -= n;
    if (index < (n = smmu_lines(u, index, &b))) return;
    index -= n;
    if (index < (n = cb_lines(u, index, &b))) return;
    index -= n;
    if (index < (n = s2_lines(u, index, &b))) return;
    index -= n;
    for (n = 0; n < 4; n++) {
        char walk[EL2Q_S2W_LINE_COLS + 1];
        el2q_s2w_line(&u->walk, n, walk);
        if (!walk[0]) break;
        if (index == n) {
            put_s(&b, walk);
            return;
        }
    }
    index -= n;
    for (n = 0; n < EL2Q_S2W_SURVEY_LINES; n++) {
        char survey[EL2Q_S2W_LINE_COLS + 1];
        el2q_s2w_survey_line(&u->survey, n, survey);
        if (!survey[0]) break;
        if (index == n) {
            put_s(&b, survey);
            return;
        }
    }
    index -= n;
    for (n = 0; n < EL2Q_S2W_REGION_LINES; n++) {
        char region[EL2Q_S2W_LINE_COLS + 1];
        el2q_s2w_region_line(&u->region, n, region);
        if (!region[0]) break;
        if (index == n) {
            put_s(&b, region);
            return;
        }
    }
    index -= n;
    if (index == 0) put_s(&b, u->isr & (1u << 8) ? "USB SERROR PENDING" : "USB SERROR NONE");
}
