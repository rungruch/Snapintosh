# gaokun3 port: handover to the next agent

For a coding agent picking up the gaokun3 port cold. It covers:

- the owner's standing rules for working here;
- the exact state of the two repositories;
- the next task, in enough detail to start;
- the working methods and traps from the session that got here.

[GAOKUN3-HANDOVER.md](GAOKUN3-HANDOVER.md) is the general entry point,
and the phase documents are the record. Where this page and the tree
disagree, the tree wins (AGENTS.md, "Start Here").

**As of 2026-10-31**, after run 53. **Phase 4 is PASSED on the
gaokun3** (run 48): the stock 26A428 VirtualMac2,1 kernel mounted an APFS
root filesystem from the RAM-backed VirtIO block device with `BAR0=226`,
`VQ=62`, `IRQ=65` and Line 2 permanently locked in green (`APFS MOUNT: COMPLETE`).
**Phase 5 Step 5A is PASSED on the gaokun3** (run 53): Darwin read freestanding
`/sbin/launchd` Mach-O segments (`BAR0=232`, `VQ=65`), created PID 1, and
transitioned to EL0 userspace. The monitor band latched bright yellow:
`Line 2: USERSPACE: PID 1 RUNNING (/SBIN/LAUNCHD)`, `Line 3: INIT LOADED: VQ=65 BAR0=232`.
(VERIFIED on hardware, photograph `media_1793451743799_61945587.jpg`).

## Read first, in this order

1. [AGENTS.md](../AGENTS.md): the policy. It is not optional.
2. [GAOKUN3-HANDOVER.md](GAOKUN3-HANDOVER.md): the target, the phases,
   the decisions, where things live.
3. [GAOKUN3-PHASE4.md](GAOKUN3-PHASE4.md) and [GAOKUN3-PHASE5.md](GAOKUN3-PHASE5.md):
   the storage and userspace transition records.
4. [GAOKUN3-QUALIFICATION.md](GAOKUN3-QUALIFICATION.md): how hardware
   runs are done and recorded.
5. q1n1's own `AGENTS.md` and `docs/GAOKUN3-VMAPPLE-STICK.md`, in the
   sibling checkout `../q1n1`.

## The owner's standing rules

These come from the owner's instructions over many sessions, on top of
AGENTS.md. Keep applying them unless the owner changes one.

**Git:**

- Commit only when the owner asks. Never amend; fix a mistake in a new
  commit. Never push unless asked.
- Commit titles are `module: imperative change`, at most 72 characters,
  with a body that gives the evidence: commands and PASS, FAIL, SKIP or
  NOT RUN.
- Split commits by concept. When one set of files holds several steps,
  rebuild the intermediate versions and test each commit's exact tree
  (see "Working methods").
- `gaokun3-snapintosh-porting-research-corrected.md` at the repository
  root is the owner's personal plan. It stays untracked and is never
  committed.

**Hardware:**

- Runs are the owner's: supervised, on AC power, photographed. The agent
  builds sticks and checklists and reads the photographs. Never claim a
  hardware result without one.
- No Linux is ever booted on the gaokun3 (an incident on 2026-10-23).
- Any first write to the USB controller, the SMMU or the real GIC needs
  the owner's own explicit go-ahead, separate from approval of a plan.
  So do clock, power, PHY and voltage changes. PD or UCSI requests are
  never sent.
- A safety classifier has stopped earlier attempts to wire the SMMU stage
  2 write into el2qual. Do not attempt that again; the owner may write it
  themself.

**Code:**

- New runtime behaviour is opt-in: a compile-time gate or an explicit
  option. The default build must stay unchanged. Prove that by comparing
  binaries, or q1n1's PE image apart from its timestamp at `0x80`, or the
  QEMU serial logs.
- C files stay below 1,500 lines and 50 KiB. q1n1's `main.c` is already
  over that, so add new code in new files behind a gate.
- Mark facts VERIFIED, OBSERVED, INFERRED, EXPERIMENTAL or UNIMPLEMENTED.
  A QEMU result is never a hardware result.
- No fabricated answers. An unknown register or contract is a named stop,
  never a made-up value. A seed is never fixed or derived: real `RAW`
  entropy or no boot.
