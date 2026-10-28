# gaokun3 Phase 3: the VirtualMac kernel to a console (design)

**Status, 2026-10-28: P1 to P4 and P7 done; P5 and P6 built; all host and
QEMU only. In QEMU the stock kernel reaches the root-mount wait with every
device a monitor model (P3's bar). Decisions 1, 2, 3 and 5 and the CPU
count are made; decision 4 is open. Nothing run on hardware.** It does Phase 3 before Phase 2's USB
host link, with the guest loaded from the USB stick. That
changes the order [Phase 0C](GAOKUN3-PHASE0C.md#decision) set, and
AGENTS.md calls standalone execution "a direction, not an already
implemented or authorized replacement" for the current flow. The owner
decided for it on 2026-10-27, for the gaokun3's Phase 3 only and as
transitional (decision 1 in [Decisions](#decisions-for-the-owner)).
Decisions 2 to 5 are open.

Evidence classes follow AGENTS.md: VERIFIED, OBSERVED, INFERRED,
EXPERIMENTAL and UNIMPLEMENTED. Unmarked statements about the plan are
proposals.

## Goal and acceptance

Phase 0C sets Phase 3's bar ([condition 2](GAOKUN3-PHASE0C.md#decision)):
the first gaokun3 EL1 boot of the stock 26A428 VirtualMac kernel must reach
"a console, then an unmodelled platform contract". Acceptance has two parts:

1. **A console:** the kernel's own output on the gaokun3's screen, at least
   its banner.
2. **A named stop:** the boot continues to the first platform contract the
   monitor does not model, and the monitor reports which one.

That run can **reopen Route B**. Any of these sends the decision back before
further Route B work:
- an Apple-only CPU behaviour EL2 cannot supply (overturn condition 1);
- the PAC calls failing on PAuth2 with QARMA5 (condition 2);
- a platform dependency the monitor cannot model (condition 3).

Out of scope for Phase 3: storage and a root disk (Phase 4), input
(Phase 5), the GPU (Phase 6), more than one vCPU, the AuxKC, and any device
DMA.

## Why now, and why from the stick

- **It is the project's largest open risk.** Phase 3 can reopen Route B.
  If Route B fails on real hardware, the USB link and the SMMU work matter
  far less. Testing it early tests everything built on it.
- **The USB link is blocked, and Phase 3 does not need it.** The link waits
  on USB0's SMMU translation, whose next step (stage 2 of the S2CR bypass)
  has an unknown outcome
  ([GAOKUN3-USB-DEVICE-MODE.md](GAOKUN3-USB-DEVICE-MODE.md#step-1i-stage-2-the-first-smmu-write-design-and-host-model)).
  Phase 3 uses no device DMA: every device the guest sees is a model in
  the monitor, in software. So it does not depend on the SMMU at all.
- **The stick already carries the boot chain.** Since run 11 the sticks
  boot through slbounce to EL2 (run 13's inventory stick aside), and
  q1n1's `--el2` foothold has run there with the VHE exit and the
  device-tree framebuffer (runs 14 and 15).
- **AGENTS.md's standalone-first rule** asks that new EL2 services not
  depend on a connected host for their fundamental operation. A guest that
  boots from the stick, with its console on the screen, meets that.

Phase 4 will likely meet the SMMU again: the monitor's NVMe host does DMA,
and on this SoC NVMe's PCIe traffic plausibly goes through `apps_smmu` as
USB0's does (INFERRED, unmeasured). So the SMMU work is deferred, not
wasted.

## What QEMU established

On a generic Armv8.5 CPU with PAuth and the 16 KiB granule, the stock
26A428 VirtualMac kernel reached its root-mount wait needing only these
(OBSERVED, host only;
[Phase 0B](GAOKUN3-PHASE0B.md#cpus-trimmed-2026-10-26-the-kernel-waits-for-a-root-disk)):

| Need | Contract |
| --- | --- |
| PAC hypercalls | `SET_INITIAL_STATE`, `GET_DEFAULT_KEYS` (returns `x2`/`x3`), `SET_B_KEYS`, the two diversifier calls; `x0` 0 or the CPU spins; preserve every register but `x0` to `x3` ([B6](GAOKUN3-PHASE0B.md#b6-every-hypercall-and-what-a-monitor-must-answer-26a428)) |
| OEM calls | none: `PAC_NOP` answered `NOT_SUPPORTED` turns the whole service off |
| PSCI | through `hvc`; `PSCI_VERSION` seen |
| Entry | classic: EL1, `x0` the physical address of `boot_args`, MMU and caches off; the kernel builds its own tables ([B8](GAOKUN3-PHASE0B.md#b8-the-virtualmac21-entry-and-boot-handoff-26a428)) |
| Placement | the kernelcache's physical base keeps its Mach header's 32 MiB-block offset, `0x1004000` |
| `/chosen` | 256 bytes of real entropy, a valid NVRAM bank, `dram-base` and `dram-size`, `/cpus` trimmed to the CPUs started |
| Devices | a GICv3, a PL011 and the timer, as QEMU models; **and** eleven more windows the kernel touched, every one answered by something that is not that device ([P1](#p1-result-what-the-kernel-touches)) |

Every stop was a firmware duty. None was Apple-only CPU behaviour, and the
kernel touched no implementation-defined register. But "needs only a GICv3,
a PL011 and the timer" was too strong. The smoke moves `/arm-io`'s `ranges`
onto QEMU's map, so the other template devices move into QEMU's PCIe
window, which answered every read with all-ones. The kernel reached its
root-mount wait **with all of them effectively absent**, not without
touching them.

## What changes on real hardware

1. **Stage 2 becomes mandatory and default-deny.** The QEMU stub ran the
   guest with no stage 2, against QEMU's own devices, after moving the
   tree's PL011 and GICv3 windows onto them. On the gaokun3 that would be
   unsafe: the template's `/arm-io` range, physical `0x10000000` to
   `0x80000000` (VERIFIED,
   [B3](GAOKUN3-PHASE0B.md#b3-the-virtualmac21-device-model-26a428)),
   contains real SC8280XP MMIO, for example `apps_smmu`'s window at
   `0x15000000` (VERIFIED, runs 17 to 25). The guest gets its RAM, and the
   framebuffer region it is given, through stage 2, and nothing else.
   Every other IPA is absent, and a touch stops the run with its address.
   That is already the A16 monitor's policy ("everything else absent, a
   touch stops the run with its IPA", `probes/xnuboot.py`).
   So each window [P1](#p1-result-what-the-kernel-touches) lists becomes
   a stop on the gaokun3, unless the monitor models it or the owner
   chooses another answer ([decision 5](#decisions-for-the-owner)).
2. **There is no PL011.** The gaokun3's SPCR names a UART at `0xA90000`,
   interface type `0x11`, not a PL011, and q1n1 does not drive it
   (VERIFIED, run 14). So the guest's
   PL011 is a monitor model at the template's address, and its output is
   drawn on the screen ([Console](#console)).
3. **Interrupts need a virtual GICv3.** The kernel runs `AppleARMGICv3`
   and programs a GICv3 through system registers and MMIO. The A16 monitor
   never shows its guest a GIC: it takes physical interrupts at EL2 and
   presents an emulated AIC through `HCR_EL2.VI` (`compat/irq-forward.h`).
   Route B needs the reverse:
   - the guest's CPU interface is the hardware virtual interface, through
     list registers. Run 9 found it usable at EL2, with 4 list registers
     and LR0 writable (VERIFIED);
   - the distributor and redistributors are monitor models at the
     template's addresses;
   - the physical side, taking interrupts at EL2 and completing them,
     reuses the A16 monitor's GIC handling.

   The physical GIC's addresses come from the gaokun3's ACPI MADT, read
   and checked, not assumed (AGENTS.md: register provenance).
4. **The timer** is the guest's virtual timer, at `CNTFRQ_EL0` 19,200,000
   (VERIFIED, run 9), delivered as a virtual interrupt.
5. **The CPU is a Cortex-A78C.** The boot CPU, slot 0, reports MIDR
   `0x410FD4B0` (VERIFIED, run 26's report). QEMU ran `-cpu max`, so the
   kernel has tolerated a non-Apple MIDR, but only there (OBSERVED, QEMU).
   Phase 3 presents the real MIDR. Overriding it through `VPIDR_EL2` would
   be a fabricated identity, used only if a stop shows the kernel needs
   one.
6. **PAC runs on QARMA5.** Phase 3 is the first real evidence for overturn
   condition 2.
7. **Implementation-defined registers** trap to EL2 (`HCR_EL2.TIDCP`).
   QEMU saw none, but the A78C has its own. By default a trap **stops the
   run and reports the register**: fail closed. The QEMU stub's read-as-zero
   stand-in is a diagnostic, allowed only behind an opt-in gate, because
   returning zero for an unknown register is fabricated behaviour.
8. **Memory** is laid out against the gaokun3's UEFI memory map, not
   QEMU's. The guest's IPA space follows the template: devices under
   `/arm-io`, RAM above them, with the kernelcache placed to keep the
   `0x1004000` offset. Stage 2 maps the guest's RAM onto one contiguous
   block taken from UEFI before `ExitBootServices`. QEMU's 448 MiB was
   enough to reach the root-mount wait (OBSERVED), so Phase 3 starts there.
9. **The framebuffer** is the device tree's simple-framebuffer at
   `0xC6200000`, 1600 by 2560, portrait (VERIFIED, run 26's report).

## P1 result: what the kernel touches

**2026-10-27: DONE, OBSERVED, host only.** Two runs on QEMU 11.1.2 (TCG,
`-cpu max,sve=off,sme=off`), both to the root-mount wait, harness PASS:

```sh
python3 tools/test-vmapple-smoke.py
python3 tools/vmapple-smoke/smoke.py boot --trace --mmio-trace --timeout 300 \
  --kernelcache downloads/extracted/vma2macosap/kernelcache.release.vma2
```

`--mmio-trace` turns on QEMU's `memory_region_ops_read` and `_write` trace
events. The report places each access in the completed tree's windows and
names the QEMU region that answered (`build/vmapple-smoke/mmio.tsv`). The
driver column comes from a separate local study: a scratch QEMU plugin
recorded each access's PC, matched to the kernelcache's fileset entries.
That plugin and its disassembly stay in `build/` and are not committed.

Beyond the PL011 and the GIC's distributor and redistributors, which QEMU
modelled, the kernel touched eleven windows. Every read returned all-ones.

| Window | Driver | What it did |
| --- | --- | --- |
| `avp-ctrr` | `com.apple.kernel` | one read at +0, then "No AVP CTRR device found" |
| **physical `0x30010000`, no tree node** | `AppleVirtualPlatform` | reads `/product/util`'s `regions`, maps one page at this fixed address and reads a device ID; a non-fatal assertion says it is not the "SimpleVirtio" ID; it carries on |
| **physical `0x004FFFF8`, no tree node** | `AppleVirtualPlatform` | one 64-bit store of a guest RAM address (a request it built) to this fixed address, then checks a field of that request (INFERRED: its status). QEMU's flash takes 4-byte accesses, so the trace and `mmio.tsv` show it as two writes, to `…FFF8` and `…FFFC` |
| `aes`, second window | `AppleS8000AES` | four reads at +0 |
| `gfx` | `AppleParavirtGPUIOGPUFamily` | reads at +0x1014, +0x1018 and +0x1034; writes at +0x1034 and +0x1000 |
| `iosurface-mapper` | `AppleParavirtIOSurface` | reads at +0x1028 and +0x102c; ten writes from +0x1000 to +0x1024, including two guest RAM addresses |
| `avp-rtc` | `AppleVirtualPlatform` | reads at +0x0, +0x4, +0x18 and +0x1c; writes at +0x20, +0x24, +0x38 and +0x3c |
| `pl031-rtc` | `AppleVirtualPlatform` | a write at +0x10; reads at +0xc and +0x0 |
| `pl061-buttons` | `AppleVirtualPlatform` | byte writes at +0x40c and +0x410 |
| `gic`, third window | `AppleVirtualPlatform` | one read at +0x8, just before PCI configuration (INFERRED: the MSI frame) |
| `pcie` configuration space | `AppleVirtualPlatform` | bus 0: device 0 at +0x0, +0x4 and +0x34, then the vendor ID of devices 0 to 31; logs "bridges 1, devices 0" |

`pvpanic`, `avp-sealed-registers` and the first `aes` window were not
touched.

**What it shows.**

- **Two fixed addresses no tree node describes.** A monitor cannot learn
  them from the tree, so they must be listed per kernel build.
  - `0x30010000` is an immediate in `AppleVirtualPlatform`'s code
    (OBSERVED, local kernelcache).
  - `0x004FFFF8` is fixed in practice. The studied code computes the store
    address from a value set before the window it covers, so the code does
    not show it as a constant. But it did not move when the tree was
    changed ([recheck](#recheck-2026-10-27)), so it does not come from
    `/product/util`.

  In the VirtualMac physical map, `0x30010000` falls in what
  QEMU's own reverse-engineered `vmapple` machine calls the backdoor
  interface (`0x30000000`, 2 MiB), at its AUX device (INFERRED from
  QEMU's `hw/vmapple`, which models macOS 12 guests, not 26A428). No public
  description of `0x004FFFF8` has been found (UNKNOWN).
- **The smoke wrote to a QEMU device it never meant to expose.** The store
  to `0x004FFFF8` landed on QEMU virt's `flash0`. It was harmless on the
  host, but it shows the smoke could not keep the guest off devices that
  are absent from its tree. P3's bar is the fix: run `--mmio-trace` and
  require no access answered by a QEMU device.
- **Each device was tolerated absent.** The kernel reached the same
  root-mount wait with every one of them answering all-ones. So the list
  does not trigger overturn condition 3 yet. It does not show that the
  drivers tolerate absence in the form the gaokun3 can offer, which is the
  next decision.
- **The first touch comes after the banner.** The CTRR probe follows the
  kernel's early log, so H3 (the banner) does not depend on any of these.
  Under default-deny stage 2, H4's first stop would be that probe.
- **Order.** The first three came in the same order in both runs. The rest
  follow IOKit's driver matching and changed order between runs.

### Recheck, 2026-10-27

An independent rerun and two tree experiments, all host only:

- **The rerun reproduced P1:**
  - the same root-mount wait, with harness PASS;
  - the same 14 windows, 11 of them not modelled;
  - every unmodelled read all-ones;
  - the same first three.

  Per-window access counts vary between runs: `avp-rtc`'s reads were 2, 4
  and 8 across the runs. Nothing above depends on a count.
- **The banner precedes every unmodelled access.** Rebuilding the serial
  text from the trace's PL011 writes shows `Darwin Kernel Version`,
  `RELEASE_ARM64_VMAPPLE` and `VM bootstrap` printed before the `avp-ctrr`
  probe, the first unmodelled access. The line just before that probe is
  the kernel's report that `gPanicBase` is not initialised: the
  panic-region duty.
- **`0x004FFFF8` does not come from `/product/util`.** That node carries a
  `regions` table of seven `(id, offset, size)` records, and an empty
  `reg` that is not flagged as a placeholder and that the smoke never
  fills. Regions 7 and 4 together span `0x500000` bytes, and their last 8
  bytes, counted from region 7, sit at `0x4FFFF8`. So the store looked
  derivable from the tree. Two boots from a scratch copy of the loader, in
  ignored `build/` and not committed, tested it:
  - with `reg` set to base `0x2C000000`, size `0x2000000`, the store stayed
    at `0x004FFFF8`, and nothing in `0x2C000000`–`0x2E000000` was touched;
  - with `reg` left empty and region 4 cut from `0x4FF000` to `0x3FF000`,
    the store stayed at `0x004FFFF8`.

  Both reached the root-mount wait with harness PASS, and `0x30010000`, an
  immediate, stayed put in both as a control. So the match was a
  coincidence: as far as the tree goes, the address is fixed.

## P2 result: the classic loader and the panic region

**2026-10-27: DONE, OBSERVED, host and QEMU only.**

- The classic loader lives in `probes/classicload.py` and is re-exported by
  `probes/xnuboot.py`, so there is one loader, not two. It is a separate
  module only so the smoke harness can use it without `xnuboot`'s SPTM
  import chain, which needs a newer Python than this host's 3.9. The J700
  path's `boot_args` and the classic path now share one packing function.
  The smoke's `kcload.py` is an adapter over it; its duplicate layout,
  tree-completion and NVRAM code is deleted. With the same entropy source
  its `lay_out` is byte-identical to the `xnuboot` path
  (`tools/test-vmapple-classic.py`).
- The guest profile `vma2macosap-26A428` (`abi='classic-vmapple-v1'`) is
  registered in `probes/guestprofile.py` with the locally extracted
  bundle's hashes; the SPTM/TXM fields are zero or None, and
  `xnuboot.configure_guest` refuses it (fail closed). Its host checks were
  SKIP on this host: `guestprofile.py` needs Python 3.10 or newer, and
  only 3.9 is installed here — the same pre-existing limit as the J700
  profile's consumers.
- The panic-region contract is published XNU's (`xnu-12377.121.6`,
  `pexpert/arm/pe_init.c`, `check_for_panic_log`): the region's physical
  base is `/pram`'s `reg[0]` and its usable size is
  `/chosen/embedded-panic-log-size`; either missing leaves `gPanicBase`
  unset. The template's `/pram` reg is all zero and it has no log-size
  property, so the loader supplies both — and the region must sit outside
  the DRAM the guest is told it owns, as iBoot's carved region does. A
  1 MiB region just above `topOfKernelData`, inside DRAM, regressed the
  QEMU boot to a halt before the kernel's first console line (OBSERVED);
  the same region just above the reported DRAM passes. Why the in-DRAM
  placement halts is UNKNOWN; the loader refuses it.
- Evidence: `python3 tools/test-vmapple-classic.py` PASS (16 checks; the
  profile checks above SKIP), `python3 tools/test-vmapple-smoke.py` PASS
  (78 checks, including the panic-region tree checks), `smoke.py selftest`
  PASS, and `smoke.py boot --trace --mmio-trace` PASS to the root-mount
  wait with the P1 result reproduced exactly: 14 windows touched, 11 not
  modelled.

## P3 result: the monitor in QEMU (partial)

**2026-10-27: PARTIAL, OBSERVED, QEMU only.** The Route B monitor exists
in the smoke harness and boots the stock kernel through its own models,
but the root-mount bar is not met yet. Decision 5 is still the owner's;
what is built follows its recommended split, so the decision can still
change the answers.

**Built** (`tools/vmapple-smoke/stub.S`, `monitor.c`, behind
`PARAM_FLAG_MONITOR`, off by default; the Phase 0C smoke is unchanged
without it):

- **Stage 2 default-deny.** A three-level 4 KiB stage 2 maps only guest
  RAM (2 MiB blocks, 0x44000000 to 0x61200000). The guest's IPAs are the
  VirtualMac template's own (`QEMU_MONITOR_LAYOUT` in
  `probes/classicload.py`), so QEMU's devices are unreachable. Stage 2
  translation faults carry no syndrome (ISV 0), so the monitor translates
  the guest ELR with `AT S1E1R`, reads the faulting instruction from
  guest RAM and decodes the unsigned-immediate, register-offset and
  pre/post-index load/store forms; anything else stops by name. For a
  stage 2 fault the IPA comes from `HPFAR_EL2`, not `FAR_EL2`.
- **Device models** (`monitor.c`, host-tested): PL011 (output to the
  monitor's UART), PL031 and PL061 to their published contracts, a
  register-state GICv3 distributor and redistributor (identity registers
  aligned to QEMU's measured `GICD_TYPER 0x037a0208`, `IIDR 0x43b`), the
  MSI frame, pvpanic, and PCIe configuration space answering all-ones,
  PCI's defined answer for an absent function.
- **Virtual GICv3 and timer.** `ICH_HCR_EL2` enables the virtual CPU
  interface; the physical virtual-timer PPI (27) is delivered through
  list register 0 with its EOI bit, the maintenance PPI (25) re-arms it,
  and the physical CPU interface is enabled at EL2 (`ICC_PMR_EL1`,
  `ICC_IGRPEN1_EL1` — without those, no IRQ ever reached EL2). The PAC
  service and PSCI are the stub's, unchanged. The paravirtual devices
  with nodes are dropped by the loader (option b); the fixed addresses
  stop by default (option c).

**Verified:**

- `smoke.py boot --monitor` PASS, before the interrupt changes below; the
  2026-10-28 rerun of the same command now stalls (see
  [the rerun](#rerun-2026-10-28-the-monitor-boot-stalls-after-lockdown_mode)).
  At the time: the banner and all early console output
  come through the PL011 model, GIC initialisation completes against the
  models, and the run stops by name at the first fixed address,
  `EL2: stop: unmodelled IPA 0x0000000030010000 read`. QEMU's MMIO trace
  shows only `pl011` and `gicv3_redist`, both touched by the monitor at
  EL2; no QEMU device answers the guest.
- Host suites PASS: `tools/test-vmapple-monitor.py` (21 checks, ASan and
  UBSan, including the negative cases: unmodelled and fixed IPAs stop
  without the gate), `tools/test-vmapple-smoke.py` (78),
  `tools/test-vmapple-classic.py` (20; profile checks SKIP, Python 3.9).
  The legacy smoke and `selftest` PASS unchanged.

**Not met:** under the diagnostic `--absent-fixed` gate the boot passes
both fixed addresses and a third one P3 found — a computed-pointer read
at `0x4006cc` during IOKit start, answered by QEMU's flash in the old
smoke, contract UNKNOWN — and then panics at PCI start:
`pmap_enter_pv: unexpected PV head` (`pmap_data.c:2409`). No PCIe
configuration access precedes the panic, so a model answer the PCI path
consumes before its first config read is the prime suspect (the MSI
frame read at the `gic` node's third window, +8, is the one such access
measured). Bisect aids kept in the harness, all debug-only:
`--keep-nodes` (stops at `avp-ctrr`, as expected), `--drop-pcie` (no
panic, but the boot then waits past BSD init without reaching the root
wait), `--passthrough` (stage 2 maps QEMU's GIC and PL011 instead of the
models; the guest then waits in a lock loop — inconclusive). Whether the
timer ticks reach the guest is UNVERIFIED: with the physical interface
enabled, IRQs do reach EL2, but no run has yet lasted long enough past
BSD init to prove tick delivery.

Next: identify what the PCI path reads before config space (trace model
answers against the QEMU-device boot's), then re-run the bar.

### Interrupt delivery, 2026-10-28 (OBSERVED, QEMU only)

Chasing that bar found the interrupt path broken in three places, all in
the monitor, none in the guest (the panicking thread turned out to be a
victim: without a tick, threads pile up behind deadline waits until one
of them maps a page whose PV chain a stalled driver half-built):

- QEMU's real distributor was never enabled. The guest's `GICD_CTLR`
  writes go to the model, so the physical distributor stayed at reset
  and forwarded nothing; no physical PPI reached the CPU interface at
  all (zero IRQ/FIQ exceptions at EL2 in QEMU's log). The monitor now
  enables both groups on the real distributor at setup.
- The maintenance interrupt was a storm source. With `ICH_HCR_EL2.UIE`
  set, the underflow condition (no valid list registers) asserts the
  maintenance PPI permanently: over a million IRQs at EL2 before the
  guest executed. `UIE` is off. A timer PPI masked at the redistributor
  while LR0 is busy is re-armed by the maintenance interrupt that the
  list register's EOI bit raises when the guest deactivates.
- The guest's timer reaches it as an FIQ, not an IRQ. The Phase 0C boot
  took 175 FIQs and one IRQ at EL1 in 45 seconds. The monitor injects
  virtual INTID 27 in group 0, which the guest acknowledges at
  `ICC_IAR0_EL1`. It handles FIQ and IRQ at both lower and current EL,
  because an interrupt can also land inside an EL2 fault handler, which
  previously halted the run.

An intermediate version sourced ticks from EL2's own physical timer
(`CNTHP`, PPI 26, 100 Hz). The tree no longer arms it (`monitor_init`:
"no self-tick"): a free-running tick delivered before the guest arms its
timer disrupted it. The guest's own `CNTV` deadline now fires the
physical timer PPI, and the monitor injects that. The PPI 26 handler is
still present but nothing starts the timer it serves.

### Rerun, 2026-10-28: the monitor boot stalls after `lockdown_mode`

The three monitor configurations, rerun on the current tree
(`smoke.py boot --kernelcache … --timeout 300`), OBSERVED, QEMU only:

| Configuration | Last serial line | PC when QEMU was stopped | Verdict |
| --- | --- | --- | --- |
| `--monitor` | `lockdown_mode: lockdown mode in nvram is off` | `0x4020183c`, `mon_misr_read` | FAIL, no exit within 300 s |
| `--monitor --absent-fixed` | the same | `0x40201824`, `mon_eoir1_write` | FAIL, no exit within 300 s |
| `--monitor --framebuffer` | the same | `0x402012d0`, `el2_irq_lower` | FAIL, no exit within 300 s |

A 60-second `--monitor` run stopped at the same line, with its PC in
`mon_redist_write`. Each run prints the banner and early VM start-up
through the PL011 model, then nothing.

- **This is a regression.** The default policy no longer reaches the
  named stop at `0x30010000` recorded above. That result predates the
  interrupt changes.
- **The cause, at the time INFERRED:** every sampled PC was in EL2's
  interrupt path, pointing at an interrupt storm at EL2. It was found and
  fixed the same day
  ([below](#fixed-2026-10-28-the-maintenance-interrupt-storm)).
- **The root-mount bar is still not met**, and the earlier PCI-start
  panic under `--absent-fixed` is neither cured nor reproduced: these runs
  never get that far.

Host suites on the same tree, all PASS:

- `tools/test-vmapple-monitor.py`: 22 checks;
- `tools/test-vmapple-smoke.py`: 78;
- `tools/test-vmapple-classic.py`: 42, with the two profile checks SKIP on
  Python 3.9;
- `tools/test-vmapple-stick.py`: 6;
- `smoke.py selftest`;
- `tools/test-el2qual.py`: 625,209 checks.

The host suites do not exercise the interrupt path, which is why they all
pass while the boot stalls.

### Fixed, 2026-10-28: the maintenance interrupt storm

**The cause (VERIFIED by fix and test).** Each timer tick is injected into
list register 0 with its EOI bit set. After the guest deactivates it, LR0
is left invalid with EOI still set. Under GICv3 such an LR stays in
`ICH_EISR_EL2`, so `ICH_MISR_EL2.EOI` stays set and the level-sensitive
maintenance PPI (25) stays asserted. The maintenance handler re-enabled
the timer PPIs but never cleared LR0, so PPI 25 re-fired on every `eret`
and the guest never ran again. That matches every sampled PC.

**The fix.** `monitor.c`'s maintenance handler now clears a spent LR0
(state invalid) and leaves a pending or active LR0 alone. The host test has
a new regression, "a maintenance IRQ clears a spent LR0 and its EOI bit".
It fails on the old code and passes on the new.

**After the fix** (OBSERVED, QEMU only):

- `smoke.py boot --monitor`: PASS again. Banner, then
  `EL2: stop: unmodelled IPA 0x0000000030010000 read`, the named
  default-deny stop.
- `--monitor --absent-fixed` now gets far past `lockdown_mode`: through
  AppleImage4, CTRR, the credential manager, PSCI and the PCI family's
  start. It still panics, `pmap_enter_pv: unexpected PV head`, in
  `AppleVirtualPlatform`. So working ticks did not cure it, and the earlier
  "victim of a stalled tick" explanation is refuted.

**What the `--absent-fixed` panic follows** (`--trace`, OBSERVED):

1. `AppleVirtualPlatform` reads `0x30010000`. The gate answers all-ones,
   and the driver's assertion
   `_hostInterface->custom_device_id == SimpleVirtio::kSimpleVirtioDeviceId`
   fails.
2. One instruction then reads eight words at `0x4006cc`–`0x4006e8`,
   answered with zero.
3. Soon after, the panic.

Both answers are invented (decision 5 option d), and the panic follows
them, not the monitor's models or its interrupts (INFERRED from the
order).

**The P3 bar conflicts with decision 5.** The bar asks for the smoke's
root-mount wait, using decision 5's answers for P1's windows. Decision 5's
recommended answer for the fixed addresses is (c), a stop, and a boot that
stops at `0x30010000` cannot reach the root-mount wait. Reaching it needs
a model of `AppleVirtualPlatform`'s contract behind those addresses, whose
26A428 definition is UNKNOWN. So P3 is **met on its default policy** (the
console, then a named stop, which is Phase 3's acceptance) and **open on
its root-mount bar**, pending decision 5.

### Decision 5 built, 2026-10-28: the root-mount wait, without the PCIe node

With decision 5's answers and the self-interrupt fix (OBSERVED, QEMU
only, `smoke.py boot --monitor`):

- **AppleVirtualPlatform:** reads the backdoor's AUX `STATUS` (`1`). It
  stores one mailbox request, which is absorbed and logged. It reads
  `0x4006cc`–`0x4006e8` from the configuration table, answered with zeros,
  as QEMU's would be.
- **Start-up:** BSD start-up passes `dlil_init`, which the dropped
  self-interrupt had stalled. Then AMFI, the PL031 model, `LightEmUp` and
  the PCI family's start.
- **Then it still panics:** `pmap_enter_pv: unexpected PV head`. The
  panic handler then issues a backdoor disk command, a named stop at
  `0x30010408`. The harness now counts a panic as a failure, whatever stop
  follows.

**The panic's cause: the QEMU layout puts guest RAM inside the template's
PCIe windows** (INFERRED, with one confirming run):

- The VirtualMac template's PCIe node puts ECAM at `0x40000000`–`0x4FFFFFFF`
  and its memory window from `0x50000000`. QEMU's `vmapple` machine
  therefore puts RAM at `0x70000000`.
- `QEMU_MONITOR_LAYOUT` reports guest DRAM at `0x45004000`–`0x61004000`,
  inside both. When the PCI family maps its windows, it maps pages the
  kernel also manages as RAM, its own page tables among them. That is what
  `pmap_enter_pv` refuses; the PV head's type bits, `…0003`, mark a
  page-table page.
- The confirming run: the same boot with `--drop-pcie` has no panic and
  **reaches the root-mount wait**:
  `Waiting on … IOMedia … Apple_HFS`, with no QEMU device visible to the
  guest. That is P3's bar, short of the PCIe node.

The gaokun3 layout's guest RAM, at `0xC9004000`, is clear of both windows.

### Guest RAM moved above the device windows, 2026-10-28

**The change:**

- **`QEMU_MONITOR_LAYOUT`'s guest RAM** now starts at `0x81004000`, above
  both windows. The PCIe memory window is taken as the template writes it,
  `0x2fff0000` bytes, so it runs to `0x7FFF0000`; QEMU's `vmapple` uses
  `0x1fff0000`.
- **The split screen** is at `0x9E000000`, and a monitor run gets 1536 MiB
  of QEMU RAM.
- **The stub** maps the guest RAM range the harness passes in the
  parameter page (version 2), across the 1–2 GiB and 2–3 GiB ranges. It no
  longer hard-codes a layout.
- **The loader refuses** any layout whose DRAM, panic region, framebuffer
  or monitor band overlaps a device window of the completed tree:
  `classic_device_windows` reads every `arm-io` child's `reg` and the PCIe
  `ranges`. Only the Phase 0C smoke's layout opts out, to stay as
  committed. Its RAM overlaps its moved ECAM window too, harmlessly as far
  as observed.

**Results** (OBSERVED, QEMU only; `smoke.py boot --monitor --framebuffer`,
PCIe node kept):

- **The `pmap_enter_pv` panic is gone**, which confirms the overlap as its
  cause.
- **The next stop is a different panic, in the PCIe MSI controller:**
  `AppleVirtualPlatformPCIEMSIController.cpp:64 Assertion failed: type &
  kTypeRegisterValidMask`. The panic handler then issues the backdoor disk
  command, and the run ends at that named stop. QEMU's `vmapple` machine has
  no MSI frame model to learn from: its GIC has only the distributor and
  redistributors.
- **What the assertion follows** (`--trace`): exactly one access, a read of
  the MSI frame's `+0x8` (`0x1fff0008`), which the monitor's MSI model
  answers with zero. The old smoke's QEMU window answered all-ones there,
  which passes any "valid" test by accident. In Arm's published GICv2m MSI
  frame, `+0x8` is `MSI_TYPER`, the base SPI and the number of SPIs given
  to MSIs. So a GICv2m model of the frame, option (a) of decision 5, is the
  likely fix (INFERRED from the offset and the assertion's wording). It
  needs an SPI range consistent with the GIC model's interrupt count and
  the tree's own interrupts.
- **Unchanged:** the split screen still matches pixel for pixel, and the
  legacy smoke, the selftest and the real gaokun3 bundle (same qualification
  hash) are as before.

**Evidence:**

- `tools/test-vmapple-classic.py` PASS, 75 checks. The new ones: the window
  reader, the refusal of the old QEMU placement and of RAM inside the
  memory window, and the cross-check that `smoke.py`'s parameter magic,
  flags and stub addresses match `layout.h`. A stale magic in `smoke.py`
  refused every boot once before that check existed.

### The MSI frame, 2026-10-28: P3's bar met

**Not GICv2m.** A GICv2m model of the MSI frame was tried first, as
decision 5's option (a) suggested, with `MSI_TYPER` giving INTIDs
128–191. The assertion still fired. The 26A428 kernel's own check (local
study of the kernelcache's `AppleVirtualPlatformPCIEMSIController`,
OBSERVED) reads the type register at `+0x8` and:

- requires **bit 31**, a valid bit that GICv2m's `MSI_TYPER` does not
  have;
- then takes **bits 12:0** and **bits 28:16** as two 13-bit fields of the
  MSI interrupt range it allocates. Which field is the first INTID and
  which the count is not settled by that code (INFERRED either way).

**The model.** `monitor.c` answers the type register with bit 31 set and
both fields 128. That is INTIDs 128–255 under either reading: inside the
GIC model's 288 and clear of every interrupt the tree names (the devices'
37 and 48–51, PCIe's 64–79). The range is the platform's own choice. No
other register's meaning is known, and Phase 3 has no PCIe functions to
raise an MSI, so any other access to the frame is a named stop.

**Result** (OBSERVED, QEMU only; `smoke.py boot --monitor --mmio-trace`,
PCIe node kept): **the stock 26A428 kernel reaches the root-mount wait**,
`Waiting on … IOMedia … Apple_HFS`, with no panic.

- QEMU's MMIO trace shows only the PL011, the redistributor, and one write
  to QEMU's real distributor. That write is the monitor's `GICD_CTLR`
  enable at setup, made at EL2. The bar's checker now allows exactly that
  one write and nothing more, and a host test pins it.
- No QEMU device answers the guest. **P3's bar is met.**
- With `--framebuffer`, the split screen still matches pixel for pixel.

**Evidence:**

- `tools/test-vmapple-monitor.py` PASS, 52 checks, including the type
  register's valid bit and fields, and the stops on other accesses.
- `tools/test-vmapple-smoke.py` PASS, 82 checks, including the bar's
  distributor rule: a second distributor access, a different write, or a
  guest-visible QEMU device each fail it.
- `tools/test-vmapple-classic.py` 75 and `tools/test-vmapple-stick.py` 16,
  both PASS.

## P4 result: the split console

**2026-10-28: PASS, OBSERVED, QEMU only.** `smoke.py boot --monitor
--framebuffer` splits a 1024×600 framebuffer at `0x62000000`:

- **Where it is:** above the panic chunk and below QEMU's RAM end, so it
  is neither reported DRAM nor firmware-carved memory.
- **The guest's part:** `boot_args.Video` describes the top 512 rows,
  exactly one 2 MiB stage 2 block, which is all stage 2 maps.
- **The monitor's part:** the 88 rows below are absent from stage 2, so a
  guest store there is a stage 2 fault, not a pixel. The monitor draws its
  banner in that band at start, and each named stop as a red line, with
  the project's font (`probes/font5x7.h`, now shared with el2qual; its
  binary is unchanged, `80a1036d`).
- **How a run ends:** with the split, the stub halts at a named stop
  instead of exiting. The harness polls its fault record, dumps the screen
  through QEMU's monitor (`pmemsave`), and ends the run.

The harness then checks, and writes `build/vmapple-smoke/screen.png`:

- **The guest's region:** the kernel drew on 416 of its 512 rows (its
  verbose console).
- **The monitor's band:** it equals the band the harness renders from the
  same font and the expected lines, pixel for pixel: 0 of 90,112 differ.
  Banner and stop line are both there, and the guest wrote nothing into
  the band.

**What it does not check:** the kernel's text is not read by machine; the
picture is kept for a person. On the gaokun3 the split uses 4 KiB pages,
not a 2 MiB block. Its 6400-byte rows put the boundary on a page at 2400
guest rows ([P6](#p6-result-the-gaokun3-layout)), but that mapping, and
the memory type for a real panel, are H2's work.

## P5 result: the stick bundle, placed by q1n1

**2026-10-28: built, host and QEMU only; nothing entered.** Decision 2
chose q1n1. A bundle crosses from the host to the gaokun3 in two halves.

**Snapintosh half** (`tools/vmapple-stick.py`). The output directory is
the stick's tree:

- `Snapintosh/vmapple/bundle.bin` is q1n1's binary manifest: a 96-byte
  header, then one 112-byte record per chunk with its address, size,
  SHA-256 and path. The header holds the entry, `boot_args`, the seed slot
  and the bundle's qualification hash.
- `Snapintosh/vmapple/chunkN.bin` holds the chunks.
- `manifest.json` and `devicetree.bin` are for the host.

The boot seed is never on the stick. The image is built twice, with two
fills; the bytes that differ are exactly the `/chosen/random-seed` slot,
which must be one 256-byte run in one chunk. That run is zeroed, and every
hash covers the zero slot.

`check` refuses:

- a tampered chunk;
- a `bundle.bin` that disagrees with `manifest.json`;
- a filled seed slot.

`check-boot` refuses a mismatched qualification hash. The gaokun3 build
also takes `--uefi-reserved` ([P6](#p6-result-the-gaokun3-layout)).

**q1n1 half** (q1n1 `platform/uefi/vmapple-stick.{h,c}`, behind
`-DQ1N1_VMAPPLE_STICK`, option `--vmapple-place`; q1n1's
`docs/GAOKUN3-VMAPPLE-STICK.md`). Before ExitBootServices it:

1. finds `bundle.bin` on exactly one filesystem;
2. allocates every chunk at its exact address (`AllocateAddress`, so the
   live memory map decides);
3. reads each chunk and checks its SHA-256;
4. refuses a bundle whose slot is not empty;
5. fills the slot with 256 bytes from the RNG protocol's `RAW` algorithm.
   `HASH` is never substituted, and a zero result is refused.

It reports, then releases everything and returns to the firmware. That is
step H1; nothing is entered.

**Evidence:**

- `python3 tools/test-vmapple-stick.py`: PASS, 16 checks, including the
  refusals above and the `bundle.bin` layout.
- q1n1 `python3 tools/test-q1n1-vmapple-stick.py`: PASS, 73 checks, with a
  fake firmware covering the success path, every refusal, and nothing left
  allocated after any of them. Nine deliberately broken copies of the core
  are each rejected.
- q1n1 `python3 tools/test-q1n1-vmapple-stick-qemu.py`: PASS. On QEMU's
  EDK2 from a virtual FAT disk, the chunks are placed by `AllocateAddress`
  and hashed, then refused because EDK2's RNG lists only `RNDR`. Also
  refused: no RNG protocol (`cortex-a76`), a chunk outside RAM, a
  corrupted chunk, and bundles on two disks. The default build ignores the
  option.
- **Cross-check:** a real gaokun3 bundle, built locally into ignored
  `build/` and never committed, passes q1n1's own manifest validator
  compiled on the host. A one-byte corruption fails it.
- q1n1's default build is unchanged: `q1n1.efi` differs from HEAD's only in
  the PE timestamp at `0x80`, and the four stage binaries are identical.

**What QEMU cannot show:** the success path with a real `RAW` source. No
QEMU firmware lists `RAW`; run 27 showed the gaokun3's does.

## P6 result: the gaokun3 layout

**2026-10-28: built, OBSERVED, host only.** `derive_gaokun3_layout` in
`probes/classicload.py`:

- **One CPU** (the owner's decision): `/cpus` is trimmed to the boot CPU.
- **Reserved regions from both sources:** the device tree's no-map regions,
  and with `firmware_reserved` (`vmapple-stick.py --uefi-reserved`) the
  regions UEFI's memory map does not offer. Run 24 found UEFI reserving
  `0xC6200000`–`0xC8600000`, more than the tree's framebuffer region. These
  only choose the window; q1n1's `AllocateAddress` against the live map is
  the guard on the machine.
- **The split screen** (decision 3): the device tree's framebuffer at
  `0xC6200000`, 1600×2560 with 6400-byte rows. The guest gets the top 2400
  rows, and the monitor keeps the bottom 160 rows, 25 pages, so the split
  falls on a 4 KiB page. A split off a page boundary is refused, as is a
  framebuffer that overlaps guest DRAM.
- **The rest as before:** `ram_base` keeps the kernelcache's 32 MiB-block
  offset, and a reserved overlap or an oversized guest is refused.

With the device tree's regions, the real gaokun3 bundle places the guest
at `0xC9004000` (entry `0xCBEB4480`), clear of the UEFI-reserved region.
`tools/test-vmapple-classic.py` covers the layout: PASS, 67 checks. It
also checks that `tools/vmapple-smoke/layout.h`'s QEMU screen and
`classicload`'s agree.

## Architecture

```
 USB stick (FAT32): systemd-boot, slbounce, tcblaunch.exe, the gaokun3 DTB,
                    q1n1.efi, the guest image (built on the Mac, never committed)
      |
 q1n1 --el2, gaokun3 build ..... EL2  loads the guest image from the stick before
      |                               ExitBootServices; VHE exit; DT framebuffer
      |  enters
 Snapintosh monitor, Route B ... EL2  stage 2, virtual GICv3, timer, PAC service,
      |                               PSCI, device models, on-screen console and stop
 XNU (stock 26A428 VirtualMac) . EL1
```

`probes/guestprofile.py` keeps three axes apart, and so does this design:
the **guest** is a new `vma2macosap-26A428` profile, the **host** is the
gaokun3, and the **transport** is the stick.

| Piece | Reuse | New | Gate |
| --- | --- | --- | --- |
| Guest profile `vma2macosap-26A428` | `GuestProfile` | a profile with its own `abi`. The SPTM and TXM fields, such as `txm_policy_pointer_offset` and `frame_type_info_va`, do not apply to it | registration |
| Classic loader | `xnuboot`'s `boot_args` builder (the same revision-2 structure), `appledt.py`, its image builder | the classic path: no SPTM structure, frame table or loader-built tables; the `/chosen` duties | profile |
| Stage 2 | the monitor's stage-2 management | the guest's IPA map | profile |
| Virtual GICv3 | the physical GIC handling, EL2 interrupt entry | the list-register CPU interface; distributor and redistributor models | `SNAP_VMAPPLE` |
| PAC service | `tools/vmapple-smoke/stub.S`'s handling, proven in QEMU | the monitor service | its own, per Phase 0C condition 1 |
| PSCI | | forward to the firmware's PSCI 1.1 (VERIFIED) | `SNAP_VMAPPLE` |
| Device models | | the answers decision 5 picks for P1's windows | `SNAP_VMAPPLE` |
| Console and stop | the monitor's early-stop path | drawn on the screen | `SNAP_VMAPPLE` |
| Guards | the thermal guard (run 10) and the battery guard | | unchanged |
| Stick loader | q1n1's `--el2` foothold and its preload (`platform/uefi/preload.c`) | the preload wired into `--el2`; the image placed and entered on the device | a q1n1 option, default off |

**One VirtualMac loader, not two.** `tools/vmapple-smoke/kcload.py`
already completes the tree and builds `boot_args` for QEMU. Writing a
second loader in `xnuboot` would be the parallel implementation AGENTS.md
forbids. So `xnuboot` gains the classic path, and the smoke harness moves
onto it. The cutover criterion is that the smoke's 19 loader transforms
and its refusals pass unchanged on the new path. After that, `kcload.py`'s
duplicate logic is retired, with your approval.

**Loading from the stick reuses q1n1's preload.** q1n1 already reads
verified files before `ExitBootServices`. Its preload
(`platform/uefi/preload.c`) reads a manifest, `\Snapintosh\q1n1-preload.bin`,
and up to 16 files, each checked against its SHA-256, into reserved memory.
On the A16 that spares the host the 2.6 MB/s upload. Phase 3 wires it into
the `--el2` path instead of writing new file code.

**What is new is the orchestration.** On the A16 the *host* builds the
image, places its chunks at their physical addresses through the proxy and
starts the monitor. With no host, the device must do those steps itself:

- `xnuboot` writes the built image as a self-describing file: each chunk's
  physical address and bytes, and the entry registers;
- before `ExitBootServices`, q1n1 allocates each chunk **at its exact
  address**. If UEFI holds any of them, it refuses cleanly while the
  firmware is still in charge, so the allocation doubles as the layout
  check;
- after `ExitBootServices`, q1n1 enters the monitor as the A16's host
  would.

The addresses are the gaokun3's, chosen against its memory map (step P6),
not the A16's. The monitor's runtime must not need q1n1's proxy loop,
because there is no host on the other end.

## Console

The gaokun3 has no serial line and, in Phase 3, no host link. The screen is
the only record, as it is for el2qual's runs.

**Proposal: split the framebuffer.**
- **The guest** gets a real framebuffer description in `boot_args.Video`:
  the top part of the panel. That meets Phase 0C condition 4 ("the
  framebuffer description") properly, rather than QEMU's `-noprogress`
  stand-in. The kernel's own verbose console draws there.
- **The monitor** keeps a band at the bottom. It shows the guest's PL011
  output, the monitor's own status and, on a stop, the stop report.
- **Stage 2 enforces the split.** The guest's mapping covers only its
  rows. The boundary is chosen so the guest's last row ends on a stage-2
  page, and the monitor's band is never guest-writable.

So both consoles are visible at once. The PL011 path is the one QEMU
proved, and it shows the early banner before the kernel's video console is
up. The video console is the one Phase 0C names as acceptance. Whether XNU's
verbose console works on a 1600-by-2560 portrait framebuffer is INFERRED
(standard XNU behaviour), not yet exercised; QEMU never had a framebuffer.

## The VM firmware's duties

Phase 0C [condition 4](GAOKUN3-PHASE0C.md#decision) makes each one a
monitor requirement, implemented to its published XNU contract, failing
explicitly where unmodelled. The QEMU choices are stand-ins, not designs to
copy unexamined.

| Duty | Phase 3 design |
| --- | --- |
| Boot seed | 256 bytes from a **real hardware source**, per condition 3. The gaokun3 has no `RNDR` (VERIFIED, Phase 0A), so the source is UEFI's RNG protocol before `ExitBootServices`, read with its `RAW` algorithm. This firmware offers it, and `RAW` delivered on the machine (OBSERVED, [run 27](#run-27-2026-10-27-the-firmware-offers-raw-entropy)). **No source, no boot**: a fixed, derived or zero seed is fabricated entropy |
| NVRAM bank | a valid, empty, volatile bank. Nothing persists across boots in Phase 3. The kernel's NVRAM sync fails harmlessly with no controller behind the bank, as in QEMU (OBSERVED). The bank's size and split are examined and recorded as design choices |
| `/cpus` | trimmed to the one CPU started; `CPU_ON` for any other fails explicitly |
| PSCI | `PSCI_VERSION`, `SYSTEM_OFF` and `SYSTEM_RESET` forwarded to the firmware |
| Framebuffer | the split above |
| Panic region | supplied, so a panic reports instead of nesting. QEMU's panic path faulted without one (OBSERVED, `gPanicBase` uninitialised); its contract is read from published XNU in step P2 |

## Safety and policy

- **Hardware writes, all new and each staged** ([Plan](#plan)):
  - EL2 system registers: stage 2, the virtual interface (`ICH_*`),
    `HCR_EL2`;
  - **the real GIC**: enabling the timer interrupt for the boot CPU. This
    is a write to the live interrupt controller. It comes in its own step
    with its own go-ahead.

  There is no clock, power or PHY programming, no device DMA, no SMMU
  write, and no PD or UCSI request.
- **Collapsed PAC keys** (condition 1): the service has its own gate, is
  documented as a security limitation wherever this profile is, and never
  claims per-EL key separation it does not provide.
- **Inputs:** the kernelcache and the VirtualMac device tree stay under
  ignored `downloads/`, and never enter a commit. The stick is local.
- **The A16 is untouched** (condition 6): every Route B piece is behind
  its gate, and the A16 monitor, q1n1's default build and the J700 profile
  keep their baselines byte for byte.
- **Guards:** the thermal guard runs throughout every hardware step, on AC
  power or with at least 50% battery.
- No Linux is booted. The guest is Apple's kernel, as Route B decided.

## Plan

Host first. Every hardware step is a supervised run of the owner's, with
its own go-ahead.

**Offline:**

- **P1. Measure the device set.** DONE, 2026-10-27, host only
  ([result](#p1-result-what-the-kernel-touches)): eleven windows beyond
  the GIC and the PL011, two of them at fixed addresses with no tree node.
  `smoke.py boot --mmio-trace` repeats it.
- **P2. The guest profile and the classic loader** in `xnuboot`. The smoke
  moves onto it and must pass unchanged. The panic-region contract is read
  from published XNU. DONE, 2026-10-27, host and QEMU only
  ([result](#p2-result-the-classic-loader-and-the-panic-region)).
- **P3. The monitor's Route B pieces, in QEMU**, with
  `-machine virt,virtualization=on,gic-version=3`: stage 2 default-deny,
  the virtual GICv3, the timer, the PAC service, PSCI, the PL011 model and
  the answers [decision 5](#decisions-for-the-owner) picks for P1's
  windows. The bar: the same root-mount wait as the smoke, with **no QEMU
  device visible to the guest**. Every device it sees is a monitor model,
  as on the gaokun3. `--mmio-trace` checks it: no guest access may be
  answered by a QEMU device. DONE, 2026-10-28, QEMU only: with decision
  5's answers, the self-interrupt fix, guest RAM above the device windows
  and the MSI frame's type register, the stock kernel reaches the
  root-mount wait with the PCIe node kept, and the MMIO trace shows no
  QEMU device answering the guest
  ([result](#the-msi-frame-2026-10-28-p3s-bar-met)).
- **P4. The split console**, with the framebuffer in QEMU RAM. The harness
  renders it to an image and checks the banner text in both regions.
  DONE, 2026-10-28, QEMU only: the split, with the band checked pixel for
  pixel; the guest's text is not read by machine
  ([result](#p4-result-the-split-console)).
- **P5. The stick path, behind its q1n1 option:** `xnuboot`'s
  self-describing image, the preload wired into `--el2`, placement by
  exact-address allocation, and entry. The boot seed is read here, before
  `ExitBootServices`, from the RNG protocol's `RAW` algorithm as P7 does.
  Tested in QEMU from a virtual FAT disk, including the refusals for a bad
  hash, a held address and a missing or failing `RAW` read. BUILT,
  2026-10-28, both halves, host and QEMU only; entry is not part of it
  ([result](#p5-result-the-stick-bundle-placed-by-q1n1)).
- **P6. The gaokun3 layout**, from el2qual's recorded memory map, with the
  loader refusing any overlap. BUILT, 2026-10-28, host only: one CPU,
  device-tree and UEFI reserved regions, the split screen
  ([result](#p6-result-the-gaokun3-layout)).
- **P7. The entropy source:** a read-only el2qual stage that reports
  whether UEFI offers the RNG protocol and which algorithms. If it does
  not, Phase 3 stops here until a source exists. DONE, 2026-10-27: the
  firmware lists `RAW` and `HASH`, and both deliver
  ([run 27](#run-27-2026-10-27-the-firmware-offers-raw-entropy)).

### P7: the entropy check

**2026-10-27: built, then run on the gaokun3 as run 27: `USABLE, RAW`
([below](#run-27-2026-10-27-the-firmware-offers-raw-entropy)).** el2qual's
load option `rng` (`probes/el2qual/rng.c`) runs before
`ExitBootServices`, while the firmware's services exist:

- it looks for UEFI's RNG protocol;
- it lists the protocol's algorithms;
- it reads 256 bytes twice from the default algorithm, and twice from
  `RAW` when `RAW` is listed;
- it reports whether the bytes are non-zero and differ between reads,
  and how many distinct values the first read held.

A read counts only on a status of exactly `EFI_SUCCESS`: a warning is not
delivered entropy. Each buffer is zeroed first, so a driver that claims
success without writing reads as all zero. `RAW` is preferred when it
works: UEFI defines it as entropy "directly from the source", which is
what Phase 0C condition 3 asks for. The other algorithms are DRBGs whose
seed this cannot see. The protocol's GUIDs, interface and status codes are
UEFI 2.10's, checked against EDK2's `MdePkg/Include/Guid/Rng.h`.

The screen shows up to five lines, ending in a verdict:

```
RNG PROTOCOL FOUND | NOT FOUND, STATUS <16 hex> | FOUND, NO INTERFACE
RNG ALGORITHMS <n>: <RAW CTR HASH HMAC AES 3DES RNDR ?>
RNG DEFAULT 256B OK NONZERO DIFFER DISTINCT <n>   (or the failure)
RNG RAW 256B ...                                  (only when RAW is listed)
RNG VERDICT: USABLE, RAW | USABLE, DEFAULT ALGORITHM | UNUSABLE | NO SOURCE
```

**What QEMU showed** (OBSERVED, host only). QEMU's EDK2 builds its RNG
protocol on the CPU's `RNDR`:

| Configuration | Result |
| --- | --- |
| `max-rng` (`-cpu max`, has `RNDR`) | `FOUND`, one algorithm, `RNDR`; 256 bytes non-zero and different on each read, 171 distinct values; `USABLE, DEFAULT ALGORITHM` |
| `cortex-a76-rng` (no `RNDR`, like the gaokun3) | `NOT FOUND, STATUS 800000000000000E`; `NO SOURCE` |

So QEMU cannot stand in for the gaokun3 here. The gaokun3 has no `RNDR`
(VERIFIED, Phase 0A), and its firmware is Qualcomm's and Huawei's, not
EDK2's QEMU build. Whether that firmware backs the protocol with the SoC's
own generator is what the hardware run answers.

**Evidence.**

- `python3 tools/test-el2qual.py`: the RNG tests are part of the suite.
  A fake firmware answers every way the real one might: not found, no
  interface, `GetInfo` unsupported or too small, and good, all-zero,
  repeating, failing, warning or unwritten reads. Twelve deliberately
  broken copies of `rng.c` are each rejected.
- `python3 tools/qemu-el2qual.py --config max-rng --config
  cortex-a76-rng`: PASS, the two results above. The checker's self-test
  also rejects a missing or flipped verdict, a missing pre-ExitBootServices
  line, a narrow byte spread, and RNG lines on a run that did not ask.
- `python3 tools/test-el2qual-usb.py`: the `entropy` stick, a single
  entry, `el2qual 16` (`nopac nosmp rng`).

#### Run 27 (2026-10-27): the firmware offers RAW entropy

Stick entry 16 (`nopac nosmp rng`), el2qual 14 `80a1036d` (`e8b288c`),
firmware 2.18, one boot, read only
([QUALIFICATION run 27](GAOKUN3-QUALIFICATION.md#runs-so-far)):

```
RNG PROTOCOL FOUND
RNG ALGORITHMS 2: RAW HASH
RNG DEFAULT 256B OK NONZERO DIFFER DISTINCT 159
RNG RAW 256B OK NONZERO DIFFER DISTINCT 156
RNG VERDICT: USABLE, RAW
```

What this shows (OBSERVED, on the machine):

- the firmware installs `EFI_RNG_PROTOCOL` and lists two algorithms,
  `RAW` and SP 800-90 `HASH` (a Hash_DRBG);
- each read, the firmware's default and `RAW`, returned `EFI_SUCCESS`
  twice, filled the zeroed buffer, and differed between reads;
- the spreads, 159 and 156 distinct values in 256 bytes, are close to the
  roughly 162 expected of uniform random bytes.

What it does not show:

- What backs `RAW`. UEFI defines it as output "directly from the
  source"; that is the firmware's declaration, not something el2qual can
  test. A Qualcomm hardware generator behind it is INFERRED, not
  verified.
- Entropy quality. Two reads of 256 bytes are a sanity check, not a
  statistical assessment.
- Which listed algorithm the firmware uses as its default. The report
  does not name it, and it does not matter here: Phase 3 asks for `RAW`
  by GUID.

**So P7 does not stop Phase 3.** Condition 3's named source exists on this
machine and delivers. The seed must be read as el2qual does it: `RAW` by
GUID, `EFI_SUCCESS` only, before `ExitBootServices`, refusing the boot on
any failure rather than falling back to `HASH` or a fixed seed. In the
stick path that read belongs to the loader before ExitBootServices (P5).

**On the gaokun3, each with its own go-ahead:**

| Step | What runs | Writes beyond RAM | Answers |
| --- | --- | --- | --- |
| H1 | q1n1 preloads the image, checks every hash, and allocates each chunk at its address against the live memory map. The monitor is not entered | none | the transport and the layout |
| H2 | the monitor is up, with stage 2 and the virtual interface ready, and the guest **not** entered | EL2 registers; the GIC timer interrupt | the interrupt path |
| H3 | the guest is entered, and the PL011 console shows the banner | as H2 | acceptance part 1; first QARMA5 evidence for condition 2 |
| H4 | the boot runs to its first unmodelled contract | as H2 | acceptance part 2; the run that can reopen Route B |

## Decisions for the owner

1. **Phase 3 before the USB host link, loaded from the stick.** It changes
   Phase 0C's order, and standalone is not yet authorized.
   **DECIDED, 2026-10-27, by the owner: yes.** Phase 3 comes next, with the
   guest loaded from the stick through q1n1's `--el2` preload. Its scope:
   - The stick-loaded, standalone boot is authorized for the gaokun3's
     Phase 3 only, as a transitional flow. It replaces nothing in the
     A16's q1n1 and Mac-host workflow.
   - The USB host link is deferred, not dropped: stage 2 of the S2CR
     bypass and q1n1 device mode resume after Phase 3, each still with
     its own go-ahead.
   - This authorizes the order and the host and QEMU work (P2 to P6). It
     does not authorize any hardware step: H1 to H4 each need their own
     go-ahead on the day.
2. **q1n1 as the stick loader**, rather than a new loader. Recommended: it
   is proven on the gaokun3 (runs 14 and 15), its preload already reads
   hash-checked files, and a new loader would be a parallel
   implementation. **DECIDED, 2026-10-28, by the owner: q1n1.** Built as
   P5's q1n1 half ([P5](#p5-result-the-stick-bundle-placed-by-q1n1)).
3. **The split framebuffer console.** **DECIDED, 2026-10-28, by the owner:
   split.** The guest is told about the top rows; the monitor keeps a band
   below them that stage 2 denies the guest
   ([P4](#p4-result-the-split-console)).
   - **Phase 3's CPU count**, settled with decisions 2 and 3: **DECIDED,
     2026-10-28, by the owner: one CPU**, as this design's scope has it.
     `/cpus` is trimmed to the boot CPU; more come in a later phase
     ([P6](#p6-result-the-gaokun3-layout)).
4. **The real GIC write** in H2, when H2 comes.
5. **How the monitor answers each window
   [P1](#p1-result-what-the-kernel-touches) lists.** Options:
   - (a) a model to a published contract: the PL031 and PL061, the PCIe
     configuration space with no functions (all-ones is PCI's defined
     answer for an absent function), and the MSI frame;
   - (b) the node left out of the tree, a VM without that device. Whether
     each driver accepts a missing node is UNKNOWN. This cannot apply to
     the two fixed addresses;
   - (c) a stop, the default-deny result. It is safe, but H4 would end at
     the CTRR probe;
   - (d) all-ones and ignored writes, which is what the smoke gave. For
     every window but PCIe's that is a fabricated answer, so it is
     allowed only behind an opt-in diagnostic gate, like the
     implementation-defined read-as-zero, and never by default.

   Recommended: (a) where a published contract exists, (b) for the Apple
   paravirtual devices that have nodes, and (c) for the two fixed
   addresses until their contracts are known. P3 tests each choice in
   QEMU before any hardware step.

   **Evidence for the fixed addresses, 2026-10-28** (QEMU and local study
   only; the decision stays the owner's):

   - **QEMU's `vmapple` machine** (v11.1.2, `hw/vmapple/`; it implements
     Virtualization.framework's device model for macOS 12 guests only) maps
     two of the three:
     - `0x30000000`–`0x301FFFFF` is the **backdoor interface** (`bdif`), a
       block-device mailbox for the root and AUX disks. Pages are selected
       by bits 31:16 of the offset, so `0x30010000` is the AUX device's
       register 0, `STATUS`, which QEMU answers `1`. Its command register
       is `+0x408`.
     - `0x400000`–`0x40FFFF` is the **configuration region**, a table the
       host fills: version 2, the CPU count, ECID, RAM size, MAC
       addresses, serial, model and SoC name, with QEMU's field names
       mostly `unk`. QEMU zero-fills the rest, so it answers `0x4006cc`,
       past every known field, with zero.
     - `0x004FFFF8` is not in QEMU's map.

     These are facts read from GPL source (addresses and values), not code.
     They are OBSERVED for macOS 12 and INFERRED for 26A428.
   - **The 26A428 kernel** (local study of the kernelcache's
     `AppleVirtualPlatform`, never committed):
     - **At `0x30010000`:** it maps the address, reads its first word,
       compares it with `1`, and records the address +`0x400` beside it.
       That matches QEMU's `STATUS` and command block (OBSERVED). The
       named stop's PC is that load.
     - **At `0x004FFFF8`:** it builds a 24-byte request on its stack,
       stores the request's physical address there, then reads the
       request's first halfword back. That is a host mailbox serviced
       during the trapped store (INFERRED). Nothing public describes it.
   - **Measured in QEMU**, `smoke.py boot --monitor` with the diagnostic
     `--vmapple-contracts` gate, which answers the AUX page as QEMU does
     and makes a command a named stop:

     | Answers | Result |
     | --- | --- |
     | default, stop on all three (c) | named stop at `0x30010000` |
     | backdoor modelled | the kernel's check passes, no assertion; named stop at the mailbox, `0x004FFFF8` |
     | backdoor modelled, mailbox store absorbed (no reply) | no panic: past `AppleVirtualPlatform` and BSD start-up (`MAC Framework`, `mbinit`), then waits at `dlil_init` for threads to be scheduled. `0x4006cc` is read and answered zero, as QEMU would |
     | (for contrast) all three invented, `--absent-fixed` | the `pmap_enter_pv` panic in `AppleVirtualPlatform` |

   - **The next blocker is the monitor's, not a platform contract.** In
     the last run the kernel wrote `ICC_SGI1R_EL1` once, its own software
     interrupt. With `HCR_EL2.IMO` set, that write traps to EL2. The stub
     handles every trapped system register (exception class `0x18`) as
     implementation-defined, reads as zero and ignores writes, so the
     interrupt was dropped. Threads waiting to be scheduled is what a lost
     self-interrupt would look like (INFERRED). Silently ignoring an
     architectural register is also a fabricated success, so it must be
     either injected or a named stop, whatever decision 5 says.

   **Per-window choices this evidence supports:**

   | Window | Options | Recommended |
   | --- | --- | --- |
   | Backdoor `0x30000000` | (c) stop; or (e) model the AUX page's status registers as QEMU documents, with commands a named stop (no disk in Phase 3) | (e): the kernel's own check expects exactly what QEMU answers, and nothing is invented beyond "present, no media" |
   | Mailbox `0x004FFFF8` | (c) stop; or absorb the store with no reply, logged and shown in the monitor's band | absorb, logged: the guest reads back its own request unchanged, so no value is invented. On Virtualization.framework the host would answer; here no host does, as in the old smoke |
   | Config region `0x400000` | (c) stop; or model it as QEMU does: the table with QEMU's non-identifying defaults (serial `1234`, model `VM0001`, one CPU), zero elsewhere | model it: QEMU's answer at `0x4006cc` is the zero the kernel accepted. Never the machine's own serial, ECID or MAC addresses |

   With those three, P3's root-mount bar is reachable in principle. It is
   next blocked by the monitor's dropped self-interrupt, which is P3 work
   whatever decision 5 says.

   **DECIDED, 2026-10-28, by the owner: as recommended.**
   - The paravirtual nodes stay dropped (b).
   - The published-contract models stay (a).
   - The backdoor interface is modelled as QEMU documents it, and a disk
     command is a named stop (e).
   - The mailbox store is absorbed with no answer, logged on the serial
     line and in the monitor's band.
   - The configuration region is modelled with QEMU's non-identifying
     defaults. Its `rnd` field is a named stop until a real source feeds
     it, and a write is a named stop.

   Built in `tools/vmapple-smoke/monitor.c` as the monitor's default
   answers; the `--absent-fixed` and `--vmapple-contracts` diagnostics are
   gone. The dropped self-interrupt is fixed alongside: only the
   implementation-defined register space is read-as-zero now,
   `ICC_SGI1R_EL1` is injected into list registers 1 to 3, and any other
   trapped system register is a named stop.

## Open questions

- What the firmware's `RAW` algorithm reads from. P7 answered whether
  it exists and delivers ([run 27](#run-27-2026-10-27-the-firmware-offers-raw-entropy)),
  not what backs it.
- The 26A428 contracts behind the two fixed addresses, `0x30010000` and
  `0x004FFFF8`, and whether the boot needs either
  ([P1](#p1-result-what-the-kernel-touches)).
- Whether each driver P1 lists accepts its node being absent (decision 5,
  option b).
- Timer delivery: the guest's own `CNTV` timer PPI, injected through list
  register 0 in group 0 (P3). Ticks reach the guest in QEMU (OBSERVED: the
  `--absent-fixed` boot runs far past the old stall); on the gaokun3 it is
  H2's question.
- The mailbox at `0x004FFFF8`: what Virtualization.framework answers, and
  whether any later boot step needs an answer. Today it is absorbed
  (decision 5).
- The configuration table's `rnd` field: on the gaokun3, a real source for
  it, if the kernel ever reads it.
- The MSI frame's type register: which 13-bit field is the first INTID
  and which the count. Both are 128 today, so either reading works; an
  MSI would settle it, and Phase 3 has none.
- XNU's verbose console on a portrait framebuffer.
- Whether the kernel needs anything Apple-only from the A78C. Only the
  hardware can say (overturn condition 1).
- Later, not Phase 3: [B8](GAOKUN3-PHASE0B.md#b8-the-virtualmac21-entry-and-boot-handoff-26a428)'s
  AuxKC question, and the SMMU for Phase 4's NVMe.
