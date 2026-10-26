/* SPDX-License-Identifier: MIT */
/* Host tests for probes/el2qual/usbprobe.c, the read-only USB stage. Linked
 * into tools/test-el2qual.c's binary by tools/test-el2qual.py; test_usbprobe()
 * returns its check count.
 *
 * The fake hardware logs every read. The property that matters most is the
 * gate: no word of the USB controller (0x0a600000, qscratch 0x0a6f8800) is
 * ever read unless the GCC words say its power domain is on and its master
 * and AXI clocks run. */
#include "report.h"
#include "usbprobe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

unsigned test_usbprobe(void);

#define MAXREG 512
static struct fake {
    uint64_t addr[MAXREG];
    uint32_t value[MAXREG];
    unsigned count;
    uint64_t unmapped[8], fault[8];
    unsigned nunmapped, nfault;
    uint64_t log[1024];
    unsigned nlog;
} hw_state;

static void fake_reset(void) { memset(&hw_state, 0, sizeof hw_state); }

static void set(uint64_t a, uint32_t v)
{
    for (unsigned i = 0; i < hw_state.count; i++)
        if (hw_state.addr[i] == a) { hw_state.value[i] = v; return; }
    hw_state.addr[hw_state.count] = a;
    hw_state.value[hw_state.count++] = v;
}

static int fake_mapped(void *ctx, uint64_t a)
{
    (void)ctx;
    for (unsigned i = 0; i < hw_state.nunmapped; i++)
        if (hw_state.unmapped[i] == a) return 1;
    return 0;
}

static uint64_t fake_read(void *ctx, uint64_t a, uint64_t *v)
{
    (void)ctx;
    if (hw_state.nlog < 1024) hw_state.log[hw_state.nlog++] = a;
    for (unsigned i = 0; i < hw_state.nfault; i++)
        if (hw_state.fault[i] == a) return 0x96000010;        /* a synchronous external abort */
    *v = 0;
    for (unsigned i = 0; i < hw_state.count; i++)
        if (hw_state.addr[i] == a) { *v = hw_state.value[i]; break; }
    return 0;
}

static const struct el2q_usb_hw hw = {0, fake_mapped, fake_read};

static int controller_reads(void)
{
    int n = 0;
    for (unsigned i = 0; i < hw_state.nlog; i++)
        n += (hw_state.log[i] >> 20) == 0xa6;     /* 0x0a600000 .. 0x0a6fffff */
    return n;
}

/* USB0 powered and clocked: GDSC PWR_ON, every branch CLK_OFF clear. */
static void powered(void)
{
    fake_reset();
    set(el2q_usb_gcc_address[EL2Q_USB_GDSC], 0x80000000u | 0x10);
    set(el2q_usb_gcc_address[EL2Q_USB_MASTER], 0x00000001u);
    set(el2q_usb_gcc_address[EL2Q_USB_SLEEP], 0x00000001u);
    set(el2q_usb_gcc_address[EL2Q_USB_UTMI], 0x00000001u);
    set(el2q_usb_gcc_address[EL2Q_USB_AXI], 0x00000001u);
    set(el2q_usb_gcc_address[EL2Q_USB_CNOC], 0x00000001u);
    set(el2q_usb_gcc_address[EL2Q_USB1_GDSC], 0x00000000u);
    set(el2q_usb_gcc_address[EL2Q_USB1_MASTER], 0x80000000u);
    /* The A16's firmware-running values (q1n1 docs/A16-USB-EL2-SERIAL.md). */
    static const uint32_t words[EL2Q_USB_WORD_COUNT] = {
        0x01100030, 0x02000140, 0x33313130, 0x3230302a, 0x00102001, 0x00102400,
        0x0b081402, 0x00480854, 0x10f00000, 0xbe2a0000, 0x10100000, 0x01000000};
    for (unsigned i = 0; i < EL2Q_USB_WORD_COUNT; i++) set(el2q_usb_word_address[i], words[i]);
}

