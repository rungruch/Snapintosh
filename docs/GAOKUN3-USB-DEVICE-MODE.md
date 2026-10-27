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

### Step 1c: the nested stage-2 bank (el2qual 7, read only)

Run 19 found context bank 6 nested: `CBAR` type 3, stage 1 then stage 2,
with stage 1 off. So USB0's DMA is whatever the stage-2 bank does. `usb-smmu`
now reads that bank too.

- **When.** Only after the stage-1 bank was read in full and its `CBAR` is
  type 3.
- **Which bank.** `CBAR` bits 15:8 of the stage-1 bank, `0x7E` = 126 on
  the gaokun3. This reading is INFERRED from the SMMUv2 architecture.
- **A check on that inference.** The named bank's own `CBAR` must be type 0,
  a stage-2 bank. If it is not, only that `CBAR` is read, and the stage
  reports `NOT A STAGE-2 BANK` instead of interpreting the rest.
- **Refused without a read** under the same range and mapping rules as the
  stage-1 bank: below `NUMCB`, inside the SMMU's 1 MiB window, mapped. On
  the gaokun3, bank 126's page is `0x150FE000`, the second-last page of the
  window.
- **What it reads:** `CBAR`, `CBA2R`, `SCTLR` (`M`), `TCR` (in VTCR form)
  and `TTBR0` (the VTTBR, as two 32-bit halves, INFERRED to be accepted as
  before). A stage-2 bank has no MAIRs. Nothing is read after a fault.

How the answer decides the next step:
- `SCTLR.M` clear on bank 126: USB0's DMA reaches physical memory 1:1,
  with no SMMU change needed.
- `M` set: its tables, from `TCR`/`TTBR0`, need a software walk at the
  addresses q1n1 will use. Firmware may well map stage 2 1:1 for normal
  memory, but that is not assumed.
- Otherwise: the bypass design.

| Check | Result, 2026-10-26 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS, 61212 checks. A fixture with run 19's real `ID1`, `CBAR(6)` and bank-6 values covers: bank 126 found at `0x150FE000`; nothing of a stage 2 read for a bank that is not nested; only the `CBAR` read when the named bank is not stage 2; nothing read past `NUMCB` or with an unmapped page; a fault inside `VTTBR` keeps the words before it; a stage-1 fault stops everything; the exact lines. Seven broken variants (no nesting check, no stage-1-read check, no stage-2 type check, no `NUMCB` check, no mapping check, the wrong index field, `VTTBR` upper half dropped) each fail it. Writing the tests also caught a line-formatting bug, now fixed, that prefixed every later line with the stage-2 bank's name |
| QEMU: all 13 configurations | PASS. No stage-2 lines on virt (no match). The checker requires stage-2 lines exactly after a read, nested bank; its self-test rejects both wrong combinations and accepts the right one |
| gaokun3 | run 20 (below): refused, page unmapped |

#### Run 20, 2026-10-26: the stage-2 page is unmapped

From the `usb-read` stick: el2qual 7 `5a367e0e` from `23effe8`, entry 9,
read only, no SError, the operator naming the entry.

- **What it showed.** Everything up to bank 6 is as run 19: nested,
  stage 1 off, `CBAR 00037E03`. Then `SMMU S2 NOT READ: UNMAPPED`, so
  nothing of the stage-2 bank was read.
- **Which mapping failed (INFERRED).** The GR1 page `0x15001000`, where
  `CBAR(126)` and `CBA2R(126)` live, is the page bank 6's `CBAR(6)` was
  read from. So the refused one is bank 126's own page, `0x150FE000`. The
  page tables el2qual runs on are UEFI's, and they do not map it as Device
  at its own address. Bank 6's page, `0x15086000`, is mapped.
- **Not shown.** Whether the page is absent from the tables or present
  with other attributes: the stage records no PAR for this refusal.
- **`GUSB3PIPECTL`** read `0B0C1402` again, so run 19's `0B081402` stands
  as a one-off.

The read-only approach has reached UEFI's map. Reading bank 126 now needs
el2qual to reach that page itself. One way is to add a Device mapping for
the SMMU window to its own EL2 tables, which changes only el2qual's own
memory. The other is a bounded read with its MMU off, where every data
access is Device at its physical address. Both still read the SMMU only.

The alternative is to stop measuring and design the bypass for stream
`0x820`, which q1n1 would need if stage 2 is not 1:1 anyway. That is the
first SMMU write, and it needs its own go-ahead.

### Step 1d: el2qual's own map of the SMMU window (el2qual 8, entry 10)

Load option `usb-smmu-map` (`probes/el2qual/pagemap.c`) runs `usb-smmu`
after first adding the SMMU's 1 MiB window to the EL2 page tables el2qual
runs on, so bank 126's page can be read. It writes RAM, never hardware: one
table page el2qual allocates, and the one level-2 descriptor in UEFI's
leftover tables that points at it. The SMMU is still only read. On the stick
it is a new entry 10; entry 9 stays what runs 17, 19 and 20 ran, so a hang
on 10 names the map.

- **How.** Walk `TTBR0_EL2` (4K granule only) to the window's level-2
  entry. Clone its level-3 table into the new page, fill only the invalid
  window entries, and swap the level-2 entry to the clone. If there is no
  table there, the new page starts empty.
- **New leaves.** Device memory (the first Device index in `MAIR_EL2`, read
  at runtime), identity, read-only and execute-never, with the bits of the
  regime the tables are walked in. After the VHE exit that is the EL2
  regime: `AP[2:1] 0b11` (`AP[1]` is RES1 there) and `XN`, bit 54, while
  bit 53 is RES0. With E2H still set: `AP[2:1] 0b10`, `UXN` and `PXN`.
- **Kept.** Every valid entry of UEFI's table, verbatim, and the original
  level-2 entry's hierarchical limits (bits 63:59), which bound UEFI's
  leaves too.
- **Refused without a write:** another granule, a `T0SZ` the window is
  outside, no Device byte in `MAIR_EL2`, a missing or block entry above
  level 2, a 2 MiB block that is not Device identity, an invalid level-2
  entry that still carries bits, or a valid window entry that is not a
  Device identity page. If every window page is already Device identity,
  nothing is written (`NOT NEEDED`).
- **Swap, break-before-make.** Clean the clone to the point of coherency,
  DSB. Write 0 to the level-2 entry, clean it, DSB, `TLBI ALLE2`, DSB.
  Write the new entry, clean it, DSB, ISB. Nothing el2qual uses is in the
  entry's 2 MiB region: the window is MMIO.
- **Checked.** Every window page must then pass the same `AT S1E2R` check
  the read uses. The line is `SMMU MAP OK CLONE KEPT <n>/256` (or
  `NEW TABLE`), `NOT NEEDED`, or `REFUSED: <why>`. If the map is refused,
  the SMMU read runs as on entry 9.

On QEMU's virt machine EDK2 already maps `0x15000000` as Device (PCIe
MMIO), so there the map is `NOT NEEDED`. The test-only option
`emulate-map-hole` maps two 1 MiB windows in flash0's space instead, at
`0x02000000` and `0x02100000`, which EDK2's tables leave absent (OBSERVED).
The first creates a new table, the second clones it, so the swap of a valid
entry with its break runs too, and the first window must still pass
afterwards. Neither window is read.

