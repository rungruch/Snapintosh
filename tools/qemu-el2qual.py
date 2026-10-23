#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run el2qual.efi under QEMU against CPU models with known answers.

  python3 tools/qemu-el2qual.py [--config NAME ...] [--qemu DIR] [--llvm DIR]
  python3 tools/qemu-el2qual.py --self-test
  python3 tools/qemu-el2qual.py --config max --screenshot out.png

A probe that reports "absent" on the gaokun3 proves nothing unless the same
binary reports "present" where the feature exists, and "absent" where it is
known not to. Each configuration boots QEMU's EDK2 at EL2 with four CPUs,
runs el2qual from the UEFI shell with "serial poweroff", and checks the
report it writes to the PL011:

  max          every feature present; PAuth signs; 16K walks
  max-vhe      the same, after recreating slbounce's E2H|TGE handoff, so the
               _EL12 walk and the VHE register layouts are exercised
  max-nopauth  -cpu max,pauth=off: PAuth absent, the rest present
  cortex-a76   Armv8.2, the nearest model to the gaokun3's cores: NV2, ECV,
               FGT, RNG and PAuth absent; 16K walks
  max-rotated  as max, with the firmware's GOP mode treated as having no
               framebuffer address (the gaokun3's mode 7): el2qual must move
               to another mode and say so
  max-dt-fb    every GOP mode treated that way, and the real framebuffer
               handed to the device-tree fallback as its simple-framebuffer
               (QEMU's EDK2 installs no device tree): el2qual must accept it
               against the real memory map, draw on it and say so
  max-no-fb    every GOP mode treated that way, no device tree and no
               "serial": el2qual must stop before ExitBootServices, saying
               why on the console

On every configuration that runs, all four CPUs must finish at EL2 without
a fault, the report must name the GOP mode it drew on, and each feature's
ID field must agree with whether its probe instruction ran.
--self-test feeds the checker deliberately wrong logs it must reject.

QEMU is found from --qemu, then SNAP_QEMU, then PATH; its EDK2 image from
--firmware, then <qemu>/share, then Homebrew's share/qemu.
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'probes' / 'el2qual'))
import build as el2qual_build                                         # noqa: E402

EXE = '.exe' if os.name == 'nt' else ''
SMP = 4
FEATURES = ('NV2', 'ECV', 'FGT', 'RNDR')
# The ID field value at which each feature counts as present (report.c).
PRESENT_AT = {'NV2': 2, 'ECV': 1, 'FGT': 1, 'RNDR': 1}

