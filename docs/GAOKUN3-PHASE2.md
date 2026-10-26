# gaokun3 Phase 2: EL2 entry and host link

Status, 2026-10-26: **started.** Phase 0C accepted Route B on 2026-10-26
([decision](GAOKUN3-PHASE0C.md#decision)). Phase 2's goal is q1n1 running at
EL2 on the gaokun3 and serving its proxy over USB. Its first piece, the VHE
hand-off below, passed on the gaokun3 in el2qual
([runs 11 and 12](#runs-11-and-12-2026-10-26)), and q1n1 now carries it
behind a build define and an option ([below](#in-q1n1)), tested on the host
and in QEMU but not yet run on the gaokun3.

## VHE hand-off

### The problem

slbounce enters EL2 with `HCR_EL2` set to E2H, RW and TGE: `0x488000000`,
with `SCTLR_EL2` at `0x30C5183D` (VERIFIED, el2qual runs 8 to 10,
[GAOKUN3-QUALIFICATION.md](GAOKUN3-QUALIFICATION.md#runs-so-far)). With E2H
set, EL2 translates through the EL2&0 regime, and four registers change
layout: `TCR_EL2` and `SCTLR_EL2` take the EL1 formats, and so do
`CPTR_EL2` and `CNTHCTL_EL2`.

q1n1 and the Snapintosh monitor assume a non-VHE EL2. The comments in
`probes/xnuboot.c` that program `CNTHCTL_EL2` and `CNTKCTL_EL1` say so. A
non-VHE value written while E2H is set, or the reverse, means something
else.

### Design

Two parts. One decides the values, the other switches the registers.

- **`probes/el2qual/vhe.{h,c}` decides.** `el2q_vhe_exit_plan()` takes the
  registers as found and returns either the non-VHE values that keep the
  same translation, caching and PAC controls, or the reason it cannot.
  - It writes no registers, so `tools/test-el2qual.c` checks every decision
    on the host.
  - It depends on nothing else in el2qual, so q1n1 can take it unchanged.
  - The entry conversion that the QEMU emulation uses (`el2q_vhe_tcr()`,
    previously in `el2qual.c`) lives beside it.
- **`el2q_exit_vhe` in `probes/el2qual/el2qual-asm.S` switches.** The boot
  CPU runs it alone, with interrupts masked and el2qual's vectors in place.
  The order:
  1. load every planned value;
  2. DSB, then the MMU off, then ISB;
  3. TLBI ALLE2 and DSB while E2H is still set;
  4. `HCR_EL2`, then ISB;
  5. `TCR_EL2`, `TTBR0_EL2`, `CPTR_EL2` and `CNTHCTL_EL2`;
  6. TLBI ALLE2, DSB, ISB;
  7. the planned `SCTLR_EL2`, then ISB.

  The routine's comment gives the reason for each step. UEFI maps VA = PA,
  so execution continues at the same addresses throughout.
- **Before switching**, el2qual disables EL2's own physical and virtual
  timers. Once interrupts are unmasked, an armed EL2 timer would fire, and
  after the switch the EL2 virtual timer can no longer be reached through
  the `CNTV` names.

**What the plan converts, and what it refuses.** It fails closed: anything
it does not recognise stops the exit. It was designed before slbounce's
`TCR_EL2`, `TTBR0_EL2`, `CPTR_EL2` and `CNTHCTL_EL2` had been measured.
Runs 11 and 12 measured them, and the plan accepts them unchanged.

| Register | Exit value | Refused when |
| --- | --- | --- |
| `HCR_EL2` | RW alone | E2H or TGE is clear, or any control other than E2H, TGE and RW is set |
| `TCR_EL2` | the same TTBR0 walk, rewritten in the non-VHE layout (IPS to PS, TBI0 to TBI, HA, HD, HPD0 to HPD) | TTBR0 walks are off (EPD0), TTBR1 walks are on (EPD1 clear: non-VHE EL2 has no TTBR1), or any other control is set (52-bit DS, TBID, TCMA, HWU, a RES0 bit) |
| `SCTLR_EL2` | M, A, C, SA, I, EnDB, WXN, IESB, EE, EnDA, EnIB, EnIA and DSSBS carried; the non-VHE RES1 bits filled; VHE-only EL0 controls dropped | any high-word control other than DSSBS (BT, TCF, ATA …) is set |
| `TTBR0_EL2` | the same table, ASID field cleared | never |
| `CPTR_EL2` | fixed `0x33FF`: SVE and SME trap (the gaokun3 has neither), FP/SIMD does not | never: the entry value is recorded, not converted |
| `CNTHCTL_EL2` | fixed `0x3`: the EL1 physical counter and timer usable from EL1, event stream off | never: recorded, not converted |

On the gaokun3's VERIFIED `SCTLR_EL2`, the plan returns the same number,
`0x30C5183D`. Its VHE-only EL0 bits sit exactly where the non-VHE layout
has RES1 bits.

**In el2qual**, the `exit-vhe` load option enables this. It is off by
default, and without it el2qual behaves exactly as before.

- With it, el2qual plans the exit right after its entry measurements. It
  switches only if the plan is sound, and then measures every CPU in
  non-VHE EL2. The 16K walk then goes through the EL1&0 regime the monitor
  will use, and secondaries copy the boot CPU's non-VHE controls.
- If the plan refuses, el2qual says why and measures in slbounce's state as
  before.
- The report gains a `VHE EXIT` line. `VHEX`, `VHEIN`, `VHEIN2`, `VHEOUT`
  and `VHEOUT2` carry the outcome, the registers as found, and the registers
  read back after the switch (el2qual version 4).

### Evidence

| Check | Result |
| --- | --- |
| Plan, host: `python3 tools/test-el2qual.py` | PASS, 2026-10-26. The gaokun3's VERIFIED entry values; exact round trips through both conversions for 4K, 16K and 64K walks with PS, TBI, HA, HD and HPD; each refusal, with the output left untouched; the report lines (ASan and UBSan) |
| Switch order, host: the same command | PASS. `el2q_exit_vhe` follows the order above, and 14 deliberately wrong copies are rejected: HCR after TCR, a missing ISB, DSB or TLBI, the MMU back on before the TLBI, a load with the MMU off, the MMU turned off with the planned value, a missing CPTR write, an unplanned VBAR write, the wrong TLBI. QEMU's TCG does not fault on a misordered barrier, so this check, not a QEMU run, carries the order |
| QEMU: `python3 tools/qemu-el2qual.py --config max-vhe-exit --config max-exit-novhe --config max --config max-vhe` | PASS, 2026-10-26 (OBSERVED, TCG, 4 CPUs). `max-vhe-exit` goes from emulated slbounce state (`HCR 0x408000000`) to `HCR 0x80000000`, with the registers read back equal to the plan and `TCR_EL2` back at EDK2's own `0x80853510`. All four CPUs then pass PAC and the 16K walk. `max-exit-novhe` reports `NOT-VHE` and changes nothing. `max` and `max-vhe` are unchanged |
| The QEMU checker's self-test | PASS. It rejects a missing `VHEX`, a refusal, `PLAN=DIFF`, E2H left set, a missing `VHEOUT`, a non-VHE entry, a switch that was not asked for, and a `DONE` claimed on a do-nothing run |
| gaokun3: el2qual `exit-vhe` | PASS, 2026-10-26, runs 11 and 12 (VERIFIED). The switch completed, and the registers read back equal the plan computed from the entry state the same run recorded. All 8 CPUs then ran in non-VHE EL2 with PAuth and the 16K walk ([below](#runs-11-and-12-2026-10-26)) |
| Plan against the measured state, host | PASS. The runs' `VHEIN`/`VHEIN2` values are a fixture in `tools/test-el2qual-vhe.c`, and the plan must reproduce their `VHEOUT`/`VHEOUT2` |

### Limits

- **The QEMU emulation of the entry is not slbounce.** It sets only E2H and
  TGE and converts only `TCR_EL2`. Its `CPTR_EL2` (0) and `CNTHCTL_EL2` are
  EDK2's leftovers. slbounce's real state differs: `CPTR_EL2` is `0x300000`
  and `TCR_EL2` describes a 40-bit walk. That state is now a host fixture,
  so QEMU is no longer the only check on it.
- **One firmware, one slbounce build.** The measured state is firmware 2.18
  with the slbounce and `tcblaunch.exe` of runs 8 to 12. Another firmware
  or slbounce build may hand over something else; the plan would then
  refuse, and say why.
- **Secondary CPUs.** On the gaokun3, PSCI starts them at EL2 in a state
  slbounce's code chose. el2qual's secondary entry writes the boot CPU's
  `HCR_EL2` first, then the other controls. That is correct for el2qual's
  own registers, but `CNTHCTL_EL2` and the EL2 timers on secondaries are
  left as PSCI hands them over.
- **The switch is single-CPU.** It must run before any secondary starts, as
  el2qual does.
- **q1n1 has not run it on the gaokun3.** Its copy ([below](#in-q1n1)) is
  checked on the host and in QEMU only.

### Runs 11 and 12, 2026-10-26

**How they were run.** From the `vhe-exit` stick. Its systemd-boot,
slbounce, `tcblaunch.exe` and device tree are run 10's files (same
SHA-256), with el2qual 4 (`558cc4ff`, which rebuilds identically from the
tree). Each run was a cold boot, run as
[Stage 2](GAOKUN3-QUALIFICATION.md#stage-2-secure-launch-to-el2) is, and
photographed. The runs were matched to their entries by what the photos
show.

```sh
python3 tools/el2qual-usb.py vhe-exit --systemd-boot <systemd-bootaa64.efi> \
    --slbounce <slbounce.efi> --tcblaunch <tcblaunch.exe> --dtb <gaokun3-el2.dtb>
```

| Run | Entry | Options | Result |
| --- | --- | --- | --- |
| 11 | `el2qual 6: VHE exit, boot CPU, no PAuth` | `nopac nosmp exit-vhe` | PASS: `VHE EXIT .. DONE  HCR_EL2 0000000080000000`; boot CPU `DONE`, 16K `WALKS` (`PAR 004001B00`, controls `A OK  B FAULT  C FAULT`); PAuth `NOT RUN`, as asked |
| 12 | `el2qual 7: VHE exit, all CPUs and PAuth` | `exit-vhe` | PASS: the same exit; all 8 CPUs `DONE` at EL2 through PSCI, PAC `YES` and 16K `YES` on each; PAuth signs (4/4), keys and round trip OK, `PACGA` OK; only MIDR differs between cores (cores 4 to 7 are `0x410FD4C0`), as in run 9; `VIRTUALMAC ROUTE (EL1) .. CPU OK` |

No hang, no reboot and no unexpected exception occurred. The rest of each
report (firmware 2.18, `CNTFRQ_EL0` 19200000, the GIC virtual interface,
NV2, ECV, FGT and RNDR absent, the ID registers) matches run 9.

**slbounce's EL2 state, measured for the first time** (VERIFIED, both runs,
from the `VHEIN` lines):

| Register | Value | Decoded (E2H set, so the EL1 layouts) |
| --- | --- | --- |
| `HCR_EL2` | `0x488000000` | E2H, RW, TGE, as runs 4 to 10 showed |
| `SCTLR_EL2` | `0x30C5183D` | M, C, SA, I; EL0 controls SA0, CP15BEN, EOS, nTWI, nTWE, EIS, SPAN, nTLSMD, LSMAOE |
| `TCR_EL2` | `0x280803518` | T0SZ 24 (a 40-bit VA), 4K granule, inner-shareable write-back walks, TTBR1 walks off (EPD1), IPS 40-bit |
| `TTBR0_EL2` | `0x9F519000` | UEFI's identity map, ASID 0 |
| `CPTR_EL2` | `0x300000` | FPEN `0b11`: FP/SIMD untrapped; ZEN and SMEN 0: SVE and SME trapped |
| `CNTHCTL_EL2` | `0x3` | EL0PCTEN, EL0VCTEN |

**After the switch** (`VHEOUT`): `HCR_EL2 0x80000000`, `SCTLR_EL2
0x30C5183D`, `TCR_EL2 0x80823518` (the same 40-bit 4K walk, PS 40-bit),
`TTBR0_EL2 0x9F519000`, `CPTR_EL2 0x33FF`, `CNTHCTL_EL2 0x3`. Every value
equals what `el2q_vhe_exit_plan()` computes from the entry state above.
The `PLAN=MATCH` line goes to serial only, so this was checked on the host
from the photographed numbers, and the pair is now a host fixture.

**What it shows.**

- On this machine's firmware and slbounce, EL2 can leave VHE for the
  non-VHE EL2 q1n1 and the monitor assume.
- Translation, PAuth and PSCI secondary start-up all work afterwards, on
  every core.
- It does not show q1n1 or the monitor running in that state. Neither has
  run on the gaokun3.

### In q1n1

q1n1 (`../q1n1`, cloned at `5b440d4`) carries the same hand-off. Its
`docs/GAOKUN3-VHE-EXIT.md` has the details.

- **`vhe.{h,c}`** are verbatim copies of this repository's (commit
  `04de4f8`).
- **`platform/uefi/vhe-exit.S`** has the switch, `q1n1_exit_vhe`, which is
  el2qual's sequence.
- **`vhe_exit()` in `main.c`** runs it at the top of `q1n1_main()`, before
  the proxy or the foothold starts.
- **It is opt-in twice:**
  - it is compiled only with `Q1N1_DEFINES=-DQ1N1_VHE_EXIT`, into its own
    `OUT`;
  - it runs only with `--exit-vhe`.

  The default `q1n1.efi` and all four stage binaries equal the baseline
  build byte for byte, apart from the PE TimeDateStamp, which differs
  between any two builds. The stage pins here therefore stay valid.

| Check (`python3 tools/test-q1n1-vhe-exit.py --qemu`, in q1n1) | Result, 2026-10-26 |
| --- | --- |
| Switch order | PASS: the required order, and 14 reordered copies rejected |
| Plan against the gaokun3's measured state (ASan+UBSan) | PASS |
| Default build carries no exit | PASS |
| QEMU foothold, `--emulate-vhe --exit-vhe` | PASS: `VHE EXIT DONE PLAN MATCH`, `HCR` `0x408000000` to `0x80000000`, the foothold keeps drawing |
| QEMU foothold, `--exit-vhe` alone | PASS: `NOT-VHE` |
| QEMU, default build given `--exit-vhe` | PASS: ignored |
| QEMU `--uart-proxy` after the exit | PASS: the proxy answers, and a chainloaded stage runs at EL2 |
| gaokun3 | NOT RUN |

The q1n1 change is committed locally in that checkout as `31147ab` (the code
and its test) and `660dfbb` (its docs), and pushed nowhere.

### Run 13, 2026-10-26: USB inventory

**Why.** On the A16, q1n1's link to the Mac starts from the firmware's own
USB-function (device mode) driver. Whether the gaokun3's firmware installs
one decides how Phase 2's link is built.

**How.**
- **The probe:** q1n1's passive `usb-probe.efi`, in the `usb-probe-hold`
  build (`0366c947`, built from q1n1 `660dfbb` plus its hold option), on the
  `usb-inventory` stick.
- **The stick:** systemd-boot and the probe only, with no slbounce,
  `tcblaunch.exe` or device tree, so no Secure Launch was attempted.
- **What it reads:** standard UEFI protocol lookups only: no controller
  methods, MMIO or ExitBootServices.
- **QEMU control** (`python3 tools/test-q1n1-usb-probe-qemu.py`, in q1n1):
  PASS.

**Result (VERIFIED, photographed): PASS, read only.**

| Protocol | gaokun3, firmware 2.18 |
| --- | --- |
| USB function (`EFI_USBFN_IO_PROTOCOL`) | not installed |
| USB2 host controller | 3 handles: `Pci(0x80,0x0)`, `Pci(0x81,0x0)`, `Pci(0x82,0x0)` |
| Serial I/O | not installed |

Entry was at EL1. The probe returned to the boot menu after its hold.

**Reading it.**

- **There is no firmware device-mode driver to start from.** On the A16,
  q1n1's boot window has the firmware's USB-function driver bring USB0 up in
  device mode, and q1n1 takes the controller over after ExitBootServices
  from a snapshot of that state. None of that is available here.
- **The three host controllers are, INFERRED, the three DWC3s.** The
  gaokun-android inventory (OBSERVED under Linux, 2026-10-05) maps them:
  - `a600000` (`usb_0`) serves the USB-C port next to the power button,
    and is the only one that can be a device (role switch; Android's adb
    used it);
  - `a800000` (`usb_1`) is the other port, host only;
  - `a400000` is the keyboard cover.

  The device paths are firmware-made, as with the A16's `XhciPciEmulation`
  (q1n1 `docs/A16-USB-BINDING.md`). Which handle is which controller is not
  shown.
- **No serial path either.** No Serial I/O driver is installed. The ACPI
  SPCR table does name a UART. Corrected 2026-10-26 after runs 14 and 15,
  which displayed it: this line had said the SPCR UART was missing. It is at
  `0xA90000` with interface type `0x11`, not the PL011/SBSA types el2qual
  and q1n1 drive. Whether it is reachable on this machine is UNKNOWN.

**What it decides.** Phase 2's USB link must be q1n1's own DWC3
device-mode driver (`qcom-dwc3.c`, which passed after ExitBootServices on
the A16) on `a600000`, the power-button port. It starts from a controller
the firmware left in host mode, with no firmware snapshot to adopt. That
means q1n1 must:
- switch the controller to device mode itself;
- set up whatever PHY and Type-C data-role state device mode needs;
- not rely on a boot-window snapshot.

None of this is designed yet. It is the next host-side study.

### Runs 14 and 15, 2026-10-26: q1n1's first boot

**How.** From the `q1n1-foothold` stick.
- **q1n1:** q1n1.efi `3bf2020f`, built from q1n1 `6865a21` with
  `-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER`. The same binary passed q1n1's
  `tools/test-q1n1-dt-fb-qemu.py` just before.
- **The other files:** systemd-boot, slbounce, `tcblaunch.exe` and the EL2
  device tree are run 10's (same SHA-256).
- **Procedure:** each run was a cold boot, photographed. The runs were
  matched to their entries by the photos.

```sh
python3 tools/el2qual-usb.py q1n1-foothold --q1n1 ../q1n1/build/uefi-gaokun3/q1n1.efi \
    --systemd-boot <systemd-bootaa64.efi> --slbounce <slbounce.efi> \
    --tcblaunch <tcblaunch.exe> --dtb <gaokun3-el2.dtb>
```

| Run | Entry | Options | Result |
| --- | --- | --- | --- |
| 14 | `q1n1 1: EL2 foothold (no VHE exit)` | `--el2` | PASS: green bar; `EL2 CONFIRMED AFTER EXITBOOTSERVICES`; entry EL1, current EL2; MIDR `0x410FD4B0`; `FRAMEBUFFER 0xC6200000`; `SCREEN: DT SIMPLE-FB - CLEANED DRAWS`; 43 memory descriptors |
| 15 | `q1n1 2: EL2 foothold after the VHE exit` | `--el2 --exit-vhe` | PASS: as run 14, plus `VHE EXIT DONE PLAN MATCH`. `VHE IN` `HCR 0x488000000`, `TCR 0x280803518`, `CPTR 0x300000`, `CNTHCTL 0x3`; `VHE OUT` `HCR 0x80000000`, `TCR 0x80823518`, `SCTLR 0x30C5183D`, all equal to runs 11 and 12; 45 memory descriptors |

Both screens also show `SPCR UART 0xA90000`, `TYPE 0x11`, `GSI 0x182`,
`BAUD CODE 7` and `UART TX DISABLED OR UNAVAILABLE`.

**Reading it (VERIFIED unless marked).**

- **q1n1 runs on the gaokun3.** slbounce's Secure Launch takes it to EL2,
  and its foothold runs there.
- **It draws on the device tree's framebuffer.** The fallback accepted
  `/chosen/framebuffer@c6200000` against the firmware's memory map, and
  every line q1n1 drew after its full-screen fill is legible.
- **The cleaned draws reach the display.** Whether the cleaning is
  *necessary* is not shown: no run drew uncleaned for comparison.
- **The VHE exit behaves in q1n1 as in el2qual,** on the same state.
- **Not in the photos:** the logo (top right) and the heartbeat (bottom
  left). Both photos crop the screen's right side and lower half, so this
  is INFERRED to be framing, not a failed draw.
- **The serial port:** the firmware's SPCR names a UART at `0xA90000` with
  interface type `0x11`, which q1n1 does not drive (PL011/SBSA only). This
  corrects the earlier "missing SPCR UART" (run 13's reading, above).

**What it does not show.** The proxy (`--boot`) and any USB. On the gaokun3
q1n1 has no transport to the Mac yet.

### Next

- **The USB link to the Mac** ([run 13](#run-13-2026-10-26-usb-inventory); study:
  [GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md)). It must be built on q1n1's own
  DWC3 device-mode driver, from a controller the firmware leaves in host
  mode. Its first step, el2qual's read-only `usb` stage, ran on the gaokun3
  (runs 16 and 17).
  - **USB0 stays powered after ExitBootServices.**
  - **Two things block a device-mode run.** q1n1's identity check expects
    the A16's xHCI `CAPLENGTH` (`0x30`, the gaokun3's is `0x20`), and USB0's
    DMA is translated by the SMMU through context bank 6, so it is not known
    to be identity.
  - **Run 19:** context bank 6 has stage-1 translation off, but nests a
    stage 2, INFERRED to be bank 126.
  - **Run 20:** el2qual 7 could not read that stage-2 bank. Its page is not
    in UEFI's page tables, so it was refused unread.
  - **Next:** el2qual 8's `usb-smmu-map` (stick entry 10) adds its own
    Device map of the SMMU window, then reads that bank (built, host and
    QEMU tested, [step 1d](GAOKUN3-USB-DEVICE-MODE.md#step-1d-el2quals-own-map-of-the-smmu-window-el2qual-8-entry-10)).
    A supervised run decides whether DMA is 1:1, or a bypass for stream
    `0x820` is needed.
- **q1n1's foothold on the gaokun3: done** ([runs 14 and
  15](#runs-14-and-15-2026-10-26-q1n1s-first-boot)), with the device-tree
  framebuffer and the VHE exit. q1n1 `docs/GAOKUN3-DT-FRAMEBUFFER.md` has
  the fallback's design and host evidence.