| Check | Result, 2026-10-27 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS, 63846 checks (ASan+UBSan). Against a fake that logs every write and maintenance step in order: building writes only the clone; publication is exactly the order above; the gaokun3's run-20 shape gives a clone that keeps UEFI's three leaves and fills the rest; the regime's leaf bits for E2H 0 and 1; the table limits kept; every refusal above writes nothing; a reserved `MAIR` byte is never taken as Device; a start at level 1; a second window cloning the first. 13 deliberately broken copies of `pagemap.c` (slot written while building, `PXN` as execute-never at EL2, `AP[1]` clear, table limits dropped, a bit-carrying invalid entry replaced, no break, no `TLBI`, the clone not cleaned first, no ISB, a valid entry rewritten, a non-identity entry kept, a Normal `MAIR` byte taken as Device, the start level ignored) are each rejected; the suite runs them every time |
| Host: `python3 tools/test-el2qual-usb.py` | PASS, 25 tests: entry 9 unchanged, entry 10 `nopac nosmp exit-vhe usb-smmu-map` |
| QEMU: all 16 configurations | PASS. `max-vhe-usb-smmu-map`: `SMMU MAP NOT NEEDED`. `max-vhe-usb-smmu-map-hole` (after the VHE exit) and `max-vhe-usb-smmu-map-hole-e2h` (E2H kept): `SMMU MAP OK CLONE KEPT 0/256 (TEST WINDOW)`. The checker requires each configuration's exact outcome; its self-test rejects a missing, doubled, malformed or refused line, an unnamed test window, and a write where nothing was needed. TCG does not model a walker that misses the cache, so it cannot show the cleaning is needed; the host tests check the order |
| gaokun3 | run 21 (below): PASS, `SMMU MAP OK CLONE KEPT 208/256`, bank 126 read |

#### Run 21, 2026-10-27: bank 126 translates

From the `usb-read` stick: el2qual 8 `04f5361b` from `b74bbce`, entry 10
(`nopac nosmp exit-vhe usb-smmu-map`), with no SError. Only `SMMU MAP` names
the entry: entry 9 does not print it.

- **The map worked.** `SMMU MAP OK CLONE KEPT 208/256`: UEFI's level-3
  table for the window's 2 MiB region was cloned. 208 of the window's 256
  pages were already Device identity and were kept; the other 48, bank 126's
  page among them, were added. All 256 passed the `AT` check afterwards, and
  the report went on to the end: no hang, no exception after the swap.
- **Up to bank 6, as runs 19 and 20:** stream `0x820` to group 6, bank 6
  nested (`CBAR 00037E03`), stage 1 off (`SCTLR 000000E0`).
- **Bank 126, at `0x150FE000`:**

| Word | Value | Reading |
| --- | --- | --- |
| `CBAR(126)` | `00000003` | `TYPE` 0: a stage-2 bank, so the check on run 19's inference passed. `VMID` 3, in bits 7:0 because `sCR0.VMID16EN` is clear. Bits 7:0 of `CBAR(6)` are also 3 |
| `CBA2R(126)` | `00000001` | AArch64 format (`VA64`) |
| `SCTLR` | `00CF00E1` | **`M` set: stage-2 translation on.** `CFRE`, `CFIE` and `CFCFG` set (a fault is reported, interrupts, and stalls the transaction). Bits 23:16 not decoded |
| `TCR` (VTCR form) | `0001355C` | `T0SZ` 28: 36-bit input addresses. `SL0` 1: the walk starts at level 1. `TG0` 0: 4 KiB granule. `PS` 1: 36-bit output. Walks write-back write-allocate, inner shareable. Bit 31, which Linux writes as RES1, reads 0 |
| `TTBR0` | `00000000B7609000` | the table root at physical `0xB7609000` |

**Reading it.**
- **USB0's DMA is translated.** Stage 1 passes the address through, and
  stage 2 is on. Whether that is 1:1 depends on the tables at `0xB7609000`,
  which nothing has read.
- **Bank 6 and bank 126 belong together.** The type check passed and both
  `CBAR`s carry VMID 3. The index reading of `CBAR(6)` bits 15:8 is still
  INFERRED, but nothing contradicts it.
- **VMID 3 is the OS's own (INFERRED).** Linux's
  `include/dt-bindings/firmware/qcom,scm.h` names Qualcomm's VMID 3
  `QCOM_SCM_VMID_HLOS`. That the SMMU's VMID field uses the same numbering
  is not shown. An HLOS stage 2 that maps the OS's memory 1:1, with holes
  for protected regions, would fit, but is not assumed.
- **The tables sit in a `no-map` region.** `0xB7609000` lies in the gaokun3
  device tree's `reserved-region@aeb00000` (`0xAEB00000`, size
  `0x16600000`), which Linux never maps. Reading it may be refused by the
  security controller (XPU), possibly as an asynchronous SError.

**Two ways on.**
- **Keep measuring** (chosen, step 1e): a bounded, read-only walk of bank
  126's tables for the addresses q1n1 would use for DMA, with the SError
  risk above.
- **Stop measuring:** the bypass for stream `0x820`. This is the first SMMU
  write, and it needs its own go-ahead. It makes the tables irrelevant,
  but replaces the firmware's protection for USB0's DMA.

### Step 1e: walking bank 126's tables (el2qual 9, entry 11)

Load option `usb-s2-walk` (`probes/el2qual/s2walk.c`) runs `usb-smmu`, then
walks the stage 2 it read, page by page, for the memory q1n1 would give the
DWC3. It reads descriptors and nothing else, and writes nothing, not even
RAM. On the stick it is entry 11: entry 10's options plus `usb-s2-walk`, so
a hang or reset on 11 names the walk.

- **Which addresses.** q1n1 hands the DWC3 physical addresses in a 1 MiB
  arena (`QDWC3_ARENA_SIZE`) that UEFI allocates as `EfiLoaderData` below
  4 GiB, wherever it has room (q1n1 `platform/uefi/usb-ebs.c`). el2qual
  allocates its arena the same way before ExitBootServices and walks all
  256 of its pages. The answer covers that boot's arena, which UEFI places
  where q1n1's would likely go. It does not cover all memory below 4 GiB.
- **Which format.** The bank's own registers, as run 21 read them:
  - `CBA2R.VA64` must be set, the granule 4 KiB, and `SL0` with `T0SZ` a
    start level that needs no concatenated tables;
  - `PS` bounds every address followed;
  - the root comes from `TTBR0` bits 47:1 (bit 0 is CnP) and must be
    aligned to its table.

  Anything else is `NOT RUN: FORMAT`, with nothing read. With `SCTLR.M`
  clear the answer is `NOT NEEDED: STAGE 2 OFF`, again unread.
