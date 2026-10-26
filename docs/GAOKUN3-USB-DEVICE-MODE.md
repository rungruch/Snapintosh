# gaokun3 USB device mode: study

Status, 2026-10-26: **study only.** Nothing here has been built or run.
It sets what q1n1's A16 USB device path assumes against what the gaokun3
provides, and proposes a staged plan whose first hardware step only reads.
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

### Step 1: read only (el2qual stage `usb`, not yet built)

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
