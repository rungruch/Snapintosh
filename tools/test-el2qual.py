#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for probes/el2qual's report and firmware-reader code.

  python3 tools/test-el2qual.py [--cc CLANG]

1. probes/el2qual/report.c's ID register table must name exactly the
   encodings probes/idregs.h reads, in the same order and under the same
   names, so the two cannot drift.
2. tools/test-el2qual.c is built with AddressSanitizer and UBSan where the
   host has their runtimes, otherwise with UBSan in trap mode (no runtime;
   e.g. llvm-mingw on Arm64 Windows ships no ASan), and run. The mode used
   is printed, because the two are not the same evidence.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EL2Q = ROOT / 'probes' / 'el2qual'
EXE = '.exe' if os.name == 'nt' else ''


def idregs_table():
    """(crm, op2, name) for each case of probes/idregs.h's id_register()."""
    text = (ROOT / 'probes' / 'idregs.h').read_text()
    body = text.split('static uint64_t id_register', 1)[1].split('default:', 1)[0]
    rows = []
    for case, crm, op2, name in re.findall(
            r'case 0x([0-9a-f]+): __asm__ volatile\("mrs %0, s3_0_c0_c(\d)_(\d)"[^/]*/\* (\w+) \*/',
            body):
        assert int(case, 16) == (int(crm) << 3 | int(op2)), f'idregs.h case {case} != c{crm}_{op2}'
        rows.append((int(crm), int(op2), name))
    return rows


def report_table():
    text = (EL2Q / 'report.c').read_text()
    body = text.split('el2q_idregs[EL2Q_ID_COUNT] = {', 1)[1].split('};', 1)[0]
    return [(int(c), int(o), n) for c, o, _, n in
            re.findall(r'\{(\d), (\d), ([01]), "(\w+)"\}', body)]


def short_name(name):
    """idregs.h spells registers in full; the report drops _EL1 and, for the
    AArch64 ones, the ID_ prefix so they fit its columns."""
    name = name.removesuffix('_EL1')
    return name.removeprefix('ID_') if name.startswith('ID_AA64') else name


def check_tables():
    want = [(c, o, short_name(n)) for c, o, n in idregs_table()]
    got = report_table()
    count = int(re.search(r'#define EL2Q_ID_COUNT\s+(\d+)', (EL2Q / 'report.h').read_text())[1])
    problems = []
    if len(want) != count:
        problems.append(f'idregs.h reads {len(want)} registers, EL2Q_ID_COUNT is {count}')
    if got != want:
        for i, (a, b) in enumerate(zip(want, got)):
            if a != b:
                problems.append(f'row {i}: idregs.h {a}, report.c {b}')
        if len(got) != len(want):
            problems.append(f'report.c has {len(got)} rows, idregs.h {len(want)}')
    return problems


def compile_test(cc, out, sanitizers):
    cmd = [cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitizers,
           f'-I{EL2Q}', str(ROOT / 'tools' / 'test-el2qual.c'), str(EL2Q / 'report.c'),
           str(EL2Q / 'firmware.c'), str(EL2Q / 'thermal.c'),
           str(ROOT / 'compat' / 'sc8280xp-thermal.c'), '-o', str(out)]
    return cmd, subprocess.run(cmd, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', help='host clang (default: $CC, then clang on PATH)')
    a = ap.parse_args()
    cc = a.cc or os.environ.get('CC') or shutil.which('clang')
    if not cc:
        print('NOT RUN: no host clang (pass --cc or set CC)')
        return 2

    problems = check_tables()
    for p in problems:
        print(f'FAIL table: {p}')
    if not problems:
        print(f'PASS table: report.c names the {len(report_table())} encodings idregs.h reads')

    modes = [('ASan+UBSan', ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']),
             ('UBSan trap (no ASan runtime on this host)',
              ['-fsanitize=undefined', '-fsanitize-trap=undefined'])]
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ('test-el2qual' + EXE)
        for mode, flags in modes:
            cmd, built = compile_test(cc, exe, flags)
            if built.returncode == 0:
                break
        else:
            print(built.stdout + built.stderr)
            print('FAIL build: ' + ' '.join(cmd))
            return 1
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        print((ran.stdout + ran.stderr).rstrip())
        verdict = 'PASS' if ran.returncode == 0 else f'FAIL (exit {ran.returncode})'
        print(f'{verdict} test-el2qual.c [{mode}]')
    return 1 if problems or ran.returncode else 0


if __name__ == '__main__':
    sys.exit(main())
