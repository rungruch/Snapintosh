# gaokun3 port: handover

The entry point for anyone picking up the gaokun3 port: where it stands,
where everything lives, how to rebuild and test it, the rules that bind the
work, and what comes next. It summarises; the phase documents it links are
the record, and win wherever the two disagree.

**As of 2026-10-30.** The tree, not this page, is the source of truth
(AGENTS.md, "Start Here"): check `git log` and the linked documents before
acting on anything here.

## The target

The Huawei MateBook E Go 2023, codename **gaokun3**: a Snapdragon 8cx Gen 3
(SC8280XP) tablet, firmware 2.18 (06/30/2023), product `GK-W7X`. The goal
is to run Snapintosh's EL2 monitor on it and boot macOS. Snapintosh's main
target is the ASUS Zenbook A16 (X2 Elite); the gaokun3 is a second machine
with different hardware, so most of the A16's design does not carry over
unchanged.

The route chosen in [Phase 0C](GAOKUN3-PHASE0C.md#decision) is **Route B**:
the stock 26A428 **VirtualMac2,1** kernel at EL1, under a monitor that
supplies what Apple's virtual-machine firmware would. The A16's J700 route
needs NV2, ECV and FGT, which this CPU lacks (VERIFIED, run 4). Route B
needs only PAuth and the 16 KiB granule, which it has. Route B was accepted
with conditions, and a real-hardware boot can reopen it.

## Where things stand

| Phase | State | Record |
| --- | --- | --- |
| 0A: CPU qualification | DONE. PAuth2 with QARMA5 and the 16 KiB walk on all 8 cores; NV2, ECV, FGT and RNDR absent; `CNTFRQ_EL0` 19.2 MHz; GICv3 virtual interface with 4 list registers | [QUALIFICATION](GAOKUN3-QUALIFICATION.md#runs-so-far) |
| 0B: kernel study | Mostly done; QEMU smoke boots the VirtualMac kernel to the root-disk wait (host only) | [PHASE0B](GAOKUN3-PHASE0B.md) |
| 0C: route decision | Route B ACCEPTED, 2026-10-26, with conditions | [PHASE0C](GAOKUN3-PHASE0C.md#decision) |
| 1: guards | Thermal guard pinned to tsens and run once at EL2 (run 10); battery guard and the trend check host-tested against Windows readings | [PHASE1-GUARDS](GAOKUN3-PHASE1-GUARDS.md) |
| 2: EL2 entry and USB link | VHE exit PASSED on the machine (runs 11, 12; q1n1 runs 14, 15). USB link **paused**: USB0's DMA is translated by `apps_smmu`, and the bypass's first write (stage 2) is designed and host-modelled only | [PHASE2](GAOKUN3-PHASE2.md), [USB-DEVICE-MODE](GAOKUN3-USB-DEVICE-MODE.md) |
| 3: VirtualMac kernel to a console | **PASSED on hardware (Run 47, 2026-10-30).** Stock 26A428 kernel bootstraps completely through memory management (`pmap_startup`), physical GICv3, virtual timer, PAC QARMA5, context switching (with SSBS/DIT bypassed), IOKit driver matching, and BSD domain init (`L2TP DOMAIN INIT COMPLETE`), reaching the root-mount wait (`missing-boot-media` written to NVRAM). No crash, no panic. Ready for Phase 4 (storage). | [PHASE3](GAOKUN3-PHASE3.md) |
| 4: Storage mediation and root mount | **PASSED on hardware (Run 48, 2026-10-31).** Stock 26A428 VirtualMac2,1 kernel mounted APFS root filesystem (`TestRoot`) from RAM-backed VirtIO block device on PCIe. `ACT=1`, `BAR0=226`, `VQ=62`, `IRQ=65`. Line 2 locked in green: `APFS MOUNT: COMPLETE (VOLUME TESTROOT)`. Zero NVMe writes. | [PHASE4](GAOKUN3-PHASE4.md) |
| 5: Userspace transition (`/sbin/launchd`) | **PASSED on hardware (Run 53, 2026-10-31).** Stock 26A428 VirtualMac2,1 kernel mounted APFS root filesystem (`TestRoot`), parsed `/sbin/launchd` Mach-O binary (`VQ=65`, `BAR0=232`), mapped user segments, and launched PID 1 into EL0 userspace. Line 2 latched bright yellow `USERSPACE: PID 1 RUNNING (/SBIN/LAUNCHD)`. Line 3 latched yellow `INIT LOADED: VQ=65 BAR0=232`. Steady-state idle at EL0 with zero kernel panics. | [PHASE5](GAOKUN3-PHASE5.md) |

Recent results worth knowing before anything else:

- **Run 53 (2026-10-31): Phase 5 Step 5A PASSED! Userspace PID 1 verified on hardware.**
  Darwin 26A428 mounted the APFS root volume (`TestRoot`) from the RAM-backed VirtIO block device,
  executed `load_init_program()` targeting `/sbin/launchd`, mapped Mach-O segments (`BAR0=232, VQ=65, IRQ=65`),
  and dropped to EL0 userspace executing freestanding `/sbin/launchd`.
  The top monitor banner latched bright yellow:
  `Line 2: USERSPACE: PID 1 RUNNING (/SBIN/LAUNCHD)`
  `Line 3: INIT LOADED: VQ=65 BAR0=232`
  (VERIFIED on hardware, photograph `media_1793451743799_61945587.jpg`).
- **Run 52 (2026-10-31): Phase 5 launchd load verified on clean boot arguments.**
  Restored clean `-v serial=3 -noprogress rd=disk0s1`. Darwin mounted APFS `TestRoot`
  (`APFS MOUNT: COMPLETE` in green) and read `/sbin/launchd` (`BAR0=232, VQ=65, IRQ=65`).
  (VERIFIED on hardware, photograph `media_1793450257904_85adce04.jpg`).
- **Run 51 (2026-10-31): Userspace telemetry monitor test.**
  Added hardware EL0 detection (`spsr_el2 & 0x0f == 0` / user virtual PC on `elr_el2` / `elr_el1`)
  and redistributor virtual timer PPI re-arm. In Run 51 with `-s -nobsdmgroot`, the block device initialized
  at `BAR0=104 VQ=1`, indicating root device matching did not proceed past initial sector 0 probe.
  (OBSERVED on hardware, photograph `media_1793447878036_3983f7f5.jpg`).
- **Run 50 (2026-10-31): Launchd Mach-O read from APFS root by Darwin kernel.**
  With freestanding `/sbin/launchd` installed to `TestRoot` and `rd=disk0s1` restored,
  Darwin mounted the APFS root volume (`APFS MOUNT: COMPLETE (VOLUME TESTROOT)` latched in green)
  and executed 3 additional VirtQueue requests post-mount, advancing from `VQ=62` (Run 48) to
  `VQ=65` (`BAR0=232`). This proves Darwin traversed `/sbin`, opened `/sbin/launchd`, and read its
  Mach-O load commands. Line 2 stayed green because freestanding `/sbin/launchd` writes to `/dev/console`
  (handled by the display console `km_tty`, not PL011 UART).
  (VERIFIED on hardware, photograph `media_1793446798729_380d7757.jpg`).
- **Run 49 (2026-10-31): boot-uuid injection investigation.**
  Injecting `/chosen/boot-uuid` into device tree caused Darwin's `IOKitBSDInit` to prioritize
  `boot-uuid-media` lookup, bypassing `rd=disk0s1` matching and halting at `VQ=39`. Reverted immediately.
  (OBSERVED on hardware, photograph `media_1793446116927_fc8ecee5.jpg`).

- **Run 48 (2026-10-31): Phase 4 goal achieved! Root APFS filesystem mounted on hardware.**
  The stock 26A428 VirtualMac2,1 kernel mounted the APFS root volume (`TestRoot`, `disk1s1`)
  from the RAM-backed VirtIO block device on PCIe. The monitor verified `ACT=1`, `PA=0x162108000`,
  `SZ=0x01E84800` (32 MB), `BAR0=226` (226 MMIO transactions), `VQ=62` (all 62 virtqueue requests
  completed), and `IRQ=65` (GICv3 virtual interrupt delivery). Line 2 permanently locked in green:
  `APFS MOUNT: COMPLETE (VOLUME TESTROOT)`. Zero NVMe writes (Rule 5).
  (VERIFIED on hardware, panel photograph, qualification `8850C93FC6FFEEA2`).
- **Run 47 (2026-10-30): Phase 3 goal achieved! Stock kernel boots to root-mount wait.**
  FairPlay neutralization succeeded 100%. The kernel bypassed `FEAT_SHA3`, initialized
  BSD networking (`L2TP DOMAIN INIT COMPLETE`), completed IOKit driver matching, and
  reached VFS root-mount discovery. Finding no root disk (Phase 4), it wrote
  `missing-boot-media` (size 18) to `boot-picker-bringup-reason` in NVRAM, synced NVRAM
  (`RET=E00002D8` `kIOReturnUnsupported`), and is idling live waiting for root storage.
  **No crash, no panic, no restart! Phase 3 criteria fully met on hardware.**
  (OBSERVED on hardware, 2 photographs, qualification `784D548102EAF6B4`).
- **Run 46 (2026-10-30): Page console scrollback on named stops at EL2.**
  In `stub.S`, `record_and_halt` called `monitor_halt_report(0)` before checking QEMU's
  `pt_flag`. The 18-page 1 Hz scrollback pager on band lines 4–7 successfully verified on
  hardware (OBSERVED on hardware, video IMG_1099.mp4), pinpointing the exact panic:
  `UNDEFINED KERNEL INSTRUCTION: PC=0XFFFFFE0008C692C4 INSTR=CE0148D7 @SLEH.C:1930`
  (ARMv8.2-A `EOR3` inside `com.apple.driver.FairPlayIOKit(72.19)`).
  Qualification: `6B99A9D3BD7F26D7`.
- **Run 45 (2026-10-30): SSBS & DIT neutralizations succeed; kernel bootstrap completes.**
  Context switching succeeded across all 12 patched sites. The kernel initialized the
  `1600x2400` framebuffer screen, `pmap_startup` (1789 µs), `vm_page_bootstrap` (92,765
  free pages), the Clutch scheduler, and serial KDP. `AppleVirtualPlatform` initialized,
  accessed mailbox `0x004FFFF8` (`REQUEST ABSORBED`), panicked, captured stackshot (6387
  bytes), and attempted to flush coredump to AUX disk `0x30010408` (`STOP: UNMODELLED IPA`).
  (OBSERVED on hardware, 2 photographs).
- **Run 44 (2026-10-30): SSBS bypass confirmed; DIT fault diagnosed.**
  The SSBS patch worked completely: `0xfffffe0009eb8490` was bypassed cleanly with
  NOP, and `0xfffffe0009eb849c` (`msr UAO, x2`) executed natively. The kernel faulted
  at `0xfffffe0009eb84a8` on `msr DIT, x2` (`0xD51B42A2`) because Cortex-A78C lacks
  `FEAT_DIT` (`ID_AA64PFR0_EL1.DIT = 0`).
  (OBSERVED on hardware, video IMG_1094.mp4).
- **Run 43 (2026-10-30): 96-line console scrollback paging decodes the panic.**
  The 15-page 1 Hz scrollback on band lines 4–7 captured the complete panic log
  and backtrace (OBSERVED on hardware, video IMG_1091.mp4). The fault was
  `UNDEFINED KERNEL INSTRUCTION: PC=0XFFFFFE0009EB8490 INSTR=D51B42C2 @SLEH.C:1930`.
  Instruction `0xD51B42C2` is `msr SSBS, x2`. Cortex-A78C implements `FEAT_SSBS`
  (`MSR SSBS, #imm` only) and lacks `FEAT_SSBS2` (direct system register
  `MSR/MRS SSBS, <Xt>`). XNU's feature detection (`tst x20, #0xf0`) falsely assumed
  any non-zero field implied Apple Silicon's `FEAT_SSBS2`
  ([PHASE3](GAOKUN3-PHASE3.md#run-43-outcome-observed-on-hardware-video-img_1091mp4)).
- **Run 42 (2026-10-30): dram-size fix confirmed; guest console banner
  prints.** The loader reported `dram-size` over the panic region
  (`0x60000000` -> `0x60100000`). The `dc zva` data abort in
  `check_for_panic_log` was completely eliminated. The kernel initialized
  its own console, printed `Darwin Kernel Version 27.0.0 … RELEASE_ARM64_VMAPPLE`,
  and took a successful stackshot (`** Stackshot Succeeded ** Bytes Traced 1145 …`)
  before requesting a restart via PSCI (`MACH Reboot`). The panic reason
  scrolled away before reading; run 43 paged the scrollback to reveal it
  ([PHASE3](GAOKUN3-PHASE3.md#run-42-2026-10-30-dram-size-fix-confirmed-guest-console-prints-later-panic)).
- **Run 41 (2026-10-30): the formatted panic names the fault.** The kernel
  took a data abort in `bzero`'s `dc zva` while `check_for_panic_log`
  cleared the panic region. The loader's `dram-size` ended where that
  region begins, so XNU mapped it as Device memory, and `dc zva` to Device
  memory is an Alignment fault (INFERRED). QEMU does not implement that
  fault, which is why QEMU boots never showed it
  ([PHASE3](GAOKUN3-PHASE3.md#what-run-41-showed)).
- **Run 40 (2026-10-30): the kernel panics early, and its metadata reads
  back at EL2.** The reason code (`DBOP_PANIC`), ESR, FAR and PC all read
  from XNU's `debugger_state`, but the band showed the panic's format
  string unsubstituted. `+0x20` proved to be `db_panic_args`, not the
  caller ([PHASE3](GAOKUN3-PHASE3.md#run-40-2026-10-30-the-panic-is-captured-its-format-string-is-shown-unformatted)).
- **Run 37 (2026-10-29): the XNU console reached the monitor band**, and
  the first panic was the Cortex-A78C lacking `FEAT_TLBIRANGE`: the stock
  kernel's range TLBI is an undefined instruction `HCR_EL2.TTLB` cannot
  trap ([PHASE3](GAOKUN3-PHASE3.md#run-37-2026-10-29-the-xnu-console-is-alive-range-tlbi-fault-diagnosed)).
- **Run 35 (2026-10-28): the stock kernel entered at EL1.** `SET_INITIAL_STATE`
  installed QARMA5 PAC keys and returned to EL1 cleanly (Phase 0C overturn
  condition 2's first real evidence)
  ([PHASE3](GAOKUN3-PHASE3.md#run-35-2026-10-28-the-guest-enters-at-el1-and-installs-pac-keys)).
- **Run 34 (2026-10-28): Phase 3 step H2 passes.** `monitor_setup` (stage 2)
  and `monitor_init` (the real GIC and virtual CPU interface) completed at
  EL2 on the gaokun3 with no exception, no interrupt storm and no restart
  ([PHASE3](GAOKUN3-PHASE3.md#run-34-2026-10-28-h2-passes-with-real-gic-setup)).
- **Run 28 (2026-10-28): Phase 3 step H1 passes.** From the stick, q1n1
  placed the VirtualMac bundle at its exact addresses in the gaokun3's live
  memory map, verified every hash in place, and seeded it from `RAW`.
  Nothing was entered
  ([PHASE3](GAOKUN3-PHASE3.md#run-28-2026-10-28-h1-passes)).
- **Run 27 (2026-10-27): the firmware offers real entropy.** UEFI's RNG
  protocol lists `RAW` and `HASH`, and `RAW` delivered 256 non-zero,
  varying bytes before ExitBootServices. Phase 0C condition 3's seed
  source exists ([PHASE3](GAOKUN3-PHASE3.md#run-27-2026-10-27-the-firmware-offers-raw-entropy)).
- **Run 26: USB0 is halted after ExitBootServices**, so the SMMU bypass's
  "no DMA in flight" precondition holds
  ([USB-DEVICE-MODE](GAOKUN3-USB-DEVICE-MODE.md#run-26-2026-10-27-usb0-is-halted-after-exitbootservices)).
- **Runs 21 to 25: USB0's stage 2 maps only `0xC6200000`–`0xC85FFFFF`**
  below 4 GiB, all of it firmware-reserved. That is why the USB link needs
  the S2CR bypass. The bypass is PLANNED on the machine (run 25) and has
  never been written.
- **P1: the QEMU smoke kernel touches 11 device windows nobody models**,
  including two fixed physical addresses, `0x30010000` and `0x004FFFF8`,
  whose contracts are unknown
  ([PHASE3](GAOKUN3-PHASE3.md#p1-result-what-the-kernel-touches)).

## Decisions

Made by the owner, and binding until the owner changes them:

| Date | Decision | Record |
| --- | --- | --- |
| 2026-10-23 | **No Linux is booted on the gaokun3**, after half the panel stayed corrupt following a Linux survey boot | [QUALIFICATION](GAOKUN3-QUALIFICATION.md#incidents) |
| 2026-10-26 | Route B accepted, with conditions | [PHASE0C](GAOKUN3-PHASE0C.md#decision) |
| 2026-10-27 | **Phase 3 before the USB link, loaded from the stick.** Standalone boot authorized for the gaokun3's Phase 3 only, as transitional. This covers the order and the host and QEMU work; each hardware step still needs its own go-ahead | [PHASE3](GAOKUN3-PHASE3.md#decisions-for-the-owner), decision 1 |
| 2026-10-28 | **q1n1 is the stick loader** (no new loader) | PHASE3 decision 2 |
| 2026-10-28 | **The screen is split**: the guest gets the top rows, the monitor a band below that stage 2 denies the guest | PHASE3 decision 3 |
| 2026-10-28 | **One CPU** for Phase 3; more in a later phase | PHASE3, with decision 3 |
| 2026-10-28 | **AppleVirtualPlatform's fixed addresses** answered as QEMU's `vmapple` documents them: the backdoor interface and the configuration region modelled (QEMU's non-identifying defaults), the mailbox store absorbed and logged | PHASE3 decision 5 |
| 2026-10-28 | **The real GIC write in H2**: the read-modify-write enable list — `GICD_CTLR`, the boot CPU's redistributor (found by affinity), and PPIs 25, 26 and 27 on `GICR_ISENABLER0`. PPI 26 (the EL2 `CNTHP` snapshot timer) added to the list 2026-10-30 | PHASE3 decision 4, [run 30](GAOKUN3-PHASE3.md#run-30-2026-10-28-the-monitor-is-entered-then-the-machine-restarts) |

All five decisions are made; none is open for the owner right now. Each
hardware run is still the owner's on the day.

## What comes next

1. ~~**P2: the guest profile and the classic loader in `xnuboot`**~~
   DONE, 2026-10-27 (host and QEMU only): `probes/classicload.py`,
   re-exported by `xnuboot`, with the smoke's `kcload.py` as an adapter
   over it and the panic region supplied outside the reported DRAM
   ([P2 result](GAOKUN3-PHASE3.md#p2-result-the-classic-loader-and-the-panic-region)).
2. ~~**P3 to P6**~~ as far as host and QEMU go, 2026-10-28:
   - **P3:** the EL2 interrupt storm that stalled the monitor after
     `lockdown_mode` is fixed. A spent list register's EOI bit kept the
     maintenance interrupt asserted. The default boot is back at the named
     stop
     ([fix](GAOKUN3-PHASE3.md#fixed-2026-10-28-the-maintenance-interrupt-storm)).
   - **P4:** the split console, with the band checked pixel for pixel
     ([P4](GAOKUN3-PHASE3.md#p4-result-the-split-console)).
   - **P5:** `tools/vmapple-stick.py` writes the stick's tree, and q1n1's
     `--vmapple-place` places it, checks it, fills the seed from `RAW`,
     reports and releases it
     ([P5](GAOKUN3-PHASE3.md#p5-result-the-stick-bundle-placed-by-q1n1)).
   - **P6:** one CPU, the device tree's and UEFI's reserved regions, and
     the split screen ([P6](GAOKUN3-PHASE3.md#p6-result-the-gaokun3-layout)).
3. ~~**P3's bar**~~ MET, 2026-10-28, QEMU only: the root-mount wait with
   the PCIe node kept and no QEMU device answering the guest. The MSI
   frame turned out not to be GICv2m; the kernel's own check gave its type
   register's format
   ([P3](GAOKUN3-PHASE3.md#the-msi-frame-2026-10-28-p3s-bar-met)).
4. ~~**Finish Phase 3 on the gaokun3 (H3 and H4)**~~ PASSED on hardware
   (Run 47, 2026-10-30): FairPlay neutralization succeeded 100%, kernel
   bootstrapped natively through BSD domain init to `missing-boot-media`
   ([PHASE3](GAOKUN3-PHASE3.md#run-47-2026-10-30-fairplay-neutralized-kernel-bootstraps-to-root-mount-wait)).
5. **Phase 4: Storage mediation and root filesystem mount.**
   - **Step P1:** Implement VirtIO Modern block controller on PCIe in
     `tools/vmapple-smoke/storage.c` with host tests in
     `tools/test-vmapple-storage.py`.
   - **Step P2:** QEMU smoke boot with root APFS container image.
   - **Step P3:** RAM-backed root disk bundle provisioning via q1n1 (zero NVMe risk).
   - **Step P4:** Hardware qualification on gaokun3.
   See [PHASE4](GAOKUN3-PHASE4.md).
6. **After Phase 4:** the USB link resumes at stage 2 of the S2CR bypass
   ([step 1i](GAOKUN3-USB-DEVICE-MODE.md#step-1i-stage-2-the-first-smmu-write-design-and-host-model)),
   the first SMMU write, which needs its own go-ahead.

## Where things live

This repository:

| Path | What |
| --- | --- |
| `probes/el2qual/` | The qualification probe: one EFI application that reaches EL2 through slbounce and reports on screen. Load options are in its [README](../probes/el2qual/README.md); every option is off by default. `el2qual.c` (entry and CPU measurement), `usbstage.c` (USB and SMMU glue), `usbprobe.c`, `pagemap.c`, `s2walk.c`, `s2bypass.c`, `rng.c`, `firmware.c`, `report.c` |
| `probes/el2qual/s2bwrite.c` | Stage 2 of the S2CR bypass as a **host model only**. It is not linked into el2qual; wiring it in needs a compile-time gate and a separate go-ahead |
| `tools/el2qual-usb.py` | Builds a stick's directory tree under `build/el2qual-usb/<stage>/` with a SHA-256 manifest. It never writes to a disk |
| `tools/qemu-el2qual.py` | Boots el2qual under QEMU and EDK2 in 32 configurations with known answers |
| `tools/test-el2qual*.c`, `tools/test-el2qual*.py` | Host suites, including deliberately broken copies of each module that must be rejected |
| `probes/classicload.py` | The classic VirtualMac loader (Phase 3 P2): layout, tree completion, NVRAM bank, panic region and image building. Re-exported by `probes/xnuboot.py`; the smoke's `kcload.py` is an adapter over it |
| `tools/vmapple-smoke/` | The Route B QEMU smoke and the Phase 3 monitor: an EL2 stub (`stub.S`), the kernelcache loader `kcload.py`, and `smoke.py` (`selftest`; `boot --kernelcache PATH --monitor`, with `--mmio-trace`; `build --gaokun3 --base <pa>` for the real-machine monitor image). `monitor.c`/`monitor.h` hold the device models, interrupt handling and real-GIC setup; `panic.c` reads and formats the guest's panic at EL2; `stub.S` holds stage 2, the vectors, and the guest-entry path (with the gaokun3-only range-TLBI branch bypass behind `VMAPPLE_GAOKUN3_MONITOR`). Debug-only flags include `--framebuffer`, `--keep-nodes`, `--drop-pcie` and `--passthrough` |
| `tools/vmapple-stick.py` | Builds and checks a Phase 3 stick bundle (P5 host half): one file per chunk, a hashed manifest, and a qualification hash. It never writes to a disk |
| `docs/GAOKUN3-*.md` | The phase records listed above |

Outside it:

| Where | What |
| --- | --- |
| `../q1n1` | q1n1, with its own AGENTS.md. Its gaokun3 work sits behind `Q1N1_VHE_EXIT`, `Q1N1_DT_FRAMEBUFFER`, `Q1N1_GAOKUN3` and `Q1N1_VMAPPLE_STICK` (`--vmapple-place`, Phase 3 P5), and is described in its `docs/GAOKUN3-*.md`. A `Q1N1_GAOKUN3` build drives only USB0 and refuses `--boot`, because USB0's DMA is not yet handled |
| `../gaokun_build` | the sticks' `slbounceaa64.efi` and `tcblaunch.exe` (`tools/el2/`). The EL2 device tree is not in this checkout; reuse the copy recorded in the last stick tree's manifest |
| `../gaokun-android` | the hardware inventory (`docs/hw-inventory.md`) |
| `downloads/`, `build/` | Ignored. Kernelcaches, firmware and every stick tree stay here and are never committed |

## Rebuilding and testing

Host and QEMU checks need no hardware and are the minimum before any
hardware request:

```sh
python3 tools/test-el2qual.py          # host suite, sanitizers, mutation checks
python3 tools/qemu-el2qual.py          # all 32 QEMU configurations
python3 tools/qemu-el2qual.py --self-test
python3 tools/test-el2qual-usb.py      # stick builder
python3 tools/test-vmapple-monitor.py  # monitor device models (ASan/UBSan)
python3 tools/test-vmapple-classic.py  # classic loader (probes/classicload.py)
python3 tools/test-vmapple-smoke.py    # smoke harness and loader
python3 tools/test-vmapple-stick.py    # stick bundle builder (P5 host half)
python3 tools/vmapple-smoke/smoke.py selftest
```

The gaokun3 build of q1n1, and with the Phase 3 stick placement:

```sh
make uefi OUT=build/uefi-gaokun3 \
    Q1N1_DEFINES="-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER -DQ1N1_GAOKUN3"
make uefi OUT=build/uefi-gaokun3-vmb \
    Q1N1_DEFINES="-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER -DQ1N1_GAOKUN3 -DQ1N1_VMAPPLE_STICK"
```

q1n1's own checks for it: `python3 tools/test-q1n1-vmapple-stick.py` and
`python3 tools/test-q1n1-vmapple-stick-qemu.py`, run in `../q1n1`.

The Phase 3 QEMU boots need a locally extracted kernelcache, never
committed:

```sh
python3 tools/vmapple-smoke/smoke.py boot --kernelcache <kc> --monitor
python3 tools/vmapple-smoke/smoke.py boot --kernelcache <kc> --monitor --framebuffer
```

A stick is built with, for example, `python3 tools/el2qual-usb.py entropy`
plus stage 2's inputs (`--systemd-boot`, `--slbounce`, `--tcblaunch`,
`--dtb`; see [QUALIFICATION](GAOKUN3-QUALIFICATION.md#building-a-stick)).
Reusing the previous stick tree's inputs is fine: its manifest records
their hashes.

Things that have bitten before:

- **A new el2qual option must bump `EL2Q_VERSION`** (`report.h`). The
  header line is how a photo names the binary that ran.
- **macOS leaves `._` AppleDouble files on a FAT32 stick.** A
  `._slbounceaa64.efi` matches systemd-boot's driver pattern. Delete every
  `._*` file before ejecting.
- **QEMU's monitor socket path must be under 104 bytes** on macOS. Run
  scratch QEMU boots under the repository's `build/`, not a long temporary
  path.
- **Piping a test through `tail` hides its exit code.** Capture the exit
  status separately.
- **Pre-ExitBootServices progress lines go to the firmware console**, not
  the report section of the serial log.
- **zsh does not split an unquoted `$opts`.** In a loop, `"--monitor
  --framebuffer"` reaches the harness as one argument and argparse refuses
  it; use `${=opts}`.
- **QEMU's human monitor echoes input a character at a time.** A long
  command, such as a `pmemsave` with an absolute path, may not have run
  when a script kills QEMU; use short paths and wait for the result.
- **A list register left invalid with its EOI bit set keeps the GICv3
  maintenance interrupt asserted.** Clear it in the maintenance handler, or
  EL2 takes that interrupt forever.
- **q1n1 must pass the parameter-page address in `x0`** when it enters the
  gaokun3 monitor. The old fallback to a fixed `0x43000000` (QEMU's page)
  is unmapped on the machine and reset it (runs 30 to 32). The QEMU
  hand-off rehearsal now uses a distinct params address so it genuinely
  tests the register rather than the fallback.
- **`llvm-objcopy -O binary` drops `.bss`.** The monitor's `_start` zeroes
  `[__bss_start, __bss_end)`, and q1n1 maps and zeroes the monitor's whole
  image, not only its file bytes, or stale RAM shows up as stage-2 tables
  (run 32).
- **The monitor is built `-mstrict-align`.** With `SCTLR_EL2.M` off, Clang's
  unaligned loads on `.rodata` strings faulted (`ESR 0x96000021`, run 32).
- **The Cortex-A78C lacks `FEAT_TLBIRANGE`.** The stock kernel's range TLBI
  is an unallocated opcode `HCR_EL2.TTLB` cannot trap (it faults as an
  undefined instruction before the trap logic runs). The gaokun3 build
  flips two branches in the kernel's in-memory image to bypass it (runs 37
  to 39); the rewrite is gated to that build and tied to the pinned
  kernelcache hash, and QEMU (which has the feature) boots the kernel
  unmodified.
- **QEMU does not fault `dc zva` on Device memory.** The architecture
  requires an Alignment fault there; QEMU's TCG skips it. A memory-type
  mistake can therefore pass every QEMU boot and stop the machine, as the
  panic region's did until run 41. XNU chooses Normal or Device memory for
  a write-combine mapping by `/chosen/dram-base` and `dram-size`, so a
  region the kernel clears must sit inside them even when it is outside
  `boot_args.memSize`.

## Hardware runs

The owner runs the hardware; an agent builds sticks and checklists, and
reads the photographs. Every run follows
[QUALIFICATION](GAOKUN3-QUALIFICATION.md):

- the owner's explicit go-ahead for that run, on the day;
- AC power, the BitLocker recovery key in hand, Secure Boot off;
- the firmware's one-time boot menu, never a changed boot order;
- only Enter in systemd-boot, because other keys write EFI variables;
- the whole screen photographed, then hold power, remove the stick, and
  confirm Windows starts.

Each run is recorded as a row in
[QUALIFICATION's run table](GAOKUN3-QUALIFICATION.md#runs-so-far): stick
entry, `el2qual.efi` hash and commit, el2qual version, and the result with
its evidence class. Runs 1 to 41 are recorded there.

**Writes that need their own explicit go-ahead**, separate from any
approval of the plan:

- the first write to any USB or SMMU register (the S2CR bypass);
- the first write to the real GIC (Phase 3 step H2);
- any clock, power, PHY, voltage or firmware change;
- PD or UCSI requests: never sent at all.

## Working rules

[AGENTS.md](../AGENTS.md) governs. The points that come up most here:

- Commit titles are `module: imperative change`. Commit only when asked,
  never amend, never push unless asked.
- C files stay below 1,500 lines and 50 KiB. `el2qual.c` has been split
  more than once for this: `uefi.h` with a byte-identical binary as proof,
  `usbstage.c` with byte-identical QEMU serial logs.
- New runtime behaviour is opt-in. A default el2qual build must behave
  exactly as before; prove it by diffing the QEMU serial logs against the
  previous commit's.
- Mark facts VERIFIED, OBSERVED, INFERRED, EXPERIMENTAL or UNIMPLEMENTED,
  and report checks as PASS, FAIL, SKIP or NOT RUN. A QEMU result is never
  a hardware result.
- Commit no vendor material (kernelcaches, `tcblaunch.exe`, firmware,
  disassembly) and nothing that identifies the machine or its owner.
- A personal planning file, `gaokun3-snapintosh-porting-research-corrected.md`,
  sits untracked at the repository root. It is not to be committed.
