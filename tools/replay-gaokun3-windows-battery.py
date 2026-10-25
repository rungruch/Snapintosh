#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check gaokun3 Windows battery readings against the Phase 1 battery guard. No hardware.

  python3 tools/replay-gaokun3-windows-battery.py FILE... [--json OUT]

FILE comes from tools/gaokun3-windows-battery.ps1, one per battery state
(docs/GAOKUN3-PHASE1-GUARDS.md#windows-battery-readings). The readings are
Windows' view through Huawei's battery driver, not the EC's bytes, so they
test the guard's INFERRED bounds and its AC rule, not its byte decode.
Each check prints PASS, FAIL or SKIP:

- format: complete, and only the records and keys the collector writes.
- voltage: every reading inside the guard's VOLTAGE_MIN_MV..VOLTAGE_MAX_MV.
- capacity: the full charge, converted from mWh at the measured voltage,
  inside FULL_MIN_MAH..FULL_MAX_MAH.
- gauge: the percent within PERCENT_TOLERANCE of remaining/full.
- flags: never charging and discharging together, never charging without
  the adapter. The guard refuses both (CONTRADICTION).

The bounds are read from compat/gaokun3-battery.c, so the two cannot drift.
A FAIL means a real reading the guard would refuse: the bound needs a
correction from this evidence before the guard runs on hardware. Findings,
including the AC rule on a weak source, print as INFO lines.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / 'compat' / 'gaokun3-battery.c'
COLLECTOR = ROOT / 'tools' / 'gaokun3-windows-battery.ps1'
FORMAT = 'snapintosh-gaokun3-windows-battery format=1'
STATES = {'ac-charging': 1, 'ac-full': 1, 'battery': 0, 'weak-usb': 1}
ENDS = {'COMPLETE', 'INTERRUPTED', 'STOPPED_FLOOR', 'STOPPED_UNREADABLE'}
ONCE = ('run', 'windows', 'dmi', 'battery_design', 'end')
KEYS = {
    'run': {'state', 'samples', 'interval'},
    'windows': {'version', 'build', 'arch'},
    'dmi': {'vendor', 'product', 'bios_version', 'bios_date'},
    'battery_design': {'designed_mwh', 'full_mwh', 'cycles', 'chemistry'},
    'sample': {'n', 'elapsed_s', 'online', 'charging', 'discharging', 'critical', 'voltage_mv',
               'remaining_mwh', 'charge_mw', 'discharge_mw', 'percent'},
    'end': {'status', 'samples'},
}
VALUE = re.compile(r'[A-Za-z0-9._,:/+=@-]+')    # the collector's Clean() set


def bounds():
    text = GUARD.read_text()
    get = lambda name: int(re.search(rf'#define {name} (\d+)u', text)[1])
    return dict(voltage=(get('VOLTAGE_MIN_MV'), get('VOLTAGE_MAX_MV')),
                full=(get('FULL_MIN_MAH'), get('FULL_MAX_MAH')), tolerance=get('PERCENT_TOLERANCE'))


def integer(value):
    return int(value) if re.fullmatch(r'-?[0-9]+', value or '') else None


def parse(text, name='readings'):
    """(record, problems) for one file. Any problem refuses the file."""
    lines = text.splitlines()
    if not lines or lines[0] != FORMAT:
        return None, [f'{name}: first line is not "{FORMAT}"']
    rec = {k: None for k in ONCE}
    rec.update(name=name, samples=[])
    problems = []
    for i, line in enumerate(lines[1:], 2):
        kind, _, rest = line.partition(' ')
        if kind not in KEYS:
            problems.append(f'{name}:{i}: unknown record "{kind}"')
            continue
        fields = {}
        for item in rest.split(' ') if rest else []:
            key, eq, value = item.partition('=')
            if not eq or not VALUE.fullmatch(value):
                problems.append(f'{name}:{i}: malformed field "{item[:40]}"')
            elif key in fields:
                problems.append(f'{name}:{i}: duplicate key "{key}"')
            else:
                fields[key] = value
        if set(fields) != KEYS[kind]:
            problems.append(f'{name}:{i}: {kind} keys differ (extra {sorted(set(fields) - KEYS[kind])}, '
                            f'missing {sorted(KEYS[kind] - set(fields))})')
            continue
        if rec['end'] is not None:
            problems.append(f'{name}:{i}: record after the end line')
        if kind == 'sample':
            rec['samples'].append(fields)
        elif rec[kind] is not None:
            problems.append(f'{name}:{i}: second {kind} record')
        else:
            rec[kind] = fields
    for kind in ONCE:
        if rec[kind] is None:
            problems.append(f'{name}: no {kind} record' + (' (the run did not finish)' if kind == 'end' else ''))
    if problems:
        return None, problems
    if rec['run']['state'] not in STATES:
        problems.append(f'{name}: unknown state {rec["run"]["state"]}')
    if rec['end']['status'] not in ENDS:
        problems.append(f'{name}: unknown end status {rec["end"]["status"]}')
    numbers = [integer(s['n']) for s in rec['samples']]
    if numbers != list(range(1, len(numbers) + 1)):
        problems.append(f'{name}: samples are not numbered 1..{len(numbers)}')
    if integer(rec['end']['samples']) != len(numbers):
        problems.append(f'{name}: end says {rec["end"]["samples"]} samples, file has {len(numbers)}')
    for s in rec['samples']:
        for flag in ('online', 'charging', 'discharging', 'critical'):
            if s[flag] not in ('0', '1', 'ABSENT'):
                problems.append(f'{name}: sample {s["n"]} {flag}={s[flag]} is not 0, 1 or ABSENT')
    return (None if problems else rec), problems


def analyse(recs, limits):
    """[(check, verdict, detail)] and INFO findings."""
    lo_v, hi_v = limits['voltage']
    lo_f, hi_f = limits['full']
    volts, fulls, gauges, flags, info = [], [], [], [], []
    for rec in recs:
        state, name = rec['run']['state'], rec['name']
        full_mwh = integer(rec['battery_design']['full_mwh'])
        rows = []
        for s in rec['samples']:
            v, rem, pct = integer(s['voltage_mv']), integer(s['remaining_mwh']), integer(s['percent'])
            on, chg, dis, crit = (s[k] == '1' for k in ('online', 'charging', 'discharging', 'critical'))
            rows.append(dict(n=int(s['n']), v=v, rem=rem, pct=pct, on=on, chg=chg, dis=dis, crit=crit,
                             charge=integer(s['charge_mw']), discharge=integer(s['discharge_mw'])))
            where = f'{name}#{s["n"]}'
            if v is not None:
                volts.append((v, where))
                if full_mwh:
                    fulls.append((round(full_mwh * 1000 / v), where))
            if rem is not None and pct is not None and full_mwh:
                gauges.append((abs(pct - round(rem * 100 / full_mwh)), where))
            if chg and dis:
                flags.append(f'{where} charging and discharging')
            if chg and not on:
                flags.append(f'{where} charging without the adapter')
        good = [r for r in rows if r['v'] is not None]
        if not good:
            info.append(f'{state}: no readable samples')
            continue
        pcts = [r['pct'] for r in rows if r['pct'] is not None]
        mislabel = sum(1 for r in rows if r['on'] != bool(STATES[state]))
        info.append(f'{state}: {len(rows)} samples, {rec["end"]["status"]}, percent '
                    f'{pcts[0] if pcts else "?"} -> {pcts[-1] if pcts else "?"}, voltage '
                    f'{min(r["v"] for r in good)}..{max(r["v"] for r in good)} mV, '
                    f'{mislabel} with the adapter not as the state says')
        on_dis = sum(1 for r in rows if r['on'] and r['dis'])
        on_chg = sum(1 for r in rows if r['on'] and r['chg'])
        crit = [r['pct'] for r in rows if r['crit']]
        info.append(f'{state}: adapter online in {sum(r["on"] for r in rows)}, charging in {on_chg}, '
                    f'online and discharging in {on_dis}' +
                    (f'; critical flag at {sorted(set(crit))} %' if crit else ''))
        if state == 'weak-usb':
            falling = len(pcts) > 1 and pcts[-1] < pcts[0]
            if on_dis:
                info.append('weak-usb: Windows reported discharging with the adapter online: the guard '
                            'counts that as no AC (adapter set, discharging set)')
            elif on_chg and falling:
                info.append('weak-usb: charging reported while the percent fell (BATT-1): the guard '
                            'would count it as AC')
        d = rec['battery_design']
        info.append(f'{state}: design {d["designed_mwh"]} mWh, full {d["full_mwh"]} mWh, '
                    f'{d["cycles"]} cycles, chemistry {d["chemistry"]}')
    checks = []
    def bound(name, values, lo, hi, unit):
        if not values:
            checks.append((name, 'SKIP', 'no readings'))
            return
        out = [f'{v} {unit} at {w}' for v, w in values if not lo <= v <= hi]
        span = f'{min(v for v, _ in values)}..{max(v for v, _ in values)} {unit}'
        checks.append((name, 'FAIL' if out else 'PASS',
                       f'{span} against the guard\'s {lo}..{hi}' + ('; outside: ' + ', '.join(out[:5]) if out else '')))
    bound('voltage', volts, lo_v, hi_v, 'mV')
    bound('capacity', fulls, lo_f, hi_f, 'mAh (full charge at the measured voltage)')
    if gauges:
        worst = max(gauges)
        checks.append(('gauge', 'FAIL' if worst[0] > limits['tolerance'] else 'PASS',
                       f'percent within {worst[0]} points of remaining/full (worst {worst[1]}); the '
                       f'guard allows {limits["tolerance"]}'))
    else:
        checks.append(('gauge', 'SKIP', 'no reading had both percent and remaining capacity'))
    checks.append(('flags', 'FAIL' if flags else 'PASS',
                   '; '.join(flags[:5]) or 'never charging and discharging together, never charging '
                                         'without the adapter'))
    return checks, info


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('files', nargs='+', type=Path)
    ap.add_argument('--json', type=Path, help='write the checks and findings here (not overwritten)')
    a = ap.parse_args()
    if a.json and a.json.exists():
        ap.error(f'{a.json} exists; records are not overwritten')
    recs, failed = [], False
    for path in a.files:
        rec, problems = parse(path.read_text(encoding='utf-8-sig', errors='replace'), path.name)
        for p in problems:
            print(f'FAIL format: {p}')
        if rec:
            print(f'PASS format: {path.name} ({rec["run"]["state"]}, {len(rec["samples"])} samples, '
                  f'{rec["end"]["status"]})')
            recs.append(rec)
        failed |= bool(problems)
    if not recs:
        return 1
    checks, info = analyse(recs, bounds())
    for name, verdict, detail in checks:
        print(f'{verdict} {name}: {detail}')
        failed |= verdict == 'FAIL'
    for line in info:
        print(f'INFO {line}')
    if a.json:
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        a.json.write_text(json.dumps(dict(
            scope='Windows battery readings through Huawei\'s driver; not EC bytes, not EL2 evidence',
            files={p.name: sha(p) for p in a.files},
            firmware=sorted({r['dmi']['bios_version'] for r in recs}),
            checks=[dict(check=c, verdict=v, detail=d) for c, v, d in checks], findings=info,
            source_sha256={str(p.relative_to(ROOT)): sha(p) for p in (GUARD, COLLECTOR, Path(__file__))}),
            indent=2) + '\n')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