static void test_gate(void)
{
    struct el2q_usb_phase p;

    powered();
    el2q_usb_gcc(&p, &hw);
    CHECK(p.ran && p.power == EL2Q_USB_POWER_ON);
    CHECK(controller_reads() == 0);                          /* GCC alone touches nothing else */
    CHECK(hw_state.nlog == EL2Q_USB_GCC_COUNT);
    el2q_usb_core(&p, &hw);
    CHECK(p.core == EL2Q_USB_CORE_READ);
    CHECK(p.word[EL2Q_USB_SNPSID] == 0x33313130 && p.word[EL2Q_USB_GCTL] == 0x00102001);
    CHECK(p.word[EL2Q_USB_QS_SS] == 0x01000000);
    for (unsigned i = 0; i < EL2Q_USB_WORD_COUNT; i++) CHECK(p.word_read[i] == EL2Q_USB_READ_OK);

    /* Each closed gate: GDSC collapsed; master, AXI or CNOC clock off. */
    struct { unsigned word; uint32_t value; unsigned want; } closed[] = {
        {EL2Q_USB_GDSC, 0x00000010, EL2Q_USB_POWER_GDSC_OFF},
        {EL2Q_USB_MASTER, 0x80000000, EL2Q_USB_POWER_CLOCKS_OFF},
        {EL2Q_USB_AXI, 0x80000001, EL2Q_USB_POWER_CLOCKS_OFF},
        {EL2Q_USB_CNOC, 0x80000000, EL2Q_USB_POWER_CLOCKS_OFF},
    };
    for (unsigned i = 0; i < sizeof closed / sizeof closed[0]; i++) {
        powered();
        set(el2q_usb_gcc_address[closed[i].word], closed[i].value);
        el2q_usb_gcc(&p, &hw);
        CHECK(p.power == closed[i].want);
        el2q_usb_core(&p, &hw);
        CHECK(p.core == EL2Q_USB_CORE_NOT_TRIED && controller_reads() == 0);
    }

    /* Sleep and mock UTMI off do not close it: reported, not required. */
    powered();
    set(el2q_usb_gcc_address[EL2Q_USB_SLEEP], 0x80000000);
    set(el2q_usb_gcc_address[EL2Q_USB_UTMI], 0x80000000);
    el2q_usb_gcc(&p, &hw);
    CHECK(p.power == EL2Q_USB_POWER_ON);

    /* A needed GCC word unmapped or faulting: UNREAD, and nothing further. */
    powered();
    hw_state.unmapped[hw_state.nunmapped++] = el2q_usb_gcc_address[EL2Q_USB_GDSC];
    el2q_usb_gcc(&p, &hw);
    CHECK(p.power == EL2Q_USB_POWER_UNREAD && p.gcc_read[EL2Q_USB_GDSC] == EL2Q_USB_UNMAPPED);
    CHECK(p.at == el2q_usb_gcc_address[EL2Q_USB_GDSC]);
    el2q_usb_core(&p, &hw);
    CHECK(p.core == EL2Q_USB_CORE_NOT_TRIED && controller_reads() == 0);
    powered();
    hw_state.fault[hw_state.nfault++] = el2q_usb_gcc_address[EL2Q_USB_AXI];
    el2q_usb_gcc(&p, &hw);
    CHECK(p.power == EL2Q_USB_POWER_UNREAD && p.gcc_read[EL2Q_USB_AXI] == EL2Q_USB_FAULT);
    CHECK(p.esr == 0x96000010 && p.at == el2q_usb_gcc_address[EL2Q_USB_AXI]);
    el2q_usb_core(&p, &hw);
    CHECK(controller_reads() == 0);

    /* USB1's words are for comparison only: a failure there decides nothing. */
    powered();
    hw_state.fault[hw_state.nfault++] = el2q_usb_gcc_address[EL2Q_USB1_GDSC];
    hw_state.unmapped[hw_state.nunmapped++] = el2q_usb_gcc_address[EL2Q_USB1_MASTER];
    el2q_usb_gcc(&p, &hw);
    CHECK(p.power == EL2Q_USB_POWER_ON);

    /* Controller: one word unmapped means none is read. */
    powered();
    hw_state.unmapped[hw_state.nunmapped++] = el2q_usb_word_address[EL2Q_USB_QS_SS];
    el2q_usb_gcc(&p, &hw);
    el2q_usb_core(&p, &hw);
    CHECK(p.core == EL2Q_USB_CORE_UNMAPPED && controller_reads() == 0);
    CHECK(p.at == el2q_usb_word_address[EL2Q_USB_QS_SS]);

    /* A fault stops the reads: earlier words kept, later ones not read. */
    powered();
    hw_state.fault[hw_state.nfault++] = el2q_usb_word_address[EL2Q_USB_VER];
    el2q_usb_gcc(&p, &hw);
    el2q_usb_core(&p, &hw);
    CHECK(p.core == EL2Q_USB_CORE_FAULT && p.esr == 0x96000010);
    CHECK(p.word_read[EL2Q_USB_SNPSID] == EL2Q_USB_READ_OK && p.word_read[EL2Q_USB_VER] == EL2Q_USB_FAULT);
    CHECK(p.word_read[EL2Q_USB_GCTL] == EL2Q_USB_NOT_READ);
    CHECK(controller_reads() == 4);                       /* XCAP, XHCS, SNPSID, the faulting VER */

    /* No hardware operations: nothing read, nothing decided. */
    el2q_usb_gcc(&p, NULL);
    CHECK(p.ran && p.power == EL2Q_USB_POWER_UNREAD);
    el2q_usb_core(&p, NULL);
    CHECK(p.core == EL2Q_USB_CORE_NOT_TRIED);
}

