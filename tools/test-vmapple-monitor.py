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
        # Both platform builds: QEMU's and the gaokun3 gate's. The models
        # are the same code; the gate changes only the physical GIC the
        # monitor drives and where its console goes.
        for name, defines in (('qemu', ()), ('gaokun3', ('-DVMAPPLE_GAOKUN3_MONITOR',))):
            exe = pathlib.Path(d) / f'test-vmapple-monitor-{name}'
            cmd = [clang, '-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', f'-I{ROOT / "tools"}', *defines,
                   str(ROOT / 'tools' / 'test-vmapple-monitor.c'),
                   str(ROOT / 'tools' / 'vmapple-smoke' / 'monitor.c'),
                   str(ROOT / 'tools' / 'vmapple-smoke' / 'panic.c'),
                   str(ROOT / 'tools' / 'vmapple-smoke' / 'storage.c'),
                   '-o', str(exe)]
            built = subprocess.run(cmd, capture_output=True, text=True)
            if built.returncode:
                print(built.stdout + built.stderr)
                print(f'FAIL test-vmapple-monitor ({name}): build failed')
                return 1
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            sys.stdout.write(run.stdout)
            sys.stderr.write(run.stderr)
            if run.returncode:
                return run.returncode
        return 0


if __name__ == '__main__':
    sys.exit(main())
