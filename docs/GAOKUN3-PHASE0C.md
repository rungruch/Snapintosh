# gaokun3 Phase 0C: route decision record

Status, 2026-10-23: **PROPOSED.** The machine owner decides; until then
no Phase 1 work starts. This record proposes Route B, provisionally, and
names the checks that would overturn it.

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
| Route B smoke test in QEMU: the 26A428 VirtualMac kernelcache on a generic Armv8 CPU (TCG, PAuth and 16 KiB, no Apple features), with a minimal EL2 stub serving the PAC hypercalls | overturn conditions 1 and 2, as far as early boot reaches | host only | NOT RUN |
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

Not yet taken. The owner records **ACCEPTED** (with any conditions),
**REJECTED** (and the route chosen instead) or **STOPPED** here, with the
date.
