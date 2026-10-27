/* SPDX-License-Identifier: MIT */
/* el2qual's USB stage glue: the EL2 callbacks and the order of the USB
 * read, the SMMU window map, the stage-2 walk, the S2CR bypass plan and the
 * xHCI halt read (options "usb" to "usb-halt", README.md), and the pages
 * they need, taken before ExitBootServices. The stages themselves are
 * usbprobe.c, pagemap.c, s2walk.c and s2bypass.c; this file wires them to
 * el2qual's runtime (el2qual-int.h). Moved out of el2qual.c unchanged, to
 * keep that file under AGENTS.md's 50 KiB limit. */
#include "el2qual-int.h"
#include "pagemap.h"
#include "s2bypass.h"
#include "s2walk.h"
#include "usbprobe.h"
#include "vhe.h"

/* ---- The USB read (options "usb", "usb-smmu"; usbprobe.c) --------------------
 * Before ExitBootServices the exception vectors are the firmware's, so a read
 * that faults cannot be caught: only the clock controller's status words are
 * read then, which UEFI's own clock driver reads all the time, each after a
 * mapping check at the current level. After it, el2qual's fault fixup guards
 * every read, and the controller itself is read only if those words say it
 * is powered and clocked. */

static int usb_mapped_now(void *unused, uint64_t address)
{
    (void)unused;
    if (current_el() == 2) __asm__ volatile("at s1e2r, %0" :: "r"(address) : "memory");
    else __asm__ volatile("at s1e1r, %0" :: "r"(address) : "memory");
    ISB();
    uint64_t p = RD("par_el1");
    return (p & 1) || ((p >> 56) & 0xf0) || ((p ^ address) & 0x0000fffffffff000ull);
}

static uint64_t usb_read_unguarded(void *unused, uint64_t address, uint64_t *value)
{
    (void)unused;
    *value = *(volatile uint32_t *)(uintptr_t)address;
    return 0;
}

static int usb_mapped_el2(void *unused, uint64_t address)
{
    uint64_t par;
    return tsens_mapped(unused, address, &par);
}

static uint64_t usb_read_guarded(void *unused, uint64_t address, uint64_t *value)
{
    (void)unused;
    return el2q_try_read32(&ctx[0], value, address);
}

/* "emulate-usb-halt" (test only): virt has no USB0 controller, so the halt
 * read's powered paths (HALTED/RUNNING) cannot run against real hardware,
 * and pagemap cannot turn a RAM page into Device memory (it fills only
 * absent entries). A page el2qual owns stands in for the controller; the
 * reads below map the controller's address range onto it. The read is a
 * real guarded read of real memory through the real AT identity check; only
 * the Device-attribute requirement is dropped, since virt offers no
 * controllable Device MMIO. */
static uint64_t usb_halt_fake;           /* the stand-in controller page */

static uint64_t emu_halt_target(uint64_t address)
{
    uint64_t core = el2q_usb_word_address[EL2Q_USB_XCAP];   /* USB0 controller base */
    if (address >= core && address < core + 0x1000)
        return usb_halt_fake + (address - core);
    return address;
}

static int emu_halt_mapped(void *unused, uint64_t address)
{
    (void)unused;
    uint64_t t = emu_halt_target(address);
    __asm__ volatile("at s1e2r, %0" :: "r"(t) : "memory");
    ISB();
    uint64_t p = RD("par_el1");
    if (p & 1) return 1;
    if ((p ^ t) & 0x0000fffffffff000ull) return 3;   /* present and identity; Device not required */
    return 0;
}

static uint64_t emu_halt_read(void *unused, uint64_t address, uint64_t *value)
{
    return usb_read_guarded(unused, emu_halt_target(address), value);
}

/* ---- The private SMMU window map (option "usb-smmu-map"; pagemap.c) --------
 * The table page comes from a pool allocated before ExitBootServices, only
 * when the option is given. The only stores into a pre-existing page are to
 * the level-2 descriptor; after ExitBootServices all memory is el2qual's,
 * and only the boot CPU runs. */

static uint64_t pm_pool_next, pm_pool_end;

static uint64_t pm_alloc(void *ctx)
{
    (void)ctx;
    if (!pm_pool_next || pm_pool_next >= pm_pool_end) return 0;
    uint64_t page = pm_pool_next;
    pm_pool_next += 0x1000;
    return page;
}

static uint64_t pm_read64(void *ctx, uint64_t address)
{
    (void)ctx;
    return *(volatile uint64_t *)(uintptr_t)address;
}

static void pm_write64(void *ctx, uint64_t address, uint64_t value)
{
    (void)ctx;
    *(volatile uint64_t *)(uintptr_t)address = value;
}

