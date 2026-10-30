# Porting Snapintosh to the Huawei MateBook E Go 2023 (`gaokun3`, SC8280XP)

Corrected plan, 2026-10-22. It replaces `gaokun3-snapintosh-porting-research.md`.
The hardware inventory of that version is kept. The CPU analysis, the
treatment of the missing nested-virtualization support and the phase order
are rewritten.

**Evidence labels** used below:

| Label | Meaning |
| --- | --- |
| VERIFIED | Measured on the hardware by the project, or checked in source code |
| OBSERVED | Reported by another project's logs or documents; not re-checked here |
| INFERRED | Deduced, for example from a core's Arm architecture version |
| UNKNOWN | Not yet measured; Phase 0A must answer it |

Paths are relative to the three checkouts: `Snapintosh/`, `gaokun_build/` and
`gaokun-android/`.

---

## Summary

Snapintosh runs Apple's stock macOS 27 (26A428) J700 kernel on the
Snapdragon X2 Elite. It runs that kernel at EL1 while the kernel believes it
is at EL2. That rests on four CPU features. The gaokun3's cores lack three of
them, and the fourth is unconfirmed:

| Feature | What Snapintosh uses it for | gaokun3 |
| --- | --- | --- |
| **FEAT_NV / NV2** | Redirects the kernel's EL2 register accesses to memory, traps its `ERET`, and reports EL2 in its saved modes | absent (VERIFIED, el2qual at EL2) |
| **FEAT_ECV** | Traps counter reads so the 19.2 MHz counter can be shown to the guest as 24 MHz | absent (VERIFIED, el2qual at EL2) |
| **FEAT_RNG (RNDR)** | Seeds the monitor's pointer-authentication keys | absent (VERIFIED, el2qual at EL2) |
| **PAuth** | macOS is compiled with pointer authentication throughout | works on all 8 cores (VERIFIED, el2qual at EL2) |

Whether a port is possible, and which Apple kernel it would run, is decided in
Phase 0, before any bring-up work. Three outcomes:

- **No PAuth:** stop. macOS cannot run on this CPU by any route.
- **Route A:** keep the J700 kernel. This needs a kernel-rewriting layer much
  wider than the project's current patch policy allows.
- **Route B:** switch to Apple's VirtualMac2,1 kernel. It runs at EL1 and does
  not need nested virtualization, but it needs a new guest device model.

**Phase 0A's answer (2026-10-23, VERIFIED on this gaokun3, firmware version
not yet recorded):** PAuth works and the 16 KiB granule walks on all 8 cores;
NV2, ECV and RNDR are absent. So the "No PAuth" stop does not apply, Route B
has every CPU feature it needs, and Route A is blocked on the CPU side until
Phase 0B sizes the rewriting it would take.

---

## What changed from the previous version

