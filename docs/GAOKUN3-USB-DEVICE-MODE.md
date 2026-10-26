# gaokun3 USB device mode: study

Status, 2026-10-26: **step 1 ran on the gaokun3** ([runs 16 and
17](#runs-16-and-17-2026-10-26), [run 19](#run-19-2026-10-26-context-bank-6)).
- USB0 stays powered after ExitBootServices.
- Its DMA passes the SMMU's stage 1 untranslated, then a nested stage 2
  whose mapping is not yet read.
- q1n1's identity check would refuse the controller as it stands. This document sets what q1n1's A16 USB device path assumes
against what the gaokun3 provides, and gives a staged plan whose first
hardware step only reads.
Phase 2 context: [GAOKUN3-PHASE2.md](GAOKUN3-PHASE2.md#run-13-2026-10-26-usb-inventory).

## Goal

The Mac reaches q1n1 on the gaokun3 over USB, as on the A16. The tablet is
a USB device on the power-button port, and the Mac is the host.

Run 13 settled the route. The gaokun3's firmware installs no USB-function
(device mode) driver, only three USB host controllers. So the link must be
q1n1's own DWC3 device-mode driver, brought up on a controller the firmware
leaves in host mode.

## What q1n1 does on the A16

From q1n1's source and docs (`platform/uefi/qcom-dwc3.c`, `usb-ports.c`,
`docs/A16-USB-EL2-SERIAL.md`, `docs/A16-USB-REARM.md`):

- **Two ways in.**
  - **USB0, the boot-window path.** The firmware's own USB-function driver
    brings the controller up in device mode before ExitBootServices. q1n1
    keeps a snapshot of that state and takes the controller over afterwards
    (`usb-ebs.c`). The gaokun3 has no such driver.
  - **Any port, the role-discovery path** (`usb-ports.c`). It starts each
    controller in the role firmware left in `GCTL`, here host, and dwells
    1.5 s. If no device answers, it stops, switches to device mode and takes
    the controller over (`qdwc3_takeover`). This is how USB1, which firmware
    leaves as a host, became `A16-Q1N1-EL2B` under the Mac. **It is the path
    the gaokun3 needs.**
- **The takeover writes three things only:**
  - `GCTL.PRTCAPDIR` set to device;
  - the Qualcomm qscratch VBUS overrides (`+0x10` bits 20 and 28, `+0x30`
    bit 24);
  - a device soft reset.

  It forces high speed (`DCFG` speed 0) and otherwise restores the
  firmware's global configuration. It never touches clocks, power domains,
  PHYs, PD or UCSI.
- **It assumes, before reading anything:**
  1. the controller is powered and clocked after ExitBootServices;
  2. its PHY is initialised;
  3. its identity matches: `snapshot()` requires `GSNPSID` `0x33313130`
     (DWC_usb31) and two xHCI capability bytes, and otherwise marks the
     port unsupported;
  4. DMA is identity-mapped. On the A16 the firmware's own event buffer was
     a plain physical address, and nothing at ExitBootServices reprograms the
     SMMU;
  5. the cable ends with the Mac as host.
- **The A16's own lesson.** Its firmware's `XhciPciEmulation`
  ExitBootServices handler halts host-mode controllers and **powers down
  inactive ones** (`docs/A16-USB-EL2-SERIAL.md`, from disassembly).

## What the gaokun3 provides

| Fact | Value | Class, source |
| --- | --- | --- |
| Firmware USB protocols | no USB function, no Serial I/O; 3 USB2 host handles | VERIFIED, run 13 |
| Port next to the power button | `a600000.usb` (DT `usb_0`); the only device-capable one (Type-C role switch; Android's adb used it) | OBSERVED under Linux, gaokun-android `docs/hw-inventory.md` §12 |
| Other port / keyboard cover | `a800000.usb`, host only / `a400000.usb` | OBSERVED, same |
| USB0 DWC3 core and qscratch | `0x0a600000` (size `0xcd00`) and `0x0a6f8800`, the same addresses as the A16's USB0 | the EL2 device tree (`build/phase1/gaokun3-el2.dts`, local only) |
| Glue | `qcom,sc8280xp-dwc3`. Linux drives it with the same `dwc3-qcom` driver as other Qualcomm parts, so the qscratch VBUS override bits are INFERRED to be the A16's | DT; Linux, reference only |
| HS PHY | Synopsys 5 nm femto, `qcom,usb-snps-hs-5nm-phy`, at `0x088e5000`; SS PHY: QMP combo at `0x088eb000` | DT |
| Type-C | `connector@0` of the Huawei EC (`embedded-controller@38` on I2C `0xa9c000`), dual power and data role, wired to USB0's HS and SS endpoints. The EC runs PD | DT |
| SMMU | USB0 DMA through `apps_smmu`, stream `0x820` | DT |
| Clocks, power, reset (GCC at `0x100000`) | `USB30_PRIM_GDSC` `0x10f004`; master `0x10f010`, sleep `0x10f018`, mock UTMI `0x10f01c`, AXI (voted) `0x10f080`/`0x10f084`; `GCC_USB30_PRIM_BCR` `0x10f000`; HS PHY `GCC_QUSB2PHY_PRIM_BCR` `0x112000` | Linux `drivers/clk/qcom/gcc-sc8280xp.c` and `dt-bindings/clock/qcom,gcc-sc8280xp.h`; facts only, not copied |
| Status bits | clock branch register bit 31 `CLK_OFF` (1 = off); GDSC register bit 31 `PWR_ON` | Linux `clk-branch.h`, `gdsc.c` |
| DWC3 revision, firmware `GCTL`/PHY state after ExitBootServices, SMMU state | — | UNKNOWN |
| SPCR UART | `0xA90000`, type `0x11`, not driven by q1n1 | VERIFIED, runs 14 and 15 |

## Each A16 assumption on the gaokun3

| # | Assumption | gaokun3 | What decides it |
| --- | --- | --- | --- |
| 1 | Controller powered and clocked after ExitBootServices | **UNKNOWN, and the main risk.** All three controllers are host-mode under firmware, and the power-button port may be idle; the A16's firmware powers idle host controllers down at ExitBootServices | GCC status, read before and after ExitBootServices |
| 2 | PHY initialised | UNKNOWN; follows from 1 in the A16's case | the same, plus `GUSB2PHYCFG` once the controller is known to be clocked |
| 3 | Identity `0x33313130` | UNKNOWN, possibly not. Snapshot then refuses the port: fail-closed, but useless until the gaokun3's value is pinned | `GSNPSID`/`GVERNUM`, read only when 1 holds |
| 4 | Identity DMA | UNKNOWN | the firmware's `GEVNTADRLO` while it is running, and `apps_smmu`'s `sCR0` and the stream-`0x820` mapping |
| 5 | Mac ends as host | INFERRED: the Mac is a DFP that sources power, and Android's adb ran on this port as a device. q1n1's role discovery also tries host first, so a wrong guess costs a dwell, not a failure | the first device-mode run |
| 6 | qscratch override bits | INFERRED the same (one Linux glue driver) | the first device-mode run |

Writing to a controller whose clock or power domain is off can hang the
interconnect, and so can reading it. Nothing may touch `0x0a600000` or
`0x0a6f8800` until assumption 1 is known to hold.

## Plan

### Step 1: read only (el2qual stage `usb`)

An opt-in el2qual load option, `usb`, built like the `thermal` stage:
every address checked against UEFI's page tables (`AT S1E2R`, Device, at
its own address) before the first read; a fault fixup on each read; the
SError state reported. It reads at two points: during boot services (UEFI
still runs) and after ExitBootServices at EL2 (after `exit-vhe` if given).

1. **GCC only, always:** the USB0 `GDSC` and the six branch registers above,
   and USB1's for comparison. These are status reads of the always-on clock
   controller. No enable, vote or rate is written.
2. **Only if the GDSC is on and the master and AXI clocks are on:**
   - DWC3 `GSNPSID`, `GVERNUM`, `GCTL`, `GUSB2PHYCFG`, `GUSB3PIPECTL`,
     `DCFG`, `DCTL`, `GEVNTADRLO`;
   - the xHCI capability bytes `snapshot()` checks;
   - qscratch `+0x10` and `+0x30`.
3. **If mapped and readable:** `apps_smmu`'s `sCR0` (`0x15000000`), and the
   stream-match and stream-to-context entries that cover stream `0x820`.

The screen shows each value, and `NOT READ` with the reason where a gate
stopped it. The run needs no cable. A second run with the Mac connected to
the power-button port would show whether an attached host changes anything.

**As built (el2qual 5).** The logic is in `probes/el2qual/usbprobe.{h,c}`,
with no hardware access of its own; `el2qual.c` supplies the reads.
- **The options.** `usb` reads GCC and, when it is safe, the controller;
  `usb-smmu` adds the SMMU.
- **Before ExitBootServices** only the eight GCC words are read: USB0's
  six, and USB1's GDSC and master clock for comparison. Each comes after a
  mapping check at the current EL. There is no fault fixup then, because
  the vectors are the firmware's.
- **After ExitBootServices,** following the VHE exit if `exit-vhe` is
  given, every read is behind el2qual's fixup.
- **The power gate.** The controller is read only when the GDSC is on and
  the master and both AXI clocks run. Sleep and mock UTMI are reported but
  not required.
- **SMMU reads** stop at the first fault. If `sCR0` or `IDR0` reads all
  ones, they stop there and report `NOT READ: ALL ONES` rather than search
  noise. QEMU's empty PCIe window reads exactly that at `0x15000000`.
- **The lines** (`USB0 GCC …`, `USB0 POWER …`, `USB0 CORE …`, `SMMU …`,
  `USB SERROR …`) sit near the top of the screen report, which also goes
  to serial.

| Check | Result, 2026-10-26 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` (`tools/test-el2qual-usbprobe.c`, ASan+UBSan) | PASS. Against a fake that logs every read: no controller word is read unless the gate is open, for every closed gate (GDSC off; master, AXI or CNOC off; a needed word unmapped or faulting); USB1's words decide nothing; one unmapped controller word means none is read; a fault stops the reads; the SMMU match with and without extended stream IDs, masks, invalid entries, the 128-group cap, faults, unmapped pages and all-ones reads; the exact report lines, and their place and colour on the screen. Each of eight deliberately broken variants of the gate and the SMMU search fails the test |
| QEMU: `python3 tools/qemu-el2qual.py` (`max-usb`, `max-vhe-usb-smmu`, and the eleven existing configurations) | PASS. The virt machine has none of these blocks: every GCC word `UNMAPPED`, power `UNREAD`, the controller not read, the SMMU `NOT READ: ALL ONES`, no SError, the CPU report intact. The checker recomputes each power decision from the words printed and fails a controller read while not ON; its self-test rejects wrong logs |
| gaokun3 | PASS, runs 16 and 17 (below) |

### Runs 16 and 17, 2026-10-26

**How.** From the `usb-read` stick: el2qual 5 `e1e58904`, built from
`51800a1`, with run 10's systemd-boot, slbounce, `tcblaunch.exe` and device
tree. Each run was a cold boot, read only and photographed.
- Run 16, entry 8: `nopac nosmp usb`.
- Run 17, entry 9: `nopac nosmp exit-vhe usb-smmu`.

**Ports** (the operator's account, given after the runs, for both runs):
- the stick was in the other port, USB1, the one firmware booted from;
- the charger was in the power-button port, USB0.

So USB0 had only a power-only partner, and the firmware was not using it to
boot. That is the arrangement closest to the later Mac link, whose cable
also supplies power on that port.

**Result (VERIFIED, photographed; identical in both runs unless said).**

| Word | Before / after ExitBootServices | Reading |
| --- | --- | --- |
| USB0 GDSC | `F822F000` / `F822F000` | bit 31 `PWR_ON`: powered |
| USB0 master clock | `00000221` / `00000221` | bit 31 clear: running |
| USB0 sleep, mock UTMI, AXI, CNOC | `00000001` each, both times | running |
| USB1 GDSC, master clock | `F822F000`, `00000221`, both times | powered and running too |
| `USB0 POWER` | `ON` / `ON` | |
| xHCI `CAPLENGTH`/`HCIVERSION` | `01100020` | caplength `0x20`; the A16's is `0x30` |
| xHCI `HCSPARAMS1` | `02000340` | 2 ports, as the A16's; 64 slots |
| `GSNPSID` | `33313130` | the A16's value |
| `GVERNUM` | `3139312A` | the A16's is `3230302A` |
| `GCTL` | `00101005` | `PRTCAPDIR` 1: host mode, as firmware runs it |
| `GUSB2PHYCFG` / `GUSB3PIPECTL` | `00102400` / `0B0C1402` | the first is the A16's |
| `DCFG` / `DCTL` | `00080805` / `00F00000` | device side never started (Run/Stop clear) |
| `GEVNTADRLO(0)` | `00000000` | no device event buffer: host mode |
| qscratch `HS_PHY_CTRL` / `SS_PHY_CTRL` | `10000000` / `00000000` | VBUS overrides not set |
| apps_smmu `sCR0` (run 17) | `00240406` | `CLIENTPD` clear, so the SMMU is **on**; `USFCFG` set, so unmatched streams fault; no extended IDs |
| apps_smmu `IDR0` (run 17) | `7C017ED4` | 212 stream match groups |
| stream `0x820` (run 17) | group 6: `SMR 80000820`, `S2CR 00000006` | valid, exact match; `TYPE` 0, so **translate**, through context bank 6 |

`USB SERROR NONE` in both runs. The VHE exit in run 17 matched runs 11 and
12.

**Against the assumptions above.**

1. **Powered after ExitBootServices: holds.** USB0 had only the charger,
   and the stick was on USB1. The firmware did not power USB0 down at
   ExitBootServices, although it was not booting from it, so the A16's
   takeover path applies, without clock or power programming. Not run:
   USB0 with nothing attached, or with the Mac attached.
2. **PHY initialised: likely.** The controller runs in host mode with the
   A16's `GUSB2PHYCFG`. A device-mode run shows it.
3. **Identity: q1n1 would refuse it.** `GSNPSID` and the port count match,
   but `snapshot()` also requires `CAPLENGTH` `0x30`, and the gaokun3's is
   `0x20`. q1n1 needs the gaokun3's identity, as a gaokun3 entry, not a
   looser check.
4. **Identity DMA: does not hold as such.** USB0's DMA goes through context
   bank 6 of an enabled SMMU. Whether that bank maps physical addresses 1:1
   is unknown, so no device-mode DMA may run yet. The options:
   - read context bank 6's type and translation (read only, like step 1);
   - or have q1n1 point stream `0x820` at bypass, an SMMU write that needs
     its own design and go-ahead.

**A correction made after these runs.** The stage searched at most 128
stream match groups, on the wrong belief that SMMUv2 allows no more. Linux
uses `IDR0.NUMSMRG`, all 8 bits, and this SMMU reports 212. The cap is now
255. Run 17's answer stands, because the match was group 6.

### Step 1b: the context bank (el2qual 6, read only)

Run 17 found USB0's stream translated through context bank 6. Whether that
bank maps physical addresses 1:1 decides whether q1n1 must touch the SMMU.
So `usb-smmu` now goes on to read the bank its matched `S2CR` names.

- **When.** Only for a translating `S2CR` (`TYPE` 0). A bypassing or
  faulting one reads no bank and reports `NOT TRANSLATED`.
- **Where the bank is.** From `IDR1`, as Linux's `arm-smmu.c` computes it:
  4 or 64 KiB pages from `PAGESIZE`, the banks starting
  `1 << (NUMPAGENDXB + 1)` pages in, bank n at page `numpage + n`.
- **Refused without a read** when the bank index is not below `NUMCB`, or
  its page lies outside the SMMU's 1 MiB device-tree window
  (`OUT OF RANGE`), or a page is unmapped.
- **What it reads:**
  - GR1's `CBAR(n)`, the bank's type: stage 1, stage 2 or both;
  - `CBA2R(n)`, whether it uses 64-bit addressing;
  - the bank's `SCTLR`, whose `M` bit says whether translation is on at all;
  - `TCR` and `TCR2`, the table format and sizes;
  - `TTBR0`, the table base;
  - `MAIR0` and `MAIR1`, the memory attributes.

  Nothing is read after a fault.
- **Register facts** come from Linux `arm-smmu.h`, read as a reference.
- **INFERRED:** `TTBR0` is 64-bit and is read as two 32-bit halves,
  because el2qual's fault-catching read is 32-bit. That the SMMU accepts
  32-bit accesses to its halves is not confirmed from a source here. A
  refused access would show as a fault on that word, or as an SError.

How the answer decides the next step:
- `SCTLR.M` clear: translation is off and DMA passes 1:1, so no SMMU
  change is needed.
- `M` set: the tables at `TTBR0` decide. A software walk of them, which is
  plain memory reads, would show whether they map q1n1's buffers 1:1.
- Otherwise: the bypass design.

| Check | Result, 2026-10-26 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS. Against the fake, with the gaokun3's group-6 match: the bank found at the right page for 4 and 64 KiB pages; no bank read for a bypassing or faulting `S2CR`, a bank past `NUMCB`, a page past the window or an unmapped page; a fault inside `TTBR0` keeps the words before it; the exact lines. Each of seven broken variants (no `TYPE` check, no `NUMCB` or window check, no mapping check, wrong `numpage`, `TTBR0` upper half dropped, page size ignored) fails it |
| QEMU: all 13 configurations | PASS. The virt machine's SMMU window reads all ones, so no match and no bank lines. The checker requires bank lines exactly after a match, and `NOT TRANSLATED` exactly for a bypassing or faulting `S2CR`; its self-test rejects each wrong combination |
| gaokun3 | PASS, run 19 (below) |

#### Run 19, 2026-10-26: context bank 6

From the `usb-read` stick: el2qual 6 `6d0760c2`, built from the tree before
its commit, entry 9 (`nopac nosmp exit-vhe usb-smmu`). Run 18 (entry 8)
repeated run 16 exactly. Both were read only, with no SError.

| Word | Value | Reading |
| --- | --- | --- |
| `IDR1` | `6000007F` | 4 KiB pages; banks start 128 pages in; 127 banks. Bank 6 at `0x15086000`, as computed |
| `CBAR(6)` | `00037E03` | `TYPE` 3: a stage-1 bank nested with stage 2. Bits 15:8 are `0x7E`, which the SMMUv2 architecture uses for a nested bank's stage-2 bank index: **bank 126**, the second-last (INFERRED; Linux defines no field there for this type) |
| `CBA2R(6)` | `00000001` | AArch64 format (`VA64`) |
| `SCTLR` | `000000E0` | **`M` clear: stage-1 translation off.** Fault reporting bits set |
| `TCR`, `TCR2` | `00000000`, `00000060` | no stage-1 table format configured |
| `TTBR0`, `MAIR0`, `MAIR1` | all zero | no stage-1 tables |

**Reading it.**
- **Stage 1 is off.** For USB0, a DMA address passes stage 1 unchanged.
- **Stage 2 is not answered.** The bank nests a stage 2, INFERRED to be
  bank 126, set up by the firmware or Qualcomm's hypervisor before
  slbounce. Whether DMA reaches physical memory 1:1 now depends on that
  bank: its `SCTLR.M`, and if that is set, its tables.
- **Next, read only:** el2qual reads the stage-2 bank a nested `CBAR` names
  (`CBAR`, `SCTLR`, `TCR`, `TTBR0`). The same ladder applies:
  - `M` clear means identity, with no SMMU change;
  - `M` set means a walk of its tables;
  - otherwise, the bypass design.

**One USB value changed.** `GUSB3PIPECTL` read `0B081402` in run 19 against
`0B0C1402` in runs 16 to 18, which is bit 18. It is a USB3 PIPE setting,
outside q1n1's high-speed-only path. It is recorded as photographed; what
moves it is not known.

### Step 2: decided by step 1

- **If USB0 stays powered after ExitBootServices,** the A16 path applies
  almost unchanged:
  - give q1n1 a gaokun3 port table (USB0 only; USB1 is host-only, the cover
    stays alone);
  - accept the measured `GSNPSID`;
  - run the role-discovery takeover, which uses the same three writes as on
    the A16;
  - host tests and a QEMU check first, then a supervised run with the Mac on
    the power-button port.

  That run is the first that writes to the USB controller, and it needs its
  own go-ahead.
- **If firmware powers USB0 down,** q1n1 must power it up itself:
  - turn on the GDSC;
  - enable the clocks at the device tree's rates (master 200 MHz, mock UTMI
    19.2 MHz);
  - release the resets;
  - initialise the 5 nm HS PHY.

  That is clock, power and PHY programming on the live SoC. It needs its own
  design, host model and explicit authorization (AGENTS.md rule 5), and it
  is a separate plan. A middle way is worth checking first: whether keeping
  the firmware's driver from releasing the controller before
  ExitBootServices leaves it up.
- **If DMA is not identity-mapped,** q1n1 needs SMMU handling for stream
  `0x820` before any device-mode run, whichever way the power question
  falls.

## Safety

- **Step 1 writes nothing.** Its only risk is a read the hardware refuses.
  The mapping check, the fault fixup and the power gate cover the known ways
  that happens. A GCC or SMMU register that the security controller (XPU)
  protects could raise an asynchronous SError the fixup cannot catch.
  el2qual reports SError state, and holding the power button ends any run,
  as before.
- **No PD or UCSI request is sent, ever,** in either step. The EC owns
  Type-C.
- **Linux is a reference only.** The GCC offsets and bit meanings come from
  Linux's GPL sources, read into the ignored `build/ref/` and not copied
  into this repository. The device-tree facts come from the local EL2 tree,
  which is not committed.