- No vendor material (kernelcaches, `tcblaunch.exe`, firmware,
  disassembly) and nothing machine-identifying is committed. Kernelcache
  study output stays in ignored `build/`.

## Where things stand

| Area | State |
| --- | --- |
| Route | B: the stock 26A428 VirtualMac2,1 kernel at EL1 under Snapintosh's monitor (Phase 0C) |
| Phase 3 on gaokun3 | **PASSED on hardware (Run 47).** Stock kernel boots cleanly through `pmap_startup`, GICv3, virtual timer, and BSD domain init (`L2TP DOMAIN INIT COMPLETE`) to root-mount wait (`missing-boot-media` in NVRAM). |
| Phase 4 on gaokun3 | **PASSED on hardware (Run 48).** Stock kernel mounts APFS root volume (`TestRoot`) from RAM-backed VirtIO block device on PCIe. `ACT=1`, `BAR0=226`, `VQ=62`, `IRQ=65`. Line 2 locked in green: `APFS MOUNT: COMPLETE (VOLUME TESTROOT)`. Zero NVMe writes. |
| Phase 5 on gaokun3 | **PASSED on hardware (Run 53).** Darwin mounted APFS root (`TestRoot`), mapped `/sbin/launchd` (`BAR0=232, VQ=65`), and dropped to EL0 userspace executing PID 1. Top monitor banner latched yellow: `USERSPACE: PID 1 RUNNING (/SBIN/LAUNCHD)` and `INIT LOADED: VQ=65 BAR0=232`. |
| Working tree | Clean; all 5 test suites pass (100%). |

Recent Snapintosh commits, newest first:

- `e3f50eb` monitor: latch userspace pid 1 from el0 spsr and elr telemetry
- `3221340` compat: restore direct rd=disk0s1 matching for userspace root mount
- `e403461` compat: inject boot-uuid and nobsdmgroot for apfs userspace init
- `7cd6127` docs: record Phase 4 hardware root mount in handover
- `b3b58d4` tools: verify APFS root mount on gaokun3 hardware
- `6adcf48` tools: accept version 3 bundles for the H3 stick stage

### Tests and their current counts

All PASS, host and QEMU:

| Command | Result |
| --- | --- |
| `python3 tools/test-vmapple-smoke.py` | 88 checks |
| `python3 tools/test-vmapple-classic.py` | 110 checks |
| `python3 tools/test-el2qual-usb.py` | 32 tests |
| `python3 tools/test-vmapple-storage.py` | 58 checks |
| `python3 tools/test-vmapple-monitor.py` | 110 checks |
| `python3 tools/vmapple-smoke/smoke.py selftest` | PASS |

The QEMU boots need a locally extracted kernelcache, never committed:
`downloads/extracted/vma2macosap/kernelcache.release.vma2`.

```sh
python3 tools/vmapple-smoke/smoke.py boot --kernelcache <kc> --timeout 900 --monitor --mmio-trace
python3 tools/vmapple-smoke/smoke.py boot --kernelcache <kc> --timeout 900 --monitor --framebuffer
python3 tools/vmapple-smoke/smoke.py boot --kernelcache <kc> --timeout 300 --trace
```

- **`--mmio-trace`:** the P3 bar: `RESULT: PASS (monitor: root-mount wait
  with no QEMU device visible)`.
- **`--framebuffer`:** the split screen, checked against
  `build/vmapple-smoke/screen.png`.
- **`--trace` without `--monitor`:** the Phase 0C baseline. It needs
  `--trace` for its pass check.

A monitor boot takes 5–15 minutes under TCG, so run it in the background.

## The next task: H2