| Previous claim | Correction | Evidence |
| --- | --- | --- |
| gaokun3 "lacks 16 KiB; 4 KiB / 64 KiB only" | Not measured. That was the A16's result. Arm's Cortex-A cores normally implement all three granules, so 16 KiB is likely present. It decides whether Route B is possible, so it must be measured. | A16: VERIFIED (`Snapintosh/docs/FEASIBILITY.md`). gaokun3: walks on all 8 cores (VERIFIED, el2qual stage 2 run 8, 2026-10-23) |
| Cores are "Cortex-X1 + Cortex-A78" | Cortex-X1C (part `0xd4c`) + Cortex-A78C (part `0xd4b`), Armv8.2-A. The C variants matter: Arm lists PAuth for them, not for plain X1/A78. | OBSERVED: dmesg `0x410fd4b0` in `gaokun-android/device/huawei/gaokun3/BoardConfig.mk`; `0xd4c` in `gaokun-android/docs/archive/TODO-history-2026-10.md` |
| Missing NV2 means EL2 register accesses need patching or trapping | FEAT_NV provides three things, and both proposed fixes cover only the first. See "Why the A16 mechanism does not transfer". | VERIFIED in source: `Snapintosh/probes/xnuboot.c:12415`, `Snapintosh/compat/exception.c:80-95` |
| PAuth is a Phase 0 check | PAuth is the go/no-go. Linux on these machines shows no `paca`/`pacg`, but every gaokun3 boot line (and the usual X13s one) passes `arm64.nopauth`, which tells Linux to ignore PAuth. So that absence says nothing, and the flag's presence hints that Linux saw PAuth advertised and something failed without it. | OBSERVED: Gentoo wiki X13s `lscpu`; `arm64.nopauth` in `gaokun_build/scripts/ci/50_make_image_ubuntu.sh:150` and the gaokun-android boot lines |
| ECV and RNDR not mentioned | The monitor refuses to start without either. | VERIFIED: `Snapintosh/probes/xnuboot.c:6063` (ECV, returns -15), `:5994` (RNDR, returns -24 through `pauth_domain_prepare`) |
| "Build an EFI chainload payload using slbounce" | slbounce is a UEFI driver that switches to EL2 inside the calling app's `ExitBootServices`. It has a device-tree gate, it returns with `HCR_EL2.E2H` set, and a failed launch reboots. See Phase 2. | OBSERVED: slbounce `src/bounce_main.c`, `src/trans.s` (read, not run) |
| a6xx "distinct" packet types | Both generations use PM4 type-4/type-7 packets; the plug-in already emits type-7. The gap is register layout and state programming. | VERIFIED: `cs_pkt7` in `Snapintosh/gpu/adreno-kext/plugin/AdrenoMetal/AdrenoDrawCommands.m:238` |
| a6xx GMU boot "in AdrenoX2" | GPU power-up (RPMh rails, clocks, GMU boot, HFI, OPP table) is done by q1n1, not the kext. | VERIFIED: `Snapintosh/docs/ARCHITECTURE.md:179-180` |
| GPU work lists the kext, plug-in and compiler | The monitor's command-buffer register protect table also needs an a690 version. It is GPL-2.0-only. | VERIFIED: `Snapintosh/compat/adreno-ib-tables.h` header |
| No safety phase | The thermal guard and the battery threshold are mandatory (AGENTS.md rule 5). The A16 guard reads A16-only sensors. | VERIFIED: `Snapintosh/compat/a16-thermal.h` |
| Touchscreen needs a "custom SPI HID kext ... IOGPU/HID events" | The Himax HX83121A sends raw frames; the touch points are computed by an algorithm in the Linux driver. That is far more than a HID kext, and the driver's licence must be checked before reuse. The event path is IOHIDFamily, not IOGPU. | OBSERVED: `gaokun_build/README.md` (EGoTouchRev-Linux credit), `gaokun-android/README.md` |
| Keyboard: "macOS's standard AppleUSBXHCI stack" can attach | The keyboard is behind a Qualcomm DWC3 platform xHCI controller. macOS's XHCI drivers are not expected to attach to it, so a driver or monitor-side forwarding is needed. | INFERRED |
| GOP "Mode 7 (2560x1600 / 1600x2560, 32-bit BGRA)" | No source in either repository. Record the real GOP mode in Phase 0A. | Checked: no match in `gaokun-android/docs` |
| `wcnhpbtfw21.tlv`, `wcnhpnv21g.bin` listed under networking | These are Bluetooth firmware. Wi-Fi needs the ath11k files and a board file; the community remaps the `HW_GK3` variant to X13s calibration. | OBSERVED: `gaokun-android/docs/hw-inventory.md` §2, `gaokun-android/docs/stage0-findings.md` §7 |
| ir3 a6xx `chip_id = 0x06060000` | Verify against the a690 entry in Mesa's freedreno device table before relying on it. | UNKNOWN |

---

## Hardware comparison

