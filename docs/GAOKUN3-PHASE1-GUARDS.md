# gaokun3 Phase 1: thermal and battery/AC guards

Status, 2026-10-23: **host-tested policy only.** Both guards are written
and pass their host tests on synthetic data. Neither is linked into any
monitor build, and neither has read a gaokun3 register. The thermal guard
refuses every poll until its version word is measured. Hardware: NOT RUN.

The port target is the Huawei MateBook E Go 2023 (`gaokun3`, SC8280XP).
These guards are the gaokun3 counterparts of the A16's
`compat/a16-thermal.*` and `tools/cycle.py`'s battery refusal (AGENTS.md
rule 5). They are route-independent
([GAOKUN3-PHASE0C.md](GAOKUN3-PHASE0C.md#consequences-if-route-b-is-accepted)).

| Guard | Policy | Host test |
| --- | --- | --- |
| Thermal | `compat/sc8280xp-thermal.{h,c}` | `python3 tools/test-sc8280xp-thermal.py` |
| Battery and AC | `compat/gaokun3-battery.{h,c}` | `python3 tools/test-gaokun3-battery.py` |

Both are MIT and written independently. The Linux drivers named below
(GPL-2.0) were read for register facts only; no code was taken from them.

## Thermal guard

### Sources

| Fact | Source | Class |
| --- | --- | --- |
| Four tsens controllers, `"qcom,sc8280xp-tsens", "qcom,tsens-v2"`; TM/SROT windows and sensor counts (below) | the gaokun3 EL2 DTB (`build/el2qual-usb/stage2/dtb/`, built from upstream `sc8280xp.dtsi`), checked by the host test | OBSERVED |
| tsens-v2 fields: SROT+0 version, SROT+4 control (EN, SW_RST, SENSOR_EN 18:3, CODE_OR_TEMP 21), TM+0xa0+4n status (LAST_TEMP 11:0 signed 0.1 °C, CRITICAL 19, VALID 21), TM+0xe4 TRDY | Linux `drivers/thermal/qcom/tsens-v2.c`; the A16 guard reads the same layout | VERIFIED on the X2 Elite; OBSERVED for SC8280XP |
| tsens version word | not read on a gaokun3 | UNMEASURED |
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
- **UNPINNED.** `SC8280XP_THERMAL_VERSION_PIN` is 0, so every poll trips
  before any MMIO. The A16's word (`0x30020000`) belongs to a different
  revision. To pin it, record a read-only measurement of SROT+0 on all
  four controllers, here, with the firmware version and date. That read
  also records the SoC's tsens revision, which Phase 1 asks for.

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
| Battery voltage range, full-capacity range, gauge consistency | 2-cell pack (gaokun-android power profile); 4483 mAh | INFERRED |

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
that cannot carry the load therefore does not count.

`gaokun3_battery_admit()` is `cycle_lib.common.battery_ok()`:

```
status == 0 and plausible and (ac or percent >= minimum)
```

It also applies `battery_minimum()`'s 50..100 bound on the minimum. The
host test replays every record through the Python functions and requires
the same verdict at every minimum.

### Limitations

- **Weak USB sources.** One snapshot cannot detect BATT-1: a weak source
  the EC reports as charging counts as AC, at any percent. A runtime
  monitor that watches the trend would need to catch it. None exists yet.
- **INFERRED bounds.** The unknown-bit rule and the INFERRED bounds fail
  closed. If a real gaokun3 reading trips one, runs stop until the bound
  is corrected from a recorded reading.
- **Reported only.** The guard records the rate but never uses it. The
  words at `0x98` and `0x9c` are ignored.

## Before hardware use

These are read-only measurements, none authorized yet. Each one replaces
an UNMEASURED or INFERRED entry above:

1. SROT+0 and SROT+4 on all four tsens controllers. These give the
   version pin and the enabled-sensor mask.
2. EC `0x81`, `0x82` and the `0x90` block in four states:
   - on AC, charging;
   - on AC, full;
   - on battery;
   - on a weak USB source.

   These confirm that the unknown bits read zero, the voltage range, the
   gauge agreement and the sign of the rate.
3. The charge level at which `0x82` bit 2 sets (BATT-3).