#define SMMU(off) (EL2Q_USB_SMMU_BASE + (off))

static void test_smmu(void)
{
    struct el2q_usb u;

    fake_reset();
    set(SMMU(0), 0x00200c06);                    /* no EXIDENABLE */
    set(SMMU(0x20), 4);                          /* NUMSMRG 4 */
    set(SMMU(0x800), 0x80000400);                /* valid, another stream */
    set(SMMU(0x804), 0x00000820);                /* USB0's ID but not valid */
    set(SMMU(0x808), 0x80000820);                /* valid, USB0 */
    set(SMMU(0xc08), 0x00010000);                /* S2CR TYPE bypass */
    memset(&u, 0, sizeof u);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_MATCH && u.group == 2 && u.smr == 0x80000820 && u.s2cr == 0x00010000);
    CHECK(u.smmu_words == 2 && u.scr0 == 0x00200c06);

    /* A mask makes the low bits "don't care". */
    set(SMMU(0x808), 0x80000000u | 0x003f0000u | 0x800);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_MATCH && u.group == 2);
    set(SMMU(0x808), 0x80000000u | 0x000f0000u | 0x800);  /* 0x20 lies outside the mask */
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_NO_MATCH);

    /* Extended stream IDs: validity is S2CR.EXIDVALID, not SMR bit 31. */
    fake_reset();
    set(SMMU(0), 0x00200c0e);                    /* EXIDENABLE */
    set(SMMU(0x20), 2);
    set(SMMU(0x800), 0x00000820);
    set(SMMU(0xc00), 0x00000000);                /* EXIDVALID clear */
    set(SMMU(0x804), 0x00000820);
    set(SMMU(0xc04), 0x00000400 | 0x00010000);   /* EXIDVALID, bypass */
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_MATCH && u.group == 1 && u.s2cr == 0x00010400);

    /* NUMSMRG's whole 8-bit range is searched (the gaokun3 reports 212). */
    fake_reset();
    set(SMMU(0x20), 0xff);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_NO_MATCH && hw_state.nlog == 2 + EL2Q_USB_SMR_LIMIT);

    /* All ones, as QEMU's empty PCIe window reads: not an SMMU, no search. */
    fake_reset();
    set(SMMU(0), 0xffffffffu);
    set(SMMU(0x20), 0xffffffffu);
    for (unsigned n = 0; n < 4; n++) set(SMMU(0x800 + 4 * n), 0xffffffffu);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_ALL_ONES && hw_state.nlog == 2);
    fake_reset();
    set(SMMU(0x20), 0xffffffffu);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_ALL_ONES);

    /* A fault ends it; an unmapped page reads nothing. */
    fake_reset();
    hw_state.fault[hw_state.nfault++] = SMMU(0x20);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_FAULT && u.smmu_words == 1 && u.smmu_esr == 0x96000010);
    fake_reset();
    hw_state.unmapped[hw_state.nunmapped++] = SMMU(0);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_UNMAPPED && hw_state.nlog == 0);
}