| Area | ASUS Zenbook A16 (X2E-96-100) | MateBook E Go 2023 (SC8280XP) |
| --- | --- | --- |
| CPU | Qualcomm Oryon, MIDR `0x512f0021` (VERIFIED) | 4x Cortex-A78C r0p0 (`MIDR 0x410FD4B0`, MPIDR `0x000`-`0x300`) + 4x Cortex-X1C r0p0 (`0x410FD4C0`, `0x400`-`0x700`), Armv8.2-A, no SVE; MIDR is the only ID register that differs between them (VERIFIED, el2qual stage 2) |
| FEAT_NV / NV2 | NV2, `ID_AA64MMFR2_EL1.NV = 2` (VERIFIED) | absent: `NV = 0` at EL1 and EL2, `MRS VNCR_EL2` undefined at EL2 (VERIFIED, el2qual stage 2) |
| FEAT_ECV | present, `ID_AA64MMFR0_EL1 = 0x2100122100001024` gives ECV = 2 (VERIFIED) | absent: `ECV = 0`, `MRS CNTPOFF_EL2` undefined at EL2 (VERIFIED, stage 2); FGT likewise absent (`HFGRTR_EL2` undefined) |
| FEAT_RNG | present; the monitor depends on it (VERIFIED) | absent: `RNDR = 0`, `MRS RNDR` undefined at EL2 (VERIFIED, stage 2) |
| PAuth | QARMA3, `APA3 = 5`, `GPA3 = 1` (VERIFIED) | works on all 8 cores: `ID_AA64ISAR1_EL1 = 0x01200031` (APA 3: PAuth2, QARMA5, no FPAC; GPA 1) at EL1 and EL2; at EL2 `PACIA1716` signs, the key registers exist, sign then authenticate round-trips, `PACGA` runs, and no step traps to EL3 (VERIFIED, el2qual stage 2) |
| 16 KiB granule | absent, trial D faults (VERIFIED) | walks at stage 1 on all 8 cores: trial D resolves to `0x4001000` with all three controls right (VERIFIED, el2qual stage 2 run 8); advertised `ID_AA64MMFR0_EL1.TGran16 = 1`, stage-2 field 0. The first stage 2 walks were inconclusive until the probe cleaned its table to the point of coherency |
| EL2 entry | q1n1 runs at EL2 after UEFI (VERIFIED) | UEFI runs at EL1 under Qualcomm's hypervisor; slbounce + `tcblaunch.exe` 10.0.22621.382 reach EL2 with `HCR_EL2 = 0x488000000` (E2H, RW, TGE), and PSCI 1.1 `CPU_ON` starts all 7 secondaries at EL2 (VERIFIED, el2qual stage 2) |
| Host link | q1n1 USB NCM (VERIFIED) | candidate: DWC3 `a600000.usb` (`usb_0`, port near the power button, the only port with a device role) (OBSERVED: `gaokun-android/docs/hw-inventory.md:737-741`) |
| GPU | Adreno X2-90, a8xx, chip ID `0x44070041` (VERIFIED, air2ir3) | Adreno 690, a6xx family; firmware `a660_sqe.fw`, `a660_gmu.bin` (OBSERVED: hw-inventory §2, §4) |
| Display | eDP 2880x1800, DPU interface 5 on glymur (VERIFIED: `compat/dpu-intf.h`) | MIPI-DSI 1600x2560 portrait, `DSI-1`, 120 Hz (vtotal 2736) and 60 Hz (vtotal 5472); bonded dual DSI with DSC (OBSERVED: hw-inventory §3, gaokun_build display notes). The firmware's GOP modes report no framebuffer address (OBSERVED, el2qual stage 1); the DT `simple-framebuffer` at `0xc6200000` (1600x2560 `a8r8g8b8`) shows what is written to it after ExitBootServices (VERIFIED, el2qual stage 1b) |
| Keyboard, touchpad | GENI I2C-HID (VERIFIED) | USB HID `12d1:10b8` on DWC3 `a400000.usb`, `3-3` (OBSERVED: hw-inventory.md:741) |
| Touchscreen | none in scope | Himax HX83121A on SPI `998000.spi`, raw frames plus host-side algorithm (OBSERVED) |
| Storage | NVMe behind the emulated ANS2 (VERIFIED) | NVMe, not UFS (OBSERVED: gaokun-android README) |
| Wi-Fi | QCC2072, ath12k (VERIFIED) | WCN6855, ath11k at `0006:01:00.0`, `chip_id 0x12` (OBSERVED: hw-inventory.md:676) |
| Battery | queried through q1n1 (VERIFIED) | Huawei EC (upstream `huawei-gaokun-ec`); `i2c-15` at `0x38` per the linux-gaokun reference (OBSERVED) |
| Thermal sensors | 92 A16/glymur sensors in `compat/a16-thermal.*` (VERIFIED) | SC8280XP tsens; not ported (UNKNOWN layout until taken from the upstream DTS) |
| RAM | | 16 GB, top of physical memory `0x87fffffff`, inside the monitor's 36-bit stage-2 limit (OBSERVED: TODO-history; limit VERIFIED in `docs/COMPATIBILITY.md`) |