- **How each page is classed** (the Arm ARM's stage-2 descriptors):
  - **`RW`:** identity, access flag set, `S2AP` read and write;
  - **`LIMITED`:** identity, but not both read and write;
  - **`MOVED`:** a valid leaf at another address;
  - **`FAULTS`:** invalid, a reserved encoding (bits 1:0 `0b01` at level
    3, a block at level 0), past the input or output size, or the access
    flag clear (bank 126 has `SCTLR.AFFD` clear, so that faults).

  A table descriptor past the output size is not followed (`REFUSED:
  DESCRIPTOR`).

**The SError risk, and how the walk contains it.** The tables are in a
`no-map` region. If the security controller protects it, a read may
return a synchronous external abort, which the fixup catches, or raise an
asynchronous SError, which no fixup can catch. It may also do something
the walk cannot see at all, such as a reset by the secure world.
- **No new mapping.** A table page is read only through a mapping
  UEFI's tables already have, and only if `AT S1E2R` reports it as identity,
  Normal write-back inner and outer, and inner shareable. The SMMU walks
  these tables coherently, so their latest contents may sit in the CPU's
  caches: a non-cacheable read could see stale memory, and nothing may
  invalidate those lines. If a page fails, nothing of it is read, and the
  line names the page and its `PAR`. If UEFI already maps the region as
  Normal memory, the CPU may already reach it speculatively, so the walk's
  explicit reads add the least new exposure. Adding a mapping is a
  separate decision, after this run shows whether one is needed.
- **An SError already pending refuses the walk** (`NOT RUN: SERROR ALREADY
  PENDING`), since a new one could not be told apart.
- **One load per descriptor.** Each is a single 64-bit load behind the
  fault fixup (`el2q_try_read64`), then a DSB, so the load has completed,
  then a check of `ISR_EL1.A`, with SErrors masked since ExitBootServices
  so they stay pending. The walk stops at the first fault (`FAULT AT`) or
  SError (`SERROR PENDING AFTER`), naming the descriptor.
- **Bounded.** Each descriptor is read once, since the addresses only
  increase, and the walk stops at 264 reads: 256 level-3 descriptors, plus
  the levels above for an arena crossing a 2 MiB and a 1 GiB boundary.
  For bank 126's layout that is 258 to 260 reads.
- **The first read is the probe.** It is level 1's descriptor for the
  arena, at `0xB7609000` plus `8 × (arena >> 30)`. If it is refused, the
  run ends there with one read made.

The lines (after bank 126's, before `USB SERROR`):

```
SMMU WALK ARENA <8 hex> READS <n>
SMMU WALK RW <n> LIMITED <n> MOVED <n> FAULTS <n>
SMMU WALK LEAF L<level> <descriptor>        the arena's first page
SMMU WALK ODD <address> L<level> <descriptor>   the first page not RW
```

The second line is replaced by `REFUSED: TABLE <page> PAR <par>`, `FAULT
AT`, `SERROR PENDING AFTER`, `REFUSED: DESCRIPTOR` or `REFUSED: OVER 264
READS` when the walk stops early.

How the answer decides the next step:
- `RW 256` with no odd page: USB0's DMA reaches the arena 1:1, and q1n1 can
  use stage 2 as it is. That is for this boot's arena; q1n1 would repeat
  the same walk as a guard before enabling DMA.
- `MOVED`, `LIMITED` or `FAULTS` pages: the bypass, or q1n1 allocating
  where stage 2 is identity.
- `REFUSED: TABLE`: UEFI does not map the tables as cacheable memory. The
  `PAR` says how: absent, or with other attributes. Then decide whether to
  extend el2qual's own map to them, or go to the bypass.
- `FAULT` or `SERROR`: the tables are protected from the CPU. Reading them
  is not possible from here; the bypass.

QEMU's virt machine has no stage-2 bank, so test-only options make el2qual
build its own table set for its arena, laid out like bank 126's (`VTCR
0x1355C`, tables at levels 1 to 3). The walk then runs through the real
`AT` checks, guarded loads and SError checks. `emulate-s2-odd` changes three
pages (moved, invalid, read-only). `emulate-s2-device` points the root at
virt's PL011, a Device page the walk must refuse unread.

| Check | Result, 2026-10-27 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS, 265791 checks (ASan+UBSan). Against a fake that answers `AT` per page and logs every load: run 21's layout (258 reads), arenas crossing 2 MiB (259) and 1 GiB (260) boundaries, each read preceded by a passing `AT` of its page and none repeated; every class above, 2 MiB and 1 GiB blocks, reserved encodings, an arena past the input size (no read); each format refusal, translation off, no stage 2, no arena, an SError already pending, all with no `AT` and no read; a root or level-3 page that is absent, Device, non-cacheable, write-through, outer non-cacheable, outer shareable or not identity, refused unread; a fault and an SError, with nothing read after; a table past the output size not followed; every line's exact text. 25 deliberately broken copies of `s2walk.c` (each check above removed, a reserved encoding accepted, a block compared page by page, `SL0` or CnP mishandled, and others) are each rejected; the suite runs them every time |
| Host: `python3 tools/test-el2qual-usb.py` | PASS, 25 tests: entries 8 to 10 unchanged, entry 11 `nopac nosmp exit-vhe usb-smmu-map usb-s2-walk` |
| QEMU: all 20 configurations | PASS. `max-vhe-usb-s2walk-stick` (entry 11's options): `NOT RUN: NO STAGE 2`. `max-vhe-usb-s2walk`: arena `7C121000`, 259 reads, `RW 256`, the first leaf at the arena. `-odd`: `RW 253 LIMITED 1 MOVED 1 FAULTS 1`, the odd page at arena + `0x1000`. `-device`: `REFUSED: TABLE 0000000009000000 PAR 0000000009000B00`, 0 reads. No SError on any. The checker's self-test rejects a missing or refused walk, an SError stop, unnamed test tables, over 264 reads, a first leaf elsewhere, classes not adding up to 256, and the wrong odd page. TCG raises no external aborts, so the fault and SError paths are shown by the host tests only |
| gaokun3 | run 22 (below): the walk ran; every arena page `FAULTS` |

#### Run 22, 2026-10-27: stage 2 does not map the arena

From the `usb-read` stick: el2qual 9 `1378959f` from `9214752`, entry 11
(`nopac nosmp exit-vhe usb-smmu-map usb-s2-walk`), with no SError. Only
`SMMU WALK` names the entry.

Everything up to bank 126 is as run 21: the map `OK CLONE KEPT 208/256`,
then bank 126 the same, word for word. `GUSB3PIPECTL` read `0B081402`.
Then:

```
SMMU WALK ARENA EFD41000 READS 3
SMMU WALK RW 0 LIMITED 0 MOVED 0 FAULTS 256
SMMU WALK LEAF L2 0000000000000000
SMMU WALK ODD EFD41000 L2 0000000000000000
```

**Reading it.**
- **The tables are readable from EL2.** The root page passed the `AT`
  check, so UEFI maps it as cacheable, inner-shareable identity memory, and
  the level-2 table's page passed too. No read faulted, and no SError was
  pending afterwards. The XPU concern did not show here.
- **The arena is not mapped.** UEFI put it at `0xEFD41000`, so it spans the
  2 MiB regions at `0xEFC00000` and `0xEFE00000`. The walk read three
  descriptors:
  - the level-1 descriptor for `0xC0000000`–`0xFFFFFFFF`, at
    `0xB7609018`. It was a valid table descriptor, since the walk went on to
    level 2. Its value is not on the screen;
  - the level-2 descriptors for both regions, slots 382 and 383 of that
    table. Both read 0, which is invalid.

  So USB0's DMA to any of the 256 pages would take a stage-2 translation
  fault.
- **A zero is not proven to be memory (not shown).** A read the security
  controller answers with zeros instead of an abort would look the same.
  Against that: the root page in the same reserved region returned a valid
  descriptor, and nothing faulted.
- **What stage 2 does map is not known.** Bank 126 may map only regions the
  firmware or Qualcomm's hypervisor assigned to devices, rather than all of
  the OS's memory. This run shows only that this arena is outside them.

**What it decides.** As planned for `FAULTS` pages, q1n1 cannot hand the DWC3
an arena UEFI places freely and rely on stage 2 as it is. Two ways remain:
- **Find what stage 2 does map** (read only). Survey the root's 64 level-1
  descriptors and the level-2 table under the arena's 1 GiB: 576 more reads
  from pages run 22 already read without harm. Then q1n1 could allocate its
  arena inside an identity region, if a suitable one exists. That also
  shows whether the tables hold anything besides zeros.
- **The bypass for stream `0x820`:** the first SMMU write, which needs its
  own go-ahead.

### Step 1f: surveying bank 126's top two levels (el2qual 10, entry 12)

Load option `usb-s2-survey` (`el2q_s2w_survey` in `s2walk.c`) runs the walk
of entry 11, then surveys what the same stage 2 maps. That is enough to
choose where q1n1 could put its arena. On the stick it is entry 12: entry
11's options with `usb-s2-survey` in place of `usb-s2-walk`, which it
implies. Entry 11 stays what run 22 ran.

- **What it reads.**
  - Every entry of the root: on bank 126 that is 64 level-1 entries, one
    per GiB of the 36-bit space.
  - When the arena's level-1 entry is a table, all 512 entries of that
    level-2 table, one per 2 MiB of that GiB.

  Level-3 tables are not read. A survey needs a walk that starts at level
  1, as bank 126's does; otherwise `NOT RUN: FORMAT`.
- **Bounded:** 576 reads on bank 126, and never more than 1,024. Every read
  is in the root page, which run 22 read without harm, or in the level-2
  table under the arena's GiB. That is the table run 22 also read, if UEFI
  again places the arena in `0xC0000000`–`0xFFFFFFFF`; the `GIB` line
  shows which.
- **The same guards as the walk**, from the same code:
  - the format checks, and refusal if an SError is already pending;
  - the `AT` check of each table page before its first read;
  - one guarded load per descriptor, then a check for a pending SError;
  - a stop at the first fault or SError. A descriptor followed by an
    SError is not trusted and stays unread (`-`).
- **The map.** One character per entry, classed as the walk classes a page:

| Character | Entry |
| --- | --- |
| `.` | invalid |
| `T` | a table (only the arena's level-1 table is followed) |
| `X` | a table past the output size |
| `I` | an identity block, access flag set, read and write |
| `L` | an identity block, access flag set, not both read and write |
| `M` | a block at another address, access flag set |
| `F` | a block that faults: access flag clear, or past the output size |
| `-` | not read: the survey stopped first |

The lines (after the walk's, before `USB SERROR`):

```
SMMU SURVEY READS <n>
SMMU SURVEY L1 000 <entries 0 to 31>
SMMU SURVEY L1 020 <entries 32 to 63>
SMMU SURVEY GIB <index> <the arena's level-1 descriptor>
SMMU SURVEY L2 AT <its level-2 table> EMPTY ROWS <n>
SMMU SURVEY L2 <IPA of the row's first entry> <32 entries>
```

Only level-2 rows that are neither all `.` nor all `-` are printed: at most
16 lines, and fewer when the map is sparse. `EMPTY ROWS` counts the rows
left out as all `.`. When the survey stops early, a stop line like the
walk's follows `READS`. When the arena's entry is not a table, `L2 NONE:
NOT A TABLE` replaces the level-2 lines. The report has room for 112 lines;
the survey adds at most 22 on bank 126, so at most the bottom of the ID
register table, already recorded in earlier runs, gives way to `...
TRUNCATED`.

How the answer decides the next step:
- **`I` entries (identity blocks):** stage 2 maps those GiBs or 2 MiB
  regions 1:1, read and write. q1n1 could allocate its arena inside one
  with `AllocatePages(AllocateAddress)`, if UEFI's memory map has free
  memory there, and keep the walk as a guard.
- **`T` entries only, apart from the arena's:** the mapping is finer than
  2 MiB, and level 3 would need surveying where it matters.
- **Nothing but `.`, or nothing usable:** the bypass.
- **All `.` everywhere, including where entries must exist:** this would
  hint that the reads return zeros rather than memory. The level-1 entry
  run 22 found to be a table argues against that.

QEMU has no stage-2 bank, so the survey runs over el2qual's own test tables
as in step 1e. `emulate-s2-odd` adds a 1 GiB identity block at level 1
entry 0, and a moved, a read-only and an identity 2 MiB block at level-2
entries 0 to 2 of the arena's GiB, to exercise the classes. The checker
derives every map character from the arena address el2qual prints.

| Check | Result, 2026-10-27 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS, 600882 checks (ASan+UBSan). The survey over run 22's layout: 576 reads, each once and each after its page's `AT` check. Exact lines for the fixture, the odd fixture's `I`, `M`, `L`, `I`, an arena across 1 GiB, and every character class at both levels from hand-built tables, with no level-3 page read. The arena's entry a block or invalid: no level-2 read. An arena past the input size. A 512-entry root of 16 rows, at exactly the 1,024-read bound. A level-2 start refused with no `AT` and no read, as are no stage 2, translation off and an SError already pending. A fault on a level-1 entry (earlier entries kept, the rest `-`), an SError on a level-2 entry (that entry not trusted, unread rows left out), and a refused level-2 page (nothing of it read). The walk's tests are unchanged and pass on the shared code. 35 deliberately broken copies of `s2walk.c` are rejected, 10 of them aimed at the survey: a level-2 table surveyed under a non-table, a level-2 start accepted, `X` shown as `T`, the walk's bound, the wrong arena entry, the table address unmasked, level-2 entries classed as pages, empty or unread rows printed, and wrong row addresses |
| Host: `python3 tools/test-el2qual-usb.py` | PASS, 25 tests: entries 8 to 11 unchanged, entry 12 `nopac nosmp exit-vhe usb-smmu-map usb-s2-survey` |
| QEMU: all 24 configurations | PASS. `max-vhe-usb-s2survey-stick` (entry 12's options): both passes `NOT RUN: NO STAGE 2`. `max-vhe-usb-s2survey`: arena `7C120000`, 576 survey reads, level 1 `.T…`, `GIB 01` pointing at the surveyed level-2 table, 15 empty rows and the row at `7C000000` `TT…`. `-odd`: level 1 `IT…`, and the row at `40000000` `MLI…`. `-device`: both refused with 0 reads. No SError. The checker's self-test rejects a missing or refused survey, a wrong level-1 character, a wrong empty-row count, a level-1 descriptor pointing elsewhere, a missing level-2 row, and an unasked survey |
| gaokun3 | run 23 (below): 576 reads; one 36 MiB identity region in the arena's GiB, starting at the splash framebuffer |

#### Run 23, 2026-10-27: identity only around the splash framebuffer

From the `usb-read` stick: el2qual 10 `76057a70` from `4c0f474`, entry 12
(`nopac nosmp exit-vhe usb-smmu-map usb-s2-survey`), with no SError.
Everything through the walk is as run 22, including the arena, again at
`0xEFD41000`, and `FAULTS 256`. `GUSB3PIPECTL` read `0B0C1402`. The map
rows were counted on an enlarged crop of the photograph, against the
32-character width of the `L1 020` row:

```
SMMU SURVEY READS 576
SMMU SURVEY L1 000 TTTT............TTTTTTTTTTTTTTTT
SMMU SURVEY L1 020 ................................
SMMU SURVEY GIB 03 00000000B76B1003
SMMU SURVEY L2 AT 00000000B76B1000 EMPTY ROWS 14
SMMU SURVEY L2 C4000000 .................IIIIIIIIIIIIIII
SMMU SURVEY L2 C8000000 III.............................
```

**Reading it.**
- **The whole survey read without a fault or SError.** The root and the
  level-2 table under GiB 3 (`0xB76B1000`, also inside
  `reserved-region@aeb00000`) are ordinary readable memory from EL2.
- **Level 1:** tables for GiBs 0 to 3 (`0x0`–`0xFFFFFFFF`) and 16 to 31
  (`0x400000000`–`0x7FFFFFFFF`). No blocks, and nothing above 32 GiB. The
  tables under GiBs 0 to 2 and 16 to 31 were not read, so what they map is
  not known. Why GiBs 16 to 31 have tables is not known either.
- **Level 2 under GiB 3 (`0xC0000000`–`0xFFFFFFFF`):** 18 identity 2 MiB
  blocks, read and write, access flag set: entries 49 to 66, which is
  `0xC6200000`–`0xC85FFFFF`, 36 MiB in one run. Every other entry is
  invalid, with no level-3 tables. The arena's entries, 382 and 383 in row
  11, are among the invalid ones, agreeing with run 22.
- **The region starts exactly at the splash framebuffer.** The device
  tree's `framebuffer@c6200000` is `0xFA0000` bytes, so it ends at
  `0xC71A0000`; el2qual draws there, and the display scans it out. That the
  mapping exists for the display engine's DMA, through a bank that shares
  this stage 2 (VMID 3), is INFERRED. Nothing in the device tree describes
  the remaining `0xC71A0000`–`0xC85FFFFF`, about 20 MiB.

**What it decides.** In the 4 GiB q1n1 allocates from, USB0's stage 2 maps
only this region. Two ways remain:
- **Put q1n1's arena inside it,** past the framebuffer, at a fixed address
  such as `0xC8000000`–`0xC80FFFFF`. Before that is safe, two things need
  checking, both read only:
  - what UEFI's memory map says those pages are (free, reserved for the
    display, or something else), since the display or firmware may still
    use them;
  - the walk over that exact range, for its leaf descriptors.
- **The bypass for stream `0x820`:** the first SMMU write, which needs its
  own go-ahead. It is independent of what the display region holds.

### Step 1g: a fixed arena in the identity region (el2qual 11, entry 13)

Load option `usb-s2-fixed` checks, read only, whether q1n1's arena could
sit in run 23's identity region past the framebuffer. On the stick it is
entry 13: entry 10's options plus `usb-s2-fixed`, which implies the walk.
Entries 11 and 12 are unchanged.

- **The walk over a fixed range.** It walks `0xC8000000`–`0xC80FFFFF`
  (`EL2Q_S2W_FIXED_ARENA`) instead of an allocated arena, with the same
  code and guards as step 1e. The walk reads only descriptors, so nothing
  at the range is allocated, read or written. Its lines end ` (FIXED)`.
  Run 23's map predicts two reads (level 1, then the 2 MiB block at level
  2), `RW 256`, and a level-2 leaf at `0xC8000000`.
- **UEFI's memory map over the whole region**, `0xC6200000`–`0xC85FFFFF`
  (`el2q_memmap_region`, `firmware.c`). The map is the one ExitBootServices
  accepted, already captured; nothing new is asked of the firmware.
  - Every descriptor with pages that overlaps the region is counted.
  - The eight lowest-starting are listed in address order (UEFI does not
    promise a sorted map), with start, end, type and attributes.
  - Any further ones are summed in `AND <n> MORE`.

```
UEFI MAP C6200000-C8600000 ENTRIES <n>
UEFI MAP <start> <end> <type> <attributes>
```

The type is the `EFI_MEMORY_TYPE` name: `RESERVED`, `LDR CODE`, `LDR
DATA`, `BS CODE`, `BS DATA`, `RT CODE`, `RT DATA`, `FREE`, `UNUSABLE`,
`ACPI RECL`, `ACPI NVS`, `MMIO`, `MMIO PORT`, `PAL CODE` or `PERSIST`.
Anything else prints as `T` plus 8 hex digits. If no map was captured, the
line is `UEFI MAP NOT AVAILABLE`.

How the answer decides the next step:
- **`FREE` over `0xC8000000`–`0xC80FFFFF`, and the walk `RW 256`:** q1n1
  can claim that 1 MiB with `AllocatePages(AllocateAddress)` before
  ExitBootServices, so firmware will not hand it out. Its DMA reaches it
  1:1, and the walk stays as a guard.
- **`BS CODE`, `BS DATA` or loader memory there:** something owns it until
  ExitBootServices, so the allocation would fail. q1n1 could use it only
  after ExitBootServices, and only if nothing still in use lives there.
- **`RESERVED` there:** firmware keeps the pages for something, perhaps the
  display. Using them would need to know what for. Then the bypass.
- **No entry there:** UEFI does not describe it as memory at all. The
  bypass.
- The framebuffer, `0xC6200000`–`0xC71A0000`, is expected inside a
  `RESERVED` entry: el2qual draws on it only after checking exactly that.

On QEMU, entry 13's own options give the walk `NOT RUN: NO STAGE 2
(FIXED)` and `ENTRIES 0`, since virt's RAM ends at 2 GiB. With
`emulate-s2-tables`, the test tables are built for the fixed range, and the
map is reported over 36 MiB from virt's RAM base (` (TEST RANGE)`), so a
non-empty map is printed too.

| Check | Result, 2026-10-27 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS, 600937 checks (ASan+UBSan). The memory-map scan with run 23's likely shape out of order (only the two descriptors inside the region, sorted); overlap at either end; descriptors that only touch the region excluded; no pages, a page count past 64 bits, and an end that wraps past 2^64 excluded, including one starting inside the region; twelve entries in ascending and in descending order, the eight lowest kept and `AND 4 MORE`; an OEM type; no map, a descriptor size under 40, a map shorter than one descriptor. The fixed walk over a 2 MiB identity block at `0xC8000000`: 2 reads, `RW 256`, exact lines with ` (FIXED)`, and the longest not-run line fits 64 columns. The memory map's lines follow the survey's, before `USB SERROR`. 8 deliberately broken copies of the scan in `firmware.c` (both overlap edges, zero pages, wrap, no sorting, a higher entry let into a full list, the wrong attribute offset, a short descriptor size) and one of the lines (`AND 0 MORE`) are each rejected |
| Host: `python3 tools/test-el2qual-usb.py` | PASS, 25 tests: entries 8 to 12 unchanged, entry 13 `nopac nosmp exit-vhe usb-smmu-map usb-s2-fixed` |
| QEMU: all 26 configurations | PASS. `max-vhe-usb-s2fixed-stick` (entry 13's options): `SMMU WALK NOT RUN: NO STAGE 2 (FIXED)`, `UEFI MAP C6200000-C8600000 ENTRIES 0`. `max-vhe-usb-s2fixed`: `ARENA C8000000 READS 258 (TEST TABLES) (FIXED)`, `RW 256`, leaf `00000000C80007FF`; `UEFI MAP 40000000-42400000 ENTRIES 1 (TEST RANGE)`, one `FREE` entry `0040000000`–`0044000000`, attributes `8` (write-back). The checker's self-test rejects a missing or unavailable map, entries out of order or outside the range, an entry count that disagrees, an unnamed test range, a non-empty map where virt has none, and an unasked one |
| gaokun3 | run 24 (below): the range maps 1:1, read and write; the whole region is one `RESERVED` descriptor |

#### Run 24, 2026-10-27: the region is firmware-reserved

From the `usb-read` stick: el2qual 11 `45a95b3d` from `50fb2ad`, entry 13
(`nopac nosmp exit-vhe usb-smmu-map usb-s2-fixed`), with no SError.
Everything through bank 126 is as runs 21 to 23; `GUSB3PIPECTL` read
`0B081402`. Then:

```
SMMU WALK ARENA C8000000 READS 2 (FIXED)
SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0
SMMU WALK LEAF L2 00000000C80004FD
UEFI MAP C6200000-C8600000 ENTRIES 1
UEFI MAP 00C6200000 00C8600000 RESERVED 000000000000100F
```

**Reading it.**
- **The range maps 1:1, as run 23 predicted.** Two reads: level 1, then
  a 2 MiB block at level 2. All 256 pages are identity, readable and
  writable. The leaf `0xC80004FD` decodes as:
  - a block (bits 1:0 `0b01`) at `0xC8000000`;
  - `MemAttr` `0xF`: Normal, write-back inner and outer;
  - `S2AP` `0b11`: read and write;
  - `SH` `0b00`: non-shareable;
  - access flag set.
- **UEFI reserves exactly that region.** The map has one descriptor over
  it: `EfiReservedMemoryType`, `0xC6200000`–`0xC8600000` (`0x2400` pages,
  36 MiB). Its attribute capabilities are `UC|WC|WT|WB|WP`. Its bounds
  match the stage-2 identity blocks of run 23 exactly. The framebuffer
  (`0xC6200000`–`0xC71A0000`) is inside it, as el2qual's own check requires.
- **What the rest of it is for is not known.** The firmware reserves
  36 MiB and maps the same 36 MiB for the bank USB0's stream nests. The
  framebuffer uses 16 MB of it. The device tree describes nothing beyond the
  framebuffer. That it is a display carveout, perhaps more buffers, is
  INFERRED from where it starts.
- **`SH` non-shareable** means stage 2 makes no coherency promise for those
  pages. Separately, the device tree's USB0 node has no `dma-coherent`. So
  q1n1's cache maintenance around DMA is needed in any case (OBSERVED from
  the tree; not tested).

**What it decides.** By the rule set before this run, `RESERVED` means
q1n1 should not simply allocate there:
- **The tail past the framebuffer,** `0xC71A0000`–`0xC85FFFFF`, would be
  usable only once something shows nothing else reads or writes it,
  whether the display engine or anything else the firmware reserved it
  for. One read-only way to look: read which addresses the display engine
  scans out, from its source-address registers. That would show what the
  display reads, though not every possible user.
- **The bypass for stream `0x820`** avoids depending on the carveout. It is
  the first SMMU write and needs its own go-ahead.

### Step 1h: the S2CR bypass design (el2qual 12, entry 14)

Runs 21 to 24 leave one way to give q1n1 usable DMA without depending on
the firmware's carveout: stop translating USB0's stream. This section is
the design. **Nothing in it has been written to the SMMU, and el2qual
cannot write it:** `probes/el2qual/s2bypass.c` has no write callback, so no
path through it reaches hardware. Entry 14 only works out the change and
reports it.

#### Why this change and not another

| Option | Blast radius | Verdict |
| --- | --- | --- |
| **`S2CR(6).TYPE` to bypass** | stream `0x820` alone: group 6's stream match is exact (`SMR 0x80000820`, mask 0) | chosen |
| Repoint bank 126's `VTTBR` at q1n1's own tables | every stream whose bank nests bank 126 (VMID 3); run 23's map of the framebuffer region suggests the display is one (INFERRED) | rejected |
| Add entries to the firmware's stage-2 tables at `0xB7609000` | the same, plus writing a `no-map` region the firmware owns | rejected |
| Put the arena in the reserved carveout | none, but run 24 showed UEFI reserves all 36 MiB | rejected unless its tail is shown unused |
| Clear `sCR0.CLIENTPD` | every stream on apps_smmu | rejected |

#### The change

One field of one 32-bit register:

| | |
| --- | --- |
| Register | `S2CR(6)`, at `0x15000000 + 0xC00 + 4×6` = **`0x15000C18`** |
| Before | `0x00000006` (`TYPE` 0, translate; `CBNDX` 6) |
| After | **`0x00010006`** (`TYPE` 1, bypass) |
| Restore | `0x00000006` |

Only bits 17:16 change. Every other bit is preserved, including `CBNDX`,
which bypass ignores, and any field this project has no verified layout
for. That is deliberate. The local Linux reference (`arm-smmu.h`) defines
only `TYPE`, `CBNDX`, `PRIVCFG` and `EXIDVALID` in `S2CR`. The SMMUv2
architecture is understood to give bypass streams attribute controls in
`S2CR` too, but no source in this tree says where, so that is INFERRED.
Writing attribute fields from a guessed layout is what AGENTS.md forbids,
so the firmware's existing bits stand.

**The resulting memory attributes are therefore not predicted.** USB0's
device-tree node has no `dma-coherent`, so q1n1 must do cache maintenance
around DMA whichever attributes result; the first DMA test is what shows
them.

#### What must be true before anyone writes it

`el2q_s2b_plan` refuses unless all of this holds, and reports which one
failed:

- the SMMU read found USB0's stream and read its context bank in full;
- `sCR0` is the measured `0x00240406`: not all ones, `CLIENTPD` clear (the
  SMMU is on), `EXIDENABLE` clear (so group validity lives in `SMR`, not
  `S2CR`);
- `S2CR(6)` still translates through the bank that was read, not some other
  bank, and is not already bypassing;
- `SMR(6)` is valid, its mask is 0, and its id is exactly `0x820`, so the
  group selects USB0 and no other device;
- **no second valid group matches `0x820`.** `usbprobe.c` stops at the
  first match, so the plan scans all `NUMSMRG` (212) groups itself;
- the group is below `NUMSMRG`. `NUMSMRG` is 8 bits, so `S2CR(group)` then
  always lies inside the SMMU's first 4 KiB, the only page the write would
  touch;
- that page translates at EL2 to itself as Device **and is writable**
  (`AT S1E2W`, which no earlier stage needed);
- no SError is pending, and `sGFSR` has no global fault latched, so a fault
  after the write can be attributed to it. The plan refuses rather than
  clearing `sGFSR`, since clearing it is a write;
- `sCR0`, `S2CR(6)` **and `SMR(6)`** re-read now equal what the SMMU read
  found, so nothing moved in between. Re-reading `SMR(6)` matters: a count
  of one matching group alone would accept `SMR(6)` having moved to another
  stream while some other group took `0x820`, and the plan would then name
  an `S2CR` serving a different device;
- no SError became pending during the plan's own reads.

The `SMR(6)` re-read and the last check were added on recheck, after the
first version was committed (`fe98671`). That version would have planned
the write in the moved-group case above. No hardware ran it.

#### The write sequence, when it is authorized

1. Re-run the plan; refuse unless `PLANNED` with the same values.
2. Write the one word, `0x00010006`.
3. Read it back; it must equal exactly what was planned. A read-back that
   differs is the signal that the security controller (XPU) or the secure
   world owns this register and dropped the write.
4. Invalidate. Linux's own reset path writes `TLBIALLH` then `TLBIALLNSNH`
   and polls `sTLBGSYNC`/`sTLBGSTATUS` (VERIFIED as Linux behaviour,
   `arm-smmu.c`); that pair is the conservative choice for a one-off change
   at boot. `TLBIVMID` with VMID 3 would be narrower, but whether it also
   covers any cached stream-to-context *configuration* is not known here:
   nothing in this tree says whether this SMMU caches it. The broad pair
   covers both cases. Its cost is that other masters re-walk once; the
   display is live at this point.
5. Check `sGFSR` and `ISR_EL1` again.

#### Recovery

- **Restore** writes `0x00000006` back and invalidates again. The quiesce
  path must do this before handing the machine on.
- **Nothing is persistent.** The SMMU's registers are volatile, so a power
  cycle with the stick removed restores the firmware's configuration
  whatever happens, as after every run so far. Holding the power button
  still ends any run.

#### What could go wrong

- **The write is dropped or faults.** Read-back catches a drop; the fault
  fixup catches a synchronous abort; an asynchronous SError shows in
  `ISR_EL1`. A reset by the secure world would show as the screen stopping
  at the stage's progress line.
- **Something else is using the stream.** The exact-match and
  second-group checks are there for this. Nothing else on the gaokun3 is
  expected to drive USB0 after ExitBootServices, since firmware's own
  driver is done.
- **A stalled master.** Bank 126's `SCTLR` has `CFCFG` set, so a stage-2
  fault *stalls* the transaction rather than terminating it. That is an
  argument for doing the bypass **before** any device-mode DMA: with
  translation on and the arena unmapped, the first DMA would stall USB0
  rather than fail cleanly.

#### The staged hardware plan

Each stage needs its own go-ahead; only the first exists.

1. **Entry 14, now: plan only.** Confirms on the real machine that every
   condition above holds and prints the exact word. Writes nothing.
2. **Write and immediately restore,** with no DMA in between: the shortest
   possible window in which the SMMU is changed. This answers whether the
   write takes at all.
3. **Write, leave bypassed, run q1n1's USB device mode,** then restore.
   This also needs q1n1's gaokun3 identity (xHCI `CAPLENGTH` `0x20`, a
   USB0-only port table) and is the first run that writes to the USB
   controller.

#### Where it ends up

The bypass belongs in q1n1, which owns the DMA, not in el2qual, which
measures. `s2bypass.c` is the model and the gate: q1n1 would carry the same
checks and the same restore. Until stage 2 above has run, this is a design
with host evidence only.

| Check | Result, 2026-10-27 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS, 624633 checks (ASan+UBSan). Run 24's measured state gives exactly the word above, with only `TYPE` changed, `matches 1/212`, and 215 reads (`sGFSR`, `sCR0`, `S2CR(6)`, 212 `SMR`s), each allowed and none twice. Every refusal above, with nothing read for those decided from what the SMMU read already found. A second matching group, exact or by mask; an invalid or differently-addressed group that is not a match; a group past `NUMSMRG`; the matching group vanishing, widening its mask, or moving to another stream while a different group takes `0x820`; an SError raised by the plan's own reads. A fault injected on each read in turn. Unknown `S2CR` bits preserved; a different group index moving both address and name. 19 deliberately broken copies of `s2bypass.c` (each check above removed, including the `SMR(6)` re-check, which reproduces the first committed version; the other fields clobbered; fault planned instead of bypass; nothing to restore to; a hole before the last line) are each rejected |
| Host: `python3 tools/test-el2qual-usb.py` | PASS, 25 tests: entries 8 to 13 unchanged, entry 14 `nopac nosmp exit-vhe usb-smmu-map usb-s2-bypass-plan`, and no entry carries a write option |
| QEMU: all 27 configurations | PASS. `max-vhe-usb-s2bypass-plan` (entry 14's options): virt's SMMU reads all ones, so `SMMU BYPASS NOT MEASURED`, then `SMMU BYPASS PLAN ONLY, NOTHING WRITTEN`. The checker requires that last line always, and its self-test rejects a missing plan, a different outcome, the dropped last line, a line claiming a write, and an unasked plan |
| gaokun3 | run 25 (below): `PLANNED`, every precondition held, the exact word above; nothing written |

#### Run 25, 2026-10-27: the plan holds on the machine

From the `usb-read` stick: el2qual 12 `712af82c` from `9496bb9`, entry 14
(`nopac nosmp exit-vhe usb-smmu-map usb-s2-bypass-plan`), with no SError.
Everything through bank 126 is as runs 21 to 24; `GUSB3PIPECTL` read
`0B0C1402`. Then:

```
SMMU BYPASS S2CR06 AT 15000C18 00000006 -> 00010006
SMMU BYPASS MATCHES 1/212 MASK 0000 GFSR 00000000
SMMU BYPASS PLAN ONLY, NOTHING WRITTEN
```

**Reading it.**
- **Every precondition held on the gaokun3,** so the plan reached
  `PLANNED`, and its word is the one the design names: `S2CR(6)` at
  `0x15000C18`, `0x00000006` to `0x00010006`.
  - The SMMU's `sCR0`, `S2CR(6)` and `SMR(6)` re-read unchanged.
  - Exactly one of the 212 groups matches `0x820`, and it is group 6, with
    mask 0.
  - `sGFSR` is clear.
  - No SError was pending before the check, and none after its 215 reads.
- **The register's page is writable at EL2.** `AT S1E2W` of `0x15000000`
  reported a Device identity translation with write permission. That is
  the first evidence that a write could even be issued from where el2qual
  runs. It is a statement about el2qual's own page tables only: whether
  the SMMU accepts the write, or the security controller drops it, is not
  shown, and only stage 2 of the plan can show it.
- **Nothing was written.** The stage has no write path, and the last line
  says so.

**What it decides.** Stage 1 of the staged hardware plan is done: the
plan's preconditions hold on the real machine, not only in the host
model. Stage 2, writing the word and restoring it at once with no DMA in
between, is the next step. It is the first SMMU write, and it needs its
own design review and explicit go-ahead before any code for it exists.

### Step 1i: stage 2, the first SMMU write (design and host model)

Stage 2 asks one question, with the least change for the shortest time:
**does the write to `S2CR(6)` take?** It writes the planned word, reads it
back, writes the old word back at once, reads that back, and then
invalidates the SMMU's TLBs as Linux's reset does. USB0 must be halted
throughout, so no transaction of its stream can meet the bypass while it
lasts.

**Status: a host model only.** `probes/el2qual/s2bwrite.c` is not built
into el2qual: `build.py` does not compile it and `el2qual.c` does not
mention it. el2qual.efi is unchanged by it, `712af82c`, the binary run 25
ran. It runs only in `tools/test-el2qual-s2bwrite.c`, against a fake SMMU.
**Nothing has been written to the SMMU.**

#### Before any write

Each of these must hold, or nothing is written and the line says
`REFUSED:` with the reason, then `NOTHING WRITTEN`:
- a stage-1 result from this boot that is `PLANNED`, and consistent:
  - it names the address of `S2CR(group)` in this SMMU's window;
  - its old word translates (`TYPE` 0);
  - its new word is the old one with only `TYPE` set to bypass;
  - its restore word is the old one.

  A result that was altered, or made for another SMMU, is not written
  from;
- no SError pending;
- **USB0 halted:** xHCI `USBCMD.R/S` (bit 0) clear and `USBSTS.HCH` (bit 0)
  set, at the operational registers (core + `CAPLENGTH`). "No DMA" is the
  premise of the stage, and this is what checks it. Offsets and bits are
  from Linux `drivers/usb/host/xhci.h` (`struct xhci_op_regs`) and
  `xhci-ext-caps.h` (`XHCI_CMD_RUN`, `XHCI_STS_HALT`), read as a reference.
  The operational base is not an assumed offset: `CAPLENGTH` is read from
  the controller, which
  [step 1j](#step-1j-the-usb0-halt-check-el2qual-13-entry-15) does on its
  own (the A16 reports `0x30`, not the `0x20` q1n1 assumed). **These
  registers have not been read on the gaokun3 yet**: if UEFI left the
  controller running, the stage refuses, and that is an answer too;
- `S2CR(group)` still reads as the old word.

#### The writes

Every write the stage can make, in order, each at most once, each followed
by a DSB before anything reads it back:

| # | Register | Address | Value |
| --- | --- | --- | --- |
| 1 | `S2CR(6)` | `0x15000C18` | `0x00010006`, the bypass |
| 2 | `S2CR(6)` | `0x15000C18` | `0x00000006`, the restore |
| 3 | `TLBIALLH` | `0x1500006C` | `0xFFFFFFFF` |
| 4 | `TLBIALLNSNH` | `0x15000068` | `0xFFFFFFFF` |
| 5 | `sTLBGSYNC` | `0x15000070` | `0xFFFFFFFF`, then `sTLBGSTATUS` polled |

Rows 3 to 5 and the dummy value are Linux's (`arm-smmu.c`'s reset and
`__arm_smmu_tlb_sync`, `QCOM_DUMMY_VAL` −1). There is no invalidation
between 1 and 2: USB0 is halted, so nothing uses the bypass, and the
question is answered by the register's read-back, not by a transaction.
The one invalidation at the end drops anything the SMMU cached while the
bypass stood.

The sync poll has two bounds: about a second of counter ticks, as Linux
allows, and 2^20 polls in case the counter does not advance.

#### Outcomes

| Outcome | Writes | Meaning | Left on the machine |
| --- | --- | --- | --- |
| `DONE: TOOK AND UNDONE` | 5 | the write takes and is undone; the SMMU reported nothing | as before |
| `DROPPED: NOT TAKEN` | 1 | read back as the old word: the security controller or the secure world owns `S2CR`, and silently ignored the write. Nothing else is written | as before |
| `UNEXPECTED READ BACK, RESTORED` | 5 | read back as neither word | as before (restore read back) |
| `FAULT ESR …, RESTORED` | 5 | the store or its read-back raised an exception; the restore still followed, since the bypass may have taken anyway | as before |
| `RESTORE FAILED: POWER CYCLE` | 2 | the restore did not read back as the old word. No more writes: an unknown register state is not improved by guessing | **USB0's stream may stay bypassed until a power cycle**, which reloads the firmware's SMMU setup |
| `RESTORED, SYNC INCOMPLETE` | 3 to 5 | restored, but the invalidation faulted or `sTLBGSTATUS` never cleared | as before, apart from TLB contents |
| `RESTORED, GLOBAL FAULT` | 5 | restored and synced, but `sGFSR` is set | as before; the fault is recorded |
| `RESTORED, SERROR PENDING` | 5 | restored and synced, but an SError is pending | as before |

Even the worst case, a failed restore, changes only how USB0's stream is
treated, while USB0 is halted. Nothing persists: the SMMU's registers are
volatile.

The lines:

```
SMMU WRITE S2CR06 00000006 -> 00010006 READ BACK <8 hex|FAULT|NOT DONE>
SMMU WRITE RESTORE 00000006 READ BACK <8 hex|FAULT|NOT DONE>
SMMU WRITE TLBI SYNC <OK|TIMEOUT|FAULT> POLLS <n> GFSR <8 hex>
SMMU WRITE <outcome>, WRITES <n>
```

A line that does not apply to the outcome is left out, with no hole
before the last. `NOT DONE` means the store before it faulted, so it was
not read back.

#### What a go-ahead would cover

Two separate decisions, in order:
1. **Wiring, code only.** Build `s2bwrite.c` into el2qual only under a
   compile-time gate (`EL2Q_S2B_WRITE`, off by default, per AGENTS.md
   rule 3), so the default build stays byte-identical to `712af82c`. Then
   a load option runs stage 1 and stage 2 back to back in one boot, plus a
   QEMU configuration for the gated build (virt's SMMU reads all ones, so
   the plan is `NOT MEASURED` and nothing is written) and a stick entry 15
   built only from the gated binary.
2. **The hardware run**, supervised, on AC power, photographed: the first
   write to the SMMU.

| Check | Result, 2026-10-27 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS, 624908 checks (ASan+UBSan). Against a fake SMMU that logs every access: run 25's plan gives exactly the five writes above, in order, each followed by a barrier, and each `S2CR` store read back right after it. The register ends at its old value, and USB0's halt is read before the first store. A dropped write gives 1 write and no restore. A corrupted read-back is restored. A bypass store that faults, with or without taking effect, and a faulting read-back are each restored. A restore that is dropped, corrupted, faults on its store or faults on its read-back stops at 2 writes. The sync is bounded by the counter, and by the poll limit when the counter is frozen. Faulting invalidate stores and status reads, a global fault, an unreadable `sGFSR`, and an SError during the writes are each reported. Every refusal (no plan, plan not `PLANNED`, another address, another word, a non-translating old word, another restore, group 255, another SMMU, a missing callback, a pending SError, USB0 running, not halted or unreadable, `S2CR` moved or unreadable) makes no write. 19 deliberately broken copies of `s2bwrite.c` are each rejected; the mutation runner now treats a copy that hangs as caught, since one removes the poll bound |
| el2qual.efi | Unchanged, `712af82c`: `s2bwrite.c` is neither built nor referenced, so there is nothing new for QEMU to run |
| gaokun3 | NOT RUN; nothing written |

### Step 1j: the USB0 halt check (el2qual 13, entry 15)

Stage 2 (step 1i) requires USB0 **halted**, and reads `S2CR` through the
xHCI operational registers at core + `CAPLENGTH`. Step 1j reads exactly
that state on its own, read only, so the precondition is measured before
any write is ever considered — and so the operational base stage 2 would
use comes from the controller, not from a guess.

**What it reads.** After the controller read (el2qual stage `usb`), which
already reads `XCAP` (`CAPLENGTH`/`HCIVERSION`, core + 0), the halt check
takes `CAPLENGTH` from `XCAP`'s low byte, forms the operational base
(core + `CAPLENGTH`), and reads `USBCMD` (op + 0) and `USBSTS` (op + 4),
each mapping-checked first. USB0 is **HALTED** when `USBCMD.R/S` (bit 0) is
clear and `USBSTS.HCH` (bit 0) is set; otherwise **RUNNING**.

**Why the base is measured, not assumed.** q1n1's A16 path takes the
operational registers at core + `0x20`. The A16's own `CAPLENGTH`, in
q1n1's firmware-running capture, is `0x30`. An operational base off by
`0x10` would read the wrong registers, so step 1j derives it from the
controller's `CAPLENGTH` and reports the base it used.

**Status: read only, on the stick.** Unlike step 1i's model, this is a
real el2qual read (stage `usb-halt`), built into el2qual at version 13, on
the USB-read stick as entry 15 (`nopac nosmp usb-halt`). It reads two more
of USB0's operational registers after the stage-`usb` controller read —
the same controller, behind the same fault fixup, the same risk class as
entry 8. It writes nothing, and does no clock, power or PHY programming.
Reading `USBCMD`/`USBSTS` has no side effect.

The line:

```
USB0 HALT <OP <a> CMD <w> STS <w> <HALTED|RUNNING>
          |NOT READ: NO CAPLENGTH
          |NOT READ: UNMAPPED AT <a>
          |FAULT AT <a> ESR <e>>
```

`NO CAPLENGTH` means the controller was not read (power off, unmapped or a
fault at or before `XCAP`), so there is no `CAPLENGTH` to derive from and
nothing in the controller's range is touched. The line carries the
`USB0 HALT` prefix, not `USB0 XHCI`, so a reader counting controller-read
lines does not mistake a `NO CAPLENGTH` line for a read.

| Check | Result, 2026-10-27 |
| --- | --- |
| Host: `python3 tools/test-el2qual.py` | PASS. Against the fake controller: with the A16's `XCAP` (`CAPLENGTH` `0x30`) the base is `0x0A600030`; `R/S` clear and `HCH` set reads `HALTED`, `R/S` set or `HCH` clear reads `RUNNING`; a `CAPLENGTH` of `0x20` moves the base to `0x0A600020`; an unmapped operational register is `UNMAPPED` and is not read; a faulting `USBCMD` is `FAULT` with its ESR, and `USBSTS` is not read; power off gives `NO CAPLENGTH` with nothing in the controller's range touched |
| QEMU: `python3 tools/qemu-el2qual.py --config max-usb-halt` (and `--self-test`) | PASS. `max-usb-halt`: virt never powers the controller, so the one `USB0 HALT` line says `NO CAPLENGTH` and touches nothing in its range |
| QEMU: `--config max-usb-halt-emulated`, `--config max-usb-halt-running` | PASS. virt has no USB0, and pagemap cannot turn RAM into Device memory, so `emulate-usb-halt` stands a page el2qual owns in for the controller (`CAPLENGTH 0x30`, base `0x0A600030`). The read runs end to end through the real `AT S1E2R` identity check and guarded read — only the Device-attribute requirement relaxed, since virt offers no controllable Device MMIO — and the live line reads `OP 0A600030 CMD 00000000 STS 00000001 HALTED (TEST)`, or `CMD 00000001 STS 00000000 RUNNING (TEST)` for the running stand-in. The `UNMAPPED`/`FAULT` paths stay host-test-only |
| el2qual.efi | Changed: version 13 adds the `usb-halt` option (and the test-only `emulate-usb-halt`). Entry 15 needs el2qual 13 |
| gaokun3 | NOT RUN |

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
  falls. Runs 22 to 24 settled that it is not, and
  [step 1h](#step-1h-the-s2cr-bypass-design-el2qual-12-entry-14) is the
  design for that handling.

## Safety

- **Step 1 writes no hardware.** Only step 1d writes at all: RAM, one
  table page of el2qual's own and one descriptor in UEFI's leftover tables,
  after ExitBootServices, with only the boot CPU running. Its only other
  risk is a read the hardware refuses.
  The mapping check, the fault fixup and the power gate cover the known ways
  that happens. A GCC or SMMU register that the security controller (XPU)
  protects could raise an asynchronous SError the fixup cannot catch.
  el2qual reports SError state, and holding the power button ends any run,
  as before.
- **Step 1e writes nothing, but reads memory the firmware reserved.** Its
  descriptor reads may be refused by the security controller. A refusal
  may come as an SError, which the walk stops on and reports. It may also
  come as a reset or hang the walk cannot report: the screen then ends at
  the walk's progress line. Nothing persistent is touched, so a power
  cycle without the stick returns to Windows, as after every earlier run.
- **Step 1f is the same kind of read,** in the root page run 22 read without
  harm and in one level-2 table, the one run 22 read if the arena lands in
  the same GiB. A hang or reset there leaves its own progress line on the
  screen, `USB: STAGE-2 SURVEY, READ ONLY. A HANG OR RESET HERE IS IT`.
- **Step 1i is not on any stick.** Its model is built only into the host
  tests; el2qual.efi does not contain it. Its first hardware run would be
  the first SMMU write, after two separate go-aheads (wiring behind a
  compile-time gate, then the supervised run). Even its worst outcome, a
  failed restore, leaves only USB0's stream bypassed while USB0 is halted,
  until a power cycle.
- **Step 1j writes nothing.** It reads two of USB0's operational registers
  (`USBCMD`, `USBSTS`) after the stage-`usb` controller read, from the base
  the controller's own `CAPLENGTH` gives. It is on the stick as entry 15
  because it is a read, the same class as entry 8; it is the read that
  measures step 1i's halt precondition and operational base.
- **Step 1h writes nothing, and cannot.** `s2bypass.c` has no write
  callback. Its reads are the SMMU global registers earlier steps already
  read, plus `sGFSR` and the remaining `SMR`s, all in the same page. The
  `AT S1E2W` check it makes asks the MMU a question; it performs no write.
  The staged plan for the write itself, which needs its own go-ahead, is in
  step 1h.
- **Step 1g reads no new kind of memory.** Its walk reads descriptors in
  the root and in run 23's level-2 table for GiB 3, and its memory-map
  report parses a copy el2qual already holds. Nothing at `0xC8000000` is
  read or written.
- **No PD or UCSI request is sent, ever,** in either step. The EC owns
  Type-C.
- **Linux is a reference only.** The GCC offsets and bit meanings come from
  Linux's GPL sources, read into the ignored `build/ref/` and not copied
  into this repository. The device-tree facts come from the local EL2 tree,
  which is not committed.