static void pm_sync(void *ctx, uint32_t op, uint64_t address, uint64_t size)
{
    (void)ctx;
    if (op == EL2Q_PM_CLEAN) dcache_range((const volatile void *)(uintptr_t)address, size, CLEAN);
    else if (op == EL2Q_PM_DSB) DSB();
    else if (op == EL2Q_PM_TLBI) __asm__ volatile("tlbi alle2" ::: "memory");
    else if (op == EL2Q_PM_ISB) ISB();
}

/* "emulate-map-hole" (test only): on QEMU's virt machine EDK2 already maps
 * the SMMU window's addresses as Device, so the map would never be written.
 * Two 1 MiB windows in flash0's space past EDK2's firmware volume, which its
 * tables leave absent (OBSERVED), are mapped instead: the first gets a new
 * level-3 table; the second, in the same 2 MiB region, then clones that one,
 * so the swap of a valid entry with its break runs too. The first window
 * must still pass afterwards. Nothing reads either. */
#define EL2Q_PM_TEST_FIRST  0x02000000ull
#define EL2Q_PM_TEST_SECOND 0x02100000ull
#define EL2Q_PM_TEST_SIZE   0x100000ull

static uint32_t map_window(uint64_t base, uint64_t size, struct el2q_pm_result *r)
{
    const struct el2q_pm_hw pmhw = {0, pm_read64, pm_write64, pm_alloc, usb_mapped_el2, pm_sync};
    uint32_t state = el2q_pm_prepare(base, size, RD("tcr_el2"), RD("ttbr0_el2"), RD("mair_el2"),
                                     !!(RD("hcr_el2") & EL2Q_HCR_E2H), &pmhw, r);
    if (state == EL2Q_PM_BUILT) {
        el2q_pm_publish(&pmhw, r);
        state = el2q_pm_verify(base, size, &pmhw, r);
    }
    return state;
}

/* Build the clone, swap it in with break-before-make (pagemap.c), then
 * every window page must pass the stage's own AT check. */
static void map_smmu_window(void)
{
    struct el2q_pm_result r;
    uint32_t state;
    if (opt.emulate_map_hole) {
        progress("USB: MAPPING TWO TEST WINDOWS IN EL2QUAL'S OWN TABLES (TEST ONLY)");
        state = map_window(EL2Q_PM_TEST_FIRST, EL2Q_PM_TEST_SIZE, &r);
        if (state == EL2Q_PM_OK) state = map_window(EL2Q_PM_TEST_SECOND, EL2Q_PM_TEST_SIZE, &r);
        if (state == EL2Q_PM_OK) {
            const struct el2q_pm_hw pmhw = {0, pm_read64, pm_write64, pm_alloc, usb_mapped_el2, pm_sync};
            struct el2q_pm_result first = r;
            if (el2q_pm_verify(EL2Q_PM_TEST_FIRST, EL2Q_PM_TEST_SIZE, &pmhw, &first) != EL2Q_PM_OK) {
                state = EL2Q_PM_REFUSED_VERIFY;
                r.mapped_after = first.mapped_after;
            }
        }
    } else {
        progress("USB: MAPPING APPS_SMMU'S WINDOW IN EL2QUAL'S OWN TABLES");
        state = map_window(EL2Q_USB_SMMU_BASE, EL2Q_USB_SMMU_SIZE, &r);
    }
    run.usb.map = state;
    run.usb.map_test = opt.emulate_map_hole;
    run.usb.map_cloned = r.cloned;
    run.usb.map_kept = r.mapped_before;
    run.usb.map_arg = state == EL2Q_PM_REFUSED_VERIFY ? r.mapped_after : r.detail;
}

/* ---- The stage-2 walk (option "usb-s2-walk"; s2walk.c) --------------------
 * The arena is allocated before ExitBootServices exactly as q1n1's usb-ebs.c
 * allocates its DWC3 arena: EfiLoaderData pages below 4 GiB. The walk then
 * reads descriptors only, each behind the fault fixup, and only through
 * mappings UEFI's tables already have. */

static uint64_t s2w_arena, s2w_pool;
/* "usb-s2-fixed" with test tables: QEMU virt's RAM starts here, so the
 * memory map report has entries to show over a range as long as the
 * gaokun3's region. */
#define EL2Q_S2W_TEST_REGION 0x40000000ull

/* "emulate-s2-device": QEMU virt's PL011, a page EDK2 maps as Device. */
#define EL2Q_S2W_TEST_DEVICE_ROOT 0x09000000ull