---

## Why the A16 mechanism does not transfer

On Apple silicon the macOS kernel runs at EL2 with VHE. Snapintosh runs it at
EL1 and uses FEAT_NV to keep the illusion. FEAT_NV does three separate jobs on
the A16:

1. **EL2 register accesses become memory accesses.** With `HCR_EL2.{NV,NV2}`
   and a `VNCR_EL2` page per CPU, the kernel's accesses to EL2 registers land
   in memory the monitor can read (`probes/xnuboot.c`, around line 6343). This
   includes `HCR_EL2` itself: `docs/COMPATIBILITY.md` notes that a zero
   virtual HCR "would select XNU's VM-exit path on ordinary kernel
   exceptions", so the kernel reads it on its exception path.
2. **`ERET` traps to EL2.** "HCR.NV traps ERET from the nested host kernel"
   (`probes/xnuboot.c:12415`, exception class `0x1a`).
3. **Saved modes report EL2.** The kernel returns with EL2t/EL2h in `SPSR`,
   and the monitor rewrites those to EL1t/EL1h on the trapped `ERET`
   (`compat/exception.c:80-95`: "NV reports EL2 in the saved mode").

On the Armv8.2 cores, without FEAT_NV:

- An EL2 register access from EL1 is undefined and is taken to EL1. The
  hypervisor never sees it.
- An `ERET` with an EL2 mode in `SPSR` is an illegal exception return. It
  cannot be trapped: the fine-grained `ERET` trap needs FEAT_FGT (Armv8.6).
- `CurrentEL` reads EL1.

The previous version's two fixes therefore fall short:

- **Rewriting EL2 register accesses** must also cover every `ERET` that can
  carry an EL2 mode, every place the kernel builds such an `SPSR`, and every
  `CurrentEL` read. That is a broad, kernel-wide rewrite. `docs/KERNEL-PATCHES.md`
  admits patches only for hot traps the monitor cannot make cheap, so Route A
  needs an explicit, reviewed exception to that policy.
- **Making the kernel's vector page execute-never** sends every exception
  (system calls, interrupts, page faults) through EL2, and still cannot fix
  `ERET`.

The other two gaps:

- **ECV.** The monitor rescales the 19.2 MHz host counter to the guest's
  24 MHz with ECV's `CNTHCTL_EL2` traps (`probes/xnuboot.c:6362`). Without
  ECV, guest counter reads cannot be trapped. The alternative is a native
  19.2 MHz guest timebase. Whether the J700 kernel and macOS user space accept
  that is unknown; `docs/COMPATIBILITY.md` says direct `CNTFRQ` consumers in
  scaled images are rejected, which suggests some exist.
