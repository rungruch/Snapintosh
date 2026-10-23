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

Recovery is the same in every case: hold the power button until the machine
is off, remove the stick, power on. Windows should start as before; if
BitLocker asks, enter the recovery key.

## Runs so far

All on one MateBook E Go 2023; firmware version not recorded for any of
them yet (stage 3 reads it from SMBIOS).

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

Runs 4 to 7 were matched to their entries by what the photographs show
(CPU count, and whether PAuth ran); the order they were run in was not
recorded. Every one ended in a complete report: no hang, no reboot, no
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

Runs 4 to 8, at EL2 after Secure Launch (VERIFIED for this machine; the
firmware version is still to be recorded):

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

Phase 0A's CPU questions are answered. The firmware version these answers
hold for is still to be recorded; stage 3 records it, together with
`CNTFRQ_EL0` and the GICv3 virtual interface Route B needs. Phase 0B, the
offline kernelcache study, is in [GAOKUN3-PHASE0B.md](GAOKUN3-PHASE0B.md),
and the route decision it feeds in [GAOKUN3-PHASE0C.md](GAOKUN3-PHASE0C.md).

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