static uint64_t s2w_par(void *unused, uint64_t address)
{
    (void)unused;
    __asm__ volatile("at s1e2r, %0" :: "r"(address) : "memory");
    ISB();
    return RD("par_el1");
}

static uint64_t s2w_read64(void *unused, uint64_t address, uint64_t *value)
{
    (void)unused;
    return el2q_try_read64(&ctx[0], value, address);
}

/* An external abort on a load the core does not report synchronously
 * becomes a pending SError once the load completes; the DSB waits for that,
 * and DAIF.A, set since ExitBootServices, keeps it pending to be seen here
 * rather than taken. */
static int s2w_serror(void *unused)
{
    (void)unused;
    DSB();
    ISB();
    return (RD("isr_el1") >> 8) & 1;
}

static void s2w_write64(void *unused, uint64_t address, uint64_t value)
{
    (void)unused;
    *(volatile uint64_t *)(uintptr_t)address = value;
}

static void walk_stage2(void)
{
    const struct el2q_s2w_hw hw = {0, s2w_par, s2w_read64, s2w_serror};
    struct el2q_s2w_bank bank, *walked = 0;
    if (opt.emulate_s2_tables) {
        progress("USB: WALKING EL2QUAL'S OWN TEST STAGE-2 TABLES (TEST ONLY)");
        if (s2w_pool && !el2q_s2w_build_test(s2w_arena, s2w_pool, opt.emulate_s2_odd, s2w_write64, 0, &bank)) {
            if (opt.emulate_s2_device) bank.ttbr = EL2Q_S2W_TEST_DEVICE_ROOT;
            walked = &bank;
        }
    } else {
        progress("USB: STAGE-2 WALK, READ ONLY. A HANG OR RESET HERE IS THE WALK");
        if (run.usb.s2 == EL2Q_USB_CB_READ) {
            bank = (struct el2q_s2w_bank){run.usb.s2_sctlr, run.usb.s2_tcr, run.usb.s2_cba2r, run.usb.s2_ttbr0};
            walked = &bank;
        }
    }
    el2q_s2w_walk(walked, s2w_arena, &hw, &run.usb.walk);
    run.usb.walk.test = opt.emulate_s2_tables;
    if (opt.usb_s2_survey) {
        progress("USB: STAGE-2 SURVEY, READ ONLY. A HANG OR RESET HERE IS IT");
        el2q_s2w_survey(walked, s2w_arena, &hw, &run.usb.survey);
        run.usb.survey.test = opt.emulate_s2_tables;
    }
    if (opt.usb_s2_fixed) {
        run.usb.walk.fixed = 1;
        uint64_t base = opt.emulate_s2_tables ? EL2Q_S2W_TEST_REGION : EL2Q_S2W_REGION_BASE;
        el2q_memmap_region(ebs_desc_size ? memory_map : 0, ebs_map_size, ebs_desc_size, base,
                           base + (EL2Q_S2W_REGION_END - EL2Q_S2W_REGION_BASE), &run.usb.region);
        run.usb.region.test = opt.emulate_s2_tables;
    }
}

/* ---- The S2CR bypass plan (option "usb-s2-bypass-plan"; s2bypass.c) -------
 * The plan is described and never performed: s2bypass.c has no write
 * callback, so no path through it reaches the SMMU. */

static int s2b_writable(void *unused, uint64_t address)
{
    (void)unused;
    __asm__ volatile("at s1e2w, %0" :: "r"(address) : "memory");
    ISB();
    uint64_t p = RD("par_el1");
    if (p & 1) return 1;
    if ((p >> 56) & 0xf0) return 2;
    if ((p ^ address) & 0x0000fffffffff000ull) return 3;
    return 0;
}

static uint64_t s2b_read32(void *unused, uint64_t address, uint32_t *value)
{
    (void)unused;
    uint64_t word = 0, esr = el2q_try_read32(&ctx[0], &word, address);
    *value = (uint32_t)word;
    return esr;
}

static void plan_bypass(void)
{
    const struct el2q_s2b_hw hw = {0, usb_mapped_el2, s2b_writable, s2b_read32, s2w_serror};
    const struct el2q_s2b_seen seen = {
        .matched = run.usb.smmu == EL2Q_USB_SMMU_MATCH,
        .bank_read = run.usb.cb == EL2Q_USB_CB_READ,
        .base = EL2Q_USB_SMMU_BASE,
        .stream = EL2Q_USB_STREAM,
        .scr0 = run.usb.scr0, .idr0 = run.usb.idr0, .smr = run.usb.smr,
        .s2cr = run.usb.s2cr, .group = run.usb.group, .cb_index = run.usb.cb_index,
    };
    progress("USB: PLANNING THE SMMU BYPASS, READ ONLY. NOTHING IS WRITTEN");
    el2q_s2b_plan(&seen, &hw, &run.usb.bypass);
}