- **RNDR.** It only seeds the PAuth key domain. A seed supplied by the host
  through q1n1, or from an SoC random source, replaces it. This is a small
  change.

---

## Routes

**Route A: the J700 kernel, as on the A16.** Reuses the SPTM/TXM layer,
`shadow/`, the Apple device models and the guest profile `j700ap-26A428`.
Needs the NV replacement from Phase 0B, a timer solution, an RNDR
replacement, and PAuth.

**Route B: the VirtualMac2,1 kernel as a second guest profile.** Apple's
kernel for Virtualization.framework guests runs at EL1, so it needs no
nested virtualization. It comes from the same UniversalMac restore image;
`tools/extract-firmware.py` takes a board name (confirm the device class in
`BuildManifest.plist`). It needs:

- PAuth;
- a 16 KiB walk, or the shadow tables;
- a different device model: a GICv3 interface, virtio devices, a PL011 UART
  and the vmapple-specific devices, instead of AIC, ANS2 and DART.

Whether this kernel uses SPTM on macOS 27 is UNKNOWN; Phase 0B checks it.
`docs/COMPATIBILITY.md` ("Before a second guest or host") lists what a second
profile requires.

**Route C: an outside feasibility check only, not a Snapintosh route.** Linux
at EL2 through slbounce, KVM and QEMU's `vmapple` machine.
`aquarat/apple-macos-arm64-vm-guest` boots macOS 13 and 26 that way, but only
on M1 hosts with a 16 KiB-page host kernel (OBSERVED). Its KVM patches carry
Apple-only PAC key registers (`APCTL_EL1`, `KERNKEYLO/HI_EL1`), so a
non-Apple version is needed, and its `LICENSING.md` marks some inherited
files as unlicensed (all rights reserved). Do not copy those files.

---

## Phases

Every phase lists its acceptance evidence. Report each check as PASS, FAIL,
SKIP or NOT RUN, as AGENTS.md requires.

### Phase 0A: Hardware measurement (go/no-go)

**Goal:** replace every UNKNOWN and INFERRED CPU fact above with a
measurement, on every core.

1. **Quick look.** Boot an EL2 Linux kernel through slbounce (the community
   published EL2 builds, for example the 2026-05-14 images listed in
   `gaokun-android/docs/stage0-findings.md:308-311`). At EL2, Linux reads
   the raw ID registers. Two boots:
   - **As shipped, with `arm64.nopauth`:** Linux logs each PAuth ID field as
     `forced to 0` (the hardware reports it present) or `already set to 0`
     (absent), per `arch/arm64/kernel/cpufeature.c`.
   - **With `arm64.nopauth` removed:** `/proc/cpuinfo` lists `paca`/`pacg`,
     and a user-space `PACIA1716` test shows whether signing works.

   The scripts in `~/Downloads/gaokun3-phase0a/` collect both. Neither boot
   measures the 16 KiB walk.
2. **Full probe.** An EL2 probe launched from systemd-boot after slbounce,
   measuring on each core:
   - the ID registers as seen at EL1 before `ExitBootServices` and at EL2
     after it;
   - whether pointer signing changes a pointer, whether the key registers
     exist, and whether sign-then-authenticate returns the original pointer;
   - the four-trial 16 KiB walk from `probes/tgran16.c`;
   - whether `VNCR_EL2` (NV2), `CNTPOFF_EL2` (ECV), `HFGRTR_EL2` (FGT) and
     `RNDR` exist;
   - PSCI `CPU_ON` and `CPU_OFF` at EL2;
   - the GOP mode and format, and whether slbounce's device-tree gate is open.

   Output goes to the screen; the gaokun3 has no serial console.

   Status: el2qual is finished and committed (Snapintosh `a83d95b`),
   validated under QEMU on seven configurations, and run under the
   supervised plan `Snapintosh/docs/GAOKUN3-QUALIFICATION.md`, which holds
   the run log. On 2026-10-23 stages 1 and 1b ran (EL1 only, no Secure
   Launch): the screen works through the device tree's framebuffer, and
   the EL1 view of the ID registers is recorded above. Stage 2 then ran
   all four entries at EL2: PAuth works on all 8 cores, NV2, ECV, FGT and
   RNDR are absent, and every core starts at EL2. Its first 16 KiB walks
   were inconclusive (the control faulted on hardware); with `tgran16.c`
   cleaning the table to the point of coherency before each walk, a rerun
   of entry 3 showed the 16 KiB granule walking on all 8 cores. Phase 0A
   is done, apart from recording the firmware version. Step 1, the Linux
   quick look, was not needed.
