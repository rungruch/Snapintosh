# gaokun3 CPU qualification: supervised hardware plan

How to run `probes/el2qual` on a Huawei MateBook E Go 2023 (`gaokun3`,
SC8280XP) to answer the CPU questions of the port's Phase 0A: whether
pointer authentication works, whether the MMU walks 16 KiB tables, whether
NV2, ECV, FGT and RNG exist, and whether the other cores start at EL2.
What el2qual measures and how it was validated under QEMU is in
[FEASIBILITY.md](FEASIBILITY.md#qualifying-a-host-without-q1n1-el2qual).

**Status: NOT RUN.** Nothing in this plan has been executed on a gaokun3.

## Authorization

Each stage below is a separate hardware run. It needs the machine owner's
explicit go-ahead for that stage, given by the person at the machine on the
day. This document does not authorize anything by itself. A coding agent
may build the stick layouts and read the photographed results; it must not
boot or run a stage.

Nothing here changes partitions, firmware images, clocks or voltages, and
nothing writes to the internal disk. The one firmware setting that changes
is Secure Boot, changed by the owner and restored afterwards.

## What el2qual does, and what it cannot protect against

el2qual reads system registers, executes the instructions under test behind
a fault handler, calls PSCI to start and stop CPUs, draws on the UEFI
framebuffer, and stops in a WFE loop. It issues no storage I/O and touches
no clock, regulator, GPU or interrupt controller. Holding the power button
ends any run.

It cannot catch two things:

- A PAuth key access or PAC instruction that EL3 firmware traps
  (`SCR_EL3.{API,APK}` clear). The exception goes to EL3, not to el2qual;
  the firmware may hang or reset.
- A secondary CPU that PSCI starts but that never comes back.

Before each of those steps el2qual draws a breadcrumb line, so after a hang
the last line on screen names the step. Stage 2 runs them last and one at a
time (see below).

**No thermal guard exists for the SC8280XP yet** (porting Phase 1). That is
acceptable only because el2qual programs no clock or voltage and halts within
seconds. Photograph the result and power off promptly; do not leave the
machine halted unattended.

## Prerequisites

1. **AC power** connected for every run (or at least 50% battery, per
   AGENTS.md rule 5). Only this one experiment on the machine at a time.
2. **The BitLocker recovery key**, in hand, not stored on this machine.
   Turning Secure Boot off changes the measured boot state, and Windows may
   ask for the recovery key on its next start. Suspending BitLocker before
   the firmware change avoids the prompt.
3. **Secure Boot off** in the firmware setup. It is required three times
   over: the stick's binaries are unsigned; systemd-boot installs an entry's
   `devicetree` only when Secure Boot is off (systemd `src/boot/boot.c`);
   and the slbounce README makes it mandatory.
4. **An empty FAT32 USB stick** (with a USB-C adapter if needed).
5. **The firmware's one-time boot menu** to pick the stick. Do not change the
   permanent boot order: with the stick removed, the machine must boot
   Windows as before.
6. **A camera.** The screen is el2qual's only output on this machine.

## The screen

el2qual can draw after `ExitBootServices` only by writing pixels to a GOP
framebuffer. The firmware leaves the screen in GOP mode 7, a 2560x1600
landscape twin of the native 1600x2560 panel that its RotateScreen driver
serves through `Blt` only, and reports framebuffer address 0 for it
(OBSERVED by gaokun-android's gk3probe, `docs/hw/gk3probe-e3-20261005.txt`).
A first run on 2026-10-23, built before el2qual handled this, showed only the
firmware console's own line `SNAPINTOSH EL2QUAL: STARTED AT EL1, CALLING
EXITBOOTSERVICES`: el2qual had nothing to draw on, so that run measured
nothing.

el2qual now switches to the largest mode that reports an address, trying
mode 0 (the native 1600x2560) first, and draws everything from then on in
portrait: rotate the tablet or the photo. That mode 0 has an address is
INFERRED (only the rotated modes are documented as `Blt`-only). If no mode
has one, el2qual says so on the firmware console and stops before
`ExitBootServices`, so no Secure Launch is attempted.

## Inputs and their provenance

| File | Stages | Source | Notes |
| --- | --- | --- | --- |
| `el2qual.efi` | all | `probes/el2qual/build.py` from this repository | SHA-256 recorded in the stick's manifest |
| `systemd-bootaa64.efi` | 1b, 2 | an arm64 Linux distribution's systemd-boot package, or the gaokun_build image | not part of this repository |
| `slbounce.efi` | 2 | built from [TravMurav/slbounce](https://github.com/TravMurav/slbounce) (BSD-3-Clause), or the gaokun_build image | must be the driver; the script installs it as `slbounceaa64.efi` |
| `tcblaunch.exe` | 2 | Microsoft; from a Windows installation you are licensed to use | must be old enough (below); never commit it |
| device tree | 1b, 2 | a gaokun3 DTB whose Adreno `zap-shader` node is `disabled` (an EL2 variant) | not needed if slbounce is built with `SLBOUNCE_ALWAYS_SWITCH=1` |

Facts from the slbounce sources (README and `src/bounce_main.c`, read
2026-10-23, not re-verified on hardware):

- slbounce is an EFI driver. It hooks `ExitBootServices` and performs Secure
  Launch when the next application calls it; for this plan that is el2qual.
- It opens `tcblaunch.exe` from the root of the volume it was loaded from.
  Its README says Microsoft removed the error handling it relies on from
  recent `tcblaunch.exe` versions, so an old enough copy is needed. The copy
  in this machine's own `Windows\System32` may be too new. The gaokun3
  community reports a working copy in gaokun_build (OBSERVED, not checked
  here).
- It switches only if the UEFI device-tree configuration table has the first
  `qcom,adreno` node's `zap-shader` child with status `disabled`, unless built
  with `SLBOUNCE_ALWAYS_SWITCH`. The gaokun3 firmware provides no device tree,
  so on this machine the gate is closed unless systemd-boot installs one.
- If authentication or launch fails after `ExitBootServices`, it calls PSCI
  reboot; its README warns the device "will likely hang or reboot".

systemd-boot loads drivers from `\EFI\systemd\drivers\` only when the file
name ends in the architecture suffix plus `.efi` (`src/boot/drivers.c`);
the script uses the AArch64 suffix `aa64` (systemd's convention, not
re-checked against its headers). The slbounce README's example name
`slbouncea64.efi` does not match that suffix and would not be loaded.

## Building a stick

`tools/el2qual-usb.py` checks every input (ARM64 PE images of the right
subsystem, a well-formed DTB) and then writes a directory tree under
`build/el2qual-usb/<stage>/`, which git ignores. It never writes to a disk:
copy the tree onto the stick by hand. `el2qual-usb-manifest.json` records
the SHA-256 of every file placed.

```sh
python3 tools/el2qual-usb.py stage1
python3 tools/el2qual-usb.py stage1b --systemd-boot <systemd-bootaa64.efi> --dtb <gaokun3-el2.dtb>
python3 tools/el2qual-usb.py stage2 --systemd-boot <systemd-bootaa64.efi> \
    --slbounce <slbounce.efi> --tcblaunch <tcblaunch.exe> --dtb <gaokun3-el2.dtb>
```

For a slbounce built with `SLBOUNCE_ALWAYS_SWITCH=1`, pass `--always-switch`
instead of `--dtb` and skip stage 1b. The stage 1b and 2 sticks hold four
systemd-boot entries, run in order as described in stage 2. In the
systemd-boot menu, only select an entry: do not set a default or edit
entries, since those keys write EFI variables.

## Stage 1: el2qual alone (no Secure Launch)

Exposure: an unsigned application started by the firmware. No Secure Launch
is attempted.

1. Build and copy the stage 1 tree. Shut Windows down completely.
2. With AC connected, open the one-time boot menu and choose the stick.
3. el2qual starts, calls `ExitBootServices`, draws its report and halts.
4. Photograph the whole screen. Hold the power button, remove the stick, and
   confirm Windows starts.

Expected screen, in portrait (see "The screen"): first the yellow
breadcrumb `SCREEN: GOP MODE 0 1600X2560 FB <address> FROM MODE 7`, then
the report: `START EL1  AFTER EBS EL1`, `SCREEN GOP MODE 0  FB <address>
FROM MODE 7`, `NOT AT EL2: ONLY THE STARTING VIEW BELOW`, `SLBOUNCE GATE: NO
DEVICE TREE: WILL NOT SWITCH`, an ACPI CPU count and GIC version, the
framebuffer size, and the ID registers as EL1 sees them.

Pass: the report is complete and legible. Record the GOP mode and address,
the framebuffer size, ACPI CPU count and GIC version, and compare the ID
registers with the values Windows caches (MIDR `0x410FD4B0` or
`0x410FD4C0`, `ID_AA64ISAR1 0x01200031`, `ID_AA64MMFR0
0x0000011100101022`; OBSERVED 2026-10-22). A difference is recorded, not
a failure: UEFI runs under Qualcomm's hypervisor and Windows may run under
Hyper-V, and either may present ID registers its own way.

## Stage 1b: systemd-boot with the device tree, no slbounce

Exposure: as stage 1, plus systemd-boot and an installed device tree. This
stage proves the stage 2 stick will open slbounce's gate, without a Secure
Launch attempt.

Run as stage 1, choosing entry 1 in the menu. Pass:
`SLBOUNCE GATE: ZAP-SHADER DISABLED: OPEN`. `NO DEVICE TREE` means
systemd-boot did not install it (is Secure Boot off?). `ZAP-SHADER ENABLED`
or `NO ADRENO ZAP-SHADER` means the DTB is not an EL2 variant. Do not
continue to stage 2 until this passes.

## Stage 2: Secure Launch to EL2

Exposure: Secure Launch through slbounce and `tcblaunch.exe`; a failure
reboots or hangs. Four runs, one per entry, each from a cold boot, each
photographed. Each adds one step to the run before it, so a hang names the
step. Go on to the next entry only when the previous one finished with a
complete report; run 3 is worth doing even if run 2 hung, since it does not
touch PAuth.

| Run | Entry | Options | Adds |
| --- | --- | --- | --- |
| 1 | `el2qual 1: boot CPU, no PAuth` | `nopac nosmp` | the EL2 switch; boot CPU registers, NV2/ECV/FGT/RNG probes, 16K walk |
| 2 | `el2qual 2: boot CPU and PAuth` | `nosmp` | PAuth key access, signing, round trip, `PACGA` on the boot CPU |
| 3 | `el2qual 3: all CPUs, no PAuth` | `nopac` | PSCI `CPU_ON` of every MADT CPU at EL2 |
| 4 | `el2qual 4: all CPUs and PAuth` | none | PAuth on every core |

PAuth comes before PSCI because el2qual measures it on the boot CPU before
it starts any other core: if `CPU_ON` hung in a combined run, the PAuth
answer already taken would never reach the screen.

The last line before the switch is `... CALLING EXITBOOTSERVICES`. Expected
after it: `START EL1  AFTER EBS EL2`, and an `ENTRY HCR_EL2` with E2H (bit
34) and TGE (bit 27) set. The porting plan reads that from slbounce's
`trans.s`; it is not re-verified, and el2qual handles either state.

Reading run 1: the `16K WALK (TRIAL D)` line and its controls (want
`OK FAULT FAULT`); `NV2`, `ECV`, `FGT` and `RNDR`, each with its ID field and
whether the instruction ran; the `PAUTH SIGNS` line reads `NOT RUN`.

Reading run 2: `PAUTH SIGNS (PACIA1716)`, the key and round-trip line, and
`PACGA`, then the two route lines. The breadcrumbs
`PAC: READING APIAKEYLO_EL1...` and `PAC: SIGNING AT EL2...` precede the
steps EL3 could trap.

Reading run 3: the CPU table should list 8 slots, all `DONE`. `PSCI FAIL`
gives the PSCI error; `NO REPLY` means `CPU_ON` succeeded but the core never
reported; `WRONG EL` with an `ENTRY MARK` means PSCI started the core below
EL2 (the low bits of the mark are its `CurrentEL`).

Reading run 4: the PAC column of the CPU table, which should agree across
the A78C and X1C cores.

## When something goes wrong

| What you see | Likely meaning | Then |
| --- | --- | --- |
| Firmware refuses the stick | Secure Boot still on, or not FAT32 | fix and retry the same stage |
| Only the firmware's small `... CALLING EXITBOOTSERVICES` line, nothing after | an `el2qual.efi` built before the screen fix: the firmware's mode 7 has no framebuffer address | rebuild from the current tree and retry |
| `NO GOP MODE HAS A FRAMEBUFFER ADDRESS` and `STOPPED BEFORE EXITBOOTSERVICES` | no GOP mode exposes a framebuffer el2qual can write; nothing was attempted | photograph, stop, report |
| Nothing drawn at all | no GOP at all, or a fault before the first line | photograph, stop, report |
| Reboot right after `CALLING EXITBOOTSERVICES` (stage 2) | Secure Launch failed: `tcblaunch.exe` too new, or slbounce/firmware mismatch | stop; check the `tcblaunch.exe` version |
| `AFTER EBS EL1` with the gate `OPEN` (stage 2) | slbounce did not switch: driver not loaded (file name) or reported Secure Launch unavailable | check the manifest paths; stop |
| Hang with `PAC: READING APIAKEYLO_EL1` last | EL3 traps PAuth key access from EL2 | a result, not a fault: record it |
| Hang with `PAC: SIGNING AT EL2` last | EL3 traps PAC instructions at EL2 | a result: record it |
| Hang with `CPU_ON SLOT n` last | that core did not return from PSCI | record; run 4 would hang the same way |
| A red `V<n> ESR ...` line under a CPU | an unexpected exception on that CPU | photograph closely; stop |

Recovery is the same in every case: hold the power button until the machine
is off, remove the stick, power on. Windows should start as before; if
BitLocker asks, enter the recovery key.

## Recording results

For every run record: the stage and entry, the date, the `el2qual.efi`
SHA-256 from the stick's manifest (and the commit it was built from), the
firmware version shown in the firmware setup, and a photograph of the whole
screen. Crop serial numbers out of any photograph that is shared.

Transcribe the answers into the porting plan. Measured values become
VERIFIED for this machine and firmware version; the plan's INFERRED and
UNKNOWN entries are replaced, not kept beside them. What they decide:

- No core has working PAuth (keys undefined and no pointer changes): stop;
  no route exists.
- A trapped PAuth step is not the same as absent PAuth: record it as
  unmeasured and decide how to measure it before deciding the route.
- 16K walks: the VirtualMac route can use the guest's own tables; if not, it
  needs the shadow layer.
- NV2, ECV, RNG absent as expected: the J700 route needs the Phase 0B
  kernelcache census before it can be judged.

## Afterwards

Remove the stick, turn Secure Boot back on, and resume BitLocker if it was
suspended. Confirm Windows starts.