void measure_usb_before(void)
{
    const struct el2q_usb_hw hw = {0, usb_mapped_now, usb_read_unguarded};
    progress("USB: GCC STATUS BEFORE EXITBOOTSERVICES, READ ONLY");
    el2q_usb_gcc(&run.usb.before, &hw);
}

void measure_usb_after(void)
{
    const struct el2q_usb_hw hw = {0, usb_mapped_el2, usb_read_guarded};
    progress("USB: GCC STATUS AFTER EXITBOOTSERVICES, READ ONLY");
    el2q_usb_gcc(&run.usb.after, &hw);
    if (run.usb.after.power == EL2Q_USB_POWER_ON)
        progress("USB: READING THE USB0 CONTROLLER. A HANG HERE IS A USB0 READ");
    el2q_usb_core(&run.usb.after, &hw);
    if (opt.usb_halt) {
        progress("USB: READING USB0 XHCI HALT STATE (USBCMD/USBSTS), READ ONLY");
        el2q_usb_halt(&run.usb, &hw);
    } else if (opt.emulate_usb_halt && usb_halt_fake) {
        progress("USB: EMULATED XHCI HALT READ, STAND-IN CONTROLLER (TEST ONLY)");
        /* A stand-in xHCI in RAM el2qual owns: CAPLENGTH 0x30 (the A16's) so
         * the operational base is core + 0x30, and USBCMD/USBSTS set to the
         * halted or running state. The controller words were not really read,
         * so XCAP is seeded for the CAPLENGTH the read derives the base from. */
        uint32_t cmd = opt.emulate_usb_halt_running ? 0x00000001u : 0x00000000u;
        uint32_t sts = opt.emulate_usb_halt_running ? 0x00000000u : 0x00000001u;
        *(volatile uint32_t *)(uintptr_t)(usb_halt_fake + 0x30) = cmd;
        *(volatile uint32_t *)(uintptr_t)(usb_halt_fake + 0x34) = sts;
        run.usb.after.word[EL2Q_USB_XCAP] = 0x01100030u;
        run.usb.after.word_read[EL2Q_USB_XCAP] = EL2Q_USB_READ_OK;
        const struct el2q_usb_hw emuhw = {0, emu_halt_mapped, emu_halt_read};
        el2q_usb_halt(&run.usb, &emuhw);
        run.usb.halt_test = 1;
    }
    if (opt.usb_smmu) {
        if (opt.usb_smmu_map) map_smmu_window();
        progress("USB: READING APPS_SMMU. A HANG HERE IS AN SMMU READ");
        el2q_usb_smmu(&run.usb, &hw);
        if (opt.usb_s2_walk) walk_stage2();
        if (opt.usb_s2_bypass_plan) plan_bypass();
    }
    run.usb.isr = RD("isr_el1");
}

/* ---- Before ExitBootServices: the stage's allocations ------------------- */

void usb_stage_reserve(struct efi_boot_services *bs)
{
    /* pagemap.c needs one 4 KiB table page per window it maps. */
    uint64_t pool = 0, pool_pages = opt.emulate_map_hole ? 2 : 1;
    if (opt.usb_smmu_map &&
        !EFI_ERROR(bs->allocate_pages(0 /* AnyPages */, 2 /* LoaderData */, pool_pages, &pool))) {
        pm_pool_next = pool;
        pm_pool_end = pool + pool_pages * 0x1000;
    }
    if (opt.usb_s2_walk) {
        uint64_t below = 0xffffffffull;
        /* "usb-s2-fixed" walks a range it never touches: nothing to allocate. */
        if (opt.usb_s2_fixed)
            s2w_arena = EL2Q_S2W_FIXED_ARENA;
        else if (!EFI_ERROR(bs->allocate_pages(1 /* MaxAddress */, 2 /* LoaderData */, EL2Q_S2W_ARENA_PAGES, &below)))
            s2w_arena = below;
        uint64_t fixture = 0;
        if (opt.emulate_s2_tables &&
            !EFI_ERROR(bs->allocate_pages(0 /* AnyPages */, 2 /* LoaderData */, EL2Q_S2W_TEST_PAGES, &fixture)))
            s2w_pool = fixture;
    }
    /* "emulate-usb-halt": one page stands in for the USB0 controller. */
    if (opt.emulate_usb_halt) {
        uint64_t fake = 0;
        if (!EFI_ERROR(bs->allocate_pages(0 /* AnyPages */, 2 /* LoaderData */, 1, &fake)))
            usb_halt_fake = fake;
    }
}
