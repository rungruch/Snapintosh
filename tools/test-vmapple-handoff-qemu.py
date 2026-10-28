#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rehearse the H2 q1n1-to-monitor hand-off in QEMU (Phase 3, H2 piece 1).

The whole chain, on QEMU's EDK2 instead of the gaokun3:

  q1n1.efi --vmapple-enter --exit-vhe   (../q1n1, built with
      -DQ1N1_VMAPPLE_STICK -DQ1N1_VMAPPLE_TEST_SEED)
    places a version 2 stick bundle (tools/vmapple-stick.py, with the
    smoke's monitor image as its last chunk), keeps it allocated through
    ExitBootServices, leaves VHE, and enters the monitor at EL2;
  the monitor (tools/vmapple-smoke/stub.S + monitor.c) enters the stock
    VirtualMac kernel, which must reach the root-mount wait.

EDK2 lists no RAW RNG, so the rehearsal seed comes from q1n1's test-only
seed path (the firmware's default algorithm, RNDR-backed there). That
gate can never be built for the gaokun3; on the machine the seed is RAW
or there is no boot. A QEMU result is never a hardware result.

Build q1n1 first:
  make -C ../q1n1 uefi OUT=build/uefi-vmb-enter \\
      Q1N1_DEFINES="-DQ1N1_VHE_EXIT -DQ1N1_VMAPPLE_STICK -DQ1N1_VMAPPLE_TEST_SEED"

  python3 tools/test-vmapple-handoff-qemu.py [--kernelcache PATH] [--timeout 900]
"""
import argparse
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
Q1N1_EFI = ROOT.parent / 'q1n1' / 'build' / 'uefi-vmb-enter' / 'q1n1.efi'
FIRMWARE = pathlib.Path('/opt/homebrew/share/qemu/edk2-aarch64-code.fd')
DEFAULT_KC = ROOT / 'downloads' / 'extracted' / 'vma2macosap' / 'kernelcache.release.vma2'
MONITOR_PA = 0x40200000
# Distinct from layout.h's PARAMS_PA (0x43000000), so the rehearsal proves
# q1n1 passes the parameter page's address in x0 instead of relying on the
# stub's fallback constant.
PARAMS_PA = 0x43200000


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--kernelcache', default=str(DEFAULT_KC))
    ap.add_argument('--timeout', type=int, default=900)
    args = ap.parse_args()
    kc = pathlib.Path(args.kernelcache)
    if not kc.exists() or not Q1N1_EFI.exists() or not FIRMWARE.exists() \
            or not shutil.which('qemu-system-aarch64'):
        print('SKIP: needs the locally extracted kernelcache, the q1n1 rehearsal '
              'build (see this file\'s header) and QEMU with EDK2')
        return 0
    smoke = load('vmapple_smoke', ROOT / 'tools' / 'vmapple-smoke' / 'smoke.py')
    out = ROOT / 'build' / 'vmapple-handoff'
    shutil.rmtree(out, ignore_errors=True)
    esp = out / 'esp'
    esp.mkdir(parents=True)
    _, stub_bin = smoke.build_image(smoke.llvm_dir(None), out, smoke.HERE / 'stub.S',
                                    'stub', MONITOR_PA)
    built = subprocess.run(
        [sys.executable, str(ROOT / 'tools' / 'vmapple-stick.py'), 'build',
         '--kernelcache', str(kc), '--layout', 'qemu-monitor',
         '--monitor', str(stub_bin), '--monitor-pa', hex(MONITOR_PA),
         '--params-pa', hex(PARAMS_PA), '--out', str(esp)],
        capture_output=True, text=True)
    print(built.stdout, end='')
    if built.returncode:
        print(built.stderr)
        return 1
    shutil.copyfile(Q1N1_EFI, esp / 'q1n1.efi')
    (esp / 'startup.nsh').write_text('fs0:\nq1n1.efi --vmapple-enter --exit-vhe\n')
    serial = out / 'serial.log'
    cmd = ['qemu-system-aarch64', '-machine', 'virt,virtualization=on,gic-version=3',
           '-accel', 'tcg', '-cpu', 'max,sve=off,sme=off', '-smp', '1', '-m', '2560M',
           '-bios', str(FIRMWARE),
           '-drive', f'format=raw,readonly=on,file=fat:{esp}',
           '-display', 'none', '-monitor', 'none', '-nic', 'none', '-no-reboot',
           '-serial', f'file:{serial}']
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and proc.poll() is None:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if 'Waiting on' in text and 'IOMedia' in text:
                break
            if 'EL2: stop:' in text or 'panic(' in text:
                break
            time.sleep(2)
    finally:
        proc.kill()
        proc.wait()
    text = serial.read_text(errors='replace').replace('\r', '') if serial.exists() else ''
    print(text[-4000:])
    problems = []
    for want in ('PLACED AND VERIFIED, SEED FROM TEST-ONLY RNG (QEMU REHEARSAL)',
                 'KEPT: ENTERING THE MONITOR (PHASE 3 STEP H2)',
                 'EL2: monitor:', 'Darwin Kernel Version'):
        if want not in text:
            problems.append(f'no "{want}"')
    if 'SEED FROM RAW' in text:
        problems.append('the rehearsal claimed the RAW seed source')
    if not ('Waiting on' in text and 'IOMedia' in text):
        problems.append('the root-mount wait was not reached')
    if 'panic(' in text:
        problems.append('the guest panicked')
    for p in problems:
        print(f'FAIL: {p}')
    if problems:
        print('FAIL vmapple-handoff-qemu (serial log: build/vmapple-handoff/serial.log)')
        return 1
    print('PASS vmapple-handoff-qemu: q1n1 placed and kept the version 2 bundle, '
          'entered the monitor at EL2, and the kernel reached the root-mount wait '
          '(QEMU only; the seed was the test-only source, never RAW)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
