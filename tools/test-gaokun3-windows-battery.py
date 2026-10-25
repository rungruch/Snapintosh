#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for the gaokun3 Windows battery collector and its reader. No hardware.

  python3 tools/test-gaokun3-windows-battery.py

1. tools/gaokun3-windows-battery.ps1 is not run here: this Mac has no
   PowerShell, and the classes it reads exist only on Windows. A lint
   instead checks what the script can do:
   - it uses Get-CimInstance only, never a CIM method or a Set-, New- or
     Remove- cmdlet on anything but its own output file;
   - it never names an identifying property;
   - every key it writes is one the reader accepts.
2. tools/replay-gaokun3-windows-battery.py must accept well-formed
   readings. It must refuse malformed or truncated files, and FAIL
   readings the battery guard would refuse.
"""
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PS1 = ROOT / 'tools' / 'gaokun3-windows-battery.ps1'
spec = importlib.util.spec_from_file_location('wb', ROOT / 'tools' / 'replay-gaokun3-windows-battery.py')
wb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wb)
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print(f'  FAIL {what}')


def lint():
    print('collector lint')
    text = PS1.read_text()
    code = '\n'.join(line.split('#', 1)[0] for line in text.splitlines())
    cmdlets = set(re.findall(r'\b([A-Z][a-z]+-[A-Z][A-Za-z]+)\b', code))
    allowed = {'Get-CimInstance', 'Get-Location', 'Get-Date', 'Join-Path', 'Test-Path',
               'New-Object', 'Start-Sleep', 'Set-StrictMode'}
    check(cmdlets <= allowed, f'cmdlets beyond the read-only set: {sorted(cmdlets - allowed)}')
    check('Invoke-CimMethod' not in code and 'InvokeMethod' not in code, 'a CIM method call')
    # New-Object only for the output writer and its encoding.
    for m in re.findall(r'New-Object\s+([\w.]+)', code):
        check(m in ('System.IO.StreamWriter', 'System.Text.UTF8Encoding'), f'New-Object {m}')
    for name in ('SerialNumber', 'UniqueID', 'DeviceID', 'InstanceName', 'PNPDeviceID', 'ComputerName',
                 'CSName', 'UserName', 'MACAddress', 'IPAddress', 'UUID', 'Name\''):
        check(name not in code, f'the collector names {name}')
    # The records and keys it writes, against the reader's.
    written = {}
    for line in re.findall(r'Emit\s+\(?((?:"[^"]*"\s*\+?\s*)+)\)?', code):
        joined = ''.join(re.findall(r'"([^"]*)"', line))
        kind, _, rest = joined.partition(' ')
        keys = set(re.findall(r'(\w+)=', rest))
        if kind:
            written.setdefault(kind, set()).update(keys)
    check(set(written) == set(wb.KEYS), f'records written {sorted(written)} != reader {sorted(wb.KEYS)}')
    check(f"Emit '{wb.FORMAT}'" in code, 'the collector does not write the reader\'s format line')
    for kind, keys in wb.KEYS.items():
        check(written.get(kind) == keys, f'{kind}: collector writes {sorted(written.get(kind, []))}, '
                                         f'reader wants {sorted(keys)}')
    # Every CIM reader must return its array with the unary comma: a bare
    # "return @(...)" unrolls one instance to a scalar, and StrictMode then
    # throws on .Count (the collector's first run on the gaokun3 stopped so).
    returns = re.findall(r'\breturn\s+(,?)\s*@\(', code)
    check(len(returns) == 4 and all(returns), 'a CIM reader returns @(...) without the leading comma')
    check('$StartMin = 55' in code and '$Floor = 50' in code, 'the 55 % start and 50 % floor')
    check("ValidateSet('ac-charging', 'ac-full', 'battery', 'weak-usb')" in code, 'the state set')
    check(set(re.findall(r"'(ac-charging|ac-full|battery|weak-usb)'", code)) == set(wb.STATES), 'states')


def sample(n, online=1, charging=1, discharging=0, critical=0, mv=8350, rem=40000, chg=12000,
           dis=0, pct=92):
    return (f'sample n={n} elapsed_s={5 * n} online={online} charging={charging} '
            f'discharging={discharging} critical={critical} voltage_mv={mv} remaining_mwh={rem} '
            f'charge_mw={chg} discharge_mw={dis} percent={pct}')


def readings(state='ac-charging', rows=None, end='COMPLETE', full=43500):
    rows = rows if rows is not None else [sample(n) for n in range(1, 4)]
    return '\n'.join([wb.FORMAT, f'run state={state} samples={len(rows)} interval=5',
                      'windows version=10.0.26100 build=26100 arch=ARM_64-bit',
                      'dmi vendor=HUAWEI product=GK-W7X bios_version=2.18 bios_date=2023-06-30',
                      f'battery_design designed_mwh=45076 full_mwh={full} cycles=112 chemistry=LION',
                      *rows, f'end status={end} samples={len(rows)}', ''])


def verdicts(text, name='x'):
    rec, problems = wb.parse(text, name)
    check(rec is not None, f'{name}: refused well-formed readings {problems}')
    checks, info = wb.analyse([rec], wb.bounds()) if rec else ([], [])
    return {c: (v, d) for c, v, d in checks}, info


def reader():
    print('reader')
    limits = wb.bounds()
    check(limits == dict(voltage=(5000, 9000), full=(1000, 10000), tolerance=10), f'bounds {limits}')
    v, info = verdicts(readings())
    check(all(v[k][0] == 'PASS' for k in ('voltage', 'capacity', 'gauge', 'flags')), f'good: {v}')
    check(any('ac-charging: 3 samples, COMPLETE, percent 92 -> 92' in i for i in info), 'good findings')
    # A battery run and a weak-source run that discharges while online.
    bat = [sample(n, online=0, charging=0, discharging=1, rem=40000 - 300 * n, chg=0, dis=9000,
                  pct=92 - n, mv=7900) for n in range(1, 5)]
    v, info = verdicts(readings('battery', bat))
    check(v['voltage'][0] == 'PASS' and v['flags'][0] == 'PASS', f'battery: {v}')
    weak = [sample(n, charging=0, discharging=1, rem=40000 - 100 * n, chg=0, dis=3000, pct=92 - n // 2)
            for n in range(1, 5)]
    v, info = verdicts(readings('weak-usb', weak))
    check(any('discharging with the adapter online' in i for i in info), 'weak-usb discharge finding')
    batt1 = [sample(n, pct=70 - n, rem=30000 - 400 * n) for n in range(1, 5)]
    v, info = verdicts(readings('weak-usb', batt1))
    check(any('BATT-1' in i for i in info), 'BATT-1 finding')
    # Readings the guard would refuse.
    for name, rows, key in (
            ('over-voltage', [sample(1, mv=9100)], 'voltage'),
            ('under-voltage', [sample(1, mv=4900)], 'voltage'),
            ('both flags', [sample(1, charging=1, discharging=1)], 'flags'),
            ('charging on battery', [sample(1, online=0)], 'flags'),
            ('gauge', [sample(1, pct=60, rem=40000)], 'gauge')):
        v, _ = verdicts(readings('ac-charging', rows), name)
        check(v[key][0] == 'FAIL', f'{name}: {key} {v.get(key)}')
    v, _ = verdicts(readings(full=90000), 'big pack')
    check(v['capacity'][0] == 'FAIL', f'capacity bound: {v["capacity"]}')
    v, _ = verdicts(readings(rows=[sample(1, mv='ABSENT', rem='ABSENT', pct='ABSENT')]), 'unreadable')
    check(v['voltage'][0] == 'SKIP' and v['gauge'][0] == 'SKIP', f'unreadable: {v}')
    # Files the reader must refuse.
    good = readings()
    for name, text in {
            'magic': good.replace('format=1', 'format=2'),
            'truncated': good.rsplit('end ', 1)[0],
            'foreign record': good.replace('end status', 'serial number=1\nend status'),
            'foreign key': good.replace('cycles=112', 'cycles=112 serial=9'),
            'missing key': good.replace(' chemistry=LION', ''),
            'bad value': good.replace('vendor=HUAWEI', 'vendor=HUA"WEI'),
            'bad flag': good.replace('online=1', 'online=yes', 1),
            'count': good.replace('end status=COMPLETE samples=3', 'end status=COMPLETE samples=4'),
            'gap': good.replace('sample n=2 ', 'sample n=5 '),
            'state': good.replace('state=ac-charging', 'state=lab'),
            'after end': good + 'sample n=9\n'}.items():
        rec, problems = wb.parse(text, name)
        check(rec is None and problems, f'accepted "{name}"')
    # Windows PowerShell 5.1 may add a BOM; the reader takes it.
    rec, _ = wb.parse(('\ufeff' + good).encode('utf-8').decode('utf-8-sig'), 'bom')
    check(rec is not None, 'BOM refused')


def main():
    lint()
    reader()
    if failures:
        print(f'FAIL test-gaokun3-windows-battery: {len(failures)} checks failed')
        return 1
    print('PASS test-gaokun3-windows-battery [lint and reader; collector NOT RUN: no PowerShell here]')
    return 0


if __name__ == '__main__':
    sys.exit(main())