3. **Validate under QEMU first:** `-cpu max` as the positive control,
   `-cpu max,pauth=off` as the negative, and `-cpu cortex-a76` as the
   nearest Armv8.2 model.

**Decision rules:**

- **PAuth:** if no core has the key registers and signing never changes a
  pointer, stop: no route exists.
- **16 KiB:** if it walks, Route B can use the guest's own tables; if not,
  Route B needs the shadow layer.
- **NV2, ECV, RNDR:** absent as expected means Route A goes through Phase 0B.
  Present would mean the inference above was wrong; update this plan.

**Safety:** the probe is meant to read registers and execute test
instructions only, with no storage, clock, voltage or GPU access. A failed
Secure Launch reboots the machine. After drawing its results the probe is
meant to halt, so a power-button hold ends the run.

### Phase 0B: Offline kernelcache census

**Handover:** `Snapintosh/docs/GAOKUN3-PHASE0B.md` holds the task list,
methods, inputs and the findings record; it supersedes the outline below
where they differ.

**Goal:** know the size of Route A before committing to it, and settle the
open Route B questions. Uses the locally extracted 26A428 kernelcache, which
is never committed, and the existing tools: `tools/kc-analyze.py`,
`tools/kc-disas.py`, `tools/kc-xref.py`, `tools/kc-symbolicate.py`.

For the J700 kernel, count and locate:

- `MRS`/`MSR` with EL2 (`op1 = 4`) and `_EL12`/`_EL02` (`op1 = 5`) encodings;
- `MRS CurrentEL`;
- `ERET` sites, and every place an `SPSR` value with an EL2 mode is built or
  stored (for example initial thread state);
- which of these sit on hot paths: exception vectors, context switch, system
  calls. Use the A16 trap census where it exists.
- consumers of `CNTFRQ` and the timebase, to decide whether a native
  19.2 MHz guest is possible.

Repeat the scan over the AuxKC kexts.

For the VirtualMac2,1 kernel:

- whether it has SPTM call stubs or a `__DATA_SPTM` segment;
- the devices its device tree expects;
- its timebase source.

**Output:** a table of counts with hotness, and a written judgement on
whether a rewrite can follow the KERNEL-PATCHES.md rules: found by
instruction pattern, never by address, opt-in, pinned to the image digest,
with a reviewed site closure.

### Phase 0C: Route decision

Combine 0A and 0B into a written decision record with the evidence: Route A,
Route B, or stop. Optionally run Route C first, to learn whether macOS boots
on these cores at all before investing in Route B. Nothing after this point
starts without the record.

### Phase 1: Safety guards

**Goal:** the guards exist and are tested before anything programs clocks,
the GPU or storage. Their code and host tests come first; they are switched
on in hardware as soon as Phase 2's link works, and every later phase
requires them.

- **Thermal guard.** An SC8280XP version of `compat/a16-thermal.*`. Take
  the sensor addresses from the upstream `sc8280xp.dtsi`, record the SoC
  revision and the source, read only. Same behaviour as on the A16: stop the
  run at the limit and refuse to start without a complete sample, with the
  DVFS governors clamping from 75 °C.
- **Battery and AC guard.** Through the Huawei EC, read only. Keep
  `tools/cycle.py`'s refusal below 50 % battery without AC.
