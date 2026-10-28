#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and run the Route B monitor device-model host test (Phase 3 P3).

The model code (tools/vmapple-smoke/monitor.c) is freestanding EL2 code;
the C test supplies its helpers as fakes and drives it through synthetic
ESR syndromes and decoded instructions, under ASan/UBSan.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    clang = 'clang'
    with tempfile.TemporaryDirectory() as d:
        exe = pathlib.Path(d) / 'test-vmapple-monitor'
        cmd = [clang, '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', f'-I{ROOT / "tools"}',
               str(ROOT / 'tools' / 'test-vmapple-monitor.c'),
               str(ROOT / 'tools' / 'vmapple-smoke' / 'monitor.c'),
               '-o', str(exe)]
        built = subprocess.run(cmd, capture_output=True, text=True)
        if built.returncode:
            print(built.stdout + built.stderr)
            print('FAIL test-vmapple-monitor: build failed')
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
