# gaokun3 Phase 0B: kernelcache study (handover)

Handover for whoever picks up the next step of porting Snapintosh to the
Huawei MateBook E Go 2023 (`gaokun3`, Snapdragon 8cx Gen 3 / SC8280XP).
Phase 0B is offline: it studies Apple's kernelcaches and device trees on a
Mac and needs no access to the gaokun3. Its answers feed Phase 0C, the
written choice between the two routes below.

Status, 2026-10-23: **STARTED, first pass only.** The pinned 26A428
inputs are not on the working Mac, so everything under "Findings" comes from
its own macOS 27.2 (26B118) kernels and must be confirmed on 26A428 before a
decision rests on it. B2, A1 to A3 and A5 have a first answer, and A4, A7,
B5, B6 and B7 a partial one. B0, B1, B3, B4, B8, A6 and the Phase 0C table
are open. `tools/kc-census.py` and its host test exist.

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
| Counter | 19.2 MHz expected, like the A16's; not measured on the gaokun3 (el2qual does not print `CNTFRQ_EL0`); no ECV to rescale it | INFERRED (Qualcomm's usual 19.2 MHz crystal); ECV above |
| Firmware version | not recorded yet | |

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
`KERNKEYLO_EL1`, `KERNKEYHI_EL1`), an `hvc #0` service with
`x0 = 0xc1000000`, and paravirtual GPU, storage and network devices. Some
of its files are unlicensed ("all rights reserved" in its `LICENSING.md`):
read them, never copy them.

## Inputs

Nothing here may be committed (AGENTS.md, rule 1). Keep inputs under
`downloads/` and outputs under `build/phase0b/`; both are ignored.

| Input | Where | Needed for | Notes |
| --- | --- | --- | --- |
| This Mac's own kernels | `/System/Library/Kernels/kernel.release.vmapple` (Apple's virtual-machine kernel, which VirtualMac2,1's kernelcache is presumably built from: INFERRED, B1 checks it) and `kernel.release.t8140` (the J700's SoC) | a first pass at B2, B5 to B7 and A1 to A5 | Present now. They are macOS 27.2 build 26B118, plain arm64e Mach-O executables: one kernel each, no kexts, so not B3 or B4. Record findings from them as 26B118, and confirm on 26A428 before a decision rests on them. Read in place; never copy them into the tree. |
| macOS 27 26A428 UniversalMac IPSW | `downloads/UniversalMac_27.0_26A428_Restore.ipsw` | everything | About 27 GB (`tools/fetch-ipsw.py`). The URL comes from the machine owner; no URL is recorded in this repository. Downloading needs the owner's go-ahead. |
| J700 set | `downloads/extracted/j700ap/` | question A | `python3 tools/extract-firmware.py <ipsw> j700ap` |
| VirtualMac2,1 set | `downloads/extracted/<class>/` | question B | the device class is not known yet (task B1) |
| KDK for 26A428 | `/Library/Developer/KDKs/KDK_27.0_26A428.kdk` | symbol names for A6 | optional; Apple developer download; `tools/kc-symbolicate.py` uses it for J700 |
| XNU source | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) | reading both kernels' contracts | the release matching macOS 27, if published |

**Disk.** The working Mac had 39 GB free on 2026-10-23: room for the IPSW
and two extracted sets, with little margin. The cheaper path is to read
only the members needed straight from Apple's server. The IPSW is a zip
archive, `tools/extract-firmware.py` opens it with `zipfile`, and
`zipfile` needs nothing but `seek` and `read`, so a file-like object that
serves `read` with HTTP `Range` requests lets it run on the URL unchanged.
That is task B0; it is not written yet.

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
and interrupts. Whether the IPSW identity carries a device tree at all is
UNKNOWN (B1 shows it); a virtual machine's tree may come from
Virtualization.framework at boot instead. If so, a macOS virtual machine
on an Apple-silicon Mac gives the same facts from inside the guest:
`ioreg -p IODeviceTree -lw0` for the tree and `ioreg -lw0` for which
drivers matched each node (OBSERVED evidence, for the macOS version the
virtual machine runs). Group them: interrupt controller (GIC, or Apple's AIC as
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
`genter`/`gexit`/`0xd4e00000`, the rest of the `0x00201xxx` block, and
IMPLEMENTATION DEFINED registers (CRn 11/15, any op1). It attributes sites
to fileset entries by each entry's own segments, names functions when `nm`
has symbols, and writes every site with `--sites` (TSV, under `build/`).
Its host test, `tools/test-kc-census.py`, encodes every class by hand from
the Arm ARM field layout, anchors the encoder and the register names a
first census printed unnamed to LLVM-assembled words, and
includes near misses that must not count (EL1/EL0/EL3 op1 values,
`MSR` immediates, `TLBI` at op1 0, `SVC`, `ERET` with an Rn, a different-op2
`CurrentEL`) and a synthetic two-entry fileset. A linear scan also matches
data words in text, so every count is an upper bound.

Not covered by the tool: `SPSR` constants (A4), data flow into `x0` for
`HVC`/`SMC` (B6; a one-off backward scan for a `MOVZ` was used, see
Findings) and the hot-path judgement (A6, which needs symbols).

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
  which is absent); no IPSW, no KDK; 39 GB free.
