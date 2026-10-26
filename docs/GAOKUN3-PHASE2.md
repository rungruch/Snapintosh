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

The q1n1 change is uncommitted in that checkout and pushed nowhere.

### Next

- **Run q1n1's `--el2 --exit-vhe` foothold on the gaokun3.** It is the only
  q1n1 mode meaningful there today. It needs a stick that boots the
  `-DQ1N1_VHE_EXIT` `q1n1.efi` under slbounce, as el2qual's stick does.
- **The rest of Phase 2:**
  - USB device mode on DWC3 `a600000.usb`, including whether q1n1 must set
    the Type-C role through the EC itself;
  - the board adapter behind `HostRuntime`;
  - the acceptance run: the proxy answering over NCM, and `probes/run.py
    idregs` and `tgran16` matching Phase 0A.
