# gaokun3 Phase 1: thermal and battery/AC guards

Status, 2026-10-25:
- **Thermal guard: pinned, and run once on the gaokun3.** el2qual's
  [EL2 tsens read](#el2-tsens-read) (qualification run 10) read the
  version word `0x30000000` on all four controllers at EL2. It ran the
  guard on the live sensors for 12 s: PASSED, nothing tripped. That makes
  `0x30000000` the guard's pin. It is still not linked into any monitor
  build.
- **Battery guard: host-tested, its bounds checked against real
  readings.** The [Windows battery readings](#results-2026-10-25)
  (2026-10-25) passed every bound the guard applies. They also
  reproduced BATT-1: a weak USB source reported as charging while the
  battery drained, which the guard would count as AC.
- The Linux survey is [retired](#linux-survey-retired) after a panel
  fault.

Hardware: the thermal guard, one supervised el2qual run (run 10). The
battery guard itself: NOT RUN; its bounds were replayed against Windows'
readings, not run on the EC. Neither guard has run inside a monitor.

The port target is the Huawei MateBook E Go 2023 (`gaokun3`, SC8280XP).
These guards are the gaokun3 counterparts of the A16's
`compat/a16-thermal.*` and `tools/cycle.py`'s battery refusal (AGENTS.md
rule 5). They are route-independent
([GAOKUN3-PHASE0C.md](GAOKUN3-PHASE0C.md#consequences-if-route-b-is-accepted)).

| Guard | Policy | Host test |
| --- | --- | --- |
| Thermal | `compat/sc8280xp-thermal.{h,c}` | `python3 tools/test-sc8280xp-thermal.py` |
| Battery and AC | `compat/gaokun3-battery.{h,c}` | `python3 tools/test-gaokun3-battery.py` |
| [EL2 tsens read](#el2-tsens-read) | `probes/el2qual/thermal.{h,c}` (el2qual option `thermal`) | `python3 tools/test-el2qual.py`; `python3 tools/qemu-el2qual.py --config max-thermal --config max-vhe-thermal` |
| [Windows battery readings](#windows-battery-readings) | `tools/gaokun3-windows-battery.ps1`, `tools/replay-gaokun3-windows-battery.py` | `python3 tools/test-gaokun3-windows-battery.py` |

Both are MIT and written independently. The Linux drivers named below
(GPL-2.0) were read for register facts only; no code was taken from them.

## Thermal guard

### Sources

| Fact | Source | Class |
| --- | --- | --- |
| Four tsens controllers, `"qcom,sc8280xp-tsens", "qcom,tsens-v2"`; TM/SROT windows and sensor counts (below) | the gaokun3 EL2 DTB (`build/el2qual-usb/stage2/dtb/`, built from upstream `sc8280xp.dtsi`), checked by the host test | OBSERVED |
| tsens-v2 fields: SROT+0 version, SROT+4 control (EN, SW_RST, SENSOR_EN 18:3, CODE_OR_TEMP 21), TM+0xa0+4n status (LAST_TEMP 11:0 signed 0.1 °C, CRITICAL 19, VALID 21), TM+0xe4 TRDY | Linux `drivers/thermal/qcom/tsens-v2.c`; the A16 guard reads the same layout | VERIFIED on the X2 Elite; OBSERVED for SC8280XP |
| tsens version word | `0x30000000` (3.0.0) on all four, read at EL2 by the guard itself, firmware 2.18 ([run 10](#run-10-2026-10-23)); Linux's debugfs reported the same | VERIFIED as read |
| Control words and enable masks | `0x9D61FFF9`, `0x9D67FFF9`, `0x9D603FF9`, `0x9D6000F9`: EN set, SW_RST clear, CODE_OR_TEMP set, and each SENSOR_EN mask exactly the DT's sensors (14, 16, 11, 5) ([run 10](#run-10-2026-10-23)) | VERIFIED as read |
| Thermal zones and trips (below) | the same DTB | OBSERVED |

| Controller | TM | SROT | Sensors |
| --- | --- | --- | --- |
| tsens0 | `0x0c263000` | `0x0c222000` | 14 |
| tsens1 | `0x0c265000` | `0x0c223000` | 16 |
| tsens2 | `0x0c251000` | `0x0c224000` | 11 |
| tsens3 | `0x0c252000` | `0x0c225000` | 5 |

The guard may read 58 words: per controller the version, control and TRDY
words, plus one status word per declared sensor (46 in all). The host test
checks that no other `/soc@0` node covers any of them. `restart@c264000`
(PS_HOLD) sits between tsens0 and tsens1, and the allowlist excludes it.

### Policy

The policy is the A16 guard's: the same reasons, grace periods and
admission rule.

- Polls every quarter second. The guard trips (one stop callback, then no
  further MMIO) on:
  - HOT: any sensor at 95 °C or more;
  - CRITICAL: a sensor's critical flag;
  - RANGE: a reading outside −40..120 °C;
  - INVALID: a sensor without a valid reading for a second;
  - LATE: a one-second gap between polls;
  - SLOW: a poll taking a quarter second;
  - UNCHANGED: ten seconds without any change;
  - CONFIG: a controller that is disabled, in reset, in ADC-code mode, or
    has a declared sensor not enabled, or a version word other than the
    pin;
  - READ: a failed read.
- Admission needs one complete 46-sensor sample within 20 attempts or
  half a second.
- **Pin.** `SC8280XP_THERMAL_VERSION_PIN` is `0x30000000`: tsens 3.0.0,
  the SoC's tsens revision, as [run 10](#run-10-2026-10-23) read it on
  firmware 2.18. The A16's word (`0x30020000`) is a different revision.
  A controller reporting anything else trips CONFIG before any sensor is
  read. A different firmware or board needs its own read first. A pin of
  0 would refuse every poll (UNPINNED).
- **Enable masks.** Run 10 read each mask equal to the DT's sensors. The
  check stays a superset: a missing sensor trips CONFIG; an extra one
  does not.

### Limit and zones

95 °C carries over the A16 rule. It is below every tsens zone's 110 °C
critical trip, and the host test enforces that. It is above two lower
trips that an OS would act on and nothing under the monitor does:

- `gpu-thermal` (tsens2 sensor 2), 85 °C passive;
- `mem-thermal` (tsens1 sensor 15), 90 °C hot.

The owner may lower the limit for this fanless tablet.

### Not covered

- `skin-temp-thermal`: the PMK8280 ADC-TM, with a 73 °C critical trip.
- The PMIC temperature alarms: 95 °C passive, 115 °C critical.
- The DVFS clamp from 75 °C.
- The transport that would run the guard on the gaokun3 (Phase 2).

## Battery and AC guard

### Sources

| Fact | Source | Class |
| --- | --- | --- |
| EC `huawei,gaokun3-ec` at I2C address 0x38 on `i2c15` | the same DTB | OBSERVED |
| `0x81`: bit 0 AC, bit 1 battery present. `0x82`: bit 0 discharging, bit 1 charging, bit 2 critical, bit 3 full. `0x90`: seven LE16 words: percent, mV, mAh now, mAh full, unknown, mA rate, unknown | Linux `drivers/power/supply/huawei-gaokun-battery.c` | OBSERVED in the driver |
| `0x82` reads `0x02` while charging at 92 % on AC; full charge 4483 mAh | gaokun-android `patches/0072-…` (`ec_raw`) and its healthd record | OBSERVED |
| `0x00` in `0x82` on AC means "not charging"; weak USB sources report charging while the battery drains (BATT-1); the charge level that sets the critical bit is unmeasured (BATT-3); the sign of the rate is unconfirmed | gaokun-android `patches/0072-…` | OBSERVED / UNMEASURED |
| On AC and `Full` at 98 %: 8475 mV, 0 mA, 5228 of 5363 mAh; design 5900 mAh at 7640 mV | Linux's battery driver, one truncated run ([retired survey](#linux-survey-retired)) | OBSERVED |
| Battery voltage range, full-capacity range, gauge consistency | 2-cell pack (gaokun-android power profile); 4483 mAh | INFERRED |
| Under Windows, 88–95 %: 8210..8517 mV; full charge 40920 mWh (4805..4984 mAh at the measured voltage), 33 cycles; percent within 1 point of remaining/full; held at 95 % on AC with neither flag set; a weak USB source reported as charging while the battery drained (BATT-1) | the [Windows battery readings](#results-2026-10-25), firmware 2.18 | OBSERVED, through Huawei's Windows driver |

### Policy

The guard does no I/O. The caller passes the three EC reads (`0x81`,
`0x82`, `0x90..0x9d`) with the time they began.

`gaokun3_battery_decode()` refuses, in this order, when:

- READ: a read did not complete;
- STALE: the reads are over 4 s old, or timestamped after now;
- BITS: either byte has a bit the guard does not know (fail-closed);
- ABSENT: no battery is reported;
- CONTRADICTION: the EC reports charging and discharging together, or
  charging without AC;
- CRITICAL: the critical bit is set, on AC or not;
- RANGE: the percent is above 100, the voltage is outside 5000..9000 mV,
  or the full capacity is outside 1000..10000 mAh;
- INCONSISTENT: the charge is above full capacity, or the percent differs
  from charge/full by more than 10 points.

AC means the adapter bit is set and the EC is not discharging. An adapter
the EC reports as discharging therefore does not count. A weak source it
reports as charging still does; see Limitations.

`gaokun3_battery_admit()` is `cycle_lib.common.battery_ok()`:

```
status == 0 and plausible and (ac or percent >= minimum)
```

It also applies `battery_minimum()`'s 50..100 bound on the minimum. The
host test replays every record through the Python functions and requires
the same verdict at every minimum.

### Limitations

- **Weak USB sources.** One snapshot cannot detect BATT-1: a weak source
  the EC reports as charging counts as AC, at any percent. The AC rule
  above (adapter set, not discharging) does not catch it either: on a
  Mac's USB-C port under load, Windows reported charging, never
  discharging, for 10 minutes while the battery lost about 5.8 W
  ([results](#results-2026-10-25)). A runtime monitor that watches the
  remaining charge fall would need to catch it. None exists yet.
- **INFERRED bounds.** The unknown-bit rule and the INFERRED bounds fail
  closed. If a real gaokun3 reading trips one, runs stop until the bound
  is corrected from a recorded reading.
- **Reported only.** The guard records the rate but never uses it. The
  words at `0x98` and `0x9c` are ignored.

## Before hardware use

Each of these replaces an UNMEASURED or INFERRED entry above:

1. **SROT+0 on all four tsens controllers,** which gives the version pin.
   Done: [run 10](#run-10-2026-10-23) read it at EL2 and ran the guard on
   live sensors for 12 s.
2. **The battery in four states:**
   - on AC, charging;
   - on AC, full;
   - on battery;
   - on a weak USB source.

   Done through Windows on 2026-10-25
   ([results](#results-2026-10-25)): every reading is inside the
   guard's voltage, capacity and gauge bounds, and BATT-1 is reproduced.
   "On AC, full" is the tablet holding the battery at 95 %. These come
   through Windows' driver, so the EC's own bytes, including `0x81`'s
   upper bits and the `0x82` decode, stay UNMEASURED.
3. **The charge level at which `0x82` bit 2 sets** (BATT-3). This needs a
   discharge far below 50 %, which neither collector does.

## EL2 tsens read

An el2qual stage, behind the `thermal` load option, off by default. It
runs `compat/sc8280xp-thermal.c` unchanged, at EL2 on the boot CPU, after
ExitBootServices and the CPU measurements. That is the level and the
firmware state the guard will run in. The stage reads no display, panel
or touch hardware; el2qual still only writes pixels into the framebuffer
the firmware shows.

### What it does

1. **Mapping check, before any read.** For each of the guard's 58 words,
   `AT S1E2R` asks the firmware's page tables how a read would translate.
   A word counts as mapped only as Device memory at its own physical
   address. If any word fails, nothing is read and the result is NOT
   MAPPED.
2. **The guard's own admission and polls.** The candidate pin is
   `0x30000000`, the value Linux reported, set in el2qual
   (`EL2Q_TSENS_PIN`). Run 10 confirmed it, so it is now the guard's
   production pin too; `thermal.c` asserts the two agree. It then polls
   at its quarter-second rate for 12 s, past its 10 s "unchanged" rule.
3. **Faults are caught, not fatal.** Each read runs behind el2qual's
   fault fixup. A synchronous abort becomes a READ trip at that address,
   and the guard reads nothing after it. Afterwards `ISR_EL1` is checked
   for an SError left pending.
4. **Breadcrumb.** One line, `TSENS: 58 WORDS, READ ONLY, 12 S`, appears
   before the first read. A hang leaves it as the last line on screen.

Only the guard's allowlisted words are read, and nothing is written to
tsens.

### Outcomes

| Screen (`TSENS GUARD (READ ONLY) ..`) | Meaning | Next |
| --- | --- | --- |
| PASSED, NOTHING TRIPPED | 46/46 sensors valid, every poll on time, the version on the pin | pin `0x30000000` if all four rows show it |
| TRIPPED: CONFIG at an SROT word | a version or control word the guard refuses; the table shows the real words | record the words; no pin |
| TRIPPED: READ, with `READ FAULTED ... ESR` | a read aborted and was caught | record the address and ESR; stop |
| TRIPPED: UNCHANGED, INVALID, SLOW, LATE | the guard's timing or validity rules refused live data | record; the rule or the hardware needs a look |
| TRIPPED: HOT or CRITICAL | a sensor at 95 °C or its critical flag | power off and let it cool |
| NOT MAPPED, NOTHING READ | the firmware's tables do not map a word as device memory | record; the stage cannot run as is |
| NO COMPLETE SAMPLE | a sensor never read valid during admission | record which (VALID column) |
| `SERROR PENDING` line | a read raised an asynchronous error | stop; report |

### Evidence so far

- **Host.** `tools/test-el2qual.c` runs the stage against a synthetic
  SC8280XP. It covers every outcome above, plus:
  - one unmapped word;
  - a stopped counter;
  - a missing operation;
  - a report the CPU tables overflow.

  It checks that no read happens before all 58 mapping checks. PASS.
- **QEMU.** The `max-thermal` and `max-vhe-thermal` configurations run
  the stage on QEMU's virt machine, which has no tsens. EDK2 maps the
  region as device memory, and the first read raised a synchronous
  external abort (ESR `96000010`). The fixup caught it, the guard tripped
  READ at `0x0C222000` and read nothing more. The full CPU report
  followed. PASS, 2026-10-23.
- **gaokun3.** PASS, run 10 (below).

### Run 10, 2026-10-23

The owner booted the `thermal` stick's only entry, `el2qual 5: tsens read
only` (`nopac nosmp thermal`), on the MateBook E Go 2023, and photographed
the report.
- **Build.** `el2qual.efi` SHA-256 `5c28c4eedcd3a84f…` (el2qual 3).
- **Platform.**
  - Firmware 2.18, 06/30/2023; product `GK-W7X`.
  - `START EL1  AFTER EBS EL2`, `ENTRY HCR_EL2 0000000488000000`, the
    same state as run 9.
  - The report was drawn on the DT simple-framebuffer at `0xC6200000`.

The stage's report, as photographed:

```
TSENS GUARD (READ ONLY) . PASSED, NOTHING TRIPPED
  PIN 30000000  VALID 46/46  PEAK 39.4 C  POLLS 49
  MAX POLL 0.0 MS  MAX GAP 250.0 MS  READS 2842
  CTRL VERSION  CONTROL   TRDY      VALID
  0    30000000  9D61FFF9  00000008  14
  1    30000000  9D67FFF9  00000008  16
  2    30000000  9D603FF9  00000008  11
  3    30000000  9D6000F9  00000009  5
```

What it shows (VERIFIED as read, this machine and firmware):
- **Mapping.** All 58 words are mapped as Device memory at their own
  addresses in the firmware's EL2 tables after slbounce. The stage
  checks that before it reads anything.
- **Version and control.** All four controllers report version
  `0x30000000`. Their control words have EN set, SW_RST clear and
  CODE_OR_TEMP set. Each SENSOR_EN mask is exactly the DT's sensors.
  Bits 31:22 read the same on all four; the guard does not interpret
  them.
- **Sensors and timing.**
  - Every sensor read valid on every poll.
  - The hottest was 39.4 °C.
  - 49 polls, one in admission and 48 in the window, read 2842 words
    (49 × 58).
  - The polls ran a quarter second apart. Each took under 0.05 ms.
  - The 10 s "unchanged" rule did not trip, so the live sensors changed
    within it.
- **Faults.** No read faulted, and no SError was pending.
- **TRDY** reads `0x8` on tsens0 to tsens2 and `0x9` on tsens3. The guard
  records TRDY but does not use it, as on the A16. Its bits are not
  interpreted.

What it does not show: a monitor running the guard alongside a guest, or
over more than 12 s. Under load, the 95 °C limit and the HOT path remain
host-tested only.

### Running it

The stick is the run 9 layout with one entry:

```
python3 tools/el2qual-usb.py thermal --systemd-boot F --slbounce F --tcblaunch F --dtb F
```

Its only entry is `el2qual 5: tsens read only`, with options `nopac
nosmp thermal`. The preparations, recovery and Secure Boot steps are the
el2qual ones
([GAOKUN3-QUALIFICATION.md](GAOKUN3-QUALIFICATION.md#prerequisites)).
Photograph the TSENS section.

## Windows battery readings

`tools/gaokun3-windows-battery.ps1` samples Windows' battery data, read
only, with no reboot. Run it from an administrator PowerShell, once per
state:

```
powershell -ExecutionPolicy Bypass -File .\gaokun3-windows-battery.ps1 -State ac-charging
powershell -ExecutionPolicy Bypass -File .\gaokun3-windows-battery.ps1 -State ac-full
powershell -ExecutionPolicy Bypass -File .\gaokun3-windows-battery.ps1 -State battery -Samples 60
powershell -ExecutionPolicy Bypass -File .\gaokun3-windows-battery.ps1 -State weak-usb -Samples 120
```

- A run takes 24 samples by default, 5 s apart.
- For `weak-usb`, a computer's USB-C port is the only source, and a load
  runs alongside: the BATT-1 procedure.
- `battery` and `weak-usb` refuse to start below 55 % and stop at 50 %.
  Every state refuses an adapter that does not match it.
- Each run writes `gaokun3-windows-battery-<state>-<time>.txt` and never
  overwrites a file.

**What it reads.** All of it comes through `Get-CimInstance`; it calls no
CIM method.
- From `root\wmi`:
  - `BatteryStatus`: the AC, charging, discharging and critical flags,
    mV, mW and mWh;
  - `BatteryFullChargedCapacity`;
  - `BatteryStaticData`: the designed capacity and chemistry;
  - `BatteryCycleCount`.
- `Win32_Battery`'s percent.
- The maker, model and firmware version, and the Windows version.

It records no serial numbers, unique IDs, device paths, computer or user
names, or network details.

**Replay.** Copy the files under `build/` and run:

```
python3 tools/replay-gaokun3-windows-battery.py build/phase1/windows/*.txt
```

The replay checks every reading against the guard's own bounds, read from
`compat/gaokun3-battery.c`:
- voltage;
- full capacity, converted to mAh at the measured voltage;
- gauge agreement;
- flag consistency.

A FAIL is a real reading the guard would refuse. That bound then needs
correcting before the guard runs on hardware.

**Limits.**
- This is Windows' view through Huawei's driver, not the EC bytes. It
  cannot test the guard's byte decode or the unknown-bit rule.
- The collector cannot run on the Mac, which has no PowerShell. Its host
  test lints what it may call and write, and tests the replay. It has run
  on the gaokun3 (below).

### Results, 2026-10-25

The owner ran the collector on the MateBook E Go 2023 (firmware 2.18,
06/30/2023; Windows build 26200) and copied the five files to
`build/phase1/windows/` (not committed). The first attempt stopped
before writing anything: under StrictMode, Windows PowerShell threw on
`.Count` for the single battery. The collector was fixed to return arrays
before these runs.

| File (state, time) | Samples | Adapter, flags | Percent | Net change | Rate Windows reported |
| --- | --- | --- | --- | --- | --- |
| `ac-charging`, 21:21 | 24 | online, neither flag | 95 → 95 | 0 mW | 0 |
| `ac-charging`, 21:33 | 24 | online, neither flag | 95 → 95 | 0 mW | 0 |
| `battery`, 21:44 | 60 | offline, discharging | 93 → 92 | −6.2 W | discharge 6.5 W (median) |
| `weak-usb`, 21:51 | 120 | online, **charging** | 91 → 89 | **−5.8 W** | **charge 6.1 W** (median) |
| `ac-charging`, 22:04 | 24 | online, charging | 88 → 89 | +14.6 W | charge 16.9 W (median) |

The net change is the remaining capacity's slope from first to last
sample. Remaining-capacity resolution is coarse (it often repeats
between samples), so the two-minute runs give rough figures.

The replay (`python3 tools/replay-gaokun3-windows-battery.py
build/phase1/windows/*.txt`):

- **PASS** format: all five complete.
- **PASS** voltage: 8210..8517 mV, against 5000..9000.
- **PASS** capacity: 4805..4984 mAh (40920 mWh at the measured
  voltage), against 1000..10000.
- **PASS** gauge: the percent within 1 point of remaining/full; the
  guard allows 10.
- **PASS** flags: never charging and discharging together, never
  charging without the adapter.

What they show (OBSERVED, through Windows' driver):

- **Held on AC.** The tablet stopped charging at 95 % with the adapter
  in, and reported neither charging nor discharging. The two runs at
  21:21 and 21:33 are labelled `ac-charging` but are this held state;
  there is no separate `ac-full` file. That matches the EC's `0x00`
  "not charging" on AC (gaokun-android). The guard counts it as AC,
  correctly.
- **BATT-1 reproduced.** Powered from a Mac's USB-C port while playing
  4K video at full brightness, Windows reported the adapter online and
  the battery charging in all 120 samples, and never discharging. Over
  the same 10 minutes the remaining capacity fell by 978 mWh, about
  −5.8 W, and the percent fell from 91 to 89. The guard's AC rule would
  admit this source at any percent.
- **The rate's sign.** In the weak-USB run Windows reported a charge
  rate of about 6 W while the battery lost about 5.8 W. So the rate Windows
  reports does not give the direction of current on its own; only the
  flags do. Whether the EC's `0x9a` rate word is likewise unsigned is
  INFERRED from this, not read.
- **Capacity.** Windows reports a full charge of 40920 mWh after 33
  cycles and no design capacity. The retired Linux run reported
  5363 mAh full and a 5900 mAh design. The two sources disagree by about
  10 %; both are inside the guard's bound.

Not covered: the EC's own bytes, the critical bit's charge level
(BATT-3), and discharge below 88 %.

## Linux survey (retired)

> **RETIRED, 2026-10-23 (owner's decision). Do not boot a Linux image on
> the gaokun3.** After two boots of one, the right half of the panel
> showed a striped band that stayed in the firmware logo and in Windows.
> A 15-minute power-off cleared it. See
> [Incident](#incident-2026-10-23).

The survey booted a community Ubuntu image from a USB drive and read
values Linux drivers had already read:
- tsens debugfs `version`, which `tsens.c` prints from SROT+0's major,
  minor and step fields;
- the battery driver's sysfs attributes, ×1000 units.

The image was the last published Ubuntu image for this device:
- release `ubuntu26.04-7.1.0-rc3-gaokun3+-el2-20260514004329` of the
  archived `KawaiiHachimi/linux-gaokun-buildbot`;
- SHA-256 `c3c151e6…c7105e59`;
- kernel `7.1.0-rc3-gaokun3+`.

Its EL2 drivers and entry were disabled on the stick. Its tools were
never committed and were deleted on 2026-10-23, together with the image,
the stick copy and the extracted logs. Its one partial run is the source
of the OBSERVED Linux values above; this record is all that remains of
it.

### Incident, 2026-10-23

What happened, in order (all OBSERVED by the owner):

1. First boot of the standard entry: the desktop never started. The
   console filled with failing services, and the right half of the panel
   showed a striped band. No text console answered, so the tablet was
   held off.
2. Second boot, a headless survey entry (AC): the band was there again.
3. Power cycle with the drive removed: the band stayed on the right half
   in the Huawei firmware logo and in Windows.
4. Shut down and left off for 15 minutes: the panel came back normal.

What is known:

- gaokun_build's `docs/display-debug.md` (2026-09-22) reports
  half-screen corruption 1–2 s into a Linux boot on this device. There
  it was a Linux-only fault, and it does not mention persistence.
- The panel is driven over two DSI links, one per half. Its display and
  touch controller is one Himax HX83121A chip.
- The image's touch driver (`himax-spi-core.c` at buildbot `11e3514`)
  only reads the chip's flash, for a CRC check. It writes chip RAM
  registers. A review of it and of the panel driver found no flash erase
  or program path. That was a source review, not a proof.
- The bad state outlasted a short power cycle but not a long one: charge
  held somewhere in the panel or its controller, not a permanent change.
  Which part held it, and whether Linux caused it or only showed it
  first, is not known.

What the stick recorded. The stick was read once into a local copy, and
its journal parsed without `journalctl`. The copy and the logs were
deleted afterwards (they held the tablet's Wi-Fi and Bluetooth
addresses), so the findings below cannot be re-read:

- **Boot 1.** The journal keeps only the first ~10 s of kernel time; the
  hard power-off lost its last 192 entries.
  - The display driver (`msm`) bound both DSI links at 0.7 s and logged
    no error.
  - Nothing came from the panel driver, and nothing from DSI or the touch
    chip.
- **Boot 2.**
  - The display driver was the same, with no error.
  - The touch driver detected the HX83121A, entered safe mode, and the
    chip reloaded its own firmware from flash (`FW reload done`). It
    logged no CRC failure.
  - systemd restored the backlight on `ae96000.dsi.0`.
- **Errors in both boots,** none of them display:
  - the missing GPU firmware `a660_sqe.fw`, so the GPU never started;
  - a sound module that failed to load;
  - correctable PCIe errors on the Wi-Fi link;
  - one USB port re-enable.
- **Temperature.** The hottest thermal zone during boot 2 was 42 °C.
- **Limit of this evidence.** These are the default log levels, and the
  panel driver logs little at them. That no error appears does not clear
  the display path.

Boot 2's survey took 17 of its 24 samples before the power-off cut it
short. Its readings are the OBSERVED Linux values in the tables above.

The owner's decision, 2026-10-23: the Ubuntu route is retired, and no
Linux image is booted on the gaokun3 again. Further hardware readings
come from UEFI applications like el2qual, and from Windows. el2qual ran
nine times before this incident without disturbing the panel.