/* The gaokun3's match (run 17): group 6, S2CR 00000006, translate through
 * bank 6. ID1 here: 4 KiB pages, NUMPAGENDXB 6 (128 pages before the
 * banks), 32 banks, so bank 6's page is at 0x15086000 and GR1 at 0x15001000. */
#define GR1(off) SMMU(0x1000 + (off))
#define CB6(off) SMMU(0x86000 + (off))
static void bank_fixture(void)
{
    fake_reset();
    set(SMMU(0), 0x00240406);
    set(SMMU(0x20), 0x7c017ed4);
    set(SMMU(0x24), (6u << 28) | 0x20);
    set(SMMU(0x818), 0x80000820);
    set(SMMU(0xc18), 0x00000006);
    set(GR1(0x18), 0x00010100);
    set(GR1(0x818), 0x00000001);
    set(CB6(0x00), 0x00001067);
    set(CB6(0x30), 0x80803520);
    set(CB6(0x10), 0x00000005);
    set(CB6(0x20), 0xa1b2c000);
    set(CB6(0x24), 0x00060000);
    set(CB6(0x38), 0x0004ff44);
    set(CB6(0x3c), 0x00000000);
}

static int bank_reads(void)
{
    int n = 0;
    for (unsigned i = 0; i < hw_state.nlog; i++)
        n += hw_state.log[i] >= SMMU(0x1000) && hw_state.log[i] != SMMU(0x24);
    return n;
}

