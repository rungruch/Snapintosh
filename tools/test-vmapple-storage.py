#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and run the VMApple VirtIO Block Storage host test suite (Phase 4 P1).

The storage model (tools/vmapple-smoke/storage.c) is freestanding EL2 code;
the C test supplies synthetic guest RAM and drives it through PCI ECAM, BAR0,
VirtIO Modern 1.0 queues, and Apple variant tagging under ASan/UBSan.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    clang = 'clang'
    with tempfile.TemporaryDirectory() as d:
        exe = pathlib.Path(d) / 'test-vmapple-storage'
        cmd = [clang, '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-DTEST_HARNESS=1',
               '-fsanitize=address,undefined', f'-I{ROOT / "tools"}',
               str(ROOT / 'tools' / 'test-vmapple-storage.c'),
               str(ROOT / 'tools' / 'vmapple-smoke' / 'storage.c'),
               '-o', str(exe)]
        built = subprocess.run(cmd, capture_output=True, text=True)
        if built.returncode:
            print(built.stdout + built.stderr)
            print('FAIL test-vmapple-storage: build failed')
            return 1

        run = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
