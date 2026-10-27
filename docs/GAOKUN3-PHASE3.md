# gaokun3 Phase 3: the VirtualMac kernel to a console (design)

**Status, 2026-10-27: DESIGN.** Nothing here is built or run. It proposes
doing Phase 3 before Phase 2's USB host link, with the guest loaded from the
USB stick. That changes the order [Phase 0C](GAOKUN3-PHASE0C.md#decision)
set, and AGENTS.md calls standalone execution "a direction, not an already
implemented or authorized replacement" for the current flow. So it needs
the machine owner's decision first ([Decisions](#decisions-for-the-owner)).

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
  from published XNU.
- **P3. The monitor's Route B pieces, in QEMU**, with
  `-machine virt,virtualization=on,gic-version=3`: stage 2 default-deny,
  the virtual GICv3, the timer, the PAC service, PSCI, the PL011 model and
  the answers [decision 5](#decisions-for-the-owner) picks for P1's
  windows. The bar: the same root-mount wait as the smoke, with **no QEMU
  device visible to the guest**. Every device it sees is a monitor model,
  as on the gaokun3. `--mmio-trace` checks it: no guest access may be
  answered by a QEMU device.
- **P4. The split console**, with the framebuffer in QEMU RAM. The harness
  renders it to an image and checks the banner text in both regions.
- **P5. The stick path, behind its q1n1 option:** `xnuboot`'s
  self-describing image, the preload wired into `--el2`, placement by
  exact-address allocation, and entry. The boot seed is read here, before
  `ExitBootServices`, from the RNG protocol's `RAW` algorithm as P7 does.
  Tested in QEMU from a virtual FAT disk, including the refusals for a bad
  hash, a held address and a missing or failing `RAW` read.
- **P6. The gaokun3 layout**, from el2qual's recorded memory map, with the
  loader refusing any overlap.
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
2. **q1n1 as the stick loader**, rather than a new loader. Recommended: it
   is proven on the gaokun3 (runs 14 and 15), its preload already reads
   hash-checked files, and a new loader would be a parallel
   implementation.
3. **The split framebuffer console.**
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

## Open questions

- What the firmware's `RAW` algorithm reads from. P7 answered whether
  it exists and delivers ([run 27](#run-27-2026-10-27-the-firmware-offers-raw-entropy)),
  not what backs it.
- The 26A428 contracts behind the two fixed addresses, `0x30010000` and
  `0x004FFFF8`, and whether the boot needs either
  ([P1](#p1-result-what-the-kernel-touches)).
- Whether each driver P1 lists accepts its node being absent (decision 5,
  option b).
- Timer delivery: a list register linked to the physical interrupt, or
  managed by the monitor.
- XNU's verbose console on a portrait framebuffer.
- Whether the kernel needs anything Apple-only from the A78C. Only the
  hardware can say (overturn condition 1).
- Later, not Phase 3: [B8](GAOKUN3-PHASE0B.md#b8-the-virtualmac21-entry-and-boot-handoff-26a428)'s
  AuxKC question, and the SMMU for Phase 4's NVMe.