static void test_bank(void)
{
    struct el2q_usb u;
    memset(&u, 0, sizeof u);

    bank_fixture();
    el2q_usb_smmu(&u, &hw);
    CHECK(u.smmu == EL2Q_USB_SMMU_MATCH && u.group == 6 && u.cb == EL2Q_USB_CB_READ);
    CHECK(u.cb_index == 6 && u.cb_page == SMMU(0x86000) && u.cb_words == 9);
    CHECK(u.cbar == 0x00010100 && u.cba2r == 1 && u.cb_sctlr == 0x00001067);
    CHECK(u.cb_tcr == 0x80803520 && u.cb_tcr2 == 5 && u.cb_ttbr0 == 0x00060000a1b2c000ull);
    CHECK(u.cb_mair0 == 0x0004ff44 && u.cb_mair1 == 0);

    /* Bypass or fault in S2CR: no bank is used, none is read. */
    bank_fixture();
    set(SMMU(0xc18), 0x00010006);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_NOT_TRANSLATED && bank_reads() == 0);
    bank_fixture();
    set(SMMU(0xc18), 0x00020006);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_NOT_TRANSLATED && bank_reads() == 0);

    /* A bank past NUMCB, or a page past the SMMU's 1 MiB window. */
    bank_fixture();
    set(SMMU(0x24), (6u << 28) | 6);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_OUT_OF_RANGE && bank_reads() == 0);
    bank_fixture();
    set(SMMU(0x24), (1u << 31) | (7u << 28) | 0x20);        /* 64 KiB pages, 256 before the banks */
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_OUT_OF_RANGE && bank_reads() == 0);

    /* 64 KiB pages that do fit: bank 6 at (2 + 6) * 64 KiB. */
    bank_fixture();
    set(SMMU(0x24), (1u << 31) | (0u << 28) | 0x20);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_READ && u.cb_page == SMMU(8u << 16));

    /* The bank's page unmapped: nothing of it is read. */
    bank_fixture();
    hw_state.unmapped[hw_state.nunmapped++] = CB6(0);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_UNMAPPED && bank_reads() == 0);

    /* A fault in TTBR0's upper half: the words before it kept, after it not read. */
    bank_fixture();
    hw_state.fault[hw_state.nfault++] = CB6(0x24);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_FAULT && u.cb_words == 6 && u.cb_at == CB6(0x24));
    CHECK(u.cb_tcr2 == 5 && u.cb_mair0 == 0);
    char l[40][EL2Q_USB_LINE_COLS + 1];
    unsigned n = 0;
    u.before.ran = 1;
    for (; n < 40; n++) { el2q_usb_line(&u, n, l[n]); if (!l[n][0]) break; }
    int seen = 0;
    for (unsigned i = 0; i < n; i++) {
        seen += !strcmp(l[i], "SMMU CB FAULT AT 15086024 ESR 96000010");
        seen += !strcmp(l[i], "SMMU CB06 TTBR0 NOT READ MAIR0 NOT READ MAIR1 NOT READ");
        seen += !strcmp(l[i], "SMMU CB06 TCR 80803520 TCR2 00000005");
    }
    CHECK(seen == 3);

    /* IDR1 itself faulting. */
    bank_fixture();
    hw_state.fault[hw_state.nfault++] = SMMU(0x24);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_FAULT && u.cb_words == 0 && u.cb_at == SMMU(0x24));
}

/* Run 19's real values: ID1 6000007F (4 KiB pages, banks from page 128,
 * 127 banks), bank 6 nested (CBAR 00037E03) with stage 1 off, naming bank
 * 0x7E, whose page is (128 + 126) * 4 KiB = 0x150FE000 and whose GR1 words
 * are CBAR(126) at 0x150011F8 and CBA2R(126) at 0x150019F8. Bank 126's
 * values here are synthetic. */
#define CB126(off) SMMU(0xfe000 + (off))
static void nested_fixture(void)
{
    bank_fixture();
    set(SMMU(0x24), 0x6000007f);
    set(GR1(0x18), 0x00037e03);
    set(CB6(0x00), 0x000000e0);
    set(CB6(0x30), 0);
    set(CB6(0x10), 0x00000060);
    set(CB6(0x20), 0);
    set(CB6(0x24), 0);
    set(CB6(0x38), 0);
    set(GR1(0x1f8), 0x00000003);                /* TYPE 0: stage 2, VMID 3 */
    set(GR1(0x9f8), 0x00000001);
    set(CB126(0x00), 0x00000061);
    set(CB126(0x30), 0x80023559);
    set(CB126(0x20), 0x9f700000);
    set(CB126(0x24), 0x00030000);
}

static int s2_reads(void)
{
    int n = 0;
    for (unsigned i = 0; i < hw_state.nlog; i++)
        n += hw_state.log[i] == GR1(0x1f8) || hw_state.log[i] == GR1(0x9f8) ||
             (hw_state.log[i] >= CB126(0) && hw_state.log[i] < CB126(0x1000));
    return n;
}

