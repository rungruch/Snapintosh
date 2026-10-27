# gaokun3 Phase 0C: route decision record

Status, 2026-10-26: **ACCEPTED, with conditions (Route B).** See
[Decision](#decision). Proposed on 2026-10-23. The proposal and the checks
that would overturn it are kept below as the record of what was decided.

## Context

Snapintosh boots macOS 27 (26A428) on the ASUS Zenbook A16 (Snapdragon X2
Elite). The port target is the Huawei MateBook E Go 2023 (`gaokun3`,
Snapdragon 8cx Gen 3 / SC8280XP). Phase 0A measured the gaokun3's CPUs at
EL2 ([GAOKUN3-QUALIFICATION.md](GAOKUN3-QUALIFICATION.md#runs-so-far)), and
Phase 0B studied the two candidate kernels offline
([GAOKUN3-PHASE0B.md](GAOKUN3-PHASE0B.md)). The side-by-side evidence is
[GAOKUN3-PHASE0B.md](GAOKUN3-PHASE0B.md#phase-0c-comparison-draft); this
record does not repeat it.

## Options

- **Route A:** the J700 (`j700ap`) kernel the A16 runs, made to work
  without FEAT_NV/NV2, FEAT_ECV and FEAT_FGT.
- **Route B:** Apple's VirtualMac2,1 (`vma2macosap`) kernel, from the same
  IPSW, as a second guest profile running at EL1.
- **Stop:** neither route is worth its cost.

## Proposed decision: Route B, provisionally

**Why.**

- **Fit.** The VirtualMac kernel needs only PAuth and the 16 KiB granule
  from the CPU, and the gaokun3 has both on all eight cores (VERIFIED,
  Phase 0A). Its census finds no EL2 register access, no SPTM call and no
  Apple counter alias (OBSERVED, 26A428). No kernel change has been found
  for it.
- **Route A's cost.** The J700 kernel would need 146 `com.apple.kernel`
  sites rewritten: 97 EL2 register moves, 36 `_EL12` aliases, 12
  `CurrentEL` reads and the exception-return `ERET`. None can trap without
  NV or FGT (INFERRED). That is beyond the
  [KERNEL-PATCHES.md](KERNEL-PATCHES.md#policy) policy, and its hot-path
  cost cannot be sized without the Kernel Debug Kit (A6).
- **What Route B drops.** No SPTM, TXM, SEP, exclaves, ANS2, DART or AIC
  (VERIFIED, B1 and B2).
- **What Route B costs**, all new work:
  - a classic loader (EL1 entry, MMU off, `boot_args` and a completed
    device tree);
  - a small virtual platform: GICv3, PL011, RTCs, AES, the `avp,*`
    devices, and a PCIe bridge with virtio devices;
  - the PAC hypercall service.
- **Unchanged either way.** A desktop needs a Metal driver for the
  Adreno 690.

**What would overturn it.** Route A comes back into play, or the answer
becomes "stop", if:

1. The VirtualMac kernel needs Apple-only CPU behaviour that EL2 cannot
   supply. An example would be an Apple instruction executed at EL1 that
   is neither a system-register access (which `HCR_EL2.TIDCP` traps) nor
   gated off by an ID register.
2. The PAC hypercalls cannot be served correctly on PAuth2 with QARMA5.
   For example, the guest might depend on Apple's PAC algorithm rather
   than only on keys it never reads.

   Partly answered by B6
   ([GAOKUN3-PHASE0B.md](GAOKUN3-PHASE0B.md#b6-every-hypercall-and-what-a-monitor-must-answer-26a428)).
   The guest expects separate EL0 and EL1 key state, which standard PAuth
   cannot provide without trapping every exception. A monitor can serve
   the calls only by collapsing the key domains: one key set for the
   kernel and every process (INFERRED). That should boot, but it gives up
   PAC isolation, so it is the owner's decision. The QEMU smoke test
   checks the functional part.
3. The kernel depends on the VirtualMac platform in ways the monitor
   cannot model: device semantics the published sources do not describe,
   or a boot-policy check (`AppleVPBootPolicy`) that refuses a loader
   other than Apple's.

Losing the AuxKC route alone would not overturn it. Apple's own virtio
drivers would still reach storage, network and input through
monitor-side devices.

## Checks before Phase 1

Each check reports PASS, FAIL, SKIP or NOT RUN, with its evidence class.

| Check | Answers | Who and where | Result |
| --- | --- | --- | --- |
| el2qual on the gaokun3, with `CNTFRQ_EL0`, the firmware version and a GICv3 virtualization probe added | the counter rate Route B's device tree must carry; the firmware the Phase 0A results hold for; whether the guest's interrupts can go through the GIC's list registers rather than a full model | built and tested on the host; one supervised stick run by the owner ([GAOKUN3-QUALIFICATION.md](GAOKUN3-QUALIFICATION.md)) | PASS, 2026-10-23 (run 9, VERIFIED): firmware 2.18; `CNTFRQ_EL0` 19,200,000 on all 8 cores; the virtual CPU interface is usable at EL2, with 4 list registers ([results](GAOKUN3-QUALIFICATION.md#stage-3-results-run-9)) |
| Route B smoke test in QEMU: the 26A428 VirtualMac kernelcache on a generic Armv8 CPU (TCG, PAuth and 16 KiB, no Apple features), with a minimal EL2 stub serving the PAC hypercalls | overturn conditions 1 and 2, as far as early boot reaches | host only | PASS, 2026-10-26 (host only): with `/chosen/random-seed` filled and `-noprogress`, the stock kernel prints its banner, makes no implementation-defined register access and no hypercall other than the PAC set, and boots into IOKit. With an empty NVRAM bank and `/cpus` trimmed to the one vCPU also supplied, it loads its security policies, runs the kernel-collection drivers and BSD start-up without a panic, and waits for a root disk (`IOMedia` `Apple_HFS`). Hypercalls are only the PAC set and `PSCI_VERSION`. The 2026-10-23 FAIL was an unfilled seed, not the debug-mask panic first reported. Overturn conditions 1 and 2 hold this far; 3 is unanswered. The harness reports PASS for this run. See [smoke result](GAOKUN3-PHASE0B.md#rerun-2026-10-26-the-real-first-stop) [the NVRAM rerun](GAOKUN3-PHASE0B.md#nvram-modelled-2026-10-26-the-next-stop-is-the-cpu-count) and [the CPU rerun](GAOKUN3-PHASE0B.md#cpus-trimmed-2026-10-26-the-kernel-waits-for-a-root-disk) |
| Optional: the as-booted VirtualMac tree and driver matches (`ioreg`) from a macOS virtual machine on an Apple-silicon Mac | B3's cross-check, and overturn condition 3 for the device tree | host only; needs a macOS 26A428 virtual machine | NOT RUN |

## Consequences if Route B is accepted

- **New guest profile.** `vma2macosap-26A428` in `probes/guestprofile.py`,
  with the pinned kernelcache digest. The J700 profile, the SPTM and TXM
  layers and the shadow MMU stay as they are for the A16. They are not
  retired (AGENTS.md rule 7); this profile just does not use them.
- **Phase 3** becomes:
  - the classic loader;
  - the VirtualMac device tree completed from the template (memory,
    `/chosen`, a `timebase-frequency` of 19,200,000, the gaokun3's
    `CNTFRQ_EL0` (VERIFIED, run 9), one running CPU per boot);
  - the platform devices and the PAC service.

  Acceptance stays a console on the framebuffer, here the device tree's
  simple-framebuffer at `0xc6200000`.
- **Phase 4.** Storage becomes virtio block devices backed by the
  monitor's NVMe host (`compat/nvme-host.c`) instead of a virtual ANS2.
- **Phase 5.** Input either goes through virtio input from monitor-side
  drivers, or through Snapintosh kexts in an AuxKC; the choice waits on
  `AppleVPBootPolicy`.
- **Phase 6 is unchanged.** A Metal driver for the Adreno 690.
- **Safety.** The Phase 1 guards (thermal, battery and AC, the GPU limits)
  are route-independent and unchanged.

## Decision

**ACCEPTED, with conditions: Route B.** Recorded by the machine owner on
2026-10-26.

**Basis.**

- **Route A's blocker stands.** The J700 kernel needs NV2, ECV and FGT,
  which the gaokun3 lacks (VERIFIED, Phase 0A). It would need 146
  `com.apple.kernel` sites rewritten, beyond the
  [KERNEL-PATCHES.md](KERNEL-PATCHES.md#policy) policy.
- **Route B's CPU needs are met.** PAuth and the 16 KiB granule are present
  on all eight cores (VERIFIED, Phase 0A).
- **The QEMU smoke test**
  ([result](GAOKUN3-PHASE0B.md#cpus-trimmed-2026-10-26-the-kernel-waits-for-a-root-disk),
  OBSERVED, host only) takes the stock 26A428 VirtualMac kernel to its
  root-mount wait, needing only:
  - the PAC hypercalls;
  - PSCI;
  - `/chosen` data the VM firmware normally supplies;
  - a GICv3, a PL011 and the timer.

  *Correction, 2026-10-27, added after the decision:* the smoke had also
  moved the other template devices into QEMU's PCIe window, which answered
  all-ones. The kernel touched eleven unmodelled windows: nine of those
  devices, and two fixed addresses outside the tree
  ([Phase 3 P1](GAOKUN3-PHASE3.md#p1-result-what-the-kernel-touches)).
  The boot tolerated them absent. This does not change the decision; it
  lengthens condition 3's list.
- **Every stop in it was a firmware duty.** None was Apple-only CPU
  behaviour, and none touched an implementation-defined register.
- **What remains open cannot be settled on the host.** It needs the
  gaokun3's own CPU, or belongs to later phases.

**Conditions.** Each is binding on the work that depends on it.

1. **Collapsed PAC key domains are an accepted, documented limitation.**
   The guest expects separate EL0 and EL1 key state, which standard PAuth
   cannot give without trapping every exception
   ([B6](GAOKUN3-PHASE0B.md#b6-every-hypercall-and-what-a-monitor-must-answer-26a428)).
   So the monitor uses one key set for the kernel and every process. That
   removes PAC isolation between processes, and between user and kernel: a
   user process could sign pointers the kernel accepts. This is accepted
   for a research port, not for anything holding data that needs that
   isolation. The PAC service that serves the calls this way:
   - is opt-in behind its own compile-time gate (AGENTS.md rule 3);
   - is described as a security limitation wherever the gaokun3 profile is
     documented;
   - never reports per-EL key separation it does not provide.
2. **Hardware can reopen the decision.** The first gaokun3 EL1 boot of
   this kernel (Phase 3) must reach a stop of the same kind as the QEMU
   test's: a console, then an unmodelled platform contract. Any of these
   reopens this record before further Route B work:
   - an Apple-only CPU behaviour EL2 cannot supply (overturn condition 1);
   - the PAC calls failing on PAuth2 with QARMA5 (condition 2);
   - a platform dependency the monitor cannot model (condition 3).
3. **The boot seed comes from a real entropy source.** XNU panics without
   256 non-zero bytes in `/chosen/random-seed`, and the gaokun3 has no
   RNDR. The loader or monitor must take the seed from a real hardware
   source, for example UEFI's RNG protocol before `ExitBootServices`, or
   the SoC's random-number generator once its registers are documented. It
   must refuse to boot without one. A fixed, derived or zero seed is
   fabricated entropy and is not allowed. *(Added after the decision,
   2026-10-27: run 27 found the gaokun3's firmware offers the RNG protocol
   with a `RAW` algorithm that delivers 256 non-zero, varying bytes before
   `ExitBootServices`; see
   [GAOKUN3-PHASE3.md](GAOKUN3-PHASE3.md#run-27-2026-10-27-the-firmware-offers-raw-entropy).)*
4. **The VM firmware's duties become monitor requirements**, each
   implemented to its published XNU contract and failing explicitly where
   unmodelled:
   - the boot seed;
   - the NVRAM bank in `/chosen`;
   - `/cpus` trimmed to the CPUs actually started;
   - PSCI (`PSCI_VERSION`, `CPU_ON`, `SYSTEM_OFF`, `SYSTEM_RESET`);
   - the framebuffer description;
   - the panic region.

   The QEMU test's choices (an empty CHRP NVRAM bank, `-noprogress`) are
   smoke-test stand-ins, not designs to copy unexamined.
5. **B8 does not decide the route.** If `AppleVPBootPolicy` or `OSKext`
   refuses a Snapintosh AuxKC, input and storage go through virtio devices
   backed by monitor-side drivers, as
   [Consequences](#consequences-if-route-b-is-accepted) describes.
6. **The A16 is untouched.** The J700 profile, the SPTM and TXM layers and
   the shadow MMU stay as they are for the A16 (AGENTS.md rule 7). Route B
   adds a profile, and builds with its gates off keep the A16 baseline
   unchanged (AGENTS.md rule 3).

**What starts now.** Phase 2 (EL2 entry and the USB host link) may begin
([GAOKUN3-PHASE2.md](GAOKUN3-PHASE2.md)). It starts with host-first work on
slbounce's VHE hand-off: `HCR_EL2 = E2H
| TGE`, with `VBAR_EL2` still at UEFI's vectors, while q1n1 and the monitor
assume a non-VHE EL2. Hardware runs remain the owner's, supervised and
under the Phase 1 guards. No Linux image is booted on the gaokun3 (the
owner's decision of 2026-10-23).
