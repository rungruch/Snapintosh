# gaokun3 CPU qualification: supervised hardware plan

How to run `probes/el2qual` on a Huawei MateBook E Go 2023 (`gaokun3`,
SC8280XP) to answer the CPU questions of the port's Phase 0A: whether
pointer authentication works, whether the MMU walks 16 KiB tables, whether
NV2, ECV, FGT and RNG exist, and whether the other cores start at EL2.
What el2qual measures and how it was validated under QEMU is in
[FEASIBILITY.md](FEASIBILITY.md#qualifying-a-host-without-q1n1-el2qual).

**Status: stages 1 to 3 run** on one gaokun3, firmware 2.18 (see "Runs so
far"). Each new stage still needs its own go-ahead.

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
a fault handler, calls PSCI to start and stop CPUs, draws on a GOP
framebuffer or, on this machine, the device tree's framebuffer in reserved
memory (see "The screen"), and stops in a WFE loop. It writes no other
memory it does not own, issues no storage I/O and touches no clock,
regulator or GPU. Since el2qual 2 (stage 3) it reads each core's GICv3 CPU
interface registers at EL2 (`ICC_SRE_EL2`, `ICH_VTR_EL2`, `ICH_HCR_EL2`).
Where that interface is off (`ICH_HCR_EL2.En` clear), it also writes list
register 0 with an invalid entry and restores it at once. An invalid entry
cannot signal, and the distributor and redistributors are never touched.
Holding the power button ends any run.

It cannot catch three things:

- A PAuth key access or PAC instruction that EL3 firmware traps
  (`SCR_EL3.{API,APK}` clear). The exception goes to EL3, not to el2qual;
  the firmware may hang or reset.
- A GIC system-register access that EL3 traps (`ICC_SRE_EL3.Enable`
  clear), with the same consequence.
- A secondary CPU that PSCI starts but that never comes back.

Before each of those steps el2qual draws a breadcrumb line, so after a hang
the last line on screen names the step. Stage 2 runs them last and one at a
time (see below).

**No thermal guard runs on the SC8280XP yet.** The Phase 1 policy is
host-tested only and unpinned
([GAOKUN3-PHASE1-GUARDS.md](GAOKUN3-PHASE1-GUARDS.md)). That is acceptable
only because el2qual programs no clock or voltage and halts within seconds.
Photograph the result and power off promptly; do not leave the machine
halted unattended.

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

el2qual then learned to switch to the largest mode that reports an address,
mode 0 (the native 1600x2560) first. A second run on 2026-10-23, from a
stage 1 stick whose `el2qual.efi` was built from commit `96e3d2c` (SHA-256
`3323c2e81227192df1764a92d719125b2c63f3798c57cc02a519705e56b1d3b4`, read
back from the stick's manifest), showed
`NO GOP MODE HAS A FRAMEBUFFER ADDRESS (MODE 7)` and `STOPPED BEFORE
EXITBOOTSERVICES: NOTHING WAS MEASURED` on the firmware console. OBSERVED
for this firmware: after `SetMode`, no other GOP mode reported an address
either, or `SetMode` failed for them; el2qual does not record which.

So el2qual now has a second source. When no GOP mode has an address, it
draws on the device tree's `/chosen` simple-framebuffer, the record Linux
uses for the same buffer: the gaokun3 EL2 DTB has
`/chosen/framebuffer@c6200000`, 1600x2560, stride 6400 bytes, `a8r8g8b8`,
size `0xfa0000` (VERIFIED by parsing the DTB with `probes/el2qual/firmware.c`).
It draws there only if the node is self-consistent and lies inside one
`Reserved` region of the firmware's memory map; the recorded map has
`Reserved 0xc6200000-0xc85fffff` (OBSERVED, gk3probe). It cleans every draw
to the point of coherency, since the display reads memory, not the CPU's
caches. The report says `SCREEN DT SIMPLE-FB  FB 00000000C6200000  NO GOP
ADDRESS`, in portrait: rotate the tablet or the photo.

Two consequences:

- The device tree comes from systemd-boot, so a stage 1 stick (el2qual
  alone) can never draw. Stage 1b is the first stage that can.
- Pixels written there appear on the panel and stay after
  `ExitBootServices`: VERIFIED by the stage 1b run of 2026-10-23 (see
  "Runs so far"), whose whole report was drawn there after it. Whether the
  firmware redraws the buffer before `ExitBootServices` was not observed,
  and does not affect the report. Before the first write el2qual still
  prints `SCREEN: DT SIMPLE-FB 1600X2560 FB 00000000C6200000, DRAWING` on
  the firmware console, so a fault on that write would leave the line as
  the last one on screen.

With neither source, el2qual says why on the firmware console (`DT
FRAMEBUFFER: <reason>`) and stops before `ExitBootServices`, so no Secure
Launch is attempted.

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
3. el2qual starts, finds nothing it can draw on (see "The screen"), says
   so on the firmware console and halts before `ExitBootServices`.
4. Photograph the whole screen. Hold the power button, remove the stick, and
   confirm Windows starts.

Expected screen, on the firmware console: `NO GOP MODE HAS A FRAMEBUFFER
ADDRESS (MODE 7)`, `DT FRAMEBUFFER: NO DEVICE TREE`, `STOPPED BEFORE
EXITBOOTSERVICES: NOTHING WAS MEASURED`.

Pass: exactly that, and Windows starts afterwards. Stage 1 now shows only
that the application runs and stops safely on this firmware; the second
run of 2026-10-23 showed all of it except the `DT FRAMEBUFFER` line, which
that build did not have.

## Stage 1b: systemd-boot with the device tree, no slbounce

Exposure: as stage 1, plus systemd-boot and an installed device tree, and
el2qual's first write to the device tree's framebuffer. No Secure Launch is
attempted. This stage proves the stage 2 stick will open slbounce's gate,
and is the first that can draw a report.

Run as stage 1, choosing entry 1 in the menu.

Expected screen, in portrait: the console line `SCREEN: DT SIMPLE-FB
1600X2560 FB 00000000C6200000, DRAWING`, then el2qual's own large text:
`START EL1  AFTER EBS EL1`, `SCREEN DT SIMPLE-FB  FB 00000000C6200000  NO
GOP ADDRESS`, `NOT AT EL2: ONLY THE STARTING VIEW BELOW`, the slbounce gate,
an ACPI CPU count and GIC version, the framebuffer size, and the ID
registers as EL1 sees them.

Pass: the report is complete and legible, and shows `SLBOUNCE GATE:
ZAP-SHADER DISABLED: OPEN`. `NO DEVICE TREE` means systemd-boot did not
install it (is Secure Boot off?). `ZAP-SHADER ENABLED` or `NO ADRENO
ZAP-SHADER` means the DTB is not an EL2 variant. Do not continue to stage 2
until this passes.

Record the screen line, the framebuffer size, ACPI CPU count and GIC
version, and compare the ID registers with the values Windows caches (MIDR
`0x410FD4B0` or `0x410FD4C0`, `ID_AA64ISAR1 0x01200031`, `ID_AA64MMFR0
0x0000011100101022`; OBSERVED 2026-10-22). A difference is recorded, not a
failure: UEFI runs under Qualcomm's hypervisor and Windows may run under
Hyper-V, and either may present ID registers its own way.

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
after it: `START EL1  AFTER EBS EL2`, and `ENTRY HCR_EL2 0000000488000000`:
E2H (bit 34), RW (bit 31, which these cores fix at 1) and TGE (bit 27),
as slbounce's `trans.s` sets them (VERIFIED, runs 4 to 7). el2qual
handles either E2H state.

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

## Stage 3: counter, GIC and firmware (el2qual 2)

Phase 0C's checks ([GAOKUN3-PHASE0C.md](GAOKUN3-PHASE0C.md#checks-before-phase-1))
need three answers that stages 1 to 2 did not record:
- `CNTFRQ_EL0`, the counter rate a VirtualMac guest's device tree must
  state;
- the firmware version all the answers hold for, read from SMBIOS before
  `ExitBootServices`;
- whether the GICv3 virtual CPU interface is usable at EL2.

el2qual 2 adds them; its report's first line reads `SNAPINTOSH EL2QUAL 2`.

Exposure is the same as stage 2, plus the GIC registers above. It takes
one run: write the stage 2 layout with el2qual 2 (`python3
tools/el2qual-usb.py stage2 ...`, as in "Building a stick") and
boot entry 3 (`nopac`, all CPUs), from a cold boot, photographed.
PAuth is already measured, so it stays out of this run.

Expected:
- **Header:** `FIRMWARE <version>  <date>`, `VENDOR <vendor>  SMBIOS 3.x`
  and `PRODUCT <maker>  <product>`. el2qual never copies SMBIOS's serial
  number or UUID.
- **Boot CPU verdicts:**
  - `COUNTER (CNTFRQ_EL0) .... 19200000 HZ` (expected, INFERRED);
  - `GIC SYSREGS AT EL2 ...... YES  ICC_SRE_EL2 ..`;
  - `GIC VIRTUAL INTERFACE ... YES  <n> LRS  PRI <p>  ID <i>  LR0 RW`.
- **Other cores:** no `CNTFRQ_EL0`, `ICH_VTR` or `LR0 TEST` line under
  `OTHER CPUS: WHAT DIFFERS FROM SLOT 0`; a line there names the core that
  differs.

The breadcrumbs `GIC: READING ICH_*_EL2. A HANG OR RESET HERE IS EL3` and
`GIC: WRITING, RESTORING ICH_LR0_EL2 (INVALID ENTRY)` precede the two GIC
steps on the boot CPU. A hang or reset after the first means EL3 traps
the GIC registers. Record it and do not repeat the run; Route B would
then need a full GICv3 model.

## When something goes wrong

| What you see | Likely meaning | Then |
| --- | --- | --- |
| Firmware refuses the stick | Secure Boot still on, or not FAT32 | fix and retry the same stage |
| Only the firmware's small `... CALLING EXITBOOTSERVICES` line, nothing after | an `el2qual.efi` built before the screen fix: the firmware's mode 7 has no framebuffer address | rebuild from the current tree and retry |
| `NO GOP MODE HAS A FRAMEBUFFER ADDRESS`, `DT FRAMEBUFFER: NO DEVICE TREE` and `STOPPED ...` (stage 1) | expected: a stage 1 stick has no device tree | stage 1 done; go on to stage 1b |
| The same with `DT FRAMEBUFFER: NO DEVICE TREE` on a stage 1b or 2 stick | systemd-boot did not install the DTB (is Secure Boot off? is the entry's `devicetree` path right?) | fix and retry the same stage |
| `DT FRAMEBUFFER:` any other reason, and `STOPPED ...` | the DTB's node or the firmware's memory map differs from the recorded ones; nothing was written | photograph, stop, report |
| Hang with `SCREEN: DT SIMPLE-FB ... DRAWING` as the last console line | el2qual's first write to the device tree's framebuffer faulted: firmware does not map it | photograph, stop, report |
| The large text appears and is then drawn over before the report | the firmware redraws the buffer while boot services run | wait for the report after `ExitBootServices`; record it |
| Nothing drawn at all | no GOP at all, or a fault before the first line | photograph, stop, report |
| Reboot right after `CALLING EXITBOOTSERVICES` (stage 2) | Secure Launch failed: `tcblaunch.exe` too new, or slbounce/firmware mismatch | stop; check the `tcblaunch.exe` version |
| `AFTER EBS EL1` with the gate `OPEN` (stage 2) | slbounce did not switch: driver not loaded (file name) or reported Secure Launch unavailable | check the manifest paths; stop |
| Hang with `PAC: READING APIAKEYLO_EL1` last | EL3 traps PAuth key access from EL2 | a result, not a fault: record it |
| Hang with `PAC: SIGNING AT EL2` last | EL3 traps PAC instructions at EL2 | a result: record it |
| Hang with `CPU_ON SLOT n` last | that core did not return from PSCI | record; run 4 would hang the same way |
| A red `V<n> ESR ...` line under a CPU | an unexpected exception on that CPU | photograph closely; stop |
| One half of the panel corrupt after a Linux boot, and still on the Huawei logo after a power-off | display state held while the tablet is "off", after Linux's display driver took the panel over ([Incidents](#incidents)) | shut down and leave it off for at least 15 minutes, which cleared it once; if it does not clear, stop. Linux is no longer booted on the gaokun3, so any recurrence is treated as hardware |

Recovery is the same in every case: hold the power button until the machine
is off, remove the stick, power on. Windows should start as before; if
BitLocker asks, enter the recovery key.

## Runs so far

All on one MateBook E Go 2023. Run 9 read the firmware from SMBIOS:
version 2.18, dated 06/30/2023. Runs 1 to 8 did not record it; they ran
the same day on the same machine, with no firmware update recorded in
between.

| # | Date | Stage | `el2qual.efi` (commit) | Result |
| --- | --- | --- | --- | --- |
| 1 | 2026-10-23 | not recorded | not recorded (before `96e3d2c`) | only the firmware console's `... CALLING EXITBOOTSERVICES`; nothing drawn, nothing measured ("The screen") |
| 2 | 2026-10-23 | 1 | `3323c2e8` (`96e3d2c`) | `NO GOP MODE HAS A FRAMEBUFFER ADDRESS (MODE 7)`, stopped before `ExitBootServices`; nothing measured |
| 3 | 2026-10-23 | 1b, entry not recorded | `df1644c4` (`a83d95b`) | PASS: full report drawn on the DT framebuffer after `ExitBootServices`; `SLBOUNCE GATE: ZAP-SHADER DISABLED: OPEN` |
| 4 | 2026-10-23 | 2, entry 1 (`nopac nosmp`) | `df1644c4` (`a83d95b`) | EL2 reached; NV2, ECV, FGT, RNDR absent; 16K walk INCONCLUSIVE (control A faulted; D `PAR 0x80D`) |
| 5 | 2026-10-23 | 2, entry 2 (`nosmp`) | `df1644c4` (`a83d95b`) | PAuth signs (4/4), keys, round trip and `PACGA` OK on the boot CPU; 16K INCONCLUSIVE (D `0x805`) |
| 6 | 2026-10-23 | 2, entry 3 (`nopac`) | `df1644c4` (`a83d95b`) | all 8 CPUs `DONE` at EL2 through PSCI; 16K INCONCLUSIVE on every core (boot CPU D `0x805`) |
| 7 | 2026-10-23 | 2, entry 4 (no options) | `df1644c4` (`a83d95b`) | PAuth signs on all 8 cores; 16K INCONCLUSIVE (boot CPU D `0x80D`) |
| 8 | 2026-10-23 | 2, entry 3 (`nopac`) | `15dfd800`, with the `tgran16.c` table cleaning | 16K WALKS on all 8 cores: controls `A OK  B FAULT  C FAULT`, boot CPU D `PAR 0x4001B00` (PA `0x4001000`, the address only a 16K walk reaches) |
| 9 | 2026-10-23 | 3, entry 3 (`nopac`) | `2738da9b` (`c2c7122`, el2qual 2) | PASS: firmware 2.18; `CNTFRQ_EL0` 19200000 on all 8 cores; GIC system registers and virtual CPU interface usable at EL2 on all 8 (4 list registers, `LR0 RW`); no hang (stage 3 results below) |
| 10 | 2026-10-23 | thermal, entry 5 (`nopac nosmp thermal`) | `5c28c4ee` (el2qual 3, built before its commit; it rebuilds identically from the tree) | PASS: the Phase 1 thermal guard ran on the live tsens at EL2 for 12 s, nothing tripped; version `0x30000000` on all four controllers, 46/46 sensors valid, peak 39.4 °C; no fault, no SError ([GAOKUN3-PHASE1-GUARDS.md](GAOKUN3-PHASE1-GUARDS.md#run-10-2026-10-23)) |
| 11 | 2026-10-26 | vhe-exit, entry 6 (`nopac nosmp exit-vhe`) | `558cc4ff` (el2qual 4, built before its commit; it rebuilds identically from the tree) | PASS: `VHE EXIT .. DONE  HCR_EL2 0000000080000000`; slbounce's full entry state recorded and the registers read back equal to the plan; boot CPU `DONE` in non-VHE EL2 with 16K `WALKS` (`PAR 004001B00`, controls `A OK  B FAULT  C FAULT`) ([GAOKUN3-PHASE2.md](GAOKUN3-PHASE2.md#runs-11-and-12-2026-10-26)) |
| 12 | 2026-10-26 | vhe-exit, entry 7 (`exit-vhe`) | `558cc4ff` (as run 11) | PASS: the same exit, then all 8 CPUs `DONE` at EL2 through PSCI with PAuth (signs 4/4, keys and round trip OK, `PACGA` OK) and 16K `WALKS`; only MIDR differs between cores, as in run 9; `VIRTUALMAC ROUTE (EL1) .. CPU OK` |
| 13 | 2026-10-26 | usb-inventory (q1n1 `usb-probe-hold`, no slbounce) | q1n1-usb-probe-hold.efi `0366c947` (built from q1n1 `660dfbb` plus the uncommitted hold option) | PASS, read only: entry EL1; `USB FUNCTION: not installed`; `USB2 HOST` 3 handles, paths `Pci(0x80,0x0)`, `Pci(0x81,0x0)`, `Pci(0x82,0x0)`; `SERIAL IO: not installed`; returned to the boot menu after its hold ([GAOKUN3-PHASE2.md](GAOKUN3-PHASE2.md#run-13-2026-10-26-usb-inventory)) |
| 14 | 2026-10-26 | q1n1-foothold, entry 1 (`--el2`) | q1n1.efi `3bf2020f` (q1n1 `6865a21`, `-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER`) | PASS: q1n1's first boot on the gaokun3. Green bar, `EL2 CONFIRMED AFTER EXITBOOTSERVICES`, entry EL1, current EL2, MIDR `0x410FD4B0`; `SCREEN: DT SIMPLE-FB - CLEANED DRAWS` at `0xC6200000`, its text legible; SPCR names a UART at `0xA90000`, type `0x11`, that q1n1 does not drive ([GAOKUN3-PHASE2.md](GAOKUN3-PHASE2.md#runs-14-and-15-2026-10-26-q1n1s-first-boot)) |
| 15 | 2026-10-26 | q1n1-foothold, entry 2 (`--el2 --exit-vhe`) | `3bf2020f` (as run 14) | PASS: as run 14, plus `VHE EXIT DONE PLAN MATCH`, with the entry and exit registers equal to runs 11 and 12 |
| 16 | 2026-10-26 | usb-read, entry 8 (`nopac nosmp usb`) | `e1e58904` (`51800a1`, el2qual 5) | PASS, read only: USB0 powered and clocked before and after ExitBootServices (`GDSC F822F000`, master `00000221`, AXI and CNOC running); controller read: `GSNPSID 33313130`, `GVERNUM 3139312A`, `GCTL 00101005` (host), xHCI `CAPLENGTH 0x20`; no SError. Stick in the other port (USB1), charger in the power-button port (USB0), the operator's account ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#runs-16-and-17-2026-10-26)) |
| 17 | 2026-10-26 | usb-read, entry 9 (`nopac nosmp exit-vhe usb-smmu`) | `e1e58904` (as run 16) | PASS, read only: the VHE exit as runs 11 and 12, then the same USB values as run 16; apps_smmu enabled (`sCR0 00240406`), 212 stream match groups, USB0's stream `0x820` matched by group 6 and **translated** through context bank 6 (`S2CR 00000006`); no SError; ports as run 16 |
| 18 | 2026-10-26 | usb-read, entry 8 (`nopac nosmp usb`) | `6d0760c2` (el2qual 6, built before its commit; it rebuilds identically from the tree) | PASS, read only: as run 16, every USB value the same; no SError |
| 19 | 2026-10-26 | usb-read, entry 9 (`nopac nosmp exit-vhe usb-smmu`) | `6d0760c2` (as run 18) | PASS, read only: as run 17, then context bank 6: `SCTLR 000000E0` (stage-1 translation **off**), `CBAR 00037E03` (stage 1 nested with stage 2, the stage-2 bank INFERRED to be 126), `TCR`/`TTBR0`/`MAIR` zero; one USB value differs from runs 16 to 18: `GUSB3PIPECTL 0B081402`; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-19-2026-10-26-context-bank-6)) |
| 20 | 2026-10-26 | usb-read, entry 9 (`nopac nosmp exit-vhe usb-smmu`) | `5a367e0e` (`23effe8`, el2qual 7) | PASS, read only: as run 19 (bank 6 nested, stage 1 off), then `SMMU S2 NOT READ: UNMAPPED`: the stage-2 bank's page, INFERRED `0x150FE000`, is not mapped as Device at its own address in the page tables el2qual runs on, so nothing of it was read; `GUSB3PIPECTL` `0B0C1402` again; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-20-2026-10-26-the-stage-2-page-is-unmapped)) |
| 21 | 2026-10-27 | usb-read, entry 10 (`nopac nosmp exit-vhe usb-smmu-map`) | `04f5361b` (`b74bbce`, el2qual 8) | PASS, SMMU read only (writes only el2qual's own page tables): `SMMU MAP OK CLONE KEPT 208/256`, then as run 20 up to bank 6, then bank 126 read: `CBAR 00000003` (a stage-2 bank, VMID 3), `SCTLR 00CF00E1` (stage-2 translation **on**), `TCR 0001355C` (4 KiB, 36-bit), `TTBR0 B7609000`; `GUSB3PIPECTL 0B081402`; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-21-2026-10-27-bank-126-translates)) |
| 22 | 2026-10-27 | usb-read, entry 11 (`nopac nosmp exit-vhe usb-smmu-map usb-s2-walk`) | `1378959f` (`9214752`, el2qual 9) | PASS, read only (writes only el2qual's own page tables): as run 21 through bank 126, then the stage-2 walk of a 1 MiB arena at `0xEFD41000`: `READS 3`, `RW 0 LIMITED 0 MOVED 0 FAULTS 256`. The level-1 descriptor was a table, and both level-2 descriptors under it read 0 (invalid), so stage 2 does not map the arena; the tables read without a fault; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-22-2026-10-27-stage-2-does-not-map-the-arena)) |
| 23 | 2026-10-27 | usb-read, entry 12 (`nopac nosmp exit-vhe usb-smmu-map usb-s2-survey`) | `76057a70` (`4c0f474`, el2qual 10) | PASS, read only (writes only el2qual's own page tables): as run 22 through the walk (arena `0xEFD41000` again, `FAULTS 256`), then the survey, 576 reads: level-1 tables for GiBs 0 to 3 and 16 to 31, nothing else; under GiB 3 (table `0xB76B1000`) 18 identity read-write 2 MiB blocks, `0xC6200000`–`0xC85FFFFF`, starting at the splash framebuffer, and every other entry invalid; `GUSB3PIPECTL 0B0C1402`; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-23-2026-10-27-identity-only-around-the-splash-framebuffer)) |
| 24 | 2026-10-27 | usb-read, entry 13 (`nopac nosmp exit-vhe usb-smmu-map usb-s2-fixed`) | `45a95b3d` (`50fb2ad`, el2qual 11) | PASS, read only (writes only el2qual's own page tables): as run 23 through bank 126, then the walk of the fixed range `0xC8000000`–`0xC80FFFFF`: `READS 2`, `RW 256`, leaf `00000000C80004FD` (a 2 MiB identity block, Normal write-back, read and write, non-shareable); UEFI's memory map over `0xC6200000`–`0xC85FFFFF`: one descriptor, `RESERVED` `0xC6200000`–`0xC8600000`, attributes `100F`, exactly the stage-2 identity region; `GUSB3PIPECTL 0B081402`; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-24-2026-10-27-the-region-is-firmware-reserved)) |
| 25 | 2026-10-27 | usb-read, entry 14 (`nopac nosmp exit-vhe usb-smmu-map usb-s2-bypass-plan`) | `712af82c` (`9496bb9`, el2qual 12) | PASS, read only (writes only el2qual's own page tables; nothing to the SMMU): as run 24 through bank 126, then the S2CR bypass plan: `S2CR06 AT 15000C18 00000006 -> 00010006`, `MATCHES 1/212 MASK 0000 GFSR 00000000`, `PLAN ONLY, NOTHING WRITTEN`. Every precondition held, including the register's page writable at EL2 (`AT S1E2W`); `GUSB3PIPECTL 0B0C1402`; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-25-2026-10-27-the-plan-holds-on-the-machine)) |
| 26 | 2026-10-27 | usb-read, entry 15 (`nopac nosmp usb-halt`) | `3728784d` (`d8c5fe4`, el2qual 13) | PASS, read only (nothing written): USB0 powered before and after ExitBootServices and the controller read, as runs 16 and 17 (`CAP 01100020`, so `CAPLENGTH 0x20`; `SNPSID 33313130`, `GCTL 00101005` host); then the halt read: `USB0 HALT OP 0A600020 CMD 00000000 STS 00000001 HALTED`. The operational base is core + the measured `CAPLENGTH` (`0x0A600020`, confirming the derivation on the machine); `USBCMD.R/S` clear and `USBSTS.HCH` set, so **USB0 is halted after ExitBootServices**; no SError ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#run-26-2026-10-27-usb0-is-halted-after-exitbootservices)) |
| 27 | 2026-10-27 | entropy, entry 16 (`nopac nosmp rng`) | `80a1036d` (`e8b288c`, el2qual 14; built before its commit, it rebuilds identically from the tree) | PASS, read only (nothing written): before ExitBootServices, `RNG PROTOCOL FOUND`, `RNG ALGORITHMS 2: RAW HASH`; `RNG DEFAULT 256B OK NONZERO DIFFER DISTINCT 159`, `RNG RAW 256B OK NONZERO DIFFER DISTINCT 156`; **`RNG VERDICT: USABLE, RAW`**. The rest of the screen is as earlier boot-CPU runs: `START EL1  AFTER EBS EL2`, slbounce gate open, 8 CPUs, firmware 2.18, `GK-W7X`, 16K `WALKS`, NV2/ECV/FGT/RNDR absent ([GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-27-2026-10-27-the-firmware-offers-raw-entropy)) |
| 28 | 2026-10-28 | vmapple-h1, entry `q1n1-vmapple-h1` (`--vmapple-place --hold`; no slbounce, never leaves the firmware) | q1n1 `8c654d82` (q1n1 `308cddf` plus the uncommitted `--hold`; `-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER -DQ1N1_GAOKUN3 -DQ1N1_VMAPPLE_STICK`), bundle qualification `56f025bb…40567be2` | PASS, Phase 3 step H1, nothing entered: `Entry EL: 1`, `DT framebuffer: OK`; `VIRTUALMAC BUNDLE: PLACED AND VERIFIED, SEED FROM RAW`, `CHUNKS 2  BYTES 85245952  ENTRY 0000000103EB4480`, `QUALIFICATION 56F025BBA955E538`, `RELEASED: NOT ENTERED (PHASE 3 STEP H1)`. The live memory map had both guest windows free (`0x101004000`, `0x161004000`, by `AllocateAddress`), both chunks hashed in place, and the seed came from UEFI's `RAW` RNG ([GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-28-2026-10-28-h1-passes)) |

| 29 | 2026-10-28 | vmapple-h2, entry `q1n1-vmapple-h2` (`--vmapple-enter --exit-vhe`) | q1n1 gaokun3-vmb build (stick tree `build/el2qual-usb/vmapple-h2`), bundle qualification `be3f9e42…41cb1` | FAIL at the monitor jump, nothing entered: `PLACED AND VERIFIED, SEED FROM RAW`, `CHUNKS 3 BYTES 85263711 ENTRY 0000000103EB4480`, `KEPT: ENTERING THE MONITOR`, then after ExitBootServices a synchronous exception, ESR `0x8600000E` (instruction abort, translation fault level 2), ELR = FAR = `0x162004000`, the monitor's entry. The monitor never ran, so no GIC write happened ([GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-29-2026-10-28-h2-stops-at-the-monitor-jump)) |

| 30 | 2026-10-28 | vmapple-h2, entry `q1n1-vmapple-h2` (`--vmapple-enter --exit-vhe`) | q1n1 `4ed8ed7a` (with its own EL2 identity map), bundle qualification `be3f9e42…41cb1` | NOT PASSED: as run 29 through `ENTERING MONITOR AT EL2`, with no q1n1 exception (the identity map worked; the monitor started), then the machine restarted (owner's report) before any monitor band was photographed. The monitor then proceeded to enter the guest, so the reset's cause is UNKNOWN; the blind `GICD_CTLR` write a later review found is a candidate (INFERRED). Decision 4 had been approved by the owner in session before run 29 ([GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-30-2026-10-28-the-monitor-is-entered-then-the-machine-restarts)) |

| 31 | 2026-10-28 | vmapple-h2, entry `q1n1-vmapple-h2` (`--vmapple-enter --exit-vhe --hold-at-monitor`) | q1n1 `daca77a3`, bundle qualification `c636d8be…21b3` | NOT PASSED: the report lines as run 30, then `ENTERING MONITOR AT EL2` (owner's report) and a restart within seconds, before the monitor's band could appear. Root cause verified after run 32 ([GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#runs-31-and-32-2026-10-28---hold-at-monitor-and---gic-hold-still-restart-root-cause-found-and-fixed)) |

| 32 | 2026-10-28 | vmapple-h2 diagnostic (`--gic-hold`: band first, halt before any GIC access) | q1n1 `5b0a3388`, bundle qualification `74ecc724…942f71c` | NOT PASSED: the report lines and `ENTERING MONITOR AT EL2`, then a restart before the GIC code was reached. **Root cause of runs 30–32 (VERIFIED by disassembly of `5b0a3388`):** `q1n1_vmb_enter_kept` jumped to `manifest.monitor_entry_pa` as `((void (*)(void))...)()` without passing `manifest.params_pa` (`0x162104000`) in `x0` (`x0` still held `el2_tables`). In `_start`, `[x0, #0x18]` did not match `SMOKE_PARAMS_MAGIC`, so `params_base` stayed at its QEMU fallback `0x43000000` (unmapped on the gaokun3), faulting at `0x43000018` before `monitor_set_band` and faulting recursively in `record_and_halt` at `0x43001000` until `sp` walked over `el2_vectors` and reset the machine ([GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#runs-31-and-32-2026-10-28---hold-at-monitor-and---gic-hold-still-restart-root-cause-found-and-fixed)) |

| 33 | 2026-10-28 | vmapple-h2, entry 1 `q1n1-vmapple-h2` (`--vmapple-enter --exit-vhe --hold-at-monitor --gic-hold`) | q1n1 `94a071e2` (with `x0 = manifest.params_pa` and `SCTLR_EL2.M`-off table switch), bundle qualification `0fe61b3b…5034e7deb` (`0FE61B3B9BD2FCB5`) | **PASS (H2 stage-2 half, GIC not touched):** `VIRTUALMAC BUNDLE: PLACED AND VERIFIED, SEED FROM RAW`, `CHUNKS 3 BYTES 85267376 ENTRY 0000000103EB4480`, `QUALIFICATION 0FE61B3B9BD2FCB5`, `KEPT: ENTERING THE MONITOR (PHASE 3 STEP H2)`, `MONITOR 0000000162004000 PARAMS 0000000162104000`, `ENTERING MONITOR AT EL2 (PHASE 3 STEP H2)`; then in the bottom monitor band: `SNAPINTOSH MONITOR: STAGE 2 DEFAULT-DENY` and `SETUP DONE: STAGE 2 READY, GIC NOT TOUCHED`, halting cleanly with no exception or restart (VERIFIED in 3 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-33-2026-10-28-the-monitor-runs-at-el2-sets-up-stage-2-and-draws-its-band)) |

| 34 | 2026-10-28 | vmapple-h2, entry 2 `q1n1-vmapple-h2-gic` (`--vmapple-enter --exit-vhe --hold-at-monitor`) | q1n1 `94a071e2`, bundle qualification `0fe61b3b…5034e7deb` (`0FE61B3B9BD2FCB5`) | **PASS (Phase 3 step H2 complete):** as run 33 through `SETUP DONE: STAGE 2 READY, GIC NOT TOUCHED`, then `monitor_init` ran on the real GIC at `0x17A00000` / `0x17A60000` and drew `GIC CTLR 00000010 -> 00000012  RD FRAME 0` in the bottom monitor band, halting cleanly before guest entry with no exception, no interrupt storm, and no restart (VERIFIED in 3 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-34-2026-10-28-h2-passes-with-real-gic-setup)) |

| 35 | 2026-10-28 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (`--vmapple-enter --exit-vhe`) | q1n1 `6f029956`, bundle qualification `7e0a5b2a…ece19c9b3` (`7E0A5B2A67807238`) | **PASS (guest entered at EL1, first HVC handled):** `VIRTUALMAC BUNDLE: PLACED AND VERIFIED, SEED FROM RAW`, `CHUNKS 3 BYTES 85267376 ENTRY 0000000103EB4480`, `QUALIFICATION 7E0A5B2A67807238`, `KEPT: ENTERING THE MONITOR (PHASE 3 STEP H2)`, `MONITOR 0000000162004000 PARAMS 0000000162104000`. Monitor band: `SNAPINTOSH MONITOR: STAGE 2 DEFAULT-DENY`, `PAC KEYS INSTALLED (QARMA5), HVC OK`, `GIC CTLR 00000010 -> 00000012  RD FRAME 0`. HVC `0xC1000000` (`SET_INITIAL_STATE`) completed and returned to EL1; screen held stable with no restart or trap, but kernel halted before PL011 output (VERIFIED in photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-35-2026-10-28-the-guest-enters-at-el1-and-installs-pac-keys)) |

| 36 | 2026-10-29 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (with live telemetry & EL2 watchdog timer snapshot) | q1n1 `6f029956`, bundle qualification `d315cdd4…460637bd` (`D315CDD493261653`) | **PASS (fault diagnosed via EL2 watchdog snapshot):** `QUALIFICATION D315CDD493261653`, `ENTRY 0000000103EB4480`. Monitor band: `PAC KEYS INSTALLED (QARMA5), HVC OK`, `GIC CTLR 00000010 -> 00000012  RD FRAME 0`, `HVC: 1 ID C1000000  MMIO: 0 IPA 00000000`, `SNAP 56 PC FFFFFE0009F09FC4 SPSR 604013C4`, `ESR_EL1 1FE00000 FAR_EL1 0000000000000000`, `ELR_EL1 FFFFFE000A07F410 SCTLR FC54793D`. Verified: kernel enabled MMU/caches (`SCTLR_EL1 = 0xFC54793D`), zero MMIO aborts occurred, trapped on `mrs x8, fpcr` at `0xfffffe000a07f410` with EC `0x07` because `CPACR_EL1` was zeroed. Preemption via EL2 timer (`CNTHP`, PPI 26) captured 56 snapshots of the kernel's `wfe` panic spin loop at `0xfffffe0009f09fc4` (VERIFIED in 2 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-36-2026-10-29-el2-watchdog-snapshot-pinpoints-the-el1-fault)) |

| 37 | 2026-10-29 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (with `CPACR_EL1` FP/SIMD enable) | q1n1 `6f029956`, bundle qualification `225eb619…ed720111` (`225EB6197F9CABB5`) | **PASS (XNU console alive on monitor band; panic diagnosed):** `QUALIFICATION 225EB6197F9CABB5`. Monitor band: `PAC KEYS INSTALLED (QARMA5), HVC OK`, `GIC CTLR 00000010 -> 00000012  RD FRAME 0`. The kernel advanced past `sysctl__hw_features` (`mrs x8, fpcr` succeeded!), initialized `pmap` and exception handlers (`sleh.c`), and printed its panic banner directly via our PL011 UART model into the monitor band: `PANIC(CPU 0 CALLER 0xFFFFFE000A0899E8): UNDEFINED KERNEL INSTRUCTION: PC=0xFFFFFE000A065974 INSTR=D50882A8 @SLEH.C:1930`, `LR: 0xF7E6FE0009F0A918  SP: 0xFFFFFE000AC67150  PC: 0xFFFFFE000A0903F4  ESR: 0x96000045  FAR: 0xB1  <END NESTED PANIC STRING>`. Verified: `INSTR=D50882A8` is `TLBI RVALE1IS, x8` in `pmap_flush_tlb_range`; Cortex-A78C lacks ARMv8.4 `FEAT_TLBIRANGE` (has `TLB=1`), so range TLBI faulted as Undefined Instruction (VERIFIED in 2 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-37-2026-10-29-the-xnu-console-is-alive-range-tlbi-fault-diagnosed)) |

| 38 | 2026-10-29 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (with `HCR_EL2.TTLB = 1` test) | q1n1 `6f029956`, bundle qualification `2c35136f…a9f83bd9` (`2C35136F5E05A04D`) | **DIAGNOSTIC RESULT (Cortex-A78C hardware decode behavior verified):** `QUALIFICATION 2C35136F5E05A04D`. Monitor band: as Run 37, panic reproduced with `INSTR=D50882A8 @SLEH.C:1930`. VERIFIED: on Cortex-A78C, an unallocated instruction (`CRm=2` on ARMv8.2) is rejected by the instruction decoder directly as an Undefined Instruction to EL1 *before* the execution unit's `HCR_EL2.TTLB` trap logic evaluates it. Confirms that Range TLBI must be bypassed at the branch level (VERIFIED in 2 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-38-2026-10-29-hcr_el2ttlb-tested-on-hardware)) |

| 39 | 2026-10-29 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (with dynamic branch bypass at EL2) | q1n1 `6f029956`, bundle qualification `3dd09aea…fb46b533` (`3DD09AEA61CE4A34`) | **PASS (Range TLBI bypassed; 3 HVCs & 6 MMIO completed):** `QUALIFICATION 3DD09AEA61CE4A34`, `CHUNKS 3 BYTES 85269304 ENTRY 0000000103EB4480`. Monitor band: `PAC KEYS INSTALLED (QARMA5), HVC OK`, `GIC CTLR 00000010 -> 00000012  RD FRAME 0`, `HVC: 3 ID C10000F0  MMIO: 6 IPA 20010030`, `SNAP 24 PC FFFFFE0009EB2758 SPSR 204013C5`, `ESR_EL1 02000000 FAR_EL1 FFFFFE000E004040`, `ELR_EL1 FFFFFE0009F0ABC8 SCTLR FC54793D`. VERIFIED: dynamic branch bypass at EL2 (`b.lo` -> `b` in `pmap_flush_tlb_range` and `b.hi` -> `b` in global flush) completely eliminated the Undefined Instruction fault. Kernel completed 3 HVCs and 6 MMIO accesses to PL011 `UARTCR` (`0x20010030`), then halted in `PEHaltRestart()` (`0xfffffe0009eb2758`) via `DebuggerTrapWithState()` (`0xfffffe0009f0abc8`) because early boot had not yet registered `PE_kputc` to unmask UART output (VERIFIED in 2 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-39-2026-10-29-dynamic-branch-bypass-at-el2-eliminates-range-tlbi-fault)) |

| 40 | 2026-10-30 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (`--vmapple-enter --exit-vhe`, with EL2 panic-metadata extraction) | q1n1 `6f029956`, bundle qualification `dd4ea5f8…a18e7dbb` (`DD4EA5F8C140D5EC`) | **OBSERVED (panic captured; its format string shown unformatted):** `QUALIFICATION DD4EA5F8C140D5EC`, `CHUNKS 3 BYTES 85270984 ENTRY 0000000103EB4480`. Monitor band: `PANIC C: FFFFFE000AC6FAE8 FAR: FFFFFE000E004040`, `ESR: 02000000 R: 3 PC: FFFFFE0009EB2758`, then XNU's kernel-abort format string verbatim (`%S AT PC 0X%016LLX, LR 0X%016LLX (SAVED STATE: %P%S)` and a register template). The `debugger_state` read at `phys_base+0x3b84140` works (`R:3` is DBOP_PANIC; ESR/FAR/PC match run 39); the `PANIC C` field read from `+0x20` is a stack address, so `+0x20` is `db_panic_args`, not the caller. Substituting the arguments is run 41's task (VERIFIED in 2 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-40-2026-10-30-the-panic-is-captured-its-format-string-is-shown-unformatted)) |

| 41 | 2026-10-30 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (`--vmapple-enter --exit-vhe`, with the EL2 panic formatter) | q1n1 `6f029956`, monitor chunk `669d18d2`, bundle qualification `229ef55e…cdf12227` (`229EF55ED9E1D6CE`) | **OBSERVED (panic message formatted; cause found):** `PANIC C FFFFFE000A82892C FAR FFFFFE000E004040`, `ESR 02000000 R 3 PC FFFFFE0009EB2758`, then the formatted message over six pages: `Invalid kernel stack pointer (probable corruption or early boot). at pc 0xfffffe000a835f28, lr 0x7ab07e000a7d8d24`, `x0 0xfffffe000e004000`, `x2 0x7ff80`, `x3 0xfffffe000e004040`, `x4 0xfffffe000e004080`, `x10 0x46554e4b`, cut at `x12:` by the monitor's 256-byte format buffer. The two-level `va_list` reading works. The PC is `bzero`'s `dc zva` in `check_for_panic_log`: the loader's `dram-size` left the panic region outside DRAM, so XNU mapped it as Device memory, and `dc zva` to Device memory is an Alignment fault, which QEMU does not implement (cause INFERRED; the saved ESR was past the cut). `FAR_EL1` is this abort's address (= `x3`) (VERIFIED in 6 video frames; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#what-run-41-showed)) |

| 42 | 2026-10-30 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (`--vmapple-enter --exit-vhe`, `dram-size` covering panic region) | q1n1 `6f029956`, monitor chunk `43570c2a`, bundle qualification `57a8cae4…242ed4e3` (`57A8CAE4BE7EF915`) | **OBSERVED (dram-size fix confirmed; guest console banner prints; stackshot succeeds):** `QUALIFICATION 57A8CAE4BE7EF915`. Monitor band line 4 (green): `Darwin Kernel Version 27.0.0 … RELEASE_ARM64_VMAPPLE` (the first guest console line). Lines 5–7 (cyan): `(none)` (kexts list), `** Stackshot Succeeded ** Bytes Traced 1145 …` (panic region confirmed working as Normal memory), and `MACH Reboot` (`PEHaltRestart(kPERestartCPU)`). VERIFIED: `dc zva` alignment data abort in `check_for_panic_log` completely eliminated by reporting `dram-size` covering the panic region (0x60000000 -> 0x60100000). The panic reason scrolled away before reading because the band previously kept only the first and last three lines, and the snapshot timer halted after the console started (VERIFIED on hardware; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-42-2026-10-30-dram-size-fix-confirmed-guest-console-prints-later-panic)) |

| 43 | 2026-10-30 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (`--vmapple-enter --exit-vhe`, monitor console scrollback paging) | q1n1 `6f029956`, monitor chunk `2c4b22bd`, bundle qualification `afeeeb95…ee59a02` (`AFEEEB952232FBA6`) | **PASS (scrollback paging decodes panic; FEAT_SSBS2 absence diagnosed):** `QUALIFICATION AFEEEB952232FBA6`. Monitor band cycled 15 pages at 1 Hz (`GUEST PANICKED: CONSOLE PAGE 1/15`): `PANIC(CPU 0 CALLER 0xFFFFFE000A0899E8): UNDEFINED KERNEL INSTRUCTION: PC=0xFFFFFE0009EB8490 INSTR=D51B42C2 @SLEH.C:1930`. Verified: `0xD51B42C2` is `msr SSBS, x2`. Cortex-A78C implements `FEAT_SSBS` (`MSR SSBS, #imm`) but lacks ARMv8.5 `FEAT_SSBS2` (direct system register `MSR/MRS SSBS, <Xt>`). XNU's feature test `tst x20, #0xf0` assumed any non-zero value implied Apple Silicon's `FEAT_SSBS2` (VERIFIED in video IMG_1091.mp4; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-43-outcome-observed-on-hardware-video-img_1091mp4)) |

| 44 | 2026-10-30 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (neutralize FEAT_SSBS2 at EL2) | q1n1 `6f029956`, monitor chunk `421db670`, bundle qualification `ade3d1f7…76af1e3c` (`ADE3D1F7187F6640`) | **PASS (SSBS bypass verified; FEAT_DIT absence diagnosed):** `QUALIFICATION ADE3D1F7187F6640`. Monitor band cycled 15 pages: `PANIC(CPU 0 CALLER 0xFFFFFE000A0899E8): UNDEFINED KERNEL INSTRUCTION: PC=0xFFFFFE0009EB84A8 INSTR=D51B42A2 @SLEH.C:1930`. Verified: `0xfffffe0009eb8490` was bypassed with NOP; `0xfffffe0009eb849c` (`msr UAO, x2`) executed natively. Next instruction `0xD51B42A2` is `msr DIT, x2`; Cortex-A78C lacks `FEAT_DIT` (`ID_AA64PFR0_EL1.DIT = 0`), which Apple Silicon compiles unconditionally into context switching (VERIFIED in video IMG_1094.mp4; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-44-outcome-observed-on-hardware-video-img_1094mp4)) |

| 45 | 2026-10-30 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (neutralize FEAT_SSBS2 and FEAT_DIT at EL2) | q1n1 `6f029956`, monitor chunk `b544eae0`, bundle qualification `8507e3fe…433117cb` (`8507E3FE5B08C9AC`) | **PASS (Kernel bootstrap complete; Clutch scheduler alive; BDIF panic stop):** `QUALIFICATION 8507E3FE5B08C9AC`. Main screen framebuffer (`1600x2400`): complete kernel boot log through `initialize_screen`, `pmap_startup()` (1789 µs), `vm_page_bootstrap` (92,765 free pages), Clutch scheduler, `IOKit IOMD setownership`, `ATM`, `BANK`, `Registered coredump handler`, and `Initializing serial KDP`. Monitor band: `STOP: UNMODELLED IPA 0000000030010408 WRITE`, `MAILBOX 4FFFF8: REQUEST ABSORBED, NO HOST`, and `** STACKSHOT SUCCEEDED ** BYTES TRACED 6387`. Verified: SSBS and DIT neutralizations eliminated context switch faults; kernel reached `AppleVirtualPlatform`, which accessed mailbox and attempted to write panic coredump to AUX disk `0x30010408` (VERIFIED in 2 photographs; [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-45-outcome-observed-on-hardware-2-photographs)) |

| 46 | 2026-10-30 | vmapple-h3, entry 1 `q1n1-vmapple-h3` (page console scrollback on named stops at EL2) | q1n1 `6f029956`, monitor chunk `b297f027`, bundle qualification `6b99a9d3…6aad78e0` (`6B99A9D3BD7F26D7`) | **PREPARED (pages 96-line console scrollback on named stops & faults):** `QUALIFICATION 6B99A9D3BD7F26D7`. In `stub.S`, `record_and_halt` now calls `monitor_halt_report(0)` directly before checking QEMU's `pt_flag`, ensuring that named stops (such as `0x30010408` AUX disk write) page the 96-line console scrollback ring across band lines 4–7 at 1 Hz, surfacing the exact panic string and stack trace. Host tests: PASS 107 checks × 2 builds, 101 classic checks, 82 smoke checks, 31 USB tests ([GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#proposed-run-46-page-console-scrollback-on-named-stops-at-el2)) |

Runs 4 to 7 were matched to their entries by what the photographs show
(CPU count, and whether PAuth ran); the order they were run in was not
recorded. Runs 11 and 12 were matched the same way: one CPU with PAuth
`NOT RUN`, then eight with PAuth. Runs 14 and 15 likewise: without, then
with the `VHE EXIT` lines. Runs 16 and 17 likewise: without, then with the
`VHE EXIT` and `SMMU` lines, and runs 18 and 19 the same way. Which port held
the stick for runs 18 and 19 was not restated; the instructions asked for
run 16's arrangement. Run 21 was matched by its `SMMU MAP` line, which only
entry 10 prints, and run 22 by its `SMMU WALK` lines, which only entry 11
prints, and run 23 by its `SMMU SURVEY` lines, which only entry 12
prints, and run 24 by its `(FIXED)` and `UEFI MAP` lines, which only
entry 13 prints, and run 25 by its `SMMU BYPASS` lines, which only entry
14 prints. Every one ended in a complete report: no hang, no reboot, no
unexpected exception.

Full SHA-256 of run 3's `el2qual.efi`:
`df1644c4e47f515d39cf92dee4c584819b75784d6b20d7261c08ead4681ca6b9`; it
rebuilds identically from `a83d95b`. The entry does not matter at stage
1b: without slbounce all four stay at EL1 and measure the same.

Run 3's report (photographed): `START EL1  AFTER EBS EL1  EBS OK, TRIES 1`;
`ACPI CPUS 8  GIC V3  PSCI NONE  FB 1600X2560` (el2qual queries PSCI only
at EL2); `SCREEN DT SIMPLE-FB  FB 00000000C6200000  NO GOP ADDRESS`. The
ID registers as UEFI sees them at EL1, under Qualcomm's hypervisor,
transcribed from the photo (all registers not listed read 0):

| Register | Value | Decoded |
| --- | --- | --- |
| `MIDR_EL1` | `0x00000000410FD4B0` | Arm Cortex-A78C r0p0 (the boot CPU) |
| `MPIDR_EL1` | `0x0000000081000000` | |
| `REVIDR_EL1` | `0x0000000000000001` | |
| `ID_AA64PFR0_EL1` | `0x1100000011111112` | EL2 and EL3 AArch64; GICv3 system registers; SVE 0 |
| `ID_AA64PFR1_EL1` | `0x0000000000000010` | SSBS 1; BT (BTI) 0 |
| `ID_AA64DFR0_EL1` | `0x0000000110305408` | |
| `ID_AA64ISAR0_EL1` | `0x0010100010211120` | RNDR 0 |
| `ID_AA64ISAR1_EL1` | `0x0000000001200031` | APA 3 (PAuth2, QARMA5, no FPAC), API 0, GPA 1, GPI 0; as Windows caches it |
| `ID_AA64MMFR0_EL1` | `0x0000000000101122` | TGran16 1 (16 KiB supported), TGran4/64 supported, stage-2 fields 0 (as stage 1); PARange 40 bits; FGT 0; ECV 0 |
| `ID_AA64MMFR1_EL1` | `0x0000000010212122` | VH 1 (VHE) |
| `ID_AA64MMFR2_EL1` | `0x0000000100001011` | NV 0 |

These are OBSERVED, not VERIFIED: a hypervisor can hide fields from EL1, or
show ones the hardware lacks, and `ID_AA64MMFR0` differs from what Windows
caches (`0x0000011100101022`, which marks big-endian and the stage-2
granules unsupported). Stage 2 reads the same registers at EL2 and
measures the behaviour behind them.

Runs 4 to 8, at EL2 after Secure Launch (VERIFIED for this machine; run 9
repeated every answer below except PAuth on firmware 2.18):

| Question | Answer | Evidence |
| --- | --- | --- |
| EL2 entry | reached on every run; `HCR_EL2 0x0000000488000000` (E2H, RW, TGE), `SCTLR_EL2 0x30C5183D` (MMU and caches on) | `START EL1  AFTER EBS EL2`, `ENTRY HCR_EL2` |
| ID registers at EL2 | identical to run 3's EL1 view, every register | the two columns of the register table |
| Cores | slots 0 to 3 Cortex-A78C r0p0 (`MIDR 0x410FD4B0`), 4 to 7 Cortex-X1C r0p0 (`0x410FD4C0`), MPIDR affinities `0x000` to `0x700`; MIDR is the only ID register that differs | CPU table, `OTHER CPUS` list |
| Secondaries | PSCI 1.1; `CPU_ON` returned 0 for slots 1 to 7, each ran el2qual at EL2, and `AFFINITY_INFO` reported it off afterwards | `PSCI 0 AFF 1` per slot |
| PAuth | works on all 8 cores: `PACIA1716` changed 4/4 pointers, the key registers exist, sign then authenticate returned the pointer, `PACGA` runs; no step trapped to EL3 | runs 5 and 7 |
| NV2, ECV, FGT, RNDR | absent: ID field 0 and the probe instruction undefined (`VNCR_EL2`, `CNTPOFF_EL2`, `HFGRTR_EL2`, `RNDR`) | runs 4 to 7 |
| 16 KiB walk | walks on all 8 cores at stage 1: trial D resolved to `0x4001000` and the controls behaved, on every core (run 8). Runs 4 to 7 could not tell: their control A faulted (below) | run 8's `16K WALK (TRIAL D) ... WALKS` and its CPU table |

Why runs 4 to 7 could not measure the walk: `probes/tgran16.c` runs its
walks with `SCTLR_EL1.C` clear, which makes the EL1&0 stage 1 table walk
Non-cacheable, while the stores that built the table went into the cache
of UEFI's EL2 regime. The walker then read stale memory, which fits trial
D's fault code changing between runs (level 2 translation fault `0x80D`,
level 2 address size fault `0x805`) and QEMU, which models no caches,
passing. That is INFERRED; what is VERIFIED is that cleaning the table to
the point of coherency before each walk, as `tgran16.c` now does, made
every control behave on every core (run 8).

### Stage 3 results (run 9)

Run 9 booted the stage 2 layout with el2qual 2: `el2qual.efi` SHA-256
`2738da9bb7583608b3a76a9b67bfaad5e3a6cae5d3997a74a47e247bde685db4`, built
from `c2c7122`. Every other file on the stick had the same SHA-256 as run
8's: systemd-boot, slbounce, `tcblaunch.exe`, the DTB and the entries. The
report was complete, with no hang, reboot or unexpected exception; the
whole screen was photographed (kept by the machine owner, not in this
repository). Transcribed from the photo:

| Reading | Value | Evidence |
| --- | --- | --- |
| Firmware (SMBIOS type 0) | version `2.18`, release date `06/30/2023`, vendor `HUAWEI`; SMBIOS 3.3 | VERIFIED |
| Product (SMBIOS type 1) | maker `HUAWEI`, product `GK-W7X` | VERIFIED |
| `CNTFRQ_EL0` | 19,200,000 on every core: the boot CPU's line reads `19200000 HZ` and no other core lists a different value | VERIFIED as read. el2qual reads the register the firmware programmed; it does not time the counter, so a tick rate of 19.2 MHz is INFERRED |
| `ICC_SRE_EL2` | `0x09`: SRE 1 (EL2 uses the GIC system registers), Enable 1 (EL1 may access `ICC_SRE_EL1`), DFB and DIB 0; the access did not trap to EL3 | VERIFIED |
| GICv3 virtual CPU interface | present on every core: `ICH_VTR_EL2` reads, giving 4 list registers, 5 virtual priority bits and 16-bit virtual INTIDs | VERIFIED |
| `ICH_HCR_EL2` | `En` clear on every core, so the interface was off at entry: el2qual ran the list-register test only because of that | VERIFIED |
| `ICH_LR0_EL2` | `RW` on every core: the invalid-entry pattern read back whole and was restored | VERIFIED |
| Everything else | as run 8: `START EL1  AFTER EBS EL2`, `HCR_EL2 0x488000000`, `SCTLR 0x30C5183D`, gate open, `ACPI CPUS 8  GIC V3  PSCI 1.1`, 8 slots `DONE` at EL2, 16K `WALKS` (`PAR 004001B00`, controls `A OK  B FAULT  C FAULT`), NV2/ECV/FGT/RNDR absent, ID registers identical at EL1 and EL2, and only MIDR differs between cores | VERIFIED, unchanged |

The verdict line read `GIC VIRTUAL INTERFACE ... YES  4 LRS  PRI 5  ID 16
LR0 RW`. Only the screen recorded the run; the serial-only `GIC <slot>`
lines, which carry the raw `ICH_VTR_EL2` value, have no output on this
machine.

What it decides:

- A Route B guest's `timebase-frequency` is 19,200,000, so the VirtualMac
  template's 24 MHz is replaced, not rescaled (there is no ECV).
- A Route B monitor can deliver the guest's interrupts through the list
  registers instead of a full GICv3 CPU interface model. With 4 list
  registers, more than 4 pending interrupts need a software queue refilled
  on maintenance interrupts, the usual practice for a hypervisor
  (INFERRED). The distributor and redistributors were not touched, so how
  the guest's view of them is presented is still open.

A report defect this run showed: with `nopac`, the `PAUTH KEYS, ROUND TRIP`
and `PACGA` lines still read `OK`. el2qual skips those probes then, and the
report treats a fault code that was never written as success. On runs 6, 8
and 9 those two lines are not measurements. PAuth on this machine rests on
runs 5 and 7, which ran them. el2qual built after run 9 prints `NOT RUN` on
both lines when PAuth is skipped.

Phase 0A's CPU questions and the Phase 0C pre-Phase 1 check 1 are
answered, for firmware 2.18. Phase 0B, the offline kernelcache study, is
in [GAOKUN3-PHASE0B.md](GAOKUN3-PHASE0B.md), and the route decision it
feeds in [GAOKUN3-PHASE0C.md](GAOKUN3-PHASE0C.md).

## Incidents

### 2026-10-23: half the panel corrupt after a Linux survey boot

This was not an el2qual run. The owner booted a USB stick with
gaokun_build's Ubuntu 26.04 image (kernel `7.1.0-rc3-gaokun3+`) through
its EL1 entry, with slbounce and the image's EL2 entry moved off the
stick. It was booted twice:
- first the image's standard entry, where the desktop never started;
- then a headless entry that ran the read-only Phase 1 survey
  (`tools/gaokun3-linux-survey.sh`, never committed, deleted on
  2026-10-23) until a power-off cut it short.

What the owner saw (OBSERVED, the owner's report; no photograph recorded):
- the right half of the panel (in portrait) showed a striped band in both
  Linux boots;
- the band stayed on the Huawei firmware logo and in Windows after a
  power cycle with the stick removed;
- the panel was whole again after the tablet was shut down and left off
  for 15 minutes.

The full record, including what the stick's journal showed, is in
[GAOKUN3-PHASE1-GUARDS.md](GAOKUN3-PHASE1-GUARDS.md#incident-2026-10-23).

What was checked, on the host only:
- **The survey only reads.** It reads sysfs, debugfs, procfs and the
  kernel log. It never opens `/dev/mem` or `/dev/i2c-*`, sends no EC
  commands, and writes only its own output file.
- **Nothing on the stick is known to write lasting panel or firmware
  state.** gaokun-android's `himax-spi` touch patches contain no flash
  writes. gaokun_build's touchscreen tuner writes only the touch driver's
  sysfs parameters, and is a desktop app the headless entries never start.
  gaokun_build's image scripts set up no firmware updater.
- **The symptom matches a known gaokun3 Linux display issue.**
  gaokun_build's `docs/display-debug.md` records one half of the panel
  corrupting about 1–2 s into boot, when the `msm` display driver takes
  over the bonded pair of DSI links that each drive half the panel (DSI1
  uses DSI0's PLL). gaokun-android once judged a similar black screen a
  panel or cable fault, then traced it to a kernel regression in the same
  DSI code.

What it means:
- **Trigger (INFERRED):** most likely Linux's `msm` driver taking over the
  panel.
- **Persistence (INFERRED):** the corruption held through a short power
  cycle but cleared after 15 minutes off. That points to state kept
  while the tablet is "off", by the EC or a panel supply it keeps up,
  rather than permanent damage. Neither is measured, and a marginal
  hardware fault is not excluded (UNKNOWN).
- **el2qual is not implicated:** it never programs the display. It draws
  on the framebuffer the firmware already set up, and stages 1 to 3 never
  showed this.

The owner's decision, 2026-10-23:
- **No Linux image is booted on the gaokun3 again**, with or without
  the `msm` display driver. The Linux survey is retired
  ([GAOKUN3-PHASE1-GUARDS.md](GAOKUN3-PHASE1-GUARDS.md#linux-survey-retired)).
  Hardware readings come only from UEFI applications like el2qual, and
  from Windows.
- If one half corrupts again, treat it as hardware, since Linux no longer
  runs: stop all runs and record it here.
- After any recurrence, photograph the firmware logo and record how long
  the tablet was off before it recovered.

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
- `CNTFRQ_EL0` (stage 3): the `timebase-frequency` a VirtualMac guest's
  device tree must carry.
- `GIC VIRTUAL INTERFACE ... YES` (stage 3): a Route B monitor can deliver
  the guest's interrupts through the list registers. `NO`, `?` or a hang:
  it needs a full GICv3 model.

## Afterwards

Remove the stick, turn Secure Boot back on, and resume BitLocker if it was
suspended. Confirm Windows starts.