- Commit titles follow AGENTS.md section 6 (`tools: ...`, `docs: ...`).

## Findings

All from `/System/Library/Kernels/kernel.release.{t8140,vmapple}`, macOS
27.2 build 26B118, read in place. Commands run from the repository root;
raw output is in `build/phase0b/` (ignored). Counts are upper bounds from a
linear scan. **26A428: NOT RUN.** Both kernels are stripped (`nm` lists no
text symbols), so no site is named by function.

**Tooling.** `python3 -I tools/test-kc-census.py`: PASS (76 checks, 0
FAIL); a copy of the tool with two register names swapped fails 6 of them.
Every architectural site word in both kernels (2,027 in `t8140`, 103 in
`vmapple`) was also disassembled with `llvm-mc --disassemble -triple=arm64
-mattr=+all` (LLVM 23.1.4, a one-off check not kept in the tree): PASS,
LLVM gives the same register and direction for every site, and no EL2 or
`_EL12` register is left unnamed. Implementation-defined registers print as
`s<op0>_<op1>_c<n>_c<m>_<op2>`; no Apple register is given a name here
without evidence.

### B2: SPTM and TXM in the VirtualMac kernel

- VERIFIED (26B118): `python3 tools/kc-analyze.py
  /System/Library/Kernels/kernel.release.vmapple` prints `__DATA_SPTM
  absent -- not an SPTM kernel` and "No monitor call sites". The census
  agrees: 0 `genter`, 0 `0xd4e00000`, 0 `gexit`.
- Same command on `kernel.release.t8140`: SPTM-based, 348 call sites in 3
  pages of 16K (174 `genter` and 174 `0xd4e00000` words), matching the
  figure already in the docs.
- UNKNOWN: whether the VirtualMac2,1 IPSW identity carries an SPTM or TXM
  image (B1). If it carries none, Route B drops `compat/sptm*.c`,
  `compat/txm*.c` and the shadow layer's SPTM side.
- Caveat: this is a plain kernel, not the IPSW kernelcache; a kernelcache
  with kexts may differ.

### A1 to A5: J700 kernel (`t8140`)

`python3 tools/kc-census.py /System/Library/Kernels/kernel.release.t8140
--sites build/phase0b/sites-t8140.tsv`

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
  sites. What they are needs symbols (A6).
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
  - KERNEL-PATCHES.md's "about 1,060" counter-alias sites were counted on
    the 26A428 kernelcache with its kexts, so the two figures are
    consistent but are not the same count.

  A7 is partly answered; the `CNTFRQ_EL0` readers are still open.
- UNKNOWN: which of these sites run on hot paths, and whether any run when
  the kernel believes it is at EL1 (A6).

### B5, B6: VirtualMac kernel (`vmapple`)

`python3 tools/kc-census.py /System/Library/Kernels/kernel.release.vmapple
--sites build/phase0b/sites-vmapple.tsv`

- OBSERVED: 0 EL2 register moves, 0 `_EL12` aliases, 0 EL2 `TLBI`/`AT`,
  1 `MRS CurrentEL`, 1 `ERET`, 0 `SMC`, 45 `HVC`, all `hvc #0`.
- OBSERVED: 56 IMPDEF register accesses, including `s3_4_c15_c1_4` (22),
  `s3_4_c15_c1_3` (9), `s3_6_c15_c8_3` and `s3_6_c15_c2_5` (op1 6), and
  `s3_4_c15_c5_0` and `s3_5_c15_c5_0`. What they are, and whether the
  monitor already handles each, is UNKNOWN until compared against the
  monitor's register list and evidence.
- OBSERVED: none of the 56 is `s3_4_c15_c10_6`, so this kernel never reads
  the counter alias that the `t8140` kernel reads 1,017 times. B7 is partly
  answered; where this kernel takes its counter frequency is still open.
- OBSERVED: 163 words in the `0x00201xxx` block (AMX-style), so this
  kernel also carries AMX code.
- OBSERVED: one `HVC` site sets `x0 = 0xc1000000` (a `MOVZ` of `0xc100`
  shifted 16), the value aquarat's notes give for `SET_INITIAL_STATE`
  (documentation only). The other 44 sites take `x0` from their callers,
  so naming them needs per-site data flow (B6 open). The 45 sites span the
  whole text rather than sitting behind one wrapper, though 17 adjacent
  pairs are under 256 bytes apart, so some come in small groups.
- INFERRED, not a decision: this kernel needs none of the register classes
  Route A must rewrite, and its `HVC` interface is the paravirtual surface
  a Route B monitor would serve.

### Still open

B0, B1, B3, B4, B8; B7 (where `vmapple` takes its counter frequency); A4
(`SPSR` constants); A6 (hot paths, needs the KDK symbols, including whether
the FGT and SME sites can run); A7 (`CNTFRQ_EL0` readers); the Route A rewrite
judgement; the Phase 0C comparison table. These need the 26A428 IPSW (the
owner's go-ahead for the download) or the KDK.
