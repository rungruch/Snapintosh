# gaokun3 Phase 0B: kernelcache study (handover)

Handover for whoever picks up the next step of porting Snapintosh to the
Huawei MateBook E Go 2023 (`gaokun3`, Snapdragon 8cx Gen 3 / SC8280XP).
Phase 0B is offline: it studies Apple's kernelcaches and device trees on a
Mac and needs no access to the gaokun3. Its answers feed Phase 0C, the
written choice between the two routes below.

Status, 2026-10-23: **STARTED.** A first pass used the working Mac's own
macOS 27.2 (26B118) kernels. The pinned 26A428 IPSW then arrived, and every
figure was rerun on its J700 and VirtualMac2,1 kernelcaches: the kernels'
own counts are identical (see "Findings"). B1, B2, B4, B6 and B8 are
answered, and B3 from the device tree the IPSW ships. A1 to A3, A5 and A7
have an answer, and A4, A6, B5 and B7 a partial one. The Phase 0C comparison
is drafted ([below](#phase-0c-comparison-draft)); B3's cross-check against
a booted virtual machine is open. B0 is no longer
needed for 26A428. `tools/kc-census.py` and its host test exist.

## Where things stand

Phase 0A (the CPU) is done: `probes/el2qual` measured it at EL2 after
Secure Launch, on all eight cores. The run log and evidence are in
[GAOKUN3-QUALIFICATION.md](GAOKUN3-QUALIFICATION.md#runs-so-far).

| Feature | gaokun3 | Evidence |
| --- | --- | --- |
| Cores | 4x Cortex-A78C r0p0 (`MIDR 0x410FD4B0`), 4x Cortex-X1C r0p0 (`0x410FD4C0`), identical feature registers | VERIFIED, stage 2 |
| Pointer authentication | works on all 8 cores: PAuth2, QARMA5 (`ISAR1.APA 3`, `GPA 1`), no FPAC | VERIFIED, runs 5 and 7 |
| 16 KiB granule | walks at stage 1 on all 8 cores | VERIFIED, run 8 |
| FEAT_NV / NV2, FEAT_ECV, FEAT_FGT, FEAT_RNG | absent: ID fields 0 and the instructions undefined at EL2 | VERIFIED, runs 4 to 8 |
| EL2 entry | slbounce + `tcblaunch.exe` 10.0.22621.382, `HCR_EL2 0x488000000` (E2H, RW, TGE); PSCI 1.1 starts every core at EL2 | VERIFIED, stage 2 |
| Screen | only the device tree's `/chosen` simple-framebuffer at `0xc6200000` (1600x2560); no GOP mode reports an address | VERIFIED, stage 1b |
| Counter | `CNTFRQ_EL0` reads 19,200,000 on all 8 cores, as on the A16; no ECV to rescale it | VERIFIED, run 9 (el2qual 2); the tick rate itself is not timed |
| GIC virtualization | GICv3 system registers at EL2 (`ICC_SRE_EL2 0x09`); a virtual CPU interface on all 8 cores with 4 list registers, 5 priority bits and 16-bit IDs; `ICH_LR0_EL2` reads back what is written | VERIFIED, run 9 |
| Firmware | Huawei 2.18, 06/30/2023 (SMBIOS 3.3, product `GK-W7X`) | VERIFIED, run 9 |

## The two routes

**Route A: keep the J700 kernel**, the stock macOS 27 (26A428) `j700ap`
kernelcache Snapintosh boots on the A16. Everything in `compat/`, `shadow/`
and `probes/xnuboot.*` is built for it. On the A16 it runs at EL1 while
believing it is at EL2, and that rests on FEAT_NV/NV2, which does three
jobs there:

- the kernel's EL2 register accesses become memory accesses (`VNCR_EL2`);
- its `ERET` traps to the monitor (`probes/xnuboot.c:12415`, exception
  class `0x1a`);
- saved modes report EL2, which the monitor rewrites to EL1 on return
  (`compat/exception.c:80-95`).

The gaokun3 has none of that. An EL2 register access at EL1 is undefined
and goes to EL1, an `ERET` with an EL2 mode in `SPSR` is an illegal return
that cannot be trapped without FEAT_FGT, and `CurrentEL` reads EL1. The
monitor also refuses to start without ECV (`probes/xnuboot.c:6063`, used
for 19.2 to 24 MHz counter scaling) and RNDR (`:5994`, the PAuth key seed;
easy to replace). Route A therefore needs a kernel-rewriting layer far
wider than [KERNEL-PATCHES.md](KERNEL-PATCHES.md#policy) allows today. How
wide is question A.

**Route B: Apple's VirtualMac2,1 kernel**, the one Apple's
Virtualization.framework boots in a Mac virtual machine. It runs at EL1,
so it needs no nested virtualization, and the gaokun3 has both features an
EL1 arm64e guest needs (PAuth, 16 KiB). It would be a second guest profile
(`probes/guestprofile.py`) with a different device model: a virtual
machine's devices instead of the J700's AIC, ANS2 storage, DART and SEP.
How different, and whether it uses SPTM at all, is question B.
[COMPATIBILITY.md](COMPATIBILITY.md#before-a-second-guest-or-host) lists
what a second guest profile requires.

The project
[aquarat/apple-macos-arm64-vm-guest](https://github.com/aquarat/apple-macos-arm64-vm-guest)
boots VirtualMac guests under Linux KVM and QEMU's `vmapple` machine on
M1 hosts (OBSERVED, from its documentation only). Its notes are a useful
map of what such a guest touches: Apple's PAC key registers (`APCTL_EL1`,
`KERNKEYLO_EL1`, `KERNKEYHI_EL1`, which the 26A428 kernelcache does not
touch; see Findings), an `hvc #0` service with
`x0 = 0xc1000000`, and paravirtual GPU, storage and network devices. Some
of its files are unlicensed ("all rights reserved" in its `LICENSING.md`):
read them, never copy them.

## Inputs

Nothing here may be committed (AGENTS.md, rule 1). Keep inputs under
`downloads/` and outputs under `build/phase0b/`; both are ignored.

| Input | Where | Needed for | Notes |
| --- | --- | --- | --- |
| This Mac's own kernels | `/System/Library/Kernels/kernel.release.vmapple` (Apple's virtual-machine kernel) and `kernel.release.t8140` (the J700's SoC) | a first pass at B2, B5 to B7 and A1 to A5 | macOS 27.2 build 26B118, plain arm64e Mach-O executables: one kernel each, no kexts, so not B3 or B4. Their census matches the `com.apple.kernel` entry of the 26A428 `vma2macosap` and `j700ap` kernelcaches site for site (Findings). Read in place; never copy them into the tree. |
| macOS 27 26A428 UniversalMac IPSW | `downloads/UniversalMac_27.0_26A428_Restore.ipsw`, or anywhere outside the tree, passed by path | everything | Present since 2026-10-23: 26,626,436,228 bytes, `BuildManifest.plist` 27.0 (26A428), 180 identities, and every member's CRC-32 passes (`zipfile.ZipFile(...).testzip()` finds no bad member). The URL comes from the machine owner; no URL is recorded in this repository. |
| J700 set | `downloads/extracted/j700ap/` | question A | `python3 tools/extract-firmware.py <ipsw> j700ap --out downloads/extracted/j700ap`. Its `kernelcache.release.mac17p` has SHA-256 `dc9019ae39187b36936c2c513b485e513f15cd966b5c887ac88ffd2925976c62`, the image `tools/test-static-trust-contract.py` pins (VERIFIED). |
| VirtualMac2,1 set | `downloads/extracted/vma2macosap/` | question B | `python3 tools/extract-firmware.py <ipsw> vma2macosap --out downloads/extracted/vma2macosap` (B1) |
| KDK for 26A428 | `/Library/Developer/KDKs/KDK_27.0_26A428.kdk` | symbol names for A6 | optional; Apple developer download; `tools/kc-symbolicate.py` uses it for J700 |
| XNU source | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) | reading both kernels' contracts | the release matching macOS 27, if published |

**Extracting.** Two things to know about `tools/extract-firmware.py`:

- It decompresses LZFSE payloads by running `lzfse -decode`, which the
  working Mac does not have. A stand-in script named `lzfse` on `PATH`
  that runs `/usr/bin/compression_tool -decode -a lzfse`, macOS's own
  decoder, from stdin to stdout, extracted both sets. The J700 kernelcache
  it produced matches the pinned digest.
- Without `--out` it writes next to the IPSW (`<ipsw folder>/extracted/`),
  not under `downloads/` in the tree.

**Disk.** The IPSW takes 26.6 GB. Reading only the members needed straight
from Apple's server would avoid that. The IPSW is a zip archive,
`tools/extract-firmware.py` opens it with `zipfile`, and `zipfile` needs
nothing but `seek` and `read`, so a file-like object that serves `read`
with HTTP `Range` requests would let it run on the URL unchanged. That is
task B0. It is not written, and 26A428 no longer needs it.

## Tasks

Each task names its method and the evidence it must produce. Record
results under "Findings" below with their evidence class (VERIFIED,
OBSERVED, INFERRED, UNKNOWN) and the exact command. Raw tool output
stays in `build/phase0b/`; the findings hold counts, names and short
facts, never disassembly listings or dumped tables.

### B0 (optional): extract from the IPSW without downloading it

- Add a `Range`-backed file object, for example an `--url` option to
  `tools/extract-firmware.py`, that reads `BuildManifest.plist` and only
  the board's components.
- Verify each member against the zip directory's CRC-32, which `zipfile`
  checks on read, and record the sizes fetched.
- Host test: serve a local zip over a local HTTP server that honours
  `Range`, and compare the extracted files with a plain `zipfile` read;
  include a server that ignores `Range` and a truncated response, which
  must be refused.
- Done when both board sets extract this way and `downloads/` holds no
  full IPSW.

### Question B: the VirtualMac2,1 kernel

**B1. Identity.** Find the VirtualMac2,1 device class in
`BuildManifest.plist`. Asking `tools/extract-firmware.py` for a class that
does not exist prints every class the IPSW carries
(`no Erase identity for X. Boards: ...`). Then extract that set and record:
the chip ID, the components present, and which are `MISSING`. The class is
expected to start with `vma2`, which is INFERRED from aquarat's notes
(`vmapple2`, `vma2`).

**B2. SPTM and TXM.** `python3 tools/kc-analyze.py <vma2 kernelcache>`
(it accepts a raw Mach-O too, so
`/System/Library/Kernels/kernel.release.vmapple` gives a first, 26B118
answer) prints `__DATA_SPTM ... absent -- not an SPTM kernel` when there is no SPTM
data segment, and otherwise counts the `genter` call stubs and their
domains. Note its exit status: it fails when the stubs do not fit its page
budget, which on a kernel with none may need reading, not trusting. Also
record whether the identity carries an SPTM or TXM image. If the kernel
uses neither, Route B drops `compat/sptm*.c`, `compat/txm*.c` and the
shadow layer's SPTM side; if it does, record which domains and tables it
calls, against the J700's in [BOOT-CHAIN.md](BOOT-CHAIN.md#4-monitor-calls-standing-in-for-sptm-and-txm).

**B3. Device model.** Dump the VirtualMac2,1 device tree with
`python3 probes/appledt.py <devicetree> --grep <text>` (default path
`/device-tree`) and list every device node: name, `compatible`, `reg`
and interrupts. The identity carries one
(`downloads/extracted/vma2macosap/DeviceTree.vma2macosap`, B1).
Virtualization.framework's boot firmware may fill in or change parts of it
at boot (UNKNOWN), so cross-check from inside a macOS virtual machine on
an Apple-silicon Mac: `ioreg -p IODeviceTree -lw0` for the tree as booted
and `ioreg -lw0` for which drivers matched each node (OBSERVED evidence,
for the macOS version the virtual machine runs). Group them: interrupt controller (GIC, or Apple's AIC as
the J700 has), timer, UART, storage, network, graphics, input, entropy or
crypto (AES), NVRAM, power and reset, PCIe, anything SEP-like. For each,
say whether Snapintosh already has a model (`compat/device.c`,
`compat/aic.c`, `compat/ans.c` and others) or would need one.

**B4. Drivers.** List the kernelcache's fileset entries (its
`LC_FILESET_ENTRY` load commands, which `tools/kc-xref.py` already parses
when it selects a `--scope`) and match drivers to the B3 nodes, for
example `AppleVirtIO`, `AppleParavirtGPU`, `AppleVirtualPlatform`. A
device with no driver in the kernelcache needs none in the model.

**B5. Apple-only instructions and registers.** Repeat the J700 inventory
of [APPLE-ENCODINGS.md](APPLE-ENCODINGS.md) on the VirtualMac2,1 kernel: a
linear scan of `__TEXT_EXEC` for the `0x00201xxx` block, and every
`MRS`/`MSR` of an implementation-defined register, attributed to fileset
entries. Compare the register list with what the monitor already handles,
especially the PAC key registers above.

**B6. Hypercalls.** Count `HVC` and `SMC` sites and their immediates, and
for the call sites found, the function ID the kernel puts in `x0` (aquarat
records `0xc1000000`, `SET_INITIAL_STATE`, for PAuth). This is the
paravirtual interface a Route B monitor must serve; name each function by
evidence, not by guess.

**B7. Timebase.** Find where the kernel takes its counter frequency
(device tree `timebase-frequency` or similar, or `CNTFRQ_EL0`) and whether
it uses Apple's counter aliases or only the architectural counters. If the
frequency comes from the device tree and only architectural counters are
read, a 19.2 MHz guest needs no ECV.

**B8. Entry and boot handoff.** From the kernel's entry and XNU source:
the exception level, MMU and cache state it expects, the boot-arguments
structure and device tree it is handed, and whether it can load an
auxiliary kext collection (Snapintosh's own drivers live in one), compared
with what `probes/xnuboot.py` builds for J700.

### Question A: the J700 kernel without nested virtualization

The pinned kernelcache is
`downloads/extracted/j700ap/kernelcache.release.mac17p`; this Mac's
`/System/Library/Kernels/kernel.release.t8140` is the same SoC's kernel
at 26B118, without kexts.

`tools/kc-census.py` linearly scans one Mach-O's executable segments and
counts, by register and direction: EL2 register moves (op1 4) and the
`_EL12`/`_EL02` aliases (op1 5), EL2 `TLBI`/`AT` (op0 1, op1 4/5),
`MRS CurrentEL`, `ERET`/`ERETAA`/`ERETAB`, `HVC` and `SMC` with immediates,
`genter`/`gexit`/`0xd4e00000`, the rest of the `0x00201xxx` block,
IMPLEMENTATION DEFINED registers (CRn 11/15, any op1), and the
architectural counter and timer registers (CRn 14 at op1 3, and
`CNTKCTL_EL1`). It attributes sites
to fileset entries by each entry's own segments, names functions when `nm`
has symbols (in a fileset, only with a symbol of the site's own entry), and
writes every site with `--sites` (TSV, under `build/`).
Its host test, `tools/test-kc-census.py`, encodes every class by hand from
the Arm ARM field layout, anchors the encoder and the register names a
first census printed unnamed to LLVM-assembled words, and
includes near misses that must not count (EL1/EL0/EL3 op1 values,
`MSR` immediates, `TLBI` at op1 0, `SVC`, `ERET` with an Rn, a different-op2
`CurrentEL`), a synthetic two-entry fileset, and a symbol of one entry that
must not name another entry's site. A linear scan also matches data words
in text, so every count is an upper bound.

Not covered by the tool: `SPSR` constants (A4), data flow into `x0` for
`HVC`/`SMC` (B6; a one-off scan of each site's basic block was used, see
Findings) and the hot-path judgement (A6). 26A428's kexts carry text
symbols but `com.apple.kernel` carries none, so the kernel's own sites
still need the KDK.

| Task | What to count |
| --- | --- |
| A1 | `MRS`/`MSR` of EL2 registers (op0 3, op1 4) and of the `_EL12`/`_EL02` aliases (op1 5), by register, read and write |
| A2 | EL2-only system instructions: `TLBI` and `AT` with op1 4 |
| A3 | `MRS CurrentEL` |
| A4 | `ERET`, `ERETAA`, `ERETAB`, and how the kernel builds the `SPSR` values it returns with: find the EL2t/EL2h constants in XNU source (`osfmk/arm64/proc_reg.h`) and where they are used |
| A5 | `HVC`, `SMC`, and the 348 SPTM call words already known (174 `genter`, 174 `0xd4e00000`; `tools/kc-analyze.py`) |
| A6 | which of A1 to A4 sit on hot paths: exception entry and return, context switch, system call return. Use the KDK's symbols (`tools/kc-symbolicate.py`) and XNU source names. The A16's trap census (`docs/TRAP-CENSUS-20260929.md`, cited by `tools/build-fast-traps-monitor.py`) is not in this tree; [KERNEL-PATCHES.md](KERNEL-PATCHES.md) has some of its totals |
| A7 | the counter: Apple counter-alias sites (about 1,060, [KERNEL-PATCHES.md](KERNEL-PATCHES.md#proposed-not-implemented), `SNAP_KERNEL_NATIVE_COUNTER`) and `CNTFRQ_EL0` readers; that deferred patch is what a 19.2 MHz J700 guest would need |

Then judge, in writing, whether a rewrite of those sites could follow the
[KERNEL-PATCHES.md](KERNEL-PATCHES.md#policy) rules (found by instruction
pattern, never by address; opt-in; pinned to the image digest; a reviewed
site closure), and which sites it could not reach, `ERET` with an EL2 mode
being the known hard case.

## Done means

- B1 to B8 and A1 to A7 each have a finding with its evidence class and
  command, or an explicit UNKNOWN and why.
- New tools have host tests (`tools/test-*.py`), report PASS, FAIL, SKIP or
  NOT RUN, and carry no Apple data in their fixtures.
- A short comparison table for Phase 0C: per route, the CPU fit (done),
  the monitor pieces reused and new, the device models needed, the kernel
  changes needed, and the main risks. Phase 0C's decision record is a
  separate document.

## Constraints

- Offline only. Nothing in Phase 0B runs on the gaokun3 or programs any
  hardware.
- Apple material stays local (`downloads/`, `build/`, the KDK); findings
  hold counts and short facts. Paste nothing proprietary into the docs.
- The working Mac, as of 2026-10-23: macOS 27.2 (26B118) on Apple
  silicon; Homebrew LLVM 23.1.4, lld and QEMU 11.1.2; system Python 3.9
  (`tools/test-link.py` needs 3.10 or newer, and a `../q1n1` checkout,
  which is absent); the 26A428 IPSW present outside the tree; no KDK; no
  `lzfse` (see "Extracting" above).
- Commit titles follow AGENTS.md section 6 (`tools: ...`, `docs: ...`).

## Findings

Two inputs, both read in place:

- the working Mac's `/System/Library/Kernels/kernel.release.{t8140,vmapple}`,
  macOS 27.2 build 26B118: plain kernels with no kexts and no text symbols;
- the pinned 26A428 kernelcaches, `downloads/extracted/j700ap/kernelcache.release.mac17p`
  (362 fileset entries) and `downloads/extracted/vma2macosap/kernelcache.release.vma2`
  (216 entries). Their kexts carry text symbols, `com.apple.kernel` none.

Commands run from the repository root; raw output is in `build/phase0b/`
(ignored; 26A428 files carry `-26A428` in their names). Counts are upper
bounds from a linear scan.

**Tooling.** `python3 -I tools/test-kc-census.py`: PASS (96 checks, 0
FAIL). Three deliberately broken copies of the tool each fail:
- with two register names swapped, 6 checks fail;
- letting any nearby symbol name a site fails the cross-entry check;
- counting every CRn 14 register as a counter fails the op1 1 near miss. Every architectural site word was also disassembled with `llvm-mc
--disassemble -triple=arm64 -mattr=+all` (LLVM 23.1.4, a one-off check
kept only in `build/phase0b/`): 2,037 in `t8140`, 1,012 in `vmapple`,
3,179 in the 26A428 J700 kernelcache and 1,013 in the VirtualMac one,
counter and timer registers included. PASS: LLVM
gives the same register and direction for every site, and no EL2 or `_EL12`
register is left unnamed. Implementation-defined registers print as
`s<op0>_<op1>_c<n>_c<m>_<op2>`; no Apple register is given a name here
without evidence.

### B1: the VirtualMac2,1 identity (26A428)

- VERIFIED: `BuildManifest.plist` lists `VirtualMac2,1` as the only
  virtual product. Its device class is `vma2macosap` (2 `Erase`
  identities, 1 `Update`): chip ID `0xFE00`, board ID `0x20`, security
  domain `0x01`.
- VERIFIED: its manifest names a kernelcache (`kernelcache.release.vma2`),
  a device tree (`DeviceTree.vma2macosap`), iBoot, LLB, iBSS and iBEC, all
  present. It names no SEP, no `Ap,TrustedExecutionMonitor` (TXM) and no
  SPTM, and the IPSW holds no SPTM or exclave image for chip `0xFE00`
  (`tools/extract-firmware.py` resolves both by chip and found neither).
- Command: `python3 tools/extract-firmware.py <ipsw> vma2macosap --out
  downloads/extracted/vma2macosap`, which extracted 4 components.

### B2: SPTM and TXM in the VirtualMac kernel

- VERIFIED: `python3 tools/kc-analyze.py
  downloads/extracted/vma2macosap/kernelcache.release.vma2` prints
  `__DATA_SPTM absent -- not an SPTM kernel` and "No monitor call sites".
  The census agrees across all 216 entries: 0 `genter`, 0 `0xd4e00000`,
  0 `gexit`. The 26B118 `kernel.release.vmapple` gives the same result.
- Same command on the J700 kernelcache: SPTM-based, 348 call sites in 3
  pages of 16K (174 `genter` and 174 `0xd4e00000` words), all in
  `com.apple.kernel`; one stub calls TXM (domain 2). This matches the
  figure already in the docs.
- So neither the VirtualMac2,1 identity nor its kernelcache has SPTM, TXM,
  a SEP or exclaves. INFERRED: Route B needs none of `compat/sptm*.c`,
  `compat/txm*.c`, the shadow layer's SPTM side or a SEP model; what the
  shadow layer's translation side must still do is B8's question.

### B3: the VirtualMac2,1 device model (26A428)

`python3 probes/appledt.py downloads/extracted/vma2macosap/DeviceTree.vma2macosap`
reads 139 nodes in 61,208 bytes. A one-off script drove the same parser to
list every node (`build/phase0b/dt-nodes-vma2macosap-26A428.tsv`), and the
tree re-serialises to the identical bytes. VERIFIED from the tree unless
marked otherwise:

- The root is `model VirtualMac2,1`, with `compatible` including
  `AppleVirtualPlatformARM` and `target-type VMA2MACOS`.
- It is a template:
  - 25 properties carry the placeholder flag, 8 of them the root's
    identity fields (`syscfg/...` recipes);
  - the root `clock-frequency` and `/memory`'s `reg` are 0;
  - `/chosen/memory-map` holds 16 all-zero `MemoryMapReserved-*` slots.

  INFERRED: the virtual machine's boot firmware completes it, as iBoot
  completes the J700's.
- Every device is under `/arm-io` (`arm-io,vmapple1`, `soc-generation
  VMApple2`). Its `ranges` maps child address 0 to physical `0x10000000`
  (`0x70000000` bytes); the offsets below are child addresses.
- `/cpus` holds 32 `cpu` nodes (`ARM,v8`), each with `timebase-frequency`
  and `clock-frequency` 24,000,000.
- There is no timer node, and no storage, network, input, NVRAM or
  SEP-like device node. Those devices sit behind PCIe (B4).

| Group | Node (`compatible`) | Offset, size; first interrupt cell | Driver (kext: class) | In Snapintosh today |
| --- | --- | --- | --- | --- |
| Interrupt controller | `gic` (`gic,vmapple1`, `ARM,gicv3`) | `0x0` 64 KiB, `0x10000` 4 MiB, `0xfff0000` 64 KiB | `AppleARMGIC`: `AppleARMGICv3` | No; `compat/aic.c` is the J700's AIC. The gaokun3 has GICv3 system registers (`ID_AA64PFR0_EL1.GIC` 1, firmware `GIC V3`; [GAOKUN3-QUALIFICATION.md](GAOKUN3-QUALIFICATION.md#runs-so-far)) |
| Power and reset | `pmgr` (`ARM,psci`) | none | `AppleVirtualPlatform`: `ApplePSCI` | No; the gaokun3's firmware has PSCI 1.1 (VERIFIED, Phase 0A) |
| Console | `uart0` (`arm,pl011`) | `0x10010000` 64 KiB; none | `AppleSerialShim` (matched by the name `uart0`) | No; the J700's UARTs are `uart-1,samsung` |
| Clocks | `pl031-rtc` (`ARM,pl031`); `avp-rtc` (`avp,rtc`) | `0x10050000` 4 KiB; `0x20240000` 4 KiB, 51 | `AppleVirtualPlatform`: `ApplePL031RTC`, `AppleVirtualPlatformRTC` | No |
| Buttons | `pl061-buttons` (`ARM,pl061`); `/buttons` | `0x10060000` 4 KiB, 37 | `AppleVirtualPlatformButtons`; `AppleM68Buttons` | No |
| Panic | `pvpanic` (`qemu,pvpanic-mmio`) | `0x10070000` 64 KiB | `ApplePVPanic`: `ApplePVPanicMMIO` | No |
| Crypto | `aes` (`aes,s8000`, `aes-version` 3) | `0x20220000` and `0x20230000`, 16 KiB each, 50 | `AppleS8000AES`: `AppleS8000AESAccelerator` | In part: `compat/aes-engine.c` models the J700's `aes,s8000`, which is `aes-version` 5 with three regions and an IOMMU parent |
| Graphics | `gfx` (`paravirtualizedgraphics,gpu`); `iosurface-mapper` (`paravirtualizedgraphics,iosurface`) | `0x20200000` 64 KiB, 49; `0x20210000` 64 KiB, 48 | `AppleParavirtGPUIOGPUFamily`: `AppleParavirtGPU`; `AppleParavirtIOSurface` | No |
| Platform security | `avp-ctrr` (`avp,ctrr`); `avp-sealed-registers` | `0x20260000` 64 KiB; `0x20270000` 64 KiB | CTRR: no kext matches it, and the string is in `com.apple.kernel`'s own `__TEXT` (INFERRED: the kernel programs it); `AppleVirtualPlatformSealedRegisters` | No |
| PCIe | `pcie` (`pcie,vmapple1`) | `0x30000000` 256 MiB configuration space, buses 0 to 8; interrupts from 64, MSI frame 2 | `AppleVirtualPlatform`: `AppleVirtualPlatformPCIE` | No |
| Other | `cpu-debug-interface`; `port-virtual-1` | none | `AppleARMLightEmUp`; `AppleVirtualUSBPort` | No |

Two more readings, both INFERRED:

- The first interrupt cell is a GIC interrupt ID in the SPI range (32 and
  up); the second cell's meaning is UNKNOWN.
- The PCIe `ranges` (Apple's little-endian cells) give one 32-bit memory
  window of `0x2fff0000` bytes at child offset `0x40000000`.

The as-booted tree, from `ioreg` inside a macOS virtual machine, is not
checked: NOT RUN, since none is available here.

### B4: the VirtualMac2,1 drivers (26A428)

A one-off parse of the kernelcache's `__PRELINK_INFO` resolves its
`ID`/`IDREF` deduplication, which `plistlib` alone would lose
(`build/phase0b/prelink.py`, output `prelink-vma2macosap-26A428.json`). It
finds 236 kexts with 1,122 IOKit personalities in 161 of them. 215 of the
kexts carry code; with `com.apple.kernel`, which has no prelink entry,
they are the kernelcache's 216 fileset entries.

- **Device-tree nodes.** Each node's `compatible`, `name` and
  `device_type` were matched against the personalities' `IONameMatch`.
  Every device node in the table above has a driver except `avp-ctrr`.
  The platform nodes match too: the root `AppleVirtualPlatformARMPE`,
  `cpu` `AppleARMCPU`, `defaults` `AppleImage4`, `Image4` and
  `AppleMobileApNonce`.
- **PCIe.** Storage, network, input and the rest have no device-tree node.
  They match `IOPCIDevice` by `IOPCIPrimaryMatch` (device ID << 16 |
  vendor ID):
  - Standard virtio (vendor `0x1af4`, any device) goes to `AppleVirtIO`'s
    transport. Its drivers cover block storage, network, console, entropy,
    input (keyboard, mouse, trackpad), balloon, 9P, virtiofs, sockets,
    sound and an IOMMU.
  - Apple-vendor (`0x106b`) devices:

    | Device | Driver |
    | --- | --- |
    | `0x1a00` | `AppleVirtIOStorage` |
    | `0x1a0d` | `AppleVirtIO` |
    | `0x1a01`, `0x1a04`, `0x1a07`, `0x1a0c`, `0x1a11` | `AppleVirtualPlatform`: a virtual USB device controller, a HID bridge, a power source, identity and provisioning messengers |
    | `0x1a03` | VideoToolbox paravirtualisation |
    | `0x1a08` | FairPlay |

  - Class matches: a generic xHCI driver (`0x0c0330`) and a generic
    Ethernet one (`AppleUIOPCI`, `0x020000`).

  Which virtio device type selects which driver is decided in the
  transport's probe, not in these keys, so it is UNKNOWN beyond the class
  names.

**What this means for Route B on the gaokun3** (INFERRED, not a decision):

- This kernel would see none of the gaokun3's real devices. A monitor would
  present the virtual set above instead:
  - a GICv3, which the gaokun3 has. Its virtual CPU interface is usable at
    EL2, with 4 list registers (VERIFIED, run 9); the guest's distributor
    and redistributor view is still to be designed;
  - PSCI, forwarded to the firmware;
  - a PL011 and the two RTCs;
  - AES, a variant of `compat/aes-engine.c`;
  - a PCIe host bridge with virtio devices backed by the monitor's own
    drivers. The gaokun3's disk is NVMe (`nvme0n1` in gaokun-android's
    `docs/hw-inventory.md`), and `compat/nvme-host.c` already drives an
    NVMe controller on the A16.
- None of the J700's hardest models is needed: no SEP, exclaves, ANS2,
  DART, AIC or SPTM.
- Graphics is the open problem. The only display driver here is the
  paravirtual GPU, which expects Apple's ParavirtualizedGraphics host. A
  console can run on the boot framebuffer, but a desktop needs a Metal
  device (B8).

### 26A428 against 26B118

The kernels' own sites are identical: every `com.apple.kernel` site class,
register and direction in the 26A428 kernelcaches matches the 26B118
kernels (2,560 J700 sites and 1,175 VirtualMac sites, counter and timer
registers included). The kexts add:

| Class | J700 kexts | VirtualMac kexts |
| --- | --- | --- |
| `_EL12` alias moves | 42 `CONTEXTIDR_EL12` (21 reads, 21 writes), all `AppleProcessorTrace` | 0 |
| EL2 `TLBI` | 2 (`tlbi alle2is`, `tlbi alle1is`), `AppleProcessorTrace` | 0 |
| `HVC` | 0 | 1, `AppleVirtualPlatform` (`_smc_call`) |
| IMPDEF registers | 1,098: `AppleProcessorTrace` 988, `AppleT8140CLPC` 85, `AppleEverestErrorHandler` 19, `AppleARMWatchdogTimer` 4, `AppleT8140PMGR` 2 | 0 |
| everything else | 0 | 0 |

`AppleProcessorTrace`'s alias moves are its `getContextID` and
`setContextID` methods, one pair in each of 21 per-SoC classes, and its two
`TLBI`s are in `startTracingOnCore` (named by the kexts' own symbols).
INFERRED: these kexts drive Apple's trace, power and error hardware, so
they load only where the device tree has matching nodes.

### A1 to A5: J700 kernel (`t8140`)

`python3 tools/kc-census.py /System/Library/Kernels/kernel.release.t8140
--sites build/phase0b/sites-t8140.tsv`, and on 26A428
`python3 tools/kc-census.py downloads/extracted/j700ap/kernelcache.release.mac17p
--sites build/phase0b/sites-j700ap-26A428.tsv`. Everything below is the
kernel's own and holds for both builds; the kexts add only what the table
above lists.

| Class | Sites | Evidence |
| --- | --- | --- |
| EL2 register moves (op1 4) | 97 across 33 distinct registers | OBSERVED |
| `_EL12`/`_EL02` alias moves (op1 5) | 36 across 18 distinct registers | OBSERVED |
| EL2 `TLBI`/`AT` | 0 | OBSERVED |
| `MRS CurrentEL` | 12 | OBSERVED |
| `ERET`/`ERETAA`/`ERETAB` | 1 `ERET` | OBSERVED |
| `HVC` / `SMC` | 0 / 0 | OBSERVED |
| `genter` / `0xd4e00000` | 174 / 174 | OBSERVED, agrees with `kc-analyze.py` |
| other `0x00201xxx` | 175 | OBSERVED, agrees with APPLE-ENCODINGS.md |
| IMPDEF registers (CRn 11/15) | 1,881 across 188 distinct | OBSERVED |
| Counter and timer registers | 10: `CNTKCTL_EL1` (3 reads, 3 writes), `CNTP_CTL_EL0` (4 writes); no `CNTFRQ_EL0`, `CNTPCT_EL0` or `CNTVCT_EL0` | OBSERVED |

- The EL2 and `_EL12` sites are not spread through the kernel. 124 of the
  133 sit in two address clusters: 56 in `0x...7322000` to `0x...7326fff`
  and 68 in `0x...7562000` to `0x...7564fff`; 9 are isolated, 5 of those
  SME or `SCXTNUM_EL12` writes.
  - Cluster 1 lies 8 to 28 KiB into `__TEXT_EXEC` and also holds the
    kernel's only `ERET` and 10 of its 12 `MRS CurrentEL` reads.
  - Cluster 2 holds 33 of the 36 `_EL12`/`_EL02` alias moves, all 16 sites
    of the GIC list registers `ICH_LR0_EL2` to `ICH_LR7_EL2`, and all five
    FGT writes.
  - GIC-virtualization (`ICH_*_EL2`) sites number 35: 8 in cluster 1, 25 in
    cluster 2, 2 isolated.

  INFERRED: cluster 1 is exception entry and return, and cluster 2 is code
  that switches to and from another context's EL1 state, such as a virtual
  machine's. The clusters are a few routines, not thousands of scattered
  sites. What they are needs the KDK's symbols (A6): the 26A428 kernelcache
  names none of the kernel's own functions.
- Only one `ERET` exists in the whole kernel, so the hard case (an `ERET`
  with an EL2 mode in `SPSR`) is a single site to patch, not a pattern. If
  cluster 1 is the exception return path (INFERRED above), every exception
  return passes through that site, so whatever replaces it costs time on
  every exception. The `SPSR` values it returns with are not traced yet (A4
  open).
- The most frequent architectural registers are `HCR_EL2` reads (13),
  `TPIDR_EL2` reads (13) and `CPTR_EL2` reads (9) and writes (4).
- Two register groups the gaokun3 lacks are among the 133 (VERIFIED in
  Phase 0A: no FGT, and `ID_AA64PFR1_EL1` SME 0):
  - FGT: five trap-register writes, one each to `HFGRTR_EL2`,
    `HFGWTR_EL2`, `HFGITR_EL2`, `HDFGRTR_EL2` and `HDFGWTR_EL2`, all in
    cluster 2;
  - SME: 3 writes of `SMPRIMAP_EL2` (isolated), and a read and a write of
    `SMCR_EL12`.

  Without the feature these accesses are undefined at every exception
  level, so they must never run, whatever level the kernel runs at.
  `probes/idregs.h` records XNU acting on the SME field of the ID registers
  (OBSERVED on the A16), but a comment at `probes/xnuboot.c:6317` says it
  configures SME unconditionally for that board. Whether the ID registers
  keep these sites, and the FGT writes, from running is UNKNOWN (A6).
- The IMPDEF accesses dominate the totals: 1,017 are reads of
  `s3_4_c15_c10_6`. These are not architectural EL2 registers and are not
  part of the 97.
  - The monitor already treats this register as Apple's ordered counter
    alias and emulates it (`compat/fast-vector.h:91`, the
    `mach_absolute_time` read; OBSERVED on the A16).
  - The only other register of that group the kernel touches,
    `s3_4_c15_c10_4` (3 reads, 5 writes), is one the monitor answers with 0
    ([KERNEL-PATCHES.md](KERNEL-PATCHES.md)).
  - The repository's EL0 emulation test (`tools/test-fast-el0-sysreg.c:37`)
    names `S3_4_C15_C11_7` "the other counter alias"; the kernel reads it 43
    times. 1,017 + 43 = 1,060, KERNEL-PATCHES.md's "about 1,060"
    counter-alias sites (INFERRED: that document does not say how it
    counted).
  - On 26A428 both counts are unchanged and every one of these reads is in
    `com.apple.kernel`; no kext reads either alias.

  A7: the kernel never reads `CNTFRQ_EL0`, and never reads an
  architectural counter (`CNTPCT_EL0`, `CNTVCT_EL0` or their
  self-synchronised forms). Every counter read is one of the two aliases.
  It takes its frequency from the device tree, as the VirtualMac kernel
  does (B7 below).

  INFERRED: a 19.2 MHz J700 guest therefore needs a 19.2 MHz
  `timebase-frequency` and all 1,060 alias reads served at that rate. On
  the gaokun3 the alias encodings are not Apple's counter. `HCR_EL2.TIDCP`
  (Armv8.0) traps EL1 accesses to CRn 11 and 15 to EL2, so the monitor can
  trap and emulate the reads, or the deferred `SNAP_KERNEL_NATIVE_COUNTER`
  rewrite can turn them into architectural reads.
- UNKNOWN: which of these sites run on hot paths, and whether any run when
  the kernel believes it is at EL1 (A6).

### B5, B6: VirtualMac kernel (`vmapple`)

`python3 tools/kc-census.py /System/Library/Kernels/kernel.release.vmapple
--sites build/phase0b/sites-vmapple.tsv`, and on 26A428
`python3 tools/kc-census.py downloads/extracted/vma2macosap/kernelcache.release.vma2
--sites build/phase0b/sites-vma2macosap-26A428.tsv`. The kernel's figures
hold for both builds; 26A428's kexts add one `HVC`.

- OBSERVED: 0 EL2 register moves, 0 `_EL12` aliases, 0 EL2 `TLBI`/`AT`,
  1 `MRS CurrentEL`, 1 `ERET`, 0 `SMC`, 45 `HVC` in the kernel and 1 in
  `AppleVirtualPlatform`, all `hvc #0`.
- OBSERVED: 56 IMPDEF register accesses, including `s3_4_c15_c1_4` (22),
  `s3_4_c15_c1_3` (9), `s3_6_c15_c8_3` and `s3_6_c15_c2_5` (op1 6), and
  `s3_4_c15_c5_0` and `s3_5_c15_c5_0`. What they are, and whether the
  monitor already handles each, is UNKNOWN until compared against the
  monitor's register list and evidence.
- B5 against the monitor (26A428). I searched `compat/`, `probes/` and
  `shadow/` for each encoding in its name, tuple and instruction-word forms.
  The 56 accesses touch 13 registers, and the monitor already knows two:
  - `s3_4_c15_c1_3`, which it names `AMX_STATE_T_EL1`
    (`compat/fast-trap.h:99`);
  - `s3_4_c15_c1_4`, which it answers with 0 (`probes/sysregzero.py:58`).

  The other 11, at 1 to 5 sites each, are unknown to it. On the gaokun3's
  Cortex cores these encodings are either undefined or Arm's own
  IMPLEMENTATION DEFINED registers. `HCR_EL2.TIDCP` traps EL1 accesses to
  CRn 11 and 15 to EL2, and the monitor already traps Apple's registers
  that way on the A16 ([BOOT-CHAIN.md](BOOT-CHAIN.md#3-exception-levels-xnu-at-el1-believing-it-is-el2)).
  On the gaokun3 this is not measured.
- OBSERVED (26A428, a one-off scan of every executable word): the
  VirtualMac kernelcache writes no PAuth key register at all.
  - It touches none of the Apple key registers the monitor models:
    `KERNKey` lo/hi at `s3_4_c15_c1_0`/`c1_1` and `APCTL` at
    `s3_4_c15_c0_4` (`compat/pauth-user.c:34`).
  - It writes no architectural key register (`APIAKey` to `APGAKey`).
  - The J700 kernelcache, by contrast, writes `KERNKey` lo and hi 6 times
    each, and `APIBKey` and `APDBKey` lo and hi 4 times each.

  The hypervisor sets the keys, through the PAC hypercalls below (VERIFIED
  in published source, INFERRED for 26A428).
- B7 (26A428 and 26B118 alike): the counter.
  - The kernel reads `CNTVCT_EL0` at 891 sites and never reads an Apple
    counter alias or `CNTFRQ_EL0`.
  - It programs the virtual timer (`CNTV_CTL_EL0`: 2 reads, 3 writes;
    `CNTV_TVAL_EL0`: 4 writes), `CNTP_CTL_EL0` (3 writes) and
    `CNTKCTL_EL1` (3 reads, 3 writes).
  - Its frequency comes from the device tree. Published XNU
    (`pexpert/arm/pe_identify_machine.c` at tag `xnu-12377.121.6`, a
    release before macOS 27) starts from 24 MHz and takes
    `timebase-frequency` from the first `/cpus` node whose `state` is
    `running`. That string is in this kernel's `__TEXT`, and the VirtualMac
    tree gives 24 MHz. VERIFIED for the published source, INFERRED for
    26A428.

  The gaokun3's `CNTFRQ_EL0` reads 19,200,000 on all 8 cores (VERIFIED,
  run 9). INFERRED: with that rate in the tree, this kernel runs on the
  gaokun3's own counter with no ECV and no trap, because `CNTVCT_EL0`
  reads at EL1 trap only if the monitor asks them to. Still open: tick
  constants kept elsewhere (`/machine-timeouts` and the like).
- OBSERVED: 163 words in the `0x00201xxx` block (AMX-style), so this
  kernel also carries AMX code.
- The 46 `HVC` sites are all `hvc #0` SMCCC calls; B6 below names every
  one and what a monitor must answer.
- INFERRED, not a decision: this kernel needs none of the register classes
  Route A must rewrite, and its `HVC` interface is the paravirtual surface
  a Route B monitor would serve.

### B6: every hypercall, and what a monitor must answer (26A428)

Evidence, all local and uncommitted:
- disassembly windows around each site, read by hand
  (`build/phase0b/hvc-windows.py`, output `hvc-windows-26A428.txt`);
- a count of every SMCCC function ID the kernelcache builds in a register,
  per fileset entry (`build/phase0b/fid-census.py`, output
  `fid-census-vma2macosap-26A428.txt`);
- published XNU at `xnu-12377.121.6`, read for facts only:
  `osfmk/arm64/hv_hvc.h`, `pac_asm.h`, `machine_routines_asm.s`,
  `cswitch.s`, `arm64_hypercall.c`, `osfmk/arm/machine_routines_apple.c`,
  `osfmk/arm/locks_arm.c` and `pexpert/pexpert/arm64/VMAPPLE.h`.

**All 46 sites are now named** (OBSERVED in the binary). The first pass
left 25 open for two reasons. 13 build `x0` with four `MOVK`s and no
`MOVZ`, which its scanner did not read as a constant. The other 11 load
`x0` to `x5` from one structure and store the results back.

| Service | Function ID | Name | Sites | Notes |
| --- | --- | --- | --- | --- |
| PAC (CPU) | `0xc1000000` | `SET_INITIAL_STATE` | 1 | right after `VBAR_EL1` is set at boot |
| | `0xc1000001` | `GET_DEFAULT_KEYS` | 8 | a run-once guard (`swpb`) on each |
| | `0xc1000003` | `SET_B_KEYS`, `x1` the task's ROP value | 4 | context switch |
| | `0xc1000004` | `SET_EL0_DIVERSIFIER`, `x1` the task's JOP value | 4 | context switch, paired with each `SET_B_KEYS` |
| | `0xc1000005` | `SET_EL0_DIVERSIFIER_AT_EL1`, `x1` 1 or 0, `x2` a JOP value | 2 | `ml_enable_user_jop_key` and `ml_disable_user_jop_key` |
| | `0xc10000f0` | `PAC_NOP` | 1 | in `hvg_is_hcall_available` |
| OEM | `0x8300ff01`, `0x8300ff03`, `0x8300feff` | 32-bit UID, revision and features queries | 1 each | same function |
| | `0xc3000003` to `0xc3000006` | `GET_MABS_OFFSET`, `GET_BOOTSESSIONUUID`, `VCPU_WFK`, `VCPU_KICK` | 1 each | |
| PSCI | `0xc400000e`, `0x84000002` | `SYSTEM_SUSPEND`, `CPU_OFF` | 1 each | kernel sleep path; `b .` after each |
| | through `_smc_call` | `SYSTEM_OFF` `0x84000008`, `SYSTEM_RESET` `0x84000009`, `CPU_ON` `0xc4000003` | 1 | `AppleVirtualPlatform`'s one `hvc`, which takes `x0` from its caller; the three IDs are the constants that kext builds (INFERRED as its callers' arguments) |
| Nested hypervisor | `0xc6000015` to `0xc6000017` | `NESTED_SPACE_MAP`, `UNMAP`, `PROTECT` | 2, 2, 1 | |
| | from a structure | INFERRED: `NESTED_VCPU_RUN` to `NESTED_VM_DESTROY` (`0xc6000010` to `0xc6000018`) | 11 | the kernel builds those IDs as constants; the 11 sites pass `x0` to `x5` through one structure |

So PSCI goes through `hvc`, not `smc`. A guest on the gaokun3 calls the
monitor for every CPU start and stop, and the monitor forwards to the
firmware's PSCI.

**The calling contract** (VERIFIED in published source, INFERRED for
26A428):
- Success is `x0` 0 for the PAC calls. Each is followed by `cbnz x0, .`,
  so any other value leaves that CPU spinning for ever with interrupts in
  whatever state they were.
- The context-switch macros save only `x0` to `x3` around the call, so a
  monitor must preserve every other general register. `GET_DEFAULT_KEYS`
  returns the default ROP value in `x2` and the default JOP value in `x3`.
  These values are opaque to the guest: it hands them back through
  `SET_B_KEYS` and `SET_EL0_DIVERSIFIER`.
- `SET_INITIAL_STATE` runs once per CPU in `common_start`, before the
  guest enables the keys in `SCTLR_EL1`. The guest never writes a key
  register (B5).
- `cswitch.s` skips its `ISB` after a key change because "the hypervisor
  guarantees context synchronization". The monitor's `ERET` gives that.
- The OEM calls are optional. `hvg_is_hcall_available` issues `PAC_NOP`
  first and treats a negative `x0`, or `0xffffffff`, as "no OEM calls".
  The guest then uses none of them: not paravirtual locks (`VCPU_WFK` and
  `VCPU_KICK`, `osfmk/arm/locks_arm.c`), and not the time-offset or
  boot-session sysctls. So `PAC_NOP` answered with `NOT_SUPPORTED` (-1)
  turns off the whole OEM service cleanly.
- The nested hypervisor's behaviour is not in published source, only its
  IDs. A monitor that offers no nested virtualization answers
  `NOT_SUPPORTED`. Whether the guest asks for it during boot is UNKNOWN;
  the QEMU smoke test shows it.

**What standard PAuth can and cannot provide** (INFERRED from the Arm
architecture and the contract above):
- The guest expects separate key state at EL1 and EL0:
  - EL1 keys are fixed at `SET_INITIAL_STATE` and sign kernel heap
    pointers and return addresses.
  - EL0 B keys follow each task's ROP value.
  - EL0 A keys are diversified by each task's JOP value, without touching
    EL1's A keys (the `pac_asm.h` comment says so).
- FEAT_PAuth has one key set for the whole EL1&0 regime. EL0 and EL1 share
  `APIAKey_EL1` and the rest, and nothing traps an EL0-to-EL1 transition
  to EL2 for a key swap. `HCR_EL2.API` and `APK` would trap every PAC
  instruction and key access, which is far too costly to use.
- So on the gaokun3 a monitor can serve the PAC calls functionally only by
  collapsing the key domains: one random key set at `SET_INITIAL_STATE`,
  and `SET_B_KEYS` and both diversifier calls accepted without changing
  any key.
  - Changing the shared keys per task would break the kernel. Its heap
    pointers and the return addresses on every thread's kernel stack were
    signed under the EL1 keys and must still authenticate after a switch.
  - The collapsed scheme authenticates everything the guest signs. Nothing
    in the contract depends on authentication failing.
  - It removes PAC isolation between processes, and between user and
    kernel: a user process could sign pointers the kernel accepts.
- Returning success for a call whose effect is not provided is the kind of
  fabricated success AGENTS.md forbids by default. So the collapsed scheme
  needs an explicit opt-in gate, a documented security limitation, and the
  machine owner's decision before Route B depends on it. That is Phase 0C
  overturn condition 2, answered as "servable, with weaker isolation".

### B8: the VirtualMac2,1 entry and boot handoff (26A428)

**Entry.**
- VERIFIED, binary: the kernelcache's `LC_UNIXTHREAD` entry is
  `0xfffffe0009eb4480`, in `com.apple.kernel`'s `__TEXT_EXEC` at page
  offset `0x480`.
  - Its first instruction branches `0x3bf0` ahead.
  - That code writes `OSLAR_EL1`, `DAIFSet`, `VBAR_EL1`, `TPIDRRO_EL0`,
    `TPIDR_EL0` and `SPSel`.
  - It loads from the boot-args register at offsets 8, 16, 24 and `0x470`.

  The J700's entry is instead the first word of `__TEXT_BOOT_EXEC`, the
  SPTM path ([BOOT-CHAIN.md](BOOT-CHAIN.md#6-boot-structures-and-the-device-tree)).
- VERIFIED, published source: `osfmk/arm64/start.s` (`xnu-12377.121.6`,
  which has `VMAPPLE` conditionals) describes this classic entry: `_start`
  branches to `start_first_cpu`.
  - The contract: EL1, `x0` the physical address of `boot_args`, MMU and
    caches off.
  - The kernel builds its own bootstrap page tables, then enables caches,
    the MMU and PAC itself.
  - The binary's four loads are the source's `BA_VIRT_BASE`,
    `BA_PHYS_BASE`, `BA_MEM_SIZE` and `BA_BOOT_FLAGS`.

  Beyond those first instructions, the match is INFERRED for 26A428.

**What the loader hands over.**
- `boot_args` is the same revision-2 structure the J700 takes
  (`pexpert/pexpert/arm64/boot.h`):
  - memory bases and size, and the top of kernel data;
  - `Video` (base, display mode, row bytes, width, height, depth);
  - the device-tree pointer and length at `+0x60`/`+0x68`;
  - the 1,024-byte command line at `+0x6c`;
  - `bootFlags` at `+0x470`.
- A Route B loader therefore hands over `boot_args` and a completed device
  tree. There is no SPTM boot structure, frame table or loader-built
  kernel page tables, all of which the J700 path needs.
- The device tree is the B3 template, completed as iBoot would. Unlike the
  J700's tree, whose six `cpu` nodes all ship `waiting`, this one ships
  `cpu0` `running` and the other 31 `waiting`.
- INFERRED, for either route: the gaokun3 walks the 16 KiB granule natively
  (Phase 0A), so the kernel's own 16 KiB tables run directly at stage 1.
  The shadow layer's 16 KiB-on-4 KiB translation
  ([BOOT-CHAIN.md](BOOT-CHAIN.md#5-page-tables-a-16-kib-guest-on-a-4-kib-mmu))
  would not be needed, leaving the monitor a stage 2 only.

**Snapintosh's own kexts.**
- VERIFIED, source and string search: classic XNU picks up an AuxKC the
  booter placed in memory from two `/chosen/memory-map` ranges, `AuxKC`
  and `AuxKC-mach_header`.
  - The collection must sit below the kernel's lowest segment, or the
    kernel panics (`osfmk/arm64/arm_vm_init.c`, near line 1363);
    `OSKext.cpp` then consumes it.
  - The VirtualMac kernelcache contains both property names and the panic
    text.
  - The J700 kernelcache contains neither, because Snapintosh hands it the
    AuxKC through the SPTM structure (`probes/xnuboot.py:1435`).
- UNKNOWN: whether `OSKext` and `AppleVPBootPolicy`, the virtual machine's
  boot-policy kext in place of the J700's `BootPolicy`, accept a Snapintosh
  collection.
- VERIFIED: every `OSBundleLibraries` entry of `SnapintoshBootFramebuffer`,
  `AdrenoX2`, `SnapI2CHID` and `SnapintoshQCC2072` is in the VirtualMac
  kernelcache.
- VERIFIED: 198 of the 199 fileset entries both kernelcaches carry have the
  same `LC_UUID`, all but `com.apple.kernel`. They include
  `IOMobileGraphicsFamily`, `IOGPUFamily`, `IOSurface`, `IOGraphicsFamily`
  and `IOPCIFamily`.
- INFERRED: the ABI headers Snapintosh reconstructs from the J700
  kernelcache hold for these shared families. The kexts' personalities
  match J700 or Snapintosh device nodes (`disp0,t8140`, `snapintosh,...`),
  which a loader would add to the VirtualMac tree.

**Graphics without the paravirtual GPU.**
- Console: `boot_args.Video` gives the kernel a boot framebuffer, and
  `IOBootFramebuffer`, the class `SnapintoshBootFramebuffer` binds, is in
  `IOGraphicsFamily`, present here. On the gaokun3 the device tree's
  simple-framebuffer at `0xc6200000` (VERIFIED in Phase 0A) could back it
  (INFERRED).
- Desktop: on the A16, WindowServer waits 30 s for a Metal device and
  aborts, and a boot framebuffer cannot substitute
  ([GUI-BOOT-ROADMAP.md](GUI-BOOT-ROADMAP.md), section 6; OBSERVED there).
  - In a VirtualMac the Metal device is the paravirtual GPU, which needs
    Apple's ParavirtualizedGraphics host.
  - The gaokun3's GPU is an Adreno 690 (`vulkaninfo` under Linux in
    gaokun-android's `docs/hw-inventory.md`), not the X2 Elite's GPU, which
    `gpu/adreno-kext` and `gpu/air2ir3` target.

  INFERRED: a desktop on the gaokun3 needs a Metal driver for the Adreno
  690 whichever route is taken, so graphics does not separate the routes.

### Phase 0C Route B QEMU smoke test (2026-10-23)

**Status: FAIL (host-only smoke gate, not a Route B rejection).** The
harness is [`tools/vmapple-smoke/`](../tools/vmapple-smoke/). It is project
source, MIT licensed and contains no Apple material; the 26A428
`kernelcache.release.vma2` and `DeviceTree.vma2macosap` it reads stay under
ignored `downloads/`.

**Configuration.** QEMU 11.1.2's `virt` board ran under TCG with one vCPU,
GICv3 and `-cpu max,sve=off,sme=off`: generic PAuth and the 16 KiB granule,
but no SVE, SME or Apple CPU features. The smoke stub starts at EL2; it
builds the classic `boot_args` and a completed VirtualMac tree, then enters
the stock kernel at EL1 with its MMU and caches off. Its physical
kernelcache base keeps the same 32 MiB-block offset as the kernel's Mach
header (`0x1004000`), which XNU's first block mapping requires. The first
attempt matched only the 16 KiB offset and executed `__LINKEDIT` as code;
the corrected layout reaches the kernel's real PAC calls.

**Evidence.** Commands, run from the repository root:

```sh
python3 tools/test-vmapple-smoke.py
python3 tools/vmapple-smoke/smoke.py selftest --trace
python3 tools/vmapple-smoke/smoke.py boot \
  --kernelcache downloads/extracted/vma2macosap/kernelcache.release.vma2 \
  --trace --timeout 30
```

- **PASS:** the synthetic self-test (no Apple input) checks 19 loader data
  transforms. The EL1 mini-kernel also proves the EL2 stub preserves the
  DAIF debug mask over HVC returns, accepts `SET_INITIAL_STATE`,
  `GET_DEFAULT_KEYS`, `SET_B_KEYS` and `SET_EL0_DIVERSIFIER`, installs
  working architectural PAuth keys, and RAZ/WIs a TIDCP-trapped
  implementation-defined register.
- **OBSERVED:** the stock 26A428 kernel reaches EL1, its `common_start`
  path calls `VMAPPLE_PAC_SET_INITIAL_STATE` (`0xc1000000`) and
  `VMAPPLE_PAC_GET_DEFAULT_KEYS` (`0xc1000001`), then calls `PAC_NOP`
  (`0xc10000f0`). The stub returns `NOT_SUPPORTED` for `PAC_NOP`, so the
  kernel disables the optional OEM service as published XNU specifies.
  The saved guest `SPSR_EL2` at those calls was `0x600003c4` and then
  `0x800003c4`: all DAIF masks, including debug, were set.
- **FAIL:** no serial message or kernel banner appears. Local control-flow
  analysis finds the kernel calling `panic` with
  `ml_set_interrupts_enabled_with_debug: debug exceptions enabled in kernel
  mode`, then entering its `WFE` halt loop. Removing `debug=0x8` and
  disabling QEMU SVE/SME did not change it. This is an unresolved generic
  QEMU/minimal-platform discrepancy, not evidence that the gaokun3 has the
  same failure.

**What it decides.** The initial PAC interface is servable far enough for
stock XNU to initialise its key state on a generic PAuth CPU; no Apple-only
PAC instruction or key-register write blocks that path. It does **not**
prove a successful VirtualMac boot, process-context PAC switching, the
collapsed-key-domain design on the gaokun3's QARMA5 implementation, or any
platform device model. The Phase 0C QEMU check stays failed until the guest
reaches a serial console or a more specific platform failure.

**Next for this check:** identify why the generic QEMU model presents debug
exceptions unmasked to XNU after the recorded PAC calls. A direct QEMU
`vmapple` comparison is **not available for this 26A428 kernel**: installed
QEMU 11.1.2 has no `vmapple` machine, and upstream's current VMApple
instructions require an already-installed Virtualization.framework macOS
12.x guest and state that newer guests are unsupported. Building it would
not be a control for macOS 27. Do not paper over the panic by returning
fabricated success or patching the stock kernel.

### Still open

- B3's cross-check: the as-booted tree from `ioreg` inside a macOS virtual
  machine, which needs one.
- B8: whether `OSKext` and `AppleVPBootPolicy` accept a Snapintosh AuxKC.
  The Phase 0C QEMU smoke reaches the first PAC calls but fails before
  serial on the generic QEMU/minimal-platform debug-mask discrepancy.
  QEMU's `vmapple` model is not a 26A428 control: QEMU 11.1.2 lacks it and
  upstream supports only macOS 12.x guests for that model.
- B5: what the 11 implementation-defined registers the monitor does not
  know are, and what each needs.
- B6: every site is named. Open: the nested hypervisor's semantics (not
  in published source), and whether the owner accepts collapsed PAC key
  domains on the gaokun3.
- Port-wide, not a route question: a Metal driver for the gaokun3's
  Adreno 690.
- B7: tick constants outside `timebase-frequency`. The gaokun3's own
  `CNTFRQ_EL0`, 19,200,000, is measured (run 9).
- A4: the `SPSR` constants.
- A6: hot paths, including whether the FGT and SME sites can run. This
  needs the 26A428 KDK, because `com.apple.kernel` has no symbols.
- The Route A rewrite judgement, which waits on A6, and Phase 0C's
  decision record.

Everything except A6 needs only the extracted sets.

## Phase 0C comparison (draft)

Drafted 2026-10-23 from the findings above; each cell keeps the evidence
class of the finding it rests on. It is the input to Phase 0C's decision
record ([GAOKUN3-PHASE0C.md](GAOKUN3-PHASE0C.md)), not the decision.

| | Route A: J700 kernel (`j700ap`) | Route B: VirtualMac2,1 kernel (`vma2macosap`) |
| --- | --- | --- |
| Where the kernel runs | EL1, while it believes it is at EL2 under VHE | EL1, as it expects |
| CPU fit | PAuth and 16 KiB fit. NV2, ECV, FGT and RNDR, which the A16 path rests on, are absent (VERIFIED, Phase 0A) | PAuth and 16 KiB are all an EL1 arm64e guest needs (VERIFIED, Phase 0A). The kernel uses no EL2 feature (OBSERVED, census) |
| Kernel changes | 146 sites in `com.apple.kernel`: 97 EL2 register moves, 36 `_EL12` aliases, 12 `CurrentEL` reads and the one `ERET`, which returns with an EL2 mode (OBSERVED). None of them can trap without NV or FGT, so they must be rewritten (INFERRED), beyond what [KERNEL-PATCHES.md](KERNEL-PATCHES.md#policy) allows today. `AppleProcessorTrace` adds 44 more if it loads. Which sites are hot is UNKNOWN (A6) | None found. The stock kernelcache runs, with Snapintosh's kexts handed in as an AuxKC through `/chosen/memory-map` (mechanism VERIFIED; acceptance by `AppleVPBootPolicy` UNKNOWN) |
| Boot handoff | The A16's SPTM entry, reused: MMU on, loader-built tables, the SPTM boot structure, the AuxKC handed through it | The classic entry: EL1, MMU off, `boot_args` and the completed tree. No SPTM structure, frame table or loader-built tables (VERIFIED, B8). New loader path |
| SPTM and TXM | Required: 348 call words, served by `compat/sptm*.c` and `compat/txm*.c` (reused) | None (VERIFIED, B2) |
| Page tables | The kernel's 16 KiB tables can run natively at stage 1, so the shadow layer is not needed (INFERRED) | Same (INFERRED) |
| Counter and timer | All 1,060 Apple alias reads trapped through `HCR_EL2.TIDCP` and served in software, as on the A16. The 24 MHz guest rate stays, since the WD1 watchdog forces it ([BOOT-CHAIN.md](BOOT-CHAIN.md#9-timers)). Without ECV, nothing stops the physical rate leaking through the architectural counter; the kernel itself never reads one (INFERRED) | 891 `CNTVCT_EL0` reads at the device tree's rate: put the gaokun3's 19,200,000 (VERIFIED, run 9) in the tree and no ECV or trap is needed (INFERRED, B7). No WD1 watchdog driver or node (VERIFIED). Other 24 MHz assumptions UNKNOWN |
| Pointer authentication | The kernel sets its own keys (28 key-register writes, OBSERVED). The monitor's PAuth handling (`compat/pauth*.c`) applies (reused) | The hypervisor holds the keys. The PAC hypercalls must be served, or the guest spins at the first (VERIFIED in source), with a call on each context switch (OBSERVED, B6). Standard PAuth cannot give EL0 and EL1 separate keys, so the monitor can only collapse the key domains: functional, but no PAC isolation between processes or between user and kernel (INFERRED, B6) |
| Hypercalls | None: 0 `HVC`, 0 `SMC` (OBSERVED) | 46 `hvc #0` sites (B6). The PAC calls are required; the OEM calls (clock offset, boot-session UUID, vCPU wait and kick) and the nested calls are optional (VERIFIED in source). PSCI also goes through `hvc`, so the monitor forwards it. Every site is named (OBSERVED, B6) |
| Interrupts | A virtual AIC on the GIC (`compat/aic.c`, `compat/irq-forward.c`, reused) | A GICv3 for the guest. The gaokun3's virtual CPU interface is usable at EL2, with 4 list registers (VERIFIED, run 9), so the CPU interface needs no model; the distributor and redistributor view is new |
| Device models | The J700 set the A16 uses: AIC, ANS2 storage on the NVMe host, DART, SEP, PMGR and the rest (reused, re-targeted to the gaokun3's hardware) | A small virtual platform (B3, B4): PL011, PL031 and `avp,rtc`, PL061, pvpanic, `aes,s8000` version 3 (a variant of `compat/aes-engine.c`), `avp,ctrr`, `avp,sealed-registers`, and a PCIe host bridge with virtio devices backed by the monitor's own drivers (the disk through `compat/nvme-host.c`). Almost all new |
| IMPDEF registers | 1,881 accesses across 188 registers, trapped through TIDCP as on the A16 (reused) | 56 accesses across 13 registers: 2 known to the monitor, 11 new (B5) |
| Graphics | Console on the boot framebuffer, as on the A16. A desktop needs a Metal driver for the Adreno 690 | The same. The paravirtual GPU needs Apple's ParavirtualizedGraphics host, so a desktop also needs a Metal driver for the Adreno 690 |
| Main risks | The breadth of the rewrite, and its cost on hot paths (the `ERET` on every exception return, INFERRED); the policy change it needs; A6 cannot be sized without the KDK | Collapsed PAC key domains, a security loss the owner must accept (B6); AuxKC acceptance; which virtio types each driver takes; the as-booted tree (B3 cross-check); the 11 IMPDEF registers |
| Evidence still missing | A4 (`SPSR` constants), A6 (hot paths, needs the KDK) | B3 cross-check, the B5 remainder, the nested hypervisor's semantics (B6), `AppleVPBootPolicy`'s AuxKC rules |

**Reading the table** (INFERRED, not a decision):

- Route B fits the gaokun3's CPU as it stands, and no kernel change has
  been found for it. It drops the SPTM, TXM, SEP, ANS2, DART and AIC
  models. Its cost is new work: a classic loader, a small virtual platform
  with virtio, and the PAC hypercall service.
- Route A keeps most of the existing monitor. But it needs at least 146
  kernel sites rewritten, which the current policy does not allow,
  including the exception-return `ERET`, and its cost cannot be sized
  until A6 has the KDK.
- Graphics is the same problem on both routes.
- Phase 0C decides, including whether to measure Route B's unknowns first.
  Candidates are a GICv3 virtualization check in el2qual, and the
  as-booted VirtualMac tree from a macOS virtual machine on this Mac.