static void test_stage2(void)
{
    struct el2q_usb u;
    memset(&u, 0, sizeof u);

    nested_fixture();
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_READ && u.cb_sctlr == 0xe0 && u.cbar == 0x00037e03);
    CHECK(u.s2 == EL2Q_USB_CB_READ && u.s2_index == 0x7e && u.s2_page == SMMU(0xfe000));
    CHECK(u.s2_cbar == 3 && u.s2_cba2r == 1 && u.s2_sctlr == 0x61 && u.s2_tcr == 0x80023559);
    CHECK(u.s2_ttbr0 == 0x000300009f700000ull && u.s2_words == 6);

    /* Not nested: no stage-2 bank, nothing of one read. */
    nested_fixture();
    set(GR1(0x18), 0x00017e03);                 /* TYPE 1: stage 2 bypassed */
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_READ && u.s2 == EL2Q_USB_CB_NOT_TRIED && s2_reads() == 0);

    /* The named bank is not a stage-2 one: its CBAR alone is read. */
    nested_fixture();
    set(GR1(0x1f8), 0x00010003);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.s2 == EL2Q_USB_CB_NOT_STAGE2 && u.s2_cbar == 0x00010003 && s2_reads() == 1);

    /* Past NUMCB, or its page unmapped: nothing of it read. */
    nested_fixture();
    set(SMMU(0x24), 0x60000040);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.s2 == EL2Q_USB_CB_OUT_OF_RANGE && s2_reads() == 0);
    nested_fixture();
    hw_state.unmapped[hw_state.nunmapped++] = CB126(0);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.s2 == EL2Q_USB_CB_UNMAPPED && s2_reads() == 0);

    /* A fault in VTTBR's upper half keeps what came before. */
    nested_fixture();
    hw_state.fault[hw_state.nfault++] = CB126(0x24);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.s2 == EL2Q_USB_CB_FAULT && u.s2_words == 5 && u.s2_at == CB126(0x24) && u.s2_tcr == 0x80023559);

    /* A fault in the stage-1 bank: no stage-2 read is attempted. */
    nested_fixture();
    hw_state.fault[hw_state.nfault++] = CB6(0x3c);
    el2q_usb_smmu(&u, &hw);
    CHECK(u.cb == EL2Q_USB_CB_FAULT && u.s2 == EL2Q_USB_CB_NOT_TRIED && s2_reads() == 0);

    /* The lines: after the four stage-1 bank lines. */
    char l[40][EL2Q_USB_LINE_COLS + 1];
    memset(&u, 0, sizeof u);
    nested_fixture();
    el2q_usb_gcc(&u.after, &hw);
    el2q_usb_smmu(&u, &hw);
    unsigned n = 0;
    for (; n < 40; n++) {
        el2q_usb_line(&u, n, l[n]);
        if (!l[n][0]) break;
        CHECK(strlen(l[n]) <= EL2Q_USB_LINE_COLS);
    }
    CHECK(n == 4 + 1 + 2 + 4 + 3 + 1);
    CHECK(!strcmp(l[8], "SMMU CB06 CBAR 00037E03 CBA2R 00000001 SCTLR 000000E0"));
    CHECK(!strcmp(l[11], "SMMU S2 CB7E AT 150FE000 CBAR 00000003 CBA2R 00000001"));
    CHECK(!strcmp(l[12], "SMMU S2 CB7E SCTLR 00000061 TCR 80023559"));
    CHECK(!strcmp(l[13], "SMMU S2 CB7E TTBR0 000300009F700000"));
    CHECK(!strcmp(l[14], "USB SERROR NONE"));
    set(GR1(0x1f8), 0x00010003);
    el2q_usb_smmu(&u, &hw);
    el2q_usb_line(&u, 11, l[11]);
    CHECK(!strcmp(l[11], "SMMU S2 NOT A STAGE-2 BANK, CBAR 00010003"));
}

static unsigned lines(const struct el2q_usb *u, char out[][EL2Q_USB_LINE_COLS + 1], unsigned max)
{
    unsigned n = 0;
    for (; n < max; n++) {
        el2q_usb_line(u, n, out[n]);
        if (!out[n][0]) break;
        CHECK(strlen(out[n]) <= EL2Q_USB_LINE_COLS);
    }
    return n;
}

