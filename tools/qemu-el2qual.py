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
  max-thermal  as max, with the "thermal" stage: the virt machine has no
               tsens, so it must read nothing (unmapped) or have its first
               read's fault caught, and the CPU report must still follow
  max-vhe-thermal  the same after the slbounce handoff emulation, the
               state the gaokun3 runs the stage in
  max-vhe-exit as max-vhe, then the "exit-vhe" switch back to non-VHE EL2
               (vhe.c, el2q_exit_vhe): it must report DONE with the registers
               read back equal to the plan and E2H clear, and every CPU must
               then measure as on max, the 16K walk through the EL1&0 regime
  max-exit-novhe  "exit-vhe" on a machine that starts at EL2 without E2H:
               nothing to leave, so NOT-VHE, and the run as on max
  max-usb      the USB read (usbprobe.c): the virt machine has none of the
               gaokun3's clock controller, USB or SMMU, so the stage must
               refuse or read without harm; the checker recomputes the
               power decision from the words printed, and fails any
               controller read made while it is not ON, and any SError
  max-vhe-usb-smmu  the same with the SMMU, after the slbounce emulation
               and the VHE exit, the order the gaokun3 runs it in

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
    # The virt machine has no tsens: the stage must end without reading, or
    # with its first read's fault caught, and the CPU report must still come.
    'max-thermal': dict(cpu='max', options='thermal', pac='Y', walk='Y', thermal=True,
                        features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    # The same under slbounce's E2H|TGE state, where the gaokun3 runs it: the
    # mapping check's AT S1E2R then walks the EL2&0 regime.
    'max-vhe-thermal': dict(cpu='max', options='emulate-vhe thermal', pac='Y', walk='Y', vhe=True,
                            thermal=True, features=dict.fromkeys(FEATURES, True),
                            routes=('CPU OK', 'CPU OK')),
    'max-vhe-exit': dict(cpu='max', options='emulate-vhe exit-vhe', pac='Y', walk='Y', vhe=True,
                         vhe_exit='DONE', features=dict.fromkeys(FEATURES, True),
                         routes=('CPU OK', 'CPU OK')),
    'max-exit-novhe': dict(cpu='max', options='exit-vhe', pac='Y', walk='Y', vhe_exit='NOT-VHE',
                           features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    # The USB read (usbprobe.c). The virt machine has no Qualcomm clock
    # controller, USB or SMMU at those addresses: the stage must refuse or
    # read without harm, decide power from exactly the words it printed, read
    # the controller only when they say ON, and leave no SError. The second
    # runs in the order the gaokun3 would: slbounce's state, the VHE exit,
    # then the read with the SMMU.
    'max-usb': dict(cpu='max', options='usb', pac='Y', walk='Y', usb=True,
                    features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-smmu': dict(cpu='max', options='emulate-vhe exit-vhe usb-smmu', pac='Y', walk='Y',
                             vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                             features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
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


TSENS = re.compile(r'^TSENS (\w+) CHECKED=(\d+) UNMAPPED=(\d+) ADMIT=(-?\d+) POLLS=(\d+) VALID=(\d+)$')
TSENSWHY = re.compile(r'^TSENSWHY REASON=(\w+) AT=([0-9A-F]+) ESR=([0-9A-F]+) SERROR=([01])$')
TSENS_FIRST_WORD = 'C222000'      # tsens0 SROT+0, the guard's first read


def check_tsens(lines, expect):
    """The "thermal" stage on QEMU's virt machine, which has no tsens at the
    SC8280XP's addresses. Two outcomes are correct: the page tables do not
    map the words, so nothing is read; or they do, and the first read's
    fault is caught, so the guard trips READ there. Anything that admitted,
    sampled or left an SError pending read something that is not there."""
    rows = [m for line in lines for m in [TSENS.match(line)] if m]
    whys = [m for line in lines for m in [TSENSWHY.match(line)] if m]
    screen = [line for line in lines if line.startswith('TSENS GUARD (READ ONLY) . ')]
    if not expect.get('thermal'):
        if rows or whys or screen:
            return ['the tsens stage ran without the "thermal" option']
        return []
    if len(rows) != 1 or len(whys) != 1 or len(screen) != 1:
        return [f'{len(rows)} TSENS, {len(whys)} TSENSWHY and {len(screen)} TSENS GUARD lines, '
                'wanted one each']
    state, checked, unmapped, admit, polls = rows[0][1], *map(int, rows[0].group(2, 3, 4, 5))
    reason, at, esr, serror = whys[0].groups()
    problems = []
    if checked != 58:
        problems.append(f'tsens checked {checked} words for a mapping, wanted all 58')
    if serror != '0':
        problems.append('an SError was left pending by the tsens stage')
    if state == 'UNMAPPED':
        if not unmapped or polls or esr != '0':
            problems.append(f'tsens UNMAPPED with {unmapped} unmapped, {polls} polls, ESR {esr}')
    elif state == 'TRIPPED' and reason == 'READ':
        if esr == '0' or at != TSENS_FIRST_WORD or admit != 1 or polls != 1 or unmapped:
            problems.append(f'tsens READ trip at {at}, ESR {esr}, admit {admit}, {polls} polls: '
                            f'wanted the first read ({TSENS_FIRST_WORD}) faulting and caught')
    else:
        problems.append(f'tsens {state} {reason}: QEMU has no tsens, wanted UNMAPPED or a '
                        'caught READ fault')
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
    problems += check_tsens(lines, expect)
    problems += check_vhe_exit(lines, expect)
    problems += check_usb(lines, expect)
    return problems


USB_WORD = r'([0-9A-F]{8}|UNMAPPED|FAULT|NOT READ)'
USB_GCC1 = re.compile(rf'^USB0 GCC ([BA]) GDSC {USB_WORD} MASTER {USB_WORD} SLEEP {USB_WORD}$')
USB_GCC2 = re.compile(rf'^USB0 GCC ([BA]) UTMI {USB_WORD} AXI {USB_WORD} CNOC {USB_WORD}$')
USB_POWER = re.compile(r'^USB0 POWER ([BA]) (ON|GDSC OFF|CLOCKS OFF|UNREAD)$')


def usb_power(gdsc, master, axi, cnoc):
    """usbprobe.c's decision, from the printed words."""
    words = (gdsc, master, axi, cnoc)
    if any(not re.fullmatch(r'[0-9A-F]{8}', w) for w in words):
        return 'UNREAD'
    if not int(gdsc, 16) >> 31:
        return 'GDSC OFF'
    if any(int(w, 16) >> 31 for w in (master, axi, cnoc)):
        return 'CLOCKS OFF'
    return 'ON'


def check_usb(lines, expect):
    """The "usb" lines: absent unless asked; when asked, both phases, each
    power line equal to the decision its own GCC words give, the controller
    read only when the phase after ExitBootServices says ON, the SMMU lines
    exactly when asked, and no pending SError."""
    present = [line for line in lines if re.match(r'^(USB0|USB1|SMMU) ', line) or line.startswith('USB SERROR')]
    if not expect.get('usb'):
        return [f'USB lines on a run that did not ask for them: {present[0]}'] if present else []
    problems = []
    first = {m[1]: m for line in lines for m in [USB_GCC1.match(line)] if m}
    second = {m[1]: m for line in lines for m in [USB_GCC2.match(line)] if m}
    power = {m[1]: m[2] for line in lines for m in [USB_POWER.match(line)] if m}
    for phase in 'BA':
        if phase not in first or phase not in second or phase not in power:
            problems.append(f'USB phase {phase}: lines missing')
            continue
        want = usb_power(first[phase][2], first[phase][3], second[phase][3], second[phase][4])
        if power[phase] != want:
            problems.append(f'USB phase {phase}: power {power[phase]}, but its words say {want}')
    core = next((line for line in lines if line.startswith('USB0 CORE ')), None)
    read_lines = [line for line in lines if re.match(r'^USB0 (XHCI|DWC3|QSCRATCH) ', line)]
    if core is None:
        problems.append('no USB0 CORE line')
    elif power.get('A') != 'ON':
        if core != 'USB0 CORE NOT READ: POWER NOT ON' or read_lines:
            problems.append(f'the controller was read although power after ExitBootServices was '
                            f'{power.get("A")}: {core}')
    smmu = [line for line in lines if line.startswith('SMMU ')]
    if expect.get('smmu') and len(smmu) != 2:
        problems.append(f'usb-smmu asked but {len(smmu)} SMMU lines')
    if not expect.get('smmu') and smmu:
        problems.append('SMMU lines although usb-smmu was not asked')
    if any(line.startswith('SMMU STREAM 820 MATCH SMR 00 FFFFFFFF') for line in smmu):
        problems.append('an all-ones SMMU read was taken as a stream match')
    if 'USB SERROR NONE' not in lines:
        problems.append('the USB read left an SError pending, or no SERROR line')
    return problems


VHE_REGS = re.compile(r'^(VHEIN|VHEOUT) HCR=([0-9A-F]+) SCTLR=([0-9A-F]+) TCR=([0-9A-F]+)$')


def check_vhe_exit(lines, expect):
    """The "exit-vhe" lines: absent unless asked; when asked, the expected
    outcome, and for DONE the registers read back equal to the plan, E2H and
    TGE gone from HCR_EL2 (RW alone left), and E2H|TGE in the entry state."""
    want = expect.get('vhe_exit')
    got = next((line.split() for line in lines if line.startswith('VHEX ')), None)
    regs = {m[1]: m for line in lines for m in [VHE_REGS.match(line)] if m}
    if not want:
        return ['VHEX line on a run that did not ask for exit-vhe'] if got else []
    if not got or len(got) != 3:
        return ['exit-vhe asked but no well-formed VHEX line']
    problems = []
    if got[1] != want:
        problems.append(f'VHE exit {got[1]}, wanted {want}')
    if 'VHEIN' not in regs:
        problems.append('no VHEIN line')
    if want == 'DONE':
        if got[2] != 'PLAN=MATCH':
            problems.append(f'registers after the switch: {got[2]}, wanted PLAN=MATCH')
        hin = int(regs['VHEIN'][2], 16) if 'VHEIN' in regs else 0
        if not (hin >> 34) & 1 or not (hin >> 27) & 1:
            problems.append(f'VHEIN HCR {hin:#x} lacks E2H|TGE')
        if 'VHEOUT' not in regs:
            problems.append('no VHEOUT line')
        elif int(regs['VHEOUT'][2], 16) != 1 << 31:
            problems.append(f'VHEOUT HCR {regs["VHEOUT"][2]}, wanted RW alone (80000000)')
    elif 'VHEOUT' in regs:
        problems.append(f'VHEOUT line although the exit was {got[1]}')
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
    if expect.get('vhe_exit') == 'DONE':
        lines += ['VHEX DONE PLAN=MATCH',
                  'VHEIN HCR=408000000 SCTLR=30C5183D TCR=500803510',
                  'VHEIN2 TTBR0=BFFFF000 CPTR=33FF CNTHCTL=3',
                  'VHEOUT HCR=80000000 SCTLR=30C5183D TCR=80853510',
                  'VHEOUT2 TTBR0=BFFFF000 CPTR=33FF CNTHCTL=3']
    elif expect.get('vhe_exit'):
        lines += [f'VHEX {expect["vhe_exit"]} PLAN=-',
                  'VHEIN HCR=80000000 SCTLR=30C5183D TCR=80853510',
                  'VHEIN2 TTBR0=BFFFF000 CPTR=33FF CNTHCTL=3']
    if expect.get('usb'):
        for phase in 'BA':
            lines += [f'USB0 GCC {phase} GDSC UNMAPPED MASTER UNMAPPED SLEEP UNMAPPED',
                      f'USB0 GCC {phase} UTMI UNMAPPED AXI UNMAPPED CNOC UNMAPPED',
                      f'USB1 GCC {phase} GDSC UNMAPPED MASTER UNMAPPED',
                      f'USB0 POWER {phase} UNREAD']
        lines.append('USB0 CORE NOT READ: POWER NOT ON')
        if expect.get('smmu'):
            lines += ['SMMU SCR0 FFFFFFFF IDR0 FFFFFFFF', 'SMMU STREAM 820 NOT READ: ALL ONES']
        lines.append('USB SERROR NONE')
    if expect.get('thermal'):
        lines += ['TSENS GUARD (READ ONLY) . TRIPPED: READ AT 0C222000',
                  'TSENS TRIPPED CHECKED=58 UNMAPPED=0 ADMIT=1 POLLS=1 VALID=0',
                  'TSENSWHY REASON=READ AT=C222000 ESR=96000010 SERROR=0']
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
        if expect.get('thermal'):
            unmapped = (good.replace('TRIPPED: READ AT 0C222000', 'NOT MAPPED, NOTHING READ')
                        .replace('TSENS TRIPPED CHECKED=58 UNMAPPED=0 ADMIT=1 POLLS=1',
                                 'TSENS UNMAPPED CHECKED=58 UNMAPPED=58 ADMIT=0 POLLS=0')
                        .replace('REASON=READ AT=C222000 ESR=96000010', 'REASON=OK AT=C222000 ESR=0'))
            expect_result(f'{name} unmapped is fine too', unmapped, expect, True)
            expect_result(f'{name} no TSENS line', good.replace('TSENS TRIPPED', 'XSENS TRIPPED'),
                          expect, False)
            expect_result(f'{name} passed on QEMU', good.replace('TSENS TRIPPED', 'TSENS PASSED')
                          .replace('REASON=READ', 'REASON=OK'), expect, False)
            expect_result(f'{name} READ trip without a fault', good.replace('ESR=96000010', 'ESR=0'),
                          expect, False)
            expect_result(f'{name} tripped later', good.replace('POLLS=1 ', 'POLLS=3 '), expect, False)
            expect_result(f'{name} SError left', good.replace('SERROR=0', 'SERROR=1'), expect, False)
            expect_result(f'{name} words skipped', good.replace('CHECKED=58', 'CHECKED=57'),
                          expect, False)
            expect_result(f'{name} unmapped but read', unmapped.replace('POLLS=0', 'POLLS=1'),
                          expect, False)
        else:
            expect_result(f'{name} tsens unasked', good.replace(
                'EL2QUAL END', 'TSENS UNMAPPED CHECKED=58 UNMAPPED=58 ADMIT=0 POLLS=0 VALID=0\n'
                'EL2QUAL END'), expect, False)
        if expect.get('vhe_exit') == 'DONE':
            expect_result(f'{name} no VHEX', good.replace('VHEX DONE PLAN=MATCH\n', ''), expect, False)
            expect_result(f'{name} refused', good.replace('VHEX DONE', 'VHEX REFUSED-TCR'),
                          expect, False)
            expect_result(f'{name} plan differs', good.replace('PLAN=MATCH', 'PLAN=DIFF'),
                          expect, False)
            expect_result(f'{name} E2H left', good.replace('VHEOUT HCR=80000000',
                          'VHEOUT HCR=480000000'), expect, False)
            expect_result(f'{name} no VHEOUT', good.replace('VHEOUT HCR', 'VHEXXX HCR'),
                          expect, False)
            expect_result(f'{name} entry not VHE', good.replace('VHEIN HCR=408000000',
                          'VHEIN HCR=80000000'), expect, False)
        elif expect.get('vhe_exit'):
            expect_result(f'{name} switched anyway', good.replace(
                'EL2QUAL END', 'VHEOUT HCR=80000000 SCTLR=0 TCR=0\nEL2QUAL END'), expect, False)
            expect_result(f'{name} claimed DONE', good.replace(f'VHEX {expect["vhe_exit"]}',
                          'VHEX DONE'), expect, False)
        else:
            expect_result(f'{name} exit unasked', good.replace(
                'EL2QUAL END', 'VHEX DONE PLAN=MATCH\nEL2QUAL END'), expect, False)
        if expect.get('usb'):
            on = (good.replace('GDSC UNMAPPED MASTER UNMAPPED SLEEP UNMAPPED',
                               'GDSC 80000000 MASTER 00000001 SLEEP 00000001')
                  .replace('UTMI UNMAPPED AXI UNMAPPED CNOC UNMAPPED',
                           'UTMI 00000001 AXI 00000001 CNOC 00000001'))
            expect_result(f'{name} power disagrees with its words', on, expect, False)
            expect_result(f'{name} ON with its words', on.replace('POWER B UNREAD', 'POWER B ON')
                          .replace('POWER A UNREAD', 'POWER A ON')
                          .replace('USB0 CORE NOT READ: POWER NOT ON', 'USB0 CORE READ'), expect, True)
            off = on.replace('GDSC 80000000', 'GDSC 00000000', 1)
            expect_result(f'{name} GDSC off read as on', off.replace('POWER B UNREAD', 'POWER B ON')
                          .replace('POWER A UNREAD', 'POWER A ON'), expect, False)
            expect_result(f'{name} controller read while unread', good.replace(
                'USB0 CORE NOT READ: POWER NOT ON', 'USB0 CORE READ\nUSB0 DWC3 SNPSID 33313130 VER 0 GCTL 0'),
                expect, False)
            expect_result(f'{name} no before phase', good.replace('GCC B ', 'GCC X '), expect, False)
            expect_result(f'{name} SError left', good.replace('USB SERROR NONE', 'USB SERROR PENDING'),
                          expect, False)
            if expect.get('smmu'):
                expect_result(f'{name} all ones taken as a match', good.replace(
                    'SMMU STREAM 820 NOT READ: ALL ONES', 'SMMU STREAM 820 MATCH SMR 00 FFFFFFFF S2CR FFFFFFFF'),
                    expect, False)
                expect_result(f'{name} SMMU lines missing', good.replace('SMMU SCR0', 'XMMU SCR0'),
                              expect, False)
            else:
                expect_result(f'{name} SMMU unasked', good.replace(
                    'USB SERROR NONE', 'SMMU SCR0 0 IDR0 0\nSMMU STREAM 820 NO MATCH\nUSB SERROR NONE'),
                    expect, False)
        else:
            expect_result(f'{name} USB unasked', good.replace(
                'EL2QUAL END', 'USB0 POWER A UNREAD\nEL2QUAL END'), expect, False)
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