CONFIGS = {
    'max': dict(cpu='max', options='', pac='Y', walk='Y',
                features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe': dict(cpu='max', options='emulate-vhe', pac='Y', walk='Y', vhe=True,
                    features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-nopauth': dict(cpu='max,pauth=off', options='', pac='N', walk='Y',
                        features=dict.fromkeys(FEATURES, True),
                        routes=('BLOCKED: PAUTH', 'BLOCKED: PAUTH')),
    'cortex-a76': dict(cpu='cortex-a76', options='', pac='N', walk='Y',
                       features=dict.fromkeys(FEATURES, False),
                       routes=('BLOCKED: NV2 ECV RNDR PAUTH', 'BLOCKED: PAUTH')),
    'max-rotated': dict(cpu='max', options='emulate-rotated', pac='Y', walk='Y', moved=True,
                        features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-dt-fb': dict(cpu='max', options='emulate-no-fb emulate-dt-fb', pac='Y', walk='Y', dt=True,
                      features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    # Without "serial" a stop is the only correct outcome; the firmware
    # console, which EDK2 also sends to the PL011, is where it is reported.
    'max-no-fb': dict(cpu='max', options='emulate-no-fb', serial=False, stop=True),
}

STOP_LINES = ('NO GOP MODE HAS A FRAMEBUFFER ADDRESS', 'DT FRAMEBUFFER: NO DEVICE TREE',
              'STOPPED BEFORE EXITBOOTSERVICES')
SCREEN = re.compile(r'^SCREEN GOP MODE (\d+)  FB ([0-9A-F]{16})(?:  FROM MODE (\d+))?$')
DT_SCREEN = re.compile(r'^SCREEN DT SIMPLE-FB  FB ([0-9A-F]{16})  NO GOP ADDRESS$')


def check_stop(log):
    """Problems with a run that had to stop before ExitBootServices."""
    problems = [f'no "{s}" on the console' for s in STOP_LINES if s not in log]
    if 'CALLING EXITBOOTSERVICES' in log:
        problems.append('ExitBootServices was attempted with nothing to show the result on')
    if 'EL2QUAL BEGIN' in log:
        problems.append('a report was written although el2qual should have stopped')
    return problems

SUM = re.compile(r'^SUM (\d+) EL(\d) (.+?)(?: PAC=(\S) 16K=(\S) NV2=(\d+)/(\w+) ECV=(\d+)/(\w+)'
                 r' FGT=(\d+)/(\w+) RNDR=(\d+)/(\w+))?$')
GIC = re.compile(r'^GIC (\d+) FRQ=(\d+) SRE=([0-9A-F]+)/(\w+) VTR=([0-9A-F]+)/(\w+) LR=(\S+)'
                 r' HCR=([0-9A-F]+)/(\w+)$')


def check_gic(lines, smp):
    """QEMU's virt machine with virtualization=on has a GICv3 with the
    virtual CPU interface, and its EDK2 publishes SMBIOS 3: every slot must
    show both, at one counter rate, with list register 0 holding the pattern
    and the interface left off."""
    problems = []
    gics = {int(m[1]): m for line in lines for m in [GIC.match(line)] if m}
    if sorted(gics) != list(range(smp)):
        problems.append(f'expected GIC lines for slots 0..{smp - 1}, got {sorted(gics)}')
    rates = {int(m[2]) for m in gics.values()}
    if len(rates) > 1 or 0 in rates:
        problems.append(f'CNTFRQ_EL0 rates {sorted(rates)}, wanted one non-zero rate')
    for slot, m in sorted(gics.items()):
        if m[4] != 'OK' or not int(m[3], 16) & 1:
            problems.append(f'slot {slot}: ICC_SRE_EL2 {m[3]}/{m[4]}, wanted SRE set')
        if m[6] != 'OK':
            problems.append(f'slot {slot}: ICH_VTR_EL2 probe {m[6]}, wanted OK')
        if m[7] != 'RW':
            problems.append(f'slot {slot}: list register test {m[7]}, wanted RW')
        if m[9] != 'OK' or int(m[8], 16) & 1:
            problems.append(f'slot {slot}: ICH_HCR_EL2 {m[8]}/{m[9]}, wanted En clear')
    if not any(line.startswith('GIC VIRTUAL INTERFACE ... YES') for line in lines):
        problems.append('no "GIC VIRTUAL INTERFACE ... YES" verdict')
    if not any(re.match(r'FIRMWARE [^:]', line) for line in lines):
        problems.append('no FIRMWARE line read from SMBIOS')
    if not any(line.startswith('VENDOR ') and ' SMBIOS 3.' in line for line in lines):
        problems.append('no VENDOR line with an SMBIOS 3 version')
    return problems


def report_lines(log):
    """The lines between EL2QUAL BEGIN and END, or None if either is missing."""
    lines = [line.rstrip('\r') for line in log.splitlines()]
    try:
        begin = lines.index('EL2QUAL BEGIN')
        end = lines.index('EL2QUAL END', begin)
    except ValueError:
        return None
    return lines[begin + 1:end]


def check(log, expect, smp=SMP):
    """Problems with one run's serial log; empty when it is what expect says."""
    if expect.get('stop'):
        return check_stop(log)
    lines = report_lines(log)
    if lines is None:
        return ['no complete EL2QUAL BEGIN..END report on the serial port']
    problems = []
    screens = [m for line in lines for m in [SCREEN.match(line)] if m]
    dt_screens = [m for line in lines for m in [DT_SCREEN.match(line)] if m]
    if expect.get('dt'):
        if len(dt_screens) != 1 or screens:
            problems.append(f'{len(dt_screens)} SCREEN DT lines and {len(screens)} GOP ones, '
                            'wanted 1 DT line')
        elif not int(dt_screens[0][1], 16):
            problems.append('the report was drawn on framebuffer address 0')
    elif dt_screens:
        problems.append('drawn on the device tree\'s framebuffer without being asked to')
    elif len(screens) != 1:
        problems.append(f'{len(screens)} SCREEN GOP MODE lines with an address, wanted 1')
    else:
        mode, base, first = screens[0][1], int(screens[0][2], 16), screens[0][3]
        if not base:
            problems.append('the report was drawn on framebuffer address 0')
        if expect.get('moved') and (first is None or first == mode):
            problems.append(f'mode {mode}: wanted a move away from the firmware\'s mode')
        if not expect.get('moved') and first is not None:
            problems.append(f'moved from mode {first} to {mode} without being asked to')
    sums = {}
    for line in lines:
        m = SUM.match(line)
        if m:
            sums[int(m[1])] = m
    if sorted(sums) != list(range(smp)):
        problems.append(f'expected SUM lines for slots 0..{smp - 1}, got {sorted(sums)}')
    for slot, m in sorted(sums.items()):
        if m[2] != '2' or m[3] != 'DONE' or m[4] is None:
            problems.append(f'slot {slot}: EL{m[2]} {m[3]}, wanted EL2 DONE')
            continue
        if m[4] != expect['pac']:
            problems.append(f'slot {slot}: PAC={m[4]}, wanted {expect["pac"]}')
        if m[5] != expect['walk']:
            problems.append(f'slot {slot}: 16K={m[5]}, wanted {expect["walk"]}')
        for i, name in enumerate(FEATURES):
            value, probe = int(m[6 + 2 * i]), m[7 + 2 * i]
            advertised = value >= PRESENT_AT[name]
            if advertised != (probe == 'OK'):
                problems.append(f'slot {slot}: {name} field {value} but its probe gave {probe}')
            if probe not in ('OK', 'UD'):
                problems.append(f'slot {slot}: {name} probe raised an unexpected exception')
            if advertised != expect['features'][name]:
                problems.append(f'slot {slot}: {name} {"present" if advertised else "absent"}, '
                                f'wanted {"present" if expect["features"][name] else "absent"}')
    for label, want in zip(('J700 ROUTE', 'VIRTUALMAC ROUTE'), expect['routes']):
        got = next((line.split('.. ', 1)[-1] for line in lines if line.startswith(label)), None)
        if got != want:
            problems.append(f'{label}: "{got}", wanted "{want}"')
    # The CPU table's detail line for a faulted slot. (Not the word FAULT:
    # the 16K controls line says "B FAULT  C FAULT" when all is well.)
    if any(re.match(r'\s+V\d+ ESR ', line) for line in lines):
        problems.append('a CPU reported an unexpected exception')
    vhe = any(line == 'TEST ONLY: SLBOUNCE E2H STATE EMULATED' for line in lines)
    hcr = next((int(m[1], 16) for line in lines
                for m in [re.match(r'ENTRY HCR_EL2 ([0-9A-F]{16})', line)] if m), None)
    if hcr is None:
        problems.append('no ENTRY HCR_EL2 line')
    elif expect.get('vhe'):
        if not vhe:
            problems.append('VHE emulation requested but not reported')
        if not (hcr >> 34) & 1 or not (hcr >> 27) & 1:
            problems.append(f'VHE emulation reported but HCR_EL2 {hcr:#x} lacks E2H|TGE')
    elif vhe or (hcr >> 34) & 1:
        problems.append('E2H set on a run that did not ask for VHE emulation')
    problems += check_gic(lines, smp)
    return problems


# ---- Self-test: the checker must refuse logs that are wrong -----------------

def synthetic(expect, smp=SMP):
    if expect.get('stop'):
        return '\n'.join(['Shell> el2qual.efi poweroff emulate-no-fb',
                          'NO GOP MODE HAS A FRAMEBUFFER ADDRESS (MODE 0)',
                          'DT FRAMEBUFFER: NO DEVICE TREE',
                          'STOPPED BEFORE EXITBOOTSERVICES: NOTHING WAS MEASURED',
                          'HOLD THE POWER BUTTON TO TURN THE MACHINE OFF', ''])
    feats = ' '.join(f'{n}={PRESENT_AT[n] if expect["features"][n] else 0}/'
                     f'{"OK" if expect["features"][n] else "UD"}' for n in FEATURES)
    hcr = (1 << 34 | 1 << 27) if expect.get('vhe') else 1 << 31
    if expect.get('dt'):
        screen = 'SCREEN DT SIMPLE-FB  FB 0000000080000000  NO GOP ADDRESS'
    else:
        screen = 'SCREEN GOP MODE 1  FB 0000000080000000' + ('  FROM MODE 0' if expect.get('moved') else '')
    lines = ['junk before', 'EL2QUAL BEGIN', screen,
             'FIRMWARE 0.0.0  02/06/2015', 'VENDOR EFI Development Kit II / OVMF  SMBIOS 3.0',
             f'ENTRY HCR_EL2 {hcr:016X} SCTLR 0000000030C5183D']
    if expect.get('vhe'):
        lines.append('TEST ONLY: SLBOUNCE E2H STATE EMULATED')
    lines += ['GIC VIRTUAL INTERFACE ... YES  4 LRS  PRI 5  ID 16  LR0 RW',
              '  CONTROLS A OK  B FAULT  C FAULT  (WANT OK FAULT FAULT)',
              f'J700 ROUTE (AS ON A16) .. {expect["routes"][0]}',
              f'VIRTUALMAC ROUTE (EL1) .. {expect["routes"][1]}']
    for s in range(smp):
        lines.append(f'SUM {s} EL2 DONE PAC={expect["pac"]} 16K={expect["walk"]} {feats}')
        lines.append(f'GIC {s} FRQ=62500000 SRE=7/OK VTR=90000003/OK LR=RW HCR=0/OK')
    return '\n'.join(lines + ['EL2QUAL END', ''])


def self_test():
    failures = 0
    def expect_result(name, log, expect, ok):
        nonlocal failures
        problems = check(log, expect)
        if bool(problems) == ok:
            failures += 1
            print(f'FAIL self-test {name}: checker said {problems or "fine"}')
    for name, expect in CONFIGS.items():
        good = synthetic(expect)
        expect_result(f'{name} good', good, expect, True)
        if expect.get('stop'):
            expect_result(f'{name} no reason', good.replace(STOP_LINES[0], 'NOTHING'), expect, False)
            expect_result(f'{name} went on to EBS', good + 'SNAPINTOSH EL2QUAL: STARTED AT EL2, '
                          'CALLING EXITBOOTSERVICES\n', expect, False)
            expect_result(f'{name} wrote a report', good + synthetic(CONFIGS['max']), expect, False)
            continue
        expect_result(f'{name} no screen line', re.sub(r'SCREEN (GOP MODE|DT) .*\n', '', good),
                      expect, False)
        expect_result(f'{name} screen at 0', good.replace('FB 0000000080000000',
                                                          'FB 0000000000000000'), expect, False)
        if expect.get('dt'):
            expect_result(f'{name} drawn on GOP instead', good.replace(
                'SCREEN DT SIMPLE-FB  FB 0000000080000000  NO GOP ADDRESS',
                'SCREEN GOP MODE 1  FB 0000000080000000'), expect, False)
            expect_result(f'{name} DT and GOP both', good.replace(
                'EL2QUAL BEGIN', 'EL2QUAL BEGIN\nSCREEN GOP MODE 1  FB 0000000080000000'),
                expect, False)
        elif expect.get('moved'):
            expect_result(f'{name} did not move', good.replace('  FROM MODE 0', ''), expect, False)
        else:
            expect_result(f'{name} moved unasked', good.replace(
                'FB 0000000080000000', 'FB 0000000080000000  FROM MODE 0'), expect, False)
        expect_result(f'{name} truncated', good.replace('EL2QUAL END', ''), expect, False)
        expect_result(f'{name} missing slot', good.replace('SUM 3 ', 'XUM 3 '), expect, False)
        expect_result(f'{name} slot faulted',
                      re.sub(r'SUM 2 EL2 DONE .*', 'SUM 2 EL2 FAULT', good), expect, False)
        expect_result(f'{name} wrong EL',
                      good.replace('SUM 1 EL2', 'SUM 1 EL1'), expect, False)
        flipped = 'N' if expect['pac'] == 'Y' else 'Y'
        expect_result(f'{name} PAC flipped',
                      good.replace(f'SUM 0 EL2 DONE PAC={expect["pac"]}',
                                   f'SUM 0 EL2 DONE PAC={flipped}'), expect, False)
        expect_result(f'{name} 16K inconclusive', good.replace('16K=Y', '16K=I', 1), expect, False)
        # A field that says present while the instruction was undefined, or
        # the reverse, means the probe or the field reading is wrong.
        bad = re.sub(r'RNDR=(\d)/(OK|UD)', lambda m: f'RNDR={m[1]}/{"UD" if m[2] == "OK" else "OK"}',
                     good, count=1)
        expect_result(f'{name} RNDR field/probe disagree', bad, expect, False)
        # The counter, GIC and firmware lines must each be there and right.
        expect_result(f'{name} missing GIC slot', good.replace('GIC 3 ', 'XIC 3 '), expect, False)
        expect_result(f'{name} rates differ', good.replace('GIC 2 FRQ=62500000', 'GIC 2 FRQ=19200000'),
                      expect, False)
        expect_result(f'{name} rate zero', good.replace('FRQ=62500000', 'FRQ=0'), expect, False)
        expect_result(f'{name} SRE clear', good.replace('GIC 1 FRQ=62500000 SRE=7',
                                                        'GIC 1 FRQ=62500000 SRE=6'), expect, False)
        expect_result(f'{name} VTR undefined', good.replace('VTR=90000003/OK', 'VTR=0/UD', 1),
                      expect, False)
        expect_result(f'{name} LR reads zero', good.replace('LR=RW', 'LR=RAZ', 1), expect, False)
        expect_result(f'{name} interface left on', good.replace('HCR=0/OK', 'HCR=1/OK', 1),
                      expect, False)
        expect_result(f'{name} no GIC verdict', good.replace('GIC VIRTUAL INTERFACE ... YES',
                                                             'GIC VIRTUAL INTERFACE ... ?'), expect, False)
        expect_result(f'{name} no firmware', good.replace('FIRMWARE 0.0.0  02/06/2015',
                                                          'FIRMWARE: NO SMBIOS TABLE'), expect, False)
        expect_result(f'{name} SMBIOS 2', good.replace('SMBIOS 3.0', 'SMBIOS 2.8'), expect, False)
        expect_result(f'{name} probe other exception', good.replace('/UD', '/EX', 1).replace(
            '/OK', '/EX', 1), expect, False)
        expect_result(f'{name} route text', good.replace(expect['routes'][0], 'CPU MAYBE'),
                      expect, False)
        expect_result(f'{name} fault detail line', good.replace(
            'EL2QUAL END', '   V5 ESR 96000045 ELR 000040001234 FAR 000000000000\nEL2QUAL END'),
            expect, False)
        if expect.get('vhe'):
            expect_result(f'{name} E2H missing', good.replace(
                f'{1 << 34 | 1 << 27:016X}', f'{1 << 31:016X}'), expect, False)
        else:
            expect_result(f'{name} E2H unexpected', good.replace(
                f'{1 << 31:016X}', f'{1 << 34 | 1 << 27:016X}'), expect, False)
    print('PASS checker self-test' if not failures else f'FAIL checker self-test ({failures})')
    return failures


# ---- QEMU ---------------------------------------------------------------------

def find_qemu(explicit):
    for candidate in (explicit, os.environ.get('SNAP_QEMU')):
        if candidate:
            return Path(candidate) / ('qemu-system-aarch64' + EXE)
    found = shutil.which('qemu-system-aarch64')
    if not found:
        raise SystemExit('qemu-system-aarch64 not found: pass --qemu DIR or set SNAP_QEMU')
    return Path(found)


def find_firmware(explicit, qemu):
    candidates = [explicit, qemu.parent / 'share' / 'edk2-aarch64-code.fd',
                  Path('/opt/homebrew/share/qemu/edk2-aarch64-code.fd')]
    for c in candidates:
        if c and Path(c).exists():
            return Path(c)
    raise SystemExit('no edk2-aarch64-code.fd: pass --firmware')


def qmp(port, *commands):
    with socket.create_connection(('127.0.0.1', port), timeout=10) as s:
        f = s.makefile('rw')
        f.readline()
        for command in ({'execute': 'qmp_capabilities'}, *commands):
            f.write(json.dumps(command) + '\n')
            f.flush()
            while 'return' not in (reply := json.loads(f.readline())) and 'error' not in reply:
                pass
            if 'error' in reply:
                raise RuntimeError(reply['error'])


def run_config(name, efi, qemu, firmware, out, timeout, screenshot=None):
    expect = CONFIGS[name]
    work = out / name
    esp = work / 'esp'
    shutil.rmtree(work, ignore_errors=True)
    esp.mkdir(parents=True)
    shutil.copyfile(efi, esp / 'el2qual.efi')
    options = ' '.join(filter(None, ['serial' if expect.get('serial', True) else '',
                                     '' if screenshot else 'poweroff', expect['options']]))
    (esp / 'startup.nsh').write_text(f'fs0:\nel2qual.efi {options}\n')
    log = work / 'serial.log'
    port = 4444 + list(CONFIGS).index(name)
    cmd = [str(qemu), '-machine', 'virt,virtualization=on,gic-version=3', '-accel', 'tcg',
           '-cpu', expect['cpu'], '-smp', str(SMP), '-m', '1G', '-bios', str(firmware),
           '-drive', f'format=raw,readonly=on,file=fat:{esp.as_posix()}', '-device', 'ramfb',
           '-display', 'none', '-monitor', 'none', '-nic', 'none', '-no-reboot',
           '-serial', f'file:{log.as_posix()}']
    if screenshot:
        cmd += ['-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off']
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(work / 'qemu.err', 'w'))
    try:
        while proc.poll() is None and time.monotonic() - start < timeout:
            if screenshot and log.exists() and 'EL2QUAL END' in log.read_text(errors='replace'):
                time.sleep(2)
                qmp(port, {'execute': 'screendump',
                           'arguments': {'filename': str(Path(screenshot).resolve()),
                                         'format': 'png'}})
                qmp(port, {'execute': 'quit'})
            time.sleep(1)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    elapsed = time.monotonic() - start
    text = log.read_text(errors='replace') if log.exists() else ''
    problems = check(text, expect)
    if elapsed >= timeout:
        problems.insert(0, f'timed out after {timeout}s')
    return problems, elapsed, log


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--config', action='append', choices=list(CONFIGS))
    ap.add_argument('--qemu', help='directory holding qemu-system-aarch64')
    ap.add_argument('--firmware', help='EDK2 aarch64 code image')
    ap.add_argument('--llvm', help='passed to probes/el2qual/build.py')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--screenshot', help='PNG of the framebuffer (one config, no poweroff)')
    a = ap.parse_args()
    if a.self_test:
        return 1 if self_test() else 0
    if self_test():
        return 1
    names = a.config or list(CONFIGS)
    if a.screenshot and len(names) != 1:
        raise SystemExit('--screenshot takes exactly one --config')
    qemu = find_qemu(a.qemu)
    firmware = find_firmware(a.firmware, qemu)
    efi = el2qual_build.build(llvm=a.llvm)
    out = ROOT / 'build' / 'el2qual-qemu'
    failed = 0
    for name in names:
        problems, elapsed, log = run_config(name, efi, qemu, firmware, out, a.timeout,
                                            a.screenshot)
        rel = log.relative_to(ROOT)
        if problems:
            failed += 1
            print(f'FAIL {name} ({elapsed:.0f}s, {rel})')
            for p in problems:
                print(f'  {p}')
        else:
            print(f'PASS {name} ({elapsed:.0f}s, {rel})')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