static void test_lines(void)
{
    static struct el2q_usb u;
    char l[40][EL2Q_USB_LINE_COLS + 1];

    memset(&u, 0, sizeof u);
    CHECK(lines(&u, l, 40) == 0);                /* option not given: no lines */

    powered();
    el2q_usb_gcc(&u.before, &hw);
    el2q_usb_gcc(&u.after, &hw);
    el2q_usb_core(&u.after, &hw);
    unsigned n = lines(&u, l, 40);
    CHECK(n == 4 + 4 + 6 + 1);
    CHECK(!strcmp(l[0], "USB0 GCC B GDSC 80000010 MASTER 00000001 SLEEP 00000001"));
    CHECK(!strcmp(l[1], "USB0 GCC B UTMI 00000001 AXI 00000001 CNOC 00000001"));
    CHECK(!strcmp(l[2], "USB1 GCC B GDSC 00000000 MASTER 80000000"));
    CHECK(!strcmp(l[3], "USB0 POWER B ON"));
    CHECK(!strcmp(l[7], "USB0 POWER A ON"));
    CHECK(!strcmp(l[8], "USB0 CORE READ"));
    CHECK(!strcmp(l[9], "USB0 XHCI CAP 01100030 HCS 02000140"));
    CHECK(!strcmp(l[10], "USB0 DWC3 SNPSID 33313130 VER 3230302A GCTL 00102001"));
    CHECK(!strcmp(l[11], "USB0 DWC3 U2PHY 00102400 U3PIPE 0B081402 DCFG 00480854"));
    CHECK(!strcmp(l[12], "USB0 DWC3 DCTL 10F00000 EVNT BE2A0000"));
    CHECK(!strcmp(l[13], "USB0 QSCRATCH HS 10100000 SS 01000000"));
    CHECK(!strcmp(l[14], "USB SERROR NONE"));

    /* Powered down after ExitBootServices: the gate's reason, no controller lines. */
    set(el2q_usb_gcc_address[EL2Q_USB_GDSC], 0x00000010);
    el2q_usb_gcc(&u.after, &hw);
    el2q_usb_core(&u.after, &hw);
    u.isr = 1u << 8;
    n = lines(&u, l, 40);
    CHECK(n == 4 + 4 + 1 + 1);
    CHECK(!strcmp(l[7], "USB0 POWER A GDSC OFF"));
    CHECK(!strcmp(l[8], "USB0 CORE NOT READ: POWER NOT ON"));
    CHECK(!strcmp(l[9], "USB SERROR PENDING"));

    /* Unreadable words, a faulting core and the SMMU lines. */
    powered();
    hw_state.unmapped[hw_state.nunmapped++] = el2q_usb_gcc_address[EL2Q_USB_GDSC];
    hw_state.fault[hw_state.nfault++] = el2q_usb_gcc_address[EL2Q_USB_SLEEP];
    memset(&u, 0, sizeof u);
    el2q_usb_gcc(&u.after, &hw);
    el2q_usb_core(&u.after, &hw);
    n = lines(&u, l, 40);
    CHECK(!strcmp(l[0], "USB0 GCC A GDSC UNMAPPED MASTER 00000001 SLEEP FAULT"));
    CHECK(!strcmp(l[3], "USB0 POWER A UNREAD"));
    powered();
    hw_state.fault[hw_state.nfault++] = el2q_usb_word_address[EL2Q_USB_GCTL];
    memset(&u, 0, sizeof u);
    el2q_usb_gcc(&u.after, &hw);
    el2q_usb_core(&u.after, &hw);
    set(SMMU(0x20), 0);
    el2q_usb_smmu(&u, &hw);
    n = lines(&u, l, 40);
    CHECK(n == 4 + 6 + 2 + 1);
    CHECK(!strcmp(l[4], "USB0 CORE FAULT AT 0A60C110 ESR 96000010"));
    CHECK(!strcmp(l[6], "USB0 DWC3 SNPSID 33313130 VER 3230302A GCTL FAULT"));
    CHECK(!strcmp(l[7], "USB0 DWC3 U2PHY NOT READ U3PIPE NOT READ DCFG NOT READ"));
    CHECK(!strcmp(l[10], "SMMU SCR0 00000000 IDR0 00000000"));
    CHECK(!strcmp(l[11], "SMMU STREAM 820 NO MATCH"));
    set(SMMU(0x20), 0xffffffffu);
    el2q_usb_smmu(&u, &hw);
    n = lines(&u, l, 40);
    CHECK(!strcmp(l[11], "SMMU STREAM 820 NOT READ: ALL ONES"));

    /* A full bank read: its four lines after the stream line. */
    memset(&u, 0, sizeof u);
    bank_fixture();
    el2q_usb_gcc(&u.after, &hw);
    el2q_usb_smmu(&u, &hw);
    n = lines(&u, l, 40);
    CHECK(n == 4 + 1 + 2 + 4 + 1);
    CHECK(!strcmp(l[6], "SMMU STREAM 820 MATCH SMR 06 80000820 S2CR 00000006"));
    CHECK(!strcmp(l[7], "SMMU ID1 60000020 CB06 AT 15086000"));
    CHECK(!strcmp(l[8], "SMMU CB06 CBAR 00010100 CBA2R 00000001 SCTLR 00001067"));
    CHECK(!strcmp(l[9], "SMMU CB06 TCR 80803520 TCR2 00000005"));
    CHECK(!strcmp(l[10], "SMMU CB06 TTBR0 00060000A1B2C000 MAIR0 0004FF44 MAIR1 00000000"));
    CHECK(!strcmp(l[11], "USB SERROR NONE"));
    set(SMMU(0xc18), 0x00010006);
    el2q_usb_smmu(&u, &hw);
    n = lines(&u, l, 40);
    CHECK(!strcmp(l[7], "SMMU CB NOT TRANSLATED") && !strcmp(l[8], "USB SERROR NONE"));
}

