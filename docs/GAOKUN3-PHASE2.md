# gaokun3 Phase 2: EL2 entry and host link

Status, 2026-10-26: **started, host only.** Phase 0C accepted Route B on
2026-10-26 ([decision](GAOKUN3-PHASE0C.md#decision)). Phase 2's goal is q1n1
running at EL2 on the gaokun3 and serving its proxy over USB. Its first
piece, the VHE hand-off below, is built and tested on the host. Nothing in
this phase has run on the gaokun3 yet.

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
it does not recognise stops the exit. Of slbounce's state, `TCR_EL2`,
`TTBR0_EL2`, `CPTR_EL2` and `CNTHCTL_EL2` are UNMEASURED.

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
| gaokun3 | NOT RUN |

### Limits

- **The QEMU emulation of the entry is not slbounce.** It sets only E2H and
  TGE and converts only `TCR_EL2`. Its `CPTR_EL2` (0) and `CNTHCTL_EL2` are
  EDK2's leftovers. Whether the plan accepts the gaokun3's real `TCR_EL2`
  is UNKNOWN until a hardware run records it.
- **Secondary CPUs.** On the gaokun3, PSCI starts them at EL2 in a state
  slbounce's code chose. el2qual's secondary entry writes the boot CPU's
  `HCR_EL2` first, then the other controls. That is correct for el2qual's
  own registers, but `CNTHCTL_EL2` and the EL2 timers on secondaries are
  left as PSCI hands them over.
- **The switch is single-CPU.** It must run before any secondary starts, as
  el2qual does.
- **q1n1 is not changed yet.** No q1n1 checkout exists beside this
  repository (`../q1n1`, README). `vhe.{h,c}` and `el2q_exit_vhe` are meant
  to be adopted there once the gaokun3 run passes.

### Next: one supervised run

el2qual with `serial exit-vhe` (and optionally `thermal`) from the existing
stick, under slbounce, as in
[Stage 2](GAOKUN3-QUALIFICATION.md#stage-2-secure-launch-to-el2).

- **Risk:** el2qual touches no storage, clocks or voltages.
- **If the switch hangs,** the last on-screen line is `VHE EXIT: CLEARING
  E2H. A HANG OR RESET HERE IS THE SWITCH`. Holding the power button ends
  the run, the same recovery as every el2qual run.
- **It records,** for the first time, slbounce's `TCR_EL2`, `TTBR0_EL2`,
  `CPTR_EL2` and `CNTHCTL_EL2`.
- **What passing means:** `VHEX DONE PLAN=MATCH`, then all eight CPUs
  `DONE` with PAC and the 16K walk.
- **What a refusal means:** the plan met state it does not recognise. That
  is a finding to record, not a failure to work around.

The run is the owner's to make.