H2 is the monitor running on the gaokun3, with stage 2 and the virtual
interface ready, **guest not entered**
([PHASE3, the H table](GAOKUN3-PHASE3.md#plan)). Three pieces stand
between here and that run.

### 1. The q1n1-to-monitor hand-off — DONE, rehearsed in QEMU

`--vmapple-enter` (bundle version 2) does what this section asked:
keeps the bundle allocated, places the monitor image, writes the
parameter page, leaves VHE and jumps to the monitor at EL2. The full
chain is rehearsed by `tools/test-vmapple-handoff-qemu.py` to the
root-mount wait, with the test-only seed gate that a gaokun3 build
refuses at compile time. What remains of H2 is piece 2 below, then
decision 4. The original brief:

`--vmapple-place` places, checks and seeds the bundle, then releases it.
H2 needs a further, gated mode that:

- keeps the bundle allocated through ExitBootServices;
- also places the monitor image from the stick;
- leaves VHE (q1n1 has `--exit-vhe`, run on the machine in runs 14–15);
- jumps into the monitor at EL2 with a parameter page: guest entry,
  `boot_args`, the stage-2 RAM range, the screen split.

Design points:

- **Bundle format.** `bundle.bin` (`struct q1n1_vmb_manifest`, magic
  `Q1N1VMB1`, version 1) has no monitor entry or parameter page. Extend it
  as a new version with the two sides moving together: q1n1
  `platform/uefi/vmapple-stick.h` and Snapintosh `tools/vmapple-stick.py`
  `q1n1_bundle()`. The existing tests cross-check the layout; keep them
  doing so.
- **Rehearse the whole chain in QEMU first:** q1n1 on QEMU's EDK2
  (`virtualization=on`, so q1n1 starts at EL2), then the monitor, then the
  kernel to the root-mount wait. QEMU's EDK2 has no `RAW`; it lists only
  `RNDR`. The rehearsal therefore needs a test-only seed path that can
  never be built for the gaokun3: its own compile-time gate, refused by
  the gaokun3 build. Without it, the rehearsal stops at the seed, and that
  stop is correct behaviour.
- **The existing QEMU harness for q1n1 is
  `tools/test-q1n1-vmapple-stick-qemu.py`**, a virtual FAT disk plus a
  `startup.nsh`.

### 2. A gaokun3 build of the monitor

Today `tools/vmapple-smoke/stub.S` and `monitor.c` are QEMU-only:

| Hard-wired for QEMU | Where | On the gaokun3 |
| --- | --- | --- |
| The monitor's own console, PL011 at `0x09000000` | `layout.h` `PL011_BASE`, `stub.S` | no reachable UART (SPCR names `0xA90000`, type `0x11`): write to the monitor's screen band only |
| GIC distributor `0x08000000`, redistributor `0x080A0000` | `monitor.c` `monitor_init`, `layout.h` `PHYS_GICR_BASE` | GICD `0x17A00000` (64 KiB); GICR `0x17A60000` (1 MiB, stride `0x20000`); maintenance PPI 9 (INTID 25) (OBSERVED in the gaokun3 EL2 DTB, `arm,gic-v3`) |
| Timers | `monitor.c` | virtual timer PPI 11 (INTID 27, as in QEMU); `CNTFRQ` 19.2 MHz (VERIFIED, run 9) |
| Semihosting exits (`hlt #0xf000`) | `stub.S` `record_and_halt`, `smoke_exit` | none: halt and show the stop in the band |
| Parameter page at `0x43000000` | `layout.h` `PARAMS_PA` | from q1n1's hand-off |
| Stage 2 maps GiB 1–2 only, 2 MiB blocks | `stub.S` `monitor_setup` | guest RAM is `0x101004000`–`0x161104000` (GiB 4–5). The screen split needs 4 KiB pages: 6400-byte rows, the guest's 2400 rows end at `0xC70A6000`, which is not on a 2 MiB boundary |
| Screen | `MON_FB_*` in `layout.h` | `0xC6200000`, 1600×2560, stride 6400, `a8r8g8b8` (DTB, OBSERVED; runs 14–28). Writes need cache cleaning (q1n1's `fb_clean`). The stage-2 memory type for the real panel is an open question |

Keep the QEMU build working as it is: a gate, not a fork. AGENTS.md
forbids a parallel monitor.

### 3. The owner's decision 4

H2 enables at least the virtual timer interrupt on the real GIC. That is
Phase 3's first hardware write beyond RAM. Put the exact writes to the
owner as a list: register, value, why, how to undo. Wait for an explicit
yes before building a stick that does them.

Then H3 enters the guest; the banner should appear in the guest's rows.
H4 runs to the first stop, the run that can reopen Route B. Each needs
its own go-ahead on the day.

## Working methods that worked

- **One commit per concept, when the files mix steps.** Snapshot the
  final files, then rebuild earlier versions by undoing later edits with
  exact-text or marker replacements, asserting each match is unique. Put
  each version in the index with `git hash-object -w` and `git
  update-index --cacheinfo`, keeping the mode from `git ls-tree HEAD`.
  One slip lost `xnuboot.py`'s executable bit; `bd70a9c` fixed it. Then
  test each commit's exact tree in a throwaway `git worktree` under
  `build/`, with `downloads` symlinked in. A rebuilt base is faithful
  when it reproduces the earlier test counts exactly.
- **Kernel questions:** the panic PC, and the `EL2: decode elr` lines
  `--trace` prints, point at the code. Disassemble it locally with
  `kcload._segments` and `llvm-mc` (study only, never committed). That is
  how the MSI register format was found.
- **Public references:** QEMU's `hw/vmapple/*` and `hw/intc/arm_gicv2m.c`
  (GPL), read with `gh api` at tag `v11.1.2`, the installed QEMU's
  version. Take facts, not code, and cite them. QEMU's vmapple supports
  only macOS 12 guests, so its facts are INFERRED for 26A428. Its
  `cfg.c` offset comments are stale past `0x100`, so compile the struct
  to get the real layout.
- **Stick preparation:**
  - Build the tree under `build/el2qual-usb/<stage>`.
  - Write the checklist beside it, not inside it.
  - Before removing anything on the stick, hash-match its old files
    against a known tree's manifest.
  - Copy with `COPYFILE_DISABLE=1 rsync --exclude='._*'`, then delete
    every `._*` file. A `._slbounceaa64.efi` matches systemd-boot's driver
    pattern.
  - Hash-verify all files, `sync`, `diskutil eject`.
- **Recording a run:**
  - a row in the QUALIFICATION run table: stage, entry, binary hash and
    commit, result with its evidence class;
  - a section in the phase document quoting the photographed lines;
  - say what the run does *not* show.

## Traps already hit

- **zsh doesn't split an unquoted variable:** `opts="--monitor
  --framebuffer"` reaches argparse as one argument. Use `${=opts}` or an
  array.
- **QEMU's human monitor echoes input a character at a time,** so a long
  `pmemsave` path may not have run when you kill QEMU. Use short relative
  paths with QEMU's working directory set to the repository root, and
  wait for the file.
- **QEMU's monitor socket path must be under 104 bytes on macOS.** Run
  scratch QEMU under the repository's `build/`.
- **Piping a test into `tail` hides its exit code;** capture it
  separately.
- **A version bump must reach every copy.** `layout.h`'s
  `SMOKE_PARAMS_MAGIC` has a Python copy in `smoke.py`; a stale one
  refused every boot. `test-vmapple-classic.py` now cross-checks them.
- **A new el2qual option must bump `EL2Q_VERSION`,** since a photo names
  the binary by it.
- **A trapped register is not automatically implementation-defined.**
  The stub once answered `ICC_SGI1R_EL1` as read-as-zero and dropped the
  kernel's self-interrupt. With the monitor, only CRn 11 and 15 are
  RAZ/WI now.
- **A list register left invalid with its EOI bit set** keeps the GICv3
  maintenance interrupt asserted: an EL2 storm. The handler clears it.
- **Guest RAM inside a device window** makes the kernel map its own pages
  as a device and panic in `pmap_enter_pv`. The loader now refuses such a
  layout.
- **The gaokun3 device tree's memory node is empty** (the bootloader
  fills it). Use the UEFI memory map recorded by gk3probe, in
  `../gaokun-android/docs/hw/gk3probe-e3-20261005.txt`. Everything not
  Conventional is reserved; the first P6 placement would have overlapped
  UEFI runtime memory.

## Open questions

- **The MSI frame's type register** (bit 31 valid; two 13-bit fields):
  which field is the first INTID. Both are 128 now.
- **The mailbox at `0x004FFFF8`:** what Virtualization.framework answers.
  Absorbed today.
- **The configuration table's `rnd`:** a real source, if the kernel ever
  reads it. A read is a named stop today.
- **The PCIe memory window's size:** the template says `0x2fff0000`,
  QEMU `0x1fff0000`.
- **The Phase 0C smoke layout** still overlaps its moved ECAM window,
  harmlessly; it opts out of the window check.
- **The stage-2 memory type and cache maintenance for the real panel**
  (H2/H3).
- **After Phase 3:** the USB link resumes at stage 2 of the S2CR bypass,
  the first SMMU write, which needs its own go-ahead.