/* The lines reach the screen report, near the top, with the power line in
 * green when on and yellow when not; and not at all when not asked for. */
static void test_report(void)
{
    static struct el2q_run r;
    static struct el2q_text text;
    memset(&r, 0, sizeof r);
    el2q_report(&r, &text);
    for (unsigned i = 0; i < text.count; i++) CHECK(strncmp(text.line[i], "USB", 3));
    powered();
    el2q_usb_gcc(&r.usb.before, &hw);
    el2q_usb_gcc(&r.usb.after, &hw);
    set(el2q_usb_gcc_address[EL2Q_USB_GDSC], 0x00000010);
    el2q_usb_gcc(&r.usb.after, &hw);
    el2q_usb_core(&r.usb.after, &hw);
    el2q_report(&r, &text);
    int on = -1, off = -1, core = -1;
    for (unsigned i = 0; i < text.count; i++) {
        if (!strcmp(text.line[i], "USB0 POWER B ON")) on = (int)i;
        if (!strcmp(text.line[i], "USB0 POWER A GDSC OFF")) off = (int)i;
        if (!strcmp(text.line[i], "USB0 CORE NOT READ: POWER NOT ON")) core = (int)i;
    }
    CHECK(on >= 0 && off > on && core > off && core < 30);
    CHECK(text.color[on] == EL2Q_GREEN && text.color[off] == EL2Q_YELLOW && text.color[core] == EL2Q_WHITE);
}

unsigned test_usbprobe(void)
{
    test_gate();
    test_smmu();
    test_bank();
    test_stage2();
    test_lines();
    test_report();
    return checks;
}
