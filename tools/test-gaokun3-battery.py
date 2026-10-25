#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for compat/gaokun3-battery.c, the gaokun3 battery/AC guard. No hardware.

  python3 tools/test-gaokun3-battery.py [--cc CLANG]

1. tools/test-gaokun3-battery.c is built with ASan+UBSan, or UBSan in trap
   mode where the host has no ASan runtime, and run. It decodes synthetic EC
   snapshots, among them the faulty readings the policy must refuse, and
   prints each record with its admission verdict at every minimum 50..100.
   It then runs the trend check (BATT-1) on synthetic drains and on the
   2026-10-25 Windows readings (tools/test-gaokun3-battery-readings.h): the
   weak USB run must trip DRAIN, the held, charging and battery runs must
   not. Those lines are informational; the C test's CHECKs decide.
2. Every record is replayed through tools/cycle_lib/common.py's battery_ok():
   the C verdict must be OK exactly where battery_ok() is true, so the guard
   and the A16 host policy cannot drift apart.
3. The minimums the C policy accepts must be the ones battery_minimum()
   accepts, and its floor must be the pinned battery.floor.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'compat' / 'gaokun3-battery.c'
HDR = ROOT / 'compat' / 'gaokun3-battery.h'
TEST = ROOT / 'tools' / 'test-gaokun3-battery.c'
EXE = '.exe' if os.name == 'nt' else ''
sys.path.insert(0, str(ROOT / 'tools'))


def compile_test(cc, out, sanitizers):
    cmd = [cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitizers,
           f'-I{ROOT / "compat"}', str(TEST), str(SRC), '-o', str(out)]
    return cmd, subprocess.run(cmd, capture_output=True, text=True)


def accepts(battery_minimum, m):
    try:
        battery_minimum(m)
        return True
    except Exception:  # cycle_lib's need() raises Refused
        return False


def parity(lines, common):
    """Problems where the C verdict and battery_ok() disagree, and the case count."""
    problems, count = [], 0
    floor = int(re.search(r'#define GAOKUN3_BATTERY_FLOOR_PERCENT (\d+)u', HDR.read_text())[1])
    pinned = common.PINS['battery']['floor']
    if floor != pinned:
        problems.append(f'GAOKUN3_BATTERY_FLOOR_PERCENT {floor} != {common.PINS_PATH.name} battery.floor {pinned}')
    bounds = None
    for line in lines:
        if not line.startswith('{'):
            continue
        rec = json.loads(line)
        if 'minimum_bounds' in rec:
            bounds = rec['minimum_bounds']
            continue
        count += 1
        battery = dict(plausible=rec['plausible'], ac=rec['ac'], percent=rec['percent'])
        if len(rec['verdicts']) != 101 - floor:
            problems.append(f'{rec["case"]}: {len(rec["verdicts"])} verdicts for minimums {floor}..100')
            continue
        for m, verdict in zip(range(floor, 101), rec['verdicts']):
            want = bool(common.battery_ok(rec['status'], battery, m))
            if want != (verdict == 0):
                problems.append(f'{rec["case"]} at minimum {m}: C verdict {verdict}, battery_ok {want}')
    py = [m for m in range(0, 201) if accepts(common.battery_minimum, m)]
    if bounds is None:
        problems.append('test printed no minimum_bounds')
    elif py != list(range(bounds[0], bounds[1] + 1)):
        problems.append(f'C accepts minimums {bounds[0]}..{bounds[1]}, battery_minimum {py[0]}..{py[-1]}')
    return problems, count


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', help='host clang (default: $CC, then clang on PATH)')
    a = ap.parse_args()
    cc = a.cc or os.environ.get('CC') or shutil.which('clang')
    if not cc:
        print('NOT RUN test-gaokun3-battery.c: no host clang (pass --cc or set CC)')
        return 2
    modes = [('ASan+UBSan', ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']),
             ('UBSan trap (no ASan runtime on this host)',
              ['-fsanitize=undefined', '-fsanitize-trap=undefined'])]
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ('test-gaokun3-battery' + EXE)
        for mode, flags in modes:
            cmd, built = compile_test(cc, exe, flags)
            if built.returncode == 0:
                break
        else:
            print(built.stdout + built.stderr)
            print('FAIL build: ' + ' '.join(cmd))
            return 1
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
    if ran.returncode:
        print((ran.stdout + ran.stderr).rstrip())
        print(f'FAIL (exit {ran.returncode}) test-gaokun3-battery.c [{mode}]')
        print('NOT RUN parity: the C test did not complete')
        return 1
    # One line per case instead of the 51-verdict JSON the parity check reads.
    names = re.findall(r'GAOKUN3_BATTERY_(\w+),', HDR.read_text().split('enum gaokun3_battery_reason', 1)[1])
    for line in ran.stdout.splitlines():
        rec = json.loads(line) if line.startswith('{') else None
        if rec and 'case' in rec:
            admitted = [m for m, v in zip(range(101 - len(rec['verdicts']), 101), rec['verdicts']) if v == 0]
            detail = ''
            if not rec['reason']:
                detail = f'  admits minimums {admitted[0]}..{admitted[-1]}' if admitted else '  admits no minimum'
            print(f'  {rec["case"]:<26} {names[rec["reason"]]:<13} ac={rec["ac"]} '
                  f'percent={rec["percent"]}{detail}')
        elif not rec:
            print(line)
    print(f'PASS test-gaokun3-battery.c [{mode}]')

    from cycle_lib import common
    problems, count = parity(ran.stdout.splitlines(), common)
    for p in problems:
        print(f'FAIL parity: {p}')
    if not problems:
        print(f'PASS parity: {count} records give the verdict cycle_lib.common.battery_ok() gives '
              f'at every minimum, and the same minimum bounds ({common.PINS_PATH.name})')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
