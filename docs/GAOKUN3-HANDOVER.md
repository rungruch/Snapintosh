# gaokun3 port: handover

The entry point for anyone picking up the gaokun3 port: where it stands,
where everything lives, how to rebuild and test it, the rules that bind the
work, and what comes next. It summarises; the phase documents it links are
the record, and win wherever the two disagree.

**As of 2026-10-28.** The tree, not this page, is the source of truth
(AGENTS.md, "Start Here"): check `git log` and the linked documents before
acting on anything here.

## The target

The Huawei MateBook E Go 2023, codename **gaokun3**: a Snapdragon 8cx Gen 3
(SC8280XP) tablet, firmware 2.18 (06/30/2023), product `GK-W7X`. The goal
is to run Snapintosh's EL2 monitor on it and boot macOS. Snapintosh's main
target is the ASUS Zenbook A16 (X2 Elite); the gaokun3 is a second machine
with different hardware, so most of the A16's design does not carry over
unchanged.

The route chosen in [Phase 0C](GAOKUN3-PHASE0C.md#decision) is **Route B**:
the stock 26A428 **VirtualMac2,1** kernel at EL1, under a monitor that
supplies what Apple's virtual-machine firmware would. The A16's J700 route
needs NV2, ECV and FGT, which this CPU lacks (VERIFIED, run 4). Route B
needs only PAuth and the 16 KiB granule, which it has. Route B was accepted
with conditions, and a real-hardware boot can reopen it.

## Where things stand

| Phase | State | Record |
| --- | --- | --- |
| 0A: CPU qualification | DONE. PAuth2 with QARMA5 and the 16 KiB walk on all 8 cores; NV2, ECV, FGT and RNDR absent; `CNTFRQ_EL0` 19.2 MHz; GICv3 virtual interface with 4 list registers | [QUALIFICATION](GAOKUN3-QUALIFICATION.md#runs-so-far) |
| 0B: kernel study | Mostly done; QEMU smoke boots the VirtualMac kernel to the root-disk wait (host only) | [PHASE0B](GAOKUN3-PHASE0B.md) |
| 0C: route decision | Route B ACCEPTED, 2026-10-26, with conditions | [PHASE0C](GAOKUN3-PHASE0C.md#decision) |
| 1: guards | Thermal guard pinned to tsens and run once at EL2 (run 10); battery guard and the trend check host-tested against Windows readings | [PHASE1-GUARDS](GAOKUN3-PHASE1-GUARDS.md) |
| 2: EL2 entry and USB link | VHE exit PASSED on the machine (runs 11, 12; q1n1 runs 14, 15). USB link **paused**: USB0's DMA is translated by `apps_smmu`, and the bypass's first write (stage 2) is designed and host-modelled only | [PHASE2](GAOKUN3-PHASE2.md), [USB-DEVICE-MODE](GAOKUN3-USB-DEVICE-MODE.md) |
| 3: VirtualMac kernel to a console | **In progress, QEMU and host only.** P1, P2, P4 and P7 done; decisions 1, 2, 3 and 5 made. P3: **the stock kernel reaches the root-mount wait in QEMU with every device a monitor model** (decision 5's answers, the self-interrupt fix, guest RAM above the device windows, the MSI frame's type register). P5: the bundle builder and q1n1's `--vmapple-place` built. P6: the gaokun3 layout with one CPU and the split screen | [PHASE3](GAOKUN3-PHASE3.md) |

Recent results worth knowing before anything else:

- **Run 27 (2026-10-27): the firmware offers real entropy.** UEFI's RNG
  protocol lists `RAW` and `HASH`, and `RAW` delivered 256 non-zero,
  varying bytes before ExitBootServices. Phase 0C condition 3's seed
  source exists ([PHASE3](GAOKUN3-PHASE3.md#run-27-2026-10-27-the-firmware-offers-raw-entropy)).
- **Run 26: USB0 is halted after ExitBootServices**, so the SMMU bypass's
  "no DMA in flight" precondition holds
  ([USB-DEVICE-MODE](GAOKUN3-USB-DEVICE-MODE.md#run-26-2026-10-27-usb0-is-halted-after-exitbootservices)).
- **Runs 21 to 25: USB0's stage 2 maps only `0xC6200000`–`0xC85FFFFF`**
  below 4 GiB, all of it firmware-reserved. That is why the USB link needs
  the S2CR bypass. The bypass is PLANNED on the machine (run 25) and has
  never been written.
- **P1: the QEMU smoke kernel touches 11 device windows nobody models**,
  including two fixed physical addresses, `0x30010000` and `0x004FFFF8`,
  whose contracts are unknown
  ([PHASE3](GAOKUN3-PHASE3.md#p1-result-what-the-kernel-touches)).

## Decisions

Made by the owner, and binding until the owner changes them:

| Date | Decision | Record |
| --- | --- | --- |
| 2026-10-23 | **No Linux is booted on the gaokun3**, after half the panel stayed corrupt following a Linux survey boot | [QUALIFICATION](GAOKUN3-QUALIFICATION.md#incidents) |
| 2026-10-26 | Route B accepted, with conditions | [PHASE0C](GAOKUN3-PHASE0C.md#decision) |
| 2026-10-27 | **Phase 3 before the USB link, loaded from the stick.** Standalone boot authorized for the gaokun3's Phase 3 only, as transitional. This covers the order and the host and QEMU work; each hardware step still needs its own go-ahead | [PHASE3](GAOKUN3-PHASE3.md#decisions-for-the-owner), decision 1 |
| 2026-10-28 | **q1n1 is the stick loader** (no new loader) | PHASE3 decision 2 |
| 2026-10-28 | **The screen is split**: the guest gets the top rows, the monitor a band below that stage 2 denies the guest | PHASE3 decision 3 |
| 2026-10-28 | **One CPU** for Phase 3; more in a later phase | PHASE3, with decision 3 |
| 2026-10-28 | **AppleVirtualPlatform's fixed addresses** answered as QEMU's `vmapple` documents them: the backdoor interface and the configuration region modelled (QEMU's non-identifying defaults), the mailbox store absorbed and logged | PHASE3 decision 5 |

Open, for the owner ([PHASE3](GAOKUN3-PHASE3.md#decisions-for-the-owner)):

- **4.** The real GIC write in step H2, when H2 comes.

## What comes next

1. ~~**P2: the guest profile and the classic loader in `xnuboot`**~~
   DONE, 2026-10-27 (host and QEMU only): `probes/classicload.py`,
   re-exported by `xnuboot`, with the smoke's `kcload.py` as an adapter
   over it and the panic region supplied outside the reported DRAM
   ([P2 result](GAOKUN3-PHASE3.md#p2-result-the-classic-loader-and-the-panic-region)).
2. ~~**P3 to P6**~~ as far as host and QEMU go, 2026-10-28:
   - **P3:** the EL2 interrupt storm that stalled the monitor after
     `lockdown_mode` is fixed. A spent list register's EOI bit kept the
     maintenance interrupt asserted. The default boot is back at the named
     stop
     ([fix](GAOKUN3-PHASE3.md#fixed-2026-10-28-the-maintenance-interrupt-storm)).
   - **P4:** the split console, with the band checked pixel for pixel
     ([P4](GAOKUN3-PHASE3.md#p4-result-the-split-console)).
   - **P5:** `tools/vmapple-stick.py` writes the stick's tree, and q1n1's
     `--vmapple-place` places it, checks it, fills the seed from `RAW`,
     reports and releases it
     ([P5](GAOKUN3-PHASE3.md#p5-result-the-stick-bundle-placed-by-q1n1)).
   - **P6:** one CPU, the device tree's and UEFI's reserved regions, and
     the split screen ([P6](GAOKUN3-PHASE3.md#p6-result-the-gaokun3-layout)).
3. ~~**P3's bar**~~ MET, 2026-10-28, QEMU only: the root-mount wait with
   the PCIe node kept and no QEMU device answering the guest. The MSI
   frame turned out not to be GICv2m; the kernel's own check gave its type
   register's format
   ([P3](GAOKUN3-PHASE3.md#the-msi-frame-2026-10-28-p3s-bar-met)).
4. **Hardware H2 to H4**, each with its own go-ahead on the day. H1
   PASSED as run 28: q1n1 placed and verified the bundle at its exact
   addresses on the gaokun3, with the seed from `RAW`
   ([run 28](GAOKUN3-PHASE3.md#run-28-2026-10-28-h1-passes)). H2 needs a
   gaokun3 build of the monitor and the q1n1-to-monitor hand-off, then
   decision 4. H2, the first write to the real GIC, needs a gaokun3
   build of the monitor, which does not exist yet.
5. **After Phase 3:** the USB link resumes at stage 2 of the S2CR bypass
   ([step 1i](GAOKUN3-USB-DEVICE-MODE.md#step-1i-stage-2-the-first-smmu-write-design-and-host-model)),
   the first SMMU write, which needs its own go-ahead.

## Where things live

This repository:

| Path | What |
| --- | --- |
| `probes/el2qual/` | The qualification probe: one EFI application that reaches EL2 through slbounce and reports on screen. Load options are in its [README](../probes/el2qual/README.md); every option is off by default. `el2qual.c` (entry and CPU measurement), `usbstage.c` (USB and SMMU glue), `usbprobe.c`, `pagemap.c`, `s2walk.c`, `s2bypass.c`, `rng.c`, `firmware.c`, `report.c` |
| `probes/el2qual/s2bwrite.c` | Stage 2 of the S2CR bypass as a **host model only**. It is not linked into el2qual; wiring it in needs a compile-time gate and a separate go-ahead |
| `tools/el2qual-usb.py` | Builds a stick's directory tree under `build/el2qual-usb/<stage>/` with a SHA-256 manifest. It never writes to a disk |
| `tools/qemu-el2qual.py` | Boots el2qual under QEMU and EDK2 in 32 configurations with known answers |
| `tools/test-el2qual*.c`, `tools/test-el2qual*.py` | Host suites, including deliberately broken copies of each module that must be rejected |
| `probes/classicload.py` | The classic VirtualMac loader (Phase 3 P2): layout, tree completion, NVRAM bank, panic region and image building. Re-exported by `probes/xnuboot.py`; the smoke's `kcload.py` is an adapter over it |
| `tools/vmapple-smoke/` | The Route B QEMU smoke: an EL2 stub (`stub.S`), the kernelcache loader `kcload.py`, and `smoke.py` (`selftest`; `boot --kernelcache PATH`, with `--mmio-trace`). The Phase 3 monitor sits behind `boot --monitor` and is off by default: `monitor.c`/`monitor.h` hold its device models and interrupt handling, `stub.S` its stage 2 and vectors. Debug-only flags include `--absent-fixed`, `--framebuffer`, `--keep-nodes`, `--drop-pcie` and `--passthrough` |
| `tools/vmapple-stick.py` | Builds and checks a Phase 3 stick bundle (P5 host half): one file per chunk, a hashed manifest, and a qualification hash. It never writes to a disk |
| `docs/GAOKUN3-*.md` | The phase records listed above |

Outside it:

| Where | What |
| --- | --- |
| `../q1n1` | q1n1, with its own AGENTS.md. Its gaokun3 work sits behind `Q1N1_VHE_EXIT`, `Q1N1_DT_FRAMEBUFFER`, `Q1N1_GAOKUN3` and `Q1N1_VMAPPLE_STICK` (`--vmapple-place`, Phase 3 P5), and is described in its `docs/GAOKUN3-*.md`. A `Q1N1_GAOKUN3` build drives only USB0 and refuses `--boot`, because USB0's DMA is not yet handled |
| `../gaokun_build` | the sticks' `slbounceaa64.efi` and `tcblaunch.exe` (`tools/el2/`). The EL2 device tree is not in this checkout; reuse the copy recorded in the last stick tree's manifest |
| `../gaokun-android` | the hardware inventory (`docs/hw-inventory.md`) |
| `downloads/`, `build/` | Ignored. Kernelcaches, firmware and every stick tree stay here and are never committed |

## Rebuilding and testing

Host and QEMU checks need no hardware and are the minimum before any
hardware request:

```sh
python3 tools/test-el2qual.py          # host suite, sanitizers, mutation checks
python3 tools/qemu-el2qual.py          # all 32 QEMU configurations
python3 tools/qemu-el2qual.py --self-test
python3 tools/test-el2qual-usb.py      # stick builder
python3 tools/test-vmapple-monitor.py  # monitor device models (ASan/UBSan)
python3 tools/test-vmapple-classic.py  # classic loader (probes/classicload.py)
python3 tools/test-vmapple-smoke.py    # smoke harness and loader
python3 tools/test-vmapple-stick.py    # stick bundle builder (P5 host half)
python3 tools/vmapple-smoke/smoke.py selftest
```

The gaokun3 build of q1n1, and with the Phase 3 stick placement:

```sh
make uefi OUT=build/uefi-gaokun3 \
    Q1N1_DEFINES="-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER -DQ1N1_GAOKUN3"
make uefi OUT=build/uefi-gaokun3-vmb \
    Q1N1_DEFINES="-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER -DQ1N1_GAOKUN3 -DQ1N1_VMAPPLE_STICK"
```

q1n1's own checks for it: `python3 tools/test-q1n1-vmapple-stick.py` and
`python3 tools/test-q1n1-vmapple-stick-qemu.py`, run in `../q1n1`.

The Phase 3 QEMU boots need a locally extracted kernelcache, never
committed:

```sh
python3 tools/vmapple-smoke/smoke.py boot --kernelcache <kc> --monitor
python3 tools/vmapple-smoke/smoke.py boot --kernelcache <kc> --monitor --framebuffer
```

A stick is built with, for example, `python3 tools/el2qual-usb.py entropy`
plus stage 2's inputs (`--systemd-boot`, `--slbounce`, `--tcblaunch`,
`--dtb`; see [QUALIFICATION](GAOKUN3-QUALIFICATION.md#building-a-stick)).
Reusing the previous stick tree's inputs is fine: its manifest records
their hashes.

Things that have bitten before:

- **A new el2qual option must bump `EL2Q_VERSION`** (`report.h`). The
  header line is how a photo names the binary that ran.
- **macOS leaves `._` AppleDouble files on a FAT32 stick.** A
  `._slbounceaa64.efi` matches systemd-boot's driver pattern. Delete every
  `._*` file before ejecting.
- **QEMU's monitor socket path must be under 104 bytes** on macOS. Run
  scratch QEMU boots under the repository's `build/`, not a long temporary
  path.
- **Piping a test through `tail` hides its exit code.** Capture the exit
  status separately.
- **Pre-ExitBootServices progress lines go to the firmware console**, not
  the report section of the serial log.
- **zsh does not split an unquoted `$opts`.** In a loop, `"--monitor
  --framebuffer"` reaches the harness as one argument and argparse refuses
  it; use `${=opts}`.
- **QEMU's human monitor echoes input a character at a time.** A long
  command, such as a `pmemsave` with an absolute path, may not have run
  when a script kills QEMU; use short paths and wait for the result.
- **A list register left invalid with its EOI bit set keeps the GICv3
  maintenance interrupt asserted.** Clear it in the maintenance handler, or
  EL2 takes that interrupt forever.

## Hardware runs

The owner runs the hardware; an agent builds sticks and checklists, and
reads the photographs. Every run follows
[QUALIFICATION](GAOKUN3-QUALIFICATION.md):

- the owner's explicit go-ahead for that run, on the day;
- AC power, the BitLocker recovery key in hand, Secure Boot off;
- the firmware's one-time boot menu, never a changed boot order;
- only Enter in systemd-boot, because other keys write EFI variables;
- the whole screen photographed, then hold power, remove the stick, and
  confirm Windows starts.

Each run is recorded as a row in
[QUALIFICATION's run table](GAOKUN3-QUALIFICATION.md#runs-so-far): stick
entry, `el2qual.efi` hash and commit, el2qual version, and the result with
its evidence class. Runs 1 to 28 are recorded there.

**Writes that need their own explicit go-ahead**, separate from any
approval of the plan:

- the first write to any USB or SMMU register (the S2CR bypass);
- the first write to the real GIC (Phase 3 step H2);
- any clock, power, PHY, voltage or firmware change;
- PD or UCSI requests: never sent at all.

## Working rules

[AGENTS.md](../AGENTS.md) governs. The points that come up most here:

- Commit titles are `module: imperative change`. Commit only when asked,
  never amend, never push unless asked.
- C files stay below 1,500 lines and 50 KiB. `el2qual.c` has been split
  more than once for this: `uefi.h` with a byte-identical binary as proof,
  `usbstage.c` with byte-identical QEMU serial logs.
- New runtime behaviour is opt-in. A default el2qual build must behave
  exactly as before; prove it by diffing the QEMU serial logs against the
  previous commit's.
- Mark facts VERIFIED, OBSERVED, INFERRED, EXPERIMENTAL or UNIMPLEMENTED,
  and report checks as PASS, FAIL, SKIP or NOT RUN. A QEMU result is never
  a hardware result.
- Commit no vendor material (kernelcaches, `tcblaunch.exe`, firmware,
  disassembly) and nothing that identifies the machine or its owner.
- A personal planning file, `gaokun3-snapintosh-porting-research-corrected.md`,
  sits untracked at the repository root. It is not to be committed.