- **Carried over unchanged:** the GPU DVFS ceiling and the 1 MiB `CP_MEMCPY`
  limit.
- **Host tests,** including deliberately faulty sensor and EC readings that
  the guards must refuse (AGENTS.md rule 6).

### Phase 2: EL2 entry and host link

**Goal:** q1n1 runs at EL2 on the gaokun3 and serves its proxy over USB.

- **Entry.** systemd-boot loads slbounce from `EFI/systemd/drivers/`. q1n1's
  own `ExitBootServices` call triggers the switch.
- **slbounce constraints** (OBSERVED in its source):
  - it switches only if the device tree installed as a UEFI configuration
    table disables the Adreno zap shader, unless slbounce is built with
    `SLBOUNCE_ALWAYS_SWITCH`;
  - it needs an old enough `tcblaunch.exe` (gaokun_build ships one verified
    to work);
  - a failed launch reboots the machine;
  - it returns with `HCR_EL2 = E2H | TGE`, `VBAR_EL2` still pointing at
    UEFI's EL1 vectors, and UEFI's translation tables copied into the EL2
    registers.
- **E2H.** q1n1 and the monitor assume a non-VHE EL2 (comment at
  `probes/xnuboot.c:6356`). q1n1 must either clear `E2H` with a safe MMU-off
  sequence or become VHE-aware.
- **USB.** Device mode on DWC3 `a600000.usb`. Under Linux the Type-C role
  and orientation go through the EC and UCSI; whether q1n1 must set them
  itself is an open question.
- **Board adapter.** Implement resource discovery against `HostRuntime`, and
  keep reset, USB, interrupt-controller and display operations behind the
  adapter (`docs/COMPATIBILITY.md`, "Before a second guest or host").

**Acceptance:** the proxy answers over NCM, memory reads and writes work,
a new q1n1 generation chainloads, and `probes/run.py idregs` and
`probes/run.py tgran16` run. Their results must match Phase 0A's.

### Phase 3: Monitor bring-up (route-specific)

**Route A:**

- the device tree from `probes/appledt.py`, adapted to the SC8280XP memory
  map and UEFI's reserved regions;
- SC8280XP GICv3 interrupts forwarded to the AIC model (`compat/irq-forward.c`);
- CPU start through PSCI;
- the timer solution and RNDR replacement;
- the NV replacement mechanism decided in 0B.

The shadow MMU works either way; a host with 16 KiB support could simplify
it later.

**Route B:** a new guest profile and device model; no SPTM layer if 0B
confirms the kernel does not use one.

**Acceptance:** the kernel prints to the GOP framebuffer console
(`probes/fbconsole.py`) and stops cleanly on request.

### Phase 4: Storage

- `compat/nvme-host.c` on the PCIe controller UEFI left running; record
  which one.
- Boot from a RAM disk first (`probes/run.py --ramdisk`); no disk writes.
- Installing to the internal NVMe changes the partition table. That is
  destructive (AGENTS.md rule 5): it needs your explicit authorization, a
  full backup and a tested recovery path. The disk may hold Windows, Linux or
  Android, and no factory image of this machine exists (gaokun-android
  README).

**Acceptance:** single-user boot from the installed volume.

### Phase 5: Display and input

- **Display.**
  - Keep the A16 approach of copying finished frames into the scanout
    buffer UEFI left running.
  - Decide how to handle the portrait 1600x2560 panel.
  - Beam sync needs the SC8280XP DSI interface timing offsets, since
    `compat/dpu-intf.h` describes the A16's eDP interface. It also has to
    allow for two DSI interfaces with DSC.
  - Start at 60 Hz.
- **Keyboard and touchpad.** USB HID behind the DWC3 xHCI on `a400000.usb`.
  Choose between a platform xHCI driver in the guest and forwarding through
  the monitor.
- **Touchscreen.** Last. Check the licence of the Linux Himax driver and its
  algorithm before any reuse; expect to reimplement it.

### Phase 6: GPU (Adreno 690)

- **q1n1:** a690 power-up: RPMh votes, clocks, GMU boot with the a660-family
  firmware, HFI and the OPP table.
- **Monitor:**
  - an a690 register protect table, derived from the a690 entry in Linux's
    `a6xx_catalog.c` the same way the X2 table was. It stays GPL-2.0-only,
    in its own file.
  - SMMU mediation for the SC8280XP.
- **Kext and plug-in:** an a6xx register header generated from Mesa, as
  `AdrenoA8xxRegs.h` was, and state programming for a6xx. Mesa's freedreno
  and Turnip code shared across a6xx to a8xx is the reference;
  `gpu/air2ir3/src/a8xx_sampler.c` already follows Mesa's
  `fd6_sampler_state_create<A8XX>`.
- **air2ir3:** target the a690's Mesa device entry; confirm its chip ID.
- **Acceptance:** the A16's pixel-probe desktop scenes pass with zero
  mismatching tiles, with the thermal guard on and inside the DVFS ceiling.

### Phase 7: Wi-Fi and Bluetooth (later)

WCN6855 (ath11k) instead of QCC2072 (ath12k). The MHI transport is related.
The board file needs the `HW_GK3` remap the Linux community uses.

---

## Open questions

1. ~~Does SC8280XP silicon implement PAuth?~~ Yes, on all 8 cores (stage
   2). Still open: why Linux boots here need `arm64.nopauth`, presumably a
   problem under Qualcomm's hypervisor at EL1. (0A)
2. ~~Does it walk 16 KiB tables?~~ Yes, on all 8 cores (stage 2 run 8).
3. ~~Do secondary cores start at EL2 through PSCI after Secure Launch?~~
   Yes (stage 2).
4. How many EL2 accesses, `CurrentEL` reads and EL2-mode `ERET`s does the
   J700 kernel have, and how hot are they? (0B)
5. Does the VirtualMac2,1 kernel use SPTM on macOS 27? (0B)
6. Can the guest run on a native 19.2 MHz timebase? (0B)
7. Can q1n1 put `usb_0` in device mode without the EC and UCSI? (2)

## Hardware status

Phase 0A done, apart from recording the firmware version. On 2026-10-23
el2qual ran on a gaokun3 at EL1 (stages 1 and 1b) and at EL2 after Secure
Launch (stage 2: all four entries, then entry 3 again with the fixed 16 KiB
walk); it wrote nothing but the screen. Nothing else in this plan has been
executed on a gaokun3. Next: Phase 0B, offline.

## References

- Snapintosh: `docs/FEASIBILITY.md`, `docs/BOOT-CHAIN.md`,
  `docs/COMPATIBILITY.md`, `docs/KERNEL-PATCHES.md`, `docs/ARCHITECTURE.md`,
  `probes/xnuboot.c`, `compat/exception.c`, `compat/adreno-ib-tables.h`,
  `compat/a16-thermal.h`, `probes/tgran16.c`
- gaokun_build: `README.md`, `docs/el2_kvm_guide_en.md`, `tools/el2/`
- gaokun-android: `README.md`, `docs/hw-inventory.md`,
  `docs/stage0-findings.md`, `device/huawei/gaokun3/BoardConfig.mk`
- slbounce: https://github.com/TravMurav/slbounce (`src/bounce_main.c`,
  `src/trans.s`)
- aquarat/apple-macos-arm64-vm-guest:
  https://github.com/aquarat/apple-macos-arm64-vm-guest
- Gentoo wiki, Lenovo ThinkPad X13s (`lscpu` flags):
  https://wiki.gentoo.org/wiki/Lenovo_ThinkPad_X13s
- Linux arm64 CPU feature registers:
  https://docs.kernel.org/arch/arm64/cpu-feature-registers.html
