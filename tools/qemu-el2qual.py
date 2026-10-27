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
  max-vhe-usb-smmu-map  the same, with "usb-smmu-map": el2qual first adds its
               own Device map of the SMMU's 1 MiB window to its EL2 tables
               (pagemap.c). EDK2 already maps that range on virt, so the
               one SMMU MAP line must say NOT NEEDED
  max-vhe-usb-smmu-map-hole  the same with "emulate-map-hole": two test
               windows EDK2 leaves unmapped are mapped instead, the second
               cloning the table the first created, so both a new table and
               the swap of a valid entry with its break run under the
               hardware walk; the line must say OK CLONE KEPT 0/256 (TEST
               WINDOW), which also requires the first window to still pass
  max-vhe-usb-smmu-map-hole-e2h  the same without the VHE exit, so the
               tables are walked in the EL2&0 regime, with its own leaf bits
  max-vhe-usb-s2walk-stick  the gaokun3 stick entry's options ("usb-smmu-map
               usb-s2-walk"): virt has no stage-2 bank, so the walk must say
               NOT RUN: NO STAGE 2 and read nothing
  max-vhe-usb-s2walk  "usb-s2-walk" over a stage-2 table set el2qual builds
               for its own arena ("emulate-s2-tables"), laid out like bank
               126's, through the real AT checks, guarded loads and SError
               checks: every page identity read-write, and the first page's
               leaf at the arena's own address
  max-vhe-usb-s2walk-odd  the same with three pages changed: one moved, one
               invalid, one read-only, and the first odd page named
  max-vhe-usb-s2walk-device  the same tables with the root pointed at
               virt's PL011, a Device page: refused with nothing read
  max-vhe-usb-s2survey-stick  the gaokun3 stick entry 12's options ("usb-
               smmu-map usb-s2-survey"): NOT RUN: NO STAGE 2 for both passes
  max-vhe-usb-s2survey  "usb-s2-survey" over el2qual's own tables: the walk
               as max-vhe-usb-s2walk, then every level-1 entry and the
               level-2 table under the arena, each map character equal to
               what the checker derives from the arena address printed
  max-vhe-usb-s2survey-odd  the same over the odd tables, whose extra
               1 GiB and 2 MiB blocks must appear as I, M, L and I
  max-vhe-usb-s2survey-device  the root at virt's PL011: both passes
               refused with nothing read
  max-vhe-usb-s2fixed-stick  the gaokun3 stick entry 13's options ("usb-
               smmu-map usb-s2-fixed"): the walk NOT RUN: NO STAGE 2
               (FIXED), and UEFI's memory map over 0xC6200000-0xC85FFFFF,
               which virt's 1 GiB of RAM at 0x40000000 leaves with no entry
  max-vhe-usb-s2bypass-plan  the gaokun3 stick entry 14's options ("usb-
               smmu-map usb-s2-bypass-plan"): virt's SMMU reads all ones, so
               the plan has nothing measured to plan from and must say so,
               and must still end with the line saying nothing was written
  max-vhe-usb-s2fixed  "usb-s2-fixed" over el2qual's own tables, built for
               the fixed range 0xC8000000: 258 reads, every page identity,
               the first leaf at 0xC8000000; and the memory map over 36 MiB
               from 0x40000000, which must list well-formed entries in
               address order that all overlap it

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
    # The window map (pagemap.c). EDK2 maps virt's 0x15000000 as Device
    # already (PCIe MMIO), so nothing is written; the SMMU itself still
    # reads all ones, so no bank lines.
    'max-vhe-usb-smmu-map': dict(cpu='max', options='emulate-vhe exit-vhe usb-smmu-map',
                                 pac='Y', walk='Y', vhe=True, vhe_exit='DONE',
                                 usb=True, smmu=True, smmumap='NOT NEEDED',
                                 features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    # The same through the writing path, on two test windows EDK2 leaves absent.
    'max-vhe-usb-smmu-map-hole': dict(cpu='max', options='emulate-vhe exit-vhe usb-smmu-map emulate-map-hole',
                                      pac='Y', walk='Y', vhe=True, vhe_exit='DONE',
                                      usb=True, smmu=True,
                                      smmumap=r'OK CLONE KEPT 0/256 \(TEST WINDOW\)',
                                      smmumap_sample='OK CLONE KEPT 0/256 (TEST WINDOW)',
                                      features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    # The same left in slbounce's E2H|TGE state: the EL2&0 regime, whose leaf
    # permission and execute-never bits differ from the EL2 regime's.
    'max-vhe-usb-smmu-map-hole-e2h': dict(cpu='max', options='emulate-vhe usb-smmu-map emulate-map-hole',
                                          pac='Y', walk='Y', vhe=True, usb=True, smmu=True,
                                          smmumap=r'OK CLONE KEPT 0/256 \(TEST WINDOW\)',
                                          smmumap_sample='OK CLONE KEPT 0/256 (TEST WINDOW)',
                                          features=dict.fromkeys(FEATURES, True),
                                          routes=('CPU OK', 'CPU OK')),
    # The stage-2 walk (s2walk.c). s2walk is a regex the walk's lines, joined
    # by newlines, must fullmatch; s2walk_sample is a log that does.
    'max-vhe-usb-s2walk-stick': dict(cpu='max', options='emulate-vhe exit-vhe usb-smmu-map usb-s2-walk',
                                     pac='Y', walk='Y', vhe=True, vhe_exit='DONE',
                                     usb=True, smmu=True, smmumap='NOT NEEDED',
                                     s2walk=r'SMMU WALK NOT RUN: NO STAGE 2',
                                     features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2walk': dict(cpu='max', options='emulate-vhe exit-vhe usb-s2-walk emulate-s2-tables',
                               pac='Y', walk='Y', vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                               s2walk=(r'SMMU WALK ARENA [0-9A-F]{8} READS (258|259|260) \(TEST TABLES\)\n'
                                       r'SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0\n'
                                       r'SMMU WALK LEAF L3 [0-9A-F]{16}'),
                               s2walk_sample=('SMMU WALK ARENA 7E600000 READS 258 (TEST TABLES)\n'
                                              'SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0\n'
                                              'SMMU WALK LEAF L3 000000007E6007FF'),
                               features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2walk-odd': dict(cpu='max', options='emulate-vhe exit-vhe usb-s2-walk emulate-s2-odd',
                                   pac='Y', walk='Y', vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                                   s2walk=(r'SMMU WALK ARENA [0-9A-F]{8} READS (258|259|260) \(TEST TABLES\)\n'
                                           r'SMMU WALK RW 253 LIMITED 1 MOVED 1 FAULTS 1\n'
                                           r'SMMU WALK LEAF L3 [0-9A-F]{16}\n'
                                           r'SMMU WALK ODD [0-9A-F]{8} L3 [0-9A-F]{16}'),
                                   s2walk_odd=0x1000,
                                   s2walk_sample=('SMMU WALK ARENA 7E600000 READS 258 (TEST TABLES)\n'
                                                  'SMMU WALK RW 253 LIMITED 1 MOVED 1 FAULTS 1\n'
                                                  'SMMU WALK LEAF L3 000000007E6007FF\n'
                                                  'SMMU WALK ODD 7E601000 L3 000000007E8017FF'),
                                   features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    # The survey (s2walk.c). s2survey names what the checker expects:
    # 'tables' or 'odd' (derived from the walk's arena), or a list of exact
    # lines (regexes).
    'max-vhe-usb-s2survey-stick': dict(cpu='max', options='emulate-vhe exit-vhe usb-smmu-map usb-s2-survey',
                                       pac='Y', walk='Y', vhe=True, vhe_exit='DONE',
                                       usb=True, smmu=True, smmumap='NOT NEEDED',
                                       s2walk=r'SMMU WALK NOT RUN: NO STAGE 2',
                                       s2survey=[r'SMMU SURVEY NOT RUN: NO STAGE 2'],
                                       features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2survey': dict(cpu='max', options='emulate-vhe exit-vhe usb-s2-survey emulate-s2-tables',
                                 pac='Y', walk='Y', vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                                 s2walk=(r'SMMU WALK ARENA [0-9A-F]{8} READS (258|259|260) \(TEST TABLES\)\n'
                                         r'SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0\n'
                                         r'SMMU WALK LEAF L3 [0-9A-F]{16}'),
                                 s2walk_sample=('SMMU WALK ARENA 7E600000 READS 258 (TEST TABLES)\n'
                                                'SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0\n'
                                                'SMMU WALK LEAF L3 000000007E6007FF'),
                                 s2survey='tables',
                                 features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2survey-odd': dict(cpu='max', options='emulate-vhe exit-vhe usb-s2-survey emulate-s2-odd',
                                     pac='Y', walk='Y', vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                                     s2walk=(r'SMMU WALK ARENA [0-9A-F]{8} READS (258|259|260) \(TEST TABLES\)\n'
                                             r'SMMU WALK RW 253 LIMITED 1 MOVED 1 FAULTS 1\n'
                                             r'SMMU WALK LEAF L3 [0-9A-F]{16}\n'
                                             r'SMMU WALK ODD [0-9A-F]{8} L3 [0-9A-F]{16}'),
                                     s2walk_odd=0x1000,
                                     s2walk_sample=('SMMU WALK ARENA 7E600000 READS 258 (TEST TABLES)\n'
                                                    'SMMU WALK RW 253 LIMITED 1 MOVED 1 FAULTS 1\n'
                                                    'SMMU WALK LEAF L3 000000007E6007FF\n'
                                                    'SMMU WALK ODD 7E601000 L3 000000007E8017FF'),
                                     s2survey='odd',
                                     features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2survey-device': dict(cpu='max', options='emulate-vhe exit-vhe usb-s2-survey emulate-s2-device',
                                        pac='Y', walk='Y', vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                                        s2walk=(r'SMMU WALK ARENA [0-9A-F]{8} READS 0 \(TEST TABLES\)\n'
                                                r'SMMU WALK REFUSED: TABLE 0000000009000000 PAR [0-9A-F]{16}'),
                                        s2walk_sample=('SMMU WALK ARENA 7E600000 READS 0 (TEST TABLES)\n'
                                                       'SMMU WALK REFUSED: TABLE 0000000009000000 PAR 0000000009000000'),
                                        s2survey=[r'SMMU SURVEY READS 0 \(TEST TABLES\)',
                                                  r'SMMU SURVEY REFUSED: TABLE 0000000009000000 PAR [0-9A-F]{16}'],
                                        features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    # The candidate region (usb-s2-fixed). region is a list of exact lines
    # (regexes), or 'test' for the structural checks on QEMU's stand-in.
    # The S2CR bypass plan (s2bypass.c). bypass is the exact lines (regexes).
    'max-vhe-usb-s2bypass-plan': dict(cpu='max', options='emulate-vhe exit-vhe usb-smmu-map usb-s2-bypass-plan',
                                      pac='Y', walk='Y', vhe=True, vhe_exit='DONE',
                                      usb=True, smmu=True, smmumap='NOT NEEDED',
                                      bypass=[r'SMMU BYPASS NOT MEASURED',
                                              r'SMMU BYPASS PLAN ONLY, NOTHING WRITTEN'],
                                      features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2fixed-stick': dict(cpu='max', options='emulate-vhe exit-vhe usb-smmu-map usb-s2-fixed',
                                      pac='Y', walk='Y', vhe=True, vhe_exit='DONE',
                                      usb=True, smmu=True, smmumap='NOT NEEDED',
                                      s2walk=r'SMMU WALK NOT RUN: NO STAGE 2 \(FIXED\)',
                                      s2walk_sample='SMMU WALK NOT RUN: NO STAGE 2 (FIXED)',
                                      region=[r'UEFI MAP C6200000-C8600000 ENTRIES 0'],
                                      features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2fixed': dict(cpu='max', options='emulate-vhe exit-vhe usb-s2-fixed emulate-s2-tables',
                                pac='Y', walk='Y', vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                                s2walk=(r'SMMU WALK ARENA C8000000 READS 258 \(TEST TABLES\) \(FIXED\)\n'
                                        r'SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0\n'
                                        r'SMMU WALK LEAF L3 00000000C80007FF'),
                                s2walk_sample=('SMMU WALK ARENA C8000000 READS 258 (TEST TABLES) (FIXED)\n'
                                               'SMMU WALK RW 256 LIMITED 0 MOVED 0 FAULTS 0\n'
                                               'SMMU WALK LEAF L3 00000000C80007FF'),
                                region='test',
                                features=dict.fromkeys(FEATURES, True), routes=('CPU OK', 'CPU OK')),
    'max-vhe-usb-s2walk-device': dict(cpu='max', options='emulate-vhe exit-vhe usb-s2-walk emulate-s2-device',
                                      pac='Y', walk='Y', vhe=True, vhe_exit='DONE', usb=True, smmu=True,
                                      s2walk=(r'SMMU WALK ARENA [0-9A-F]{8} READS 0 \(TEST TABLES\)\n'
                                              r'SMMU WALK REFUSED: TABLE 0000000009000000 PAR [0-9A-F]{16}'),
                                      s2walk_sample=('SMMU WALK ARENA 7E600000 READS 0 (TEST TABLES)\n'
                                                     'SMMU WALK REFUSED: TABLE 0000000009000000 PAR 0000000009000000'),
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
    smmu = [line for line in smmu
            if not line.startswith(('SMMU CB', 'SMMU ID1', 'SMMU S2 ', 'SMMU MAP', 'SMMU WALK',
                                    'SMMU SURVEY', 'SMMU BYPASS'))]
    if expect.get('smmu') and len(smmu) != 2:
        problems.append(f'usb-smmu asked but {len(smmu)} SMMU lines')
    if not expect.get('smmu') and smmu:
        problems.append('SMMU lines although usb-smmu was not asked')
    # The window map: exactly one well-formed line, exactly when asked, with
    # the outcome this configuration must reach.
    maplines = [line for line in lines if line.startswith('SMMU MAP ')]
    MAP_LINE = (r'^SMMU MAP (NOT NEEDED|OK (CLONE|NEW TABLE) KEPT \d+/256|'
                r'REFUSED: (REGIME|DEVICE|ALLOC|STRUCTURE [0-9A-F]{16}|VERIFY \d+/256))'
                r'( \(TEST WINDOW\))?$')
    if expect.get('smmumap'):
        if len(maplines) != 1:
            problems.append(f'usb-smmu-map asked but {len(maplines)} SMMU MAP lines')
        elif not re.match(MAP_LINE, maplines[0]):
            problems.append(f'malformed SMMU MAP line: {maplines[0]}')
        elif not re.fullmatch(expect['smmumap'], maplines[0].removeprefix('SMMU MAP ')):
            problems.append(f'{maplines[0]}, not SMMU MAP {expect["smmumap"]}')
    elif maplines:
        problems.append('an SMMU MAP line although usb-smmu-map was not asked')
    if any(line.startswith('SMMU STREAM 820 MATCH SMR 00 FFFFFFFF') for line in smmu):
        problems.append('an all-ones SMMU read was taken as a stream match')
    # The context bank: read only after a match, and only through a
    # translating S2CR (TYPE, bits 17:16, zero).
    match = next((m for line in lines for m in [re.match(
        r'^SMMU STREAM 820 MATCH SMR [0-9A-F]{2} [0-9A-F]{8} S2CR ([0-9A-F]{8})$', line)] if m), None)
    bank = [line for line in lines if line.startswith(('SMMU CB', 'SMMU ID1'))]
    stage2 = [line for line in lines if line.startswith('SMMU S2 ')]
    cbar = next((m for line in bank for m in [re.match(r'^SMMU CB[0-9A-F]{2} CBAR ([0-9A-F]{8}) ', line)] if m), None)
    nested = cbar is not None and (int(cbar[1], 16) >> 16) & 3 == 3
    if stage2 and not nested:
        problems.append(f'stage-2 lines without a read, nested bank: {stage2[0]}')
    if nested and not stage2:
        problems.append('a nested bank without its stage-2 lines')
    if not match and bank:
        problems.append(f'context-bank lines without a stream match: {bank[0]}')
    if match:
        translate = (int(match[1], 16) >> 16) & 3 == 0
        if not bank:
            problems.append('a stream match without its context-bank lines')
        elif not translate and bank != ['SMMU CB NOT TRANSLATED']:
            problems.append(f'a bypassing or faulting S2CR, yet the bank was read: {bank}')
        elif translate and 'SMMU CB NOT TRANSLATED' in bank:
            problems.append('a translating S2CR reported as not translated')
    problems += check_s2walk(lines, expect)
    problems += check_s2survey(lines, expect)
    problems += check_region(lines, expect)
    problems += check_bypass(lines, expect)
    if 'USB SERROR NONE' not in lines:
        problems.append('the USB read left an SError pending, or no SERROR line')
    return problems


def check_s2walk(lines, expect):
    """The walk's lines: present exactly when asked, matching this
    configuration's outcome, and consistent: at most 264 reads, a page count
    of 256, and the first page's leaf at the arena itself when every page
    is identity read-write."""
    walk = [line for line in lines if line.startswith('SMMU WALK ')]
    if not expect.get('s2walk'):
        return [f'SMMU WALK lines although usb-s2-walk was not asked: {walk[0]}'] if walk else []
    text = '\n'.join(walk)
    if not re.fullmatch(expect['s2walk'], text):
        return [f'the walk said {walk!r}, not {expect["s2walk"]!r}']
    problems = []
    arena = re.match(r'^SMMU WALK ARENA ([0-9A-F]{8}) READS (\d+)', walk[0])
    if arena and int(arena[2]) > 264:
        problems.append(f'{arena[2]} descriptor reads, over the walk\'s bound')
    counts = next((m for line in walk for m in [re.match(
        r'^SMMU WALK RW (\d+) LIMITED (\d+) MOVED (\d+) FAULTS (\d+)$', line)] if m), None)
    if counts and sum(int(c) for c in counts.groups()) != 256:
        problems.append(f'page classes that do not add up to 256: {counts[0]}')
    leaf = next((m for line in walk for m in [re.match(r'^SMMU WALK LEAF L(\d) ([0-9A-F]{16})$', line)] if m), None)
    if arena and leaf:
        size = 1 << (12 + 9 * (3 - int(leaf[1])))
        oa = int(leaf[2], 16) & 0x0000fffffffff000 & ~(size - 1)
        if counts and counts[1] == '256' and oa != int(arena[1], 16) & ~(size - 1):
            problems.append(f'every page identity, but the first leaf maps {oa:X}, not the arena')
    odd = next((m for line in walk for m in [re.match(r'^SMMU WALK ODD ([0-9A-F]{8}) ', line)] if m), None)
    if 's2walk_odd' in expect and (not arena or not odd or
                                   int(odd[1], 16) != int(arena[1], 16) + expect['s2walk_odd']):
        problems.append(f'the first odd page is not the arena\'s page at +{expect["s2walk_odd"]:X}')
    return problems


def check_bypass(lines, expect):
    """The bypass plan's lines: present exactly when asked, matching this
    configuration's outcome, and always ending with the line that says
    nothing was written, since that is the point of the stage."""
    bypass = [line for line in lines if line.startswith('SMMU BYPASS ')]
    want = expect.get('bypass')
    if not want:
        return [f'SMMU BYPASS lines although usb-s2-bypass-plan was not asked: {bypass[0]}'] if bypass else []
    if len(bypass) != len(want) or not all(re.fullmatch(w, line) for w, line in zip(want, bypass)):
        return [f'the bypass plan said {bypass!r}, not {want!r}']
    if bypass[-1] != 'SMMU BYPASS PLAN ONLY, NOTHING WRITTEN':
        return [f'the bypass plan does not end by saying nothing was written: {bypass[-1]}']
    return []


REGION_ENTRY = re.compile(r'^UEFI MAP ([0-9A-F]{10}) ([0-9A-F]{10}) (RESERVED|LDR CODE|LDR DATA|BS CODE|'
                          r'BS DATA|RT CODE|RT DATA|FREE|UNUSABLE|ACPI RECL|ACPI NVS|MMIO|MMIO PORT|'
                          r'PAL CODE|PERSIST|T[0-9A-F]{8}) [0-9A-F]{16}$')


def check_region(lines, expect):
    """The candidate region's memory-map lines: present exactly when asked;
    exact on the gaokun3's range; on QEMU's stand-in, a header with at least
    one entry, then that many well-formed entries (at most eight, then an
    AND MORE line), in address order, each overlapping the range."""
    region = [line for line in lines if line.startswith('UEFI MAP ')]
    want = expect.get('region')
    if not want:
        return [f'UEFI MAP lines although usb-s2-fixed was not asked: {region[0]}'] if region else []
    if want != 'test':
        if len(region) != len(want) or not all(re.fullmatch(w, line) for w, line in zip(want, region)):
            return [f'the memory map said {region!r}, not {want!r}']
        return []
    head = re.fullmatch(r'UEFI MAP ([0-9A-F]{8})-([0-9A-F]{8}) ENTRIES (\d+) \(TEST RANGE\)', region[0]) if region else None
    if not head:
        return [f'no test-range memory map header: {region[:1]!r}']
    base, end, count = int(head[1], 16), int(head[2], 16), int(head[3])
    shown = min(count, 8)
    problems = []
    if base != 0x40000000 or end - base != 0x2400000 or count < 1:
        problems.append(f'the test range header is wrong: {region[0]}')
    more = [f'UEFI MAP AND {count - shown} MORE'] if count > shown else []
    if len(region) != 1 + shown + len(more) or region[1 + shown:] != more:
        problems.append(f'{len(region) - 1} lines for {count} entries: {region!r}')
    starts = []
    for line in region[1:1 + shown]:
        m = REGION_ENTRY.match(line)
        if not m:
            problems.append(f'a malformed memory map entry: {line}')
            continue
        a, b = int(m[1], 16), int(m[2], 16)
        if not (a < end and b > base and b > a):
            problems.append(f'an entry outside the range: {line}')
        starts.append(a)
    if starts != sorted(starts):
        problems.append('memory map entries out of address order')
    return problems


def survey_lines(arena, odd, table):
    """The survey lines el2qual's own test tables must give for arena
    (s2walk.c's el2q_s2w_build_test), as regexes; table is the level-2
    table's address, or None to accept any 16 hex digits there."""
    last = arena + 0x100000 - 1
    gib = arena >> 30
    l1 = ['.'] * 64
    for a in (arena, last):
        l1[a >> 30] = 'T'
    if odd and arena >> 30 and last >> 30:
        l1[0] = 'I'
    l2 = ['.'] * 512
    for a in (arena, last):
        if a >> 30 == gib:
            l2[(a >> 21) & 511] = 'T'
    if odd:
        for e, c in enumerate('MLI'):
            ipa = (gib << 30) + (e << 21)
            if ipa >> 21 not in (arena >> 21, last >> 21):
                l2[e] = c
    rows = [''.join(l2[r * 32:(r + 1) * 32]) for r in range(16)]
    at = f'{table:016X}' if table is not None else '[0-9A-F]{16}'
    desc = f'{table | 3:016X}' if table is not None else '[0-9A-F]{16}'
    # The map rows are compared as regexes, where '.' is not literal.
    out = [r'SMMU SURVEY READS 576 \(TEST TABLES\)',
           'SMMU SURVEY L1 000 ' + re.escape(''.join(l1[:32])),
           'SMMU SURVEY L1 020 ' + re.escape(''.join(l1[32:])),
           f'SMMU SURVEY GIB {gib:02X} {desc}',
           f'SMMU SURVEY L2 AT {at} EMPTY ROWS {sum(r == "." * 32 for r in rows)}']
    out += [f'SMMU SURVEY L2 {(gib << 30) + (r * 32 << 21):08X} {re.escape(rows[r])}'
            for r in range(16) if rows[r] != '.' * 32]
    return out


def check_s2survey(lines, expect):
    """The survey's lines: present exactly when asked, and, over el2qual's
    own tables, exactly the map those tables give for the arena the walk
    printed, with the arena's level-1 descriptor pointing at the level-2
    table surveyed."""
    survey = [line for line in lines if line.startswith('SMMU SURVEY ')]
    want = expect.get('s2survey')
    if not want:
        return [f'SMMU SURVEY lines although usb-s2-survey was not asked: {survey[0]}'] if survey else []
    if want in ('tables', 'odd'):
        arena = next((m for line in lines for m in [re.match(r'^SMMU WALK ARENA ([0-9A-F]{8}) ', line)] if m), None)
        if not arena:
            return ['a survey over test tables without the walk\'s arena line']
        want = survey_lines(int(arena[1], 16), want == 'odd', None)
        gib = next((m for line in survey for m in [re.match(r'^SMMU SURVEY GIB [0-9A-F]{2} ([0-9A-F]{16})$', line)] if m), None)
        at = next((m for line in survey for m in [re.match(r'^SMMU SURVEY L2 AT ([0-9A-F]{16}) ', line)] if m), None)
        if gib and at and int(gib[1], 16) != int(at[1], 16) | 3:
            return [f'the arena\'s level-1 descriptor {gib[1]} is not a table at {at[1]}']
    if len(survey) != len(want) or not all(re.fullmatch(w, line) for w, line in zip(want, survey)):
        return [f'the survey said {survey!r}, not {want!r}']
    return []


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
        if expect.get('smmumap'):
            lines.append(f'SMMU MAP {expect.get("smmumap_sample", expect["smmumap"])}')
        if expect.get('smmu'):
            lines += ['SMMU SCR0 FFFFFFFF IDR0 FFFFFFFF', 'SMMU STREAM 820 NOT READ: ALL ONES']
        if expect.get('s2walk'):
            lines += expect.get('s2walk_sample', expect['s2walk']).split('\n')
        if expect.get('s2survey') in ('tables', 'odd'):
            lines += [line.replace('\\', '') for line in
                      survey_lines(0x7E600000, expect['s2survey'] == 'odd', 0x40001000)]
        elif expect.get('s2survey'):
            lines += [line.replace('\\', '').replace('[0-9A-F]{16}', '0000000009000B00')
                      for line in expect['s2survey']]
        if expect.get('bypass'):
            lines += expect['bypass']
        if expect.get('region') == 'test':
            lines += ['UEFI MAP 40000000-42400000 ENTRIES 2 (TEST RANGE)',
                      'UEFI MAP 0040000000 0040100000 BS DATA 000000000000000F',
                      'UEFI MAP 0040100000 0047C00000 FREE 000000000000000F']
        elif expect.get('region'):
            lines += expect['region']
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
                expect_result(f'{name} bank lines without a match', good.replace(
                    'USB SERROR NONE', 'SMMU CB NOT TRANSLATED\nUSB SERROR NONE'), expect, False)
                matched = good.replace('SMMU SCR0 FFFFFFFF IDR0 FFFFFFFF', 'SMMU SCR0 00240406 IDR0 7C017ED4')
                bank = ('SMMU STREAM 820 MATCH SMR 06 80000820 S2CR 00000006\n'
                        'SMMU ID1 60000020 CB06 AT 15086000\n'
                        'SMMU CB06 CBAR 00010100 CBA2R 00000001 SCTLR 00001067\n'
                        'SMMU CB06 TCR 80803520 TCR2 00000005\n'
                        'SMMU CB06 TTBR0 00060000A1B2C000 MAIR0 0004FF44 MAIR1 00000000')
                matched = matched.replace('SMMU STREAM 820 NOT READ: ALL ONES', bank)
                expect_result(f'{name} a matched, translated, read bank', matched, expect, True)
                expect_result(f'{name} a match without its bank', matched.replace(
                    bank, bank.split('\n')[0]), expect, False)
                expect_result(f'{name} a bypass S2CR with a bank read', matched.replace(
                    'S2CR 00000006', 'S2CR 00010006'), expect, False)
                expect_result(f'{name} a translating S2CR called not translated', matched.replace(
                    bank, bank.split('\n')[0] + '\nSMMU CB NOT TRANSLATED'), expect, False)
                expect_result(f'{name} stage-2 lines without a nested bank', matched.replace(
                    'MAIR1 00000000', 'MAIR1 00000000\nSMMU S2 CB7E SCTLR 00000061 TCR 80023559'),
                    expect, False)
                nested_log = matched.replace('CBAR 00010100', 'CBAR 00037E03')
                expect_result(f'{name} a nested bank without its stage 2', nested_log, expect, False)
                expect_result(f'{name} a nested bank with its stage 2', nested_log.replace(
                    'MAIR1 00000000', 'MAIR1 00000000\nSMMU S2 CB7E AT 150FE000 CBAR 00000003 CBA2R 00000001\n'
                    'SMMU S2 CB7E SCTLR 00000061 TCR 80023559\nSMMU S2 CB7E TTBR0 000300009F700000'),
                    expect, True)
                if expect.get('smmumap'):
                    mapline = f'SMMU MAP {expect.get("smmumap_sample", expect["smmumap"])}'
                    expect_result(f'{name} map line missing',
                                  good.replace(mapline + '\n', ''), expect, False)
                    expect_result(f'{name} map line malformed',
                                  good.replace(mapline, 'SMMU MAP MAYBE'), expect, False)
                    expect_result(f'{name} map line twice',
                                  good.replace(mapline, mapline + '\n' + mapline), expect, False)
                    expect_result(f'{name} map refused', good.replace(
                        mapline, 'SMMU MAP REFUSED: STRUCTURE 0000000000000001'), expect, False)
                    if mapline.endswith('(TEST WINDOW)'):
                        expect_result(f'{name} test window not named', good.replace(
                            mapline, mapline.removesuffix(' (TEST WINDOW)')), expect, False)
                        expect_result(f'{name} nothing written', good.replace(
                            mapline, 'SMMU MAP NOT NEEDED (TEST WINDOW)'), expect, False)
                    else:
                        expect_result(f'{name} written where nothing was needed', good.replace(
                            mapline, 'SMMU MAP OK CLONE KEPT 3/256'), expect, False)
                else:
                    expect_result(f'{name} map line unasked', good.replace(
                        'SMMU SCR0', 'SMMU MAP OK\nSMMU SCR0'), expect, False)
                if expect.get('s2walk'):
                    sample = expect.get('s2walk_sample', expect['s2walk'])
                    first = sample.split('\n')[0]
                    expect_result(f'{name} walk missing', good.replace(sample + '\n', ''), expect, False)
                    expect_result(f'{name} walk refused', good.replace(
                        sample, 'SMMU WALK NOT RUN: FORMAT'), expect, False)
                    expect_result(f'{name} walk stopped by an SError', good.replace(
                        sample, 'SMMU WALK ARENA 7E600000 READS 3\n'
                        'SMMU WALK SERROR PENDING AFTER 000000007E7FF008'), expect, False)
                    if '(TEST TABLES)' in first:
                        expect_result(f'{name} test tables not named', good.replace(
                            first, first.replace(' (TEST TABLES)', '')), expect, False)
                        expect_result(f'{name} too many reads', good.replace(
                            first, re.sub(r'READS \d+', 'READS 265', first)), expect, False)
                    if 'RW 256 ' in sample:
                        expect_result(f'{name} first leaf elsewhere', re.sub(
                            r'(SMMU WALK LEAF L3 [0-9A-F]{10})([0-9A-F])', lambda m: m[1] + '%X' % (int(m[2], 16) ^ 2),
                            good), expect, False)
                        expect_result(f'{name} pages not adding up', good.replace(
                            'RW 256 LIMITED 0', 'RW 255 LIMITED 0'), expect, False)
                    if 's2walk_odd' in expect:
                        expect_result(f'{name} the wrong odd page', good.replace(
                            'ODD 7E601000', 'ODD 7E602000'), expect, False)
                if expect.get('bypass'):
                    expect_result(f'{name} bypass plan missing', '\n'.join(
                        line for line in good.split('\n') if not line.startswith('SMMU BYPASS ')),
                        expect, False)
                    expect_result(f'{name} bypass plan different', good.replace(
                        'SMMU BYPASS NOT MEASURED', 'SMMU BYPASS REFUSED: S2CR TYPE'), expect, False)
                    expect_result(f'{name} nothing-written line dropped', good.replace(
                        '\nSMMU BYPASS PLAN ONLY, NOTHING WRITTEN', ''), expect, False)
                    expect_result(f'{name} a write reported', good.replace(
                        'SMMU BYPASS PLAN ONLY, NOTHING WRITTEN',
                        'SMMU BYPASS WROTE 00010006'), expect, False)
                else:
                    expect_result(f'{name} bypass plan unasked', good.replace(
                        'USB SERROR NONE', 'SMMU BYPASS NOT MEASURED\nUSB SERROR NONE'), expect, False)
                if expect.get('region'):
                    region = [line for line in good.split('\n') if line.startswith('UEFI MAP ')]
                    expect_result(f'{name} memory map missing', '\n'.join(
                        line for line in good.split('\n') if not line.startswith('UEFI MAP ')), expect, False)
                    expect_result(f'{name} memory map unavailable', good.replace(
                        '\n'.join(region), 'UEFI MAP NOT AVAILABLE'), expect, False)
                    if expect['region'] == 'test':
                        expect_result(f'{name} entries out of order', good.replace(
                            region[1] + '\n' + region[2], region[2] + '\n' + region[1]), expect, False)
                        expect_result(f'{name} an entry outside the range', good.replace(
                            'UEFI MAP 0040000000 0040100000', 'UEFI MAP 0050000000 0050100000'), expect, False)
                        expect_result(f'{name} an entry count that disagrees', good.replace(
                            'ENTRIES 2 (TEST RANGE)', 'ENTRIES 3 (TEST RANGE)'), expect, False)
                        expect_result(f'{name} test range not named', good.replace(
                            ' (TEST RANGE)', ''), expect, False)
                    else:
                        expect_result(f'{name} memory map not empty', good.replace(
                            'ENTRIES 0', 'ENTRIES 1\nUEFI MAP 00C6200000 00C71A0000 RESERVED 0000000000000001'),
                            expect, False)
                else:
                    expect_result(f'{name} memory map unasked', good.replace(
                        'USB SERROR NONE', 'UEFI MAP NOT AVAILABLE\nUSB SERROR NONE'), expect, False)
                if expect.get('s2survey'):
                    first = next(line for line in good.split('\n') if line.startswith('SMMU SURVEY '))
                    expect_result(f'{name} survey missing', '\n'.join(
                        line for line in good.split('\n') if not line.startswith('SMMU SURVEY ')),
                        expect, False)
                    expect_result(f'{name} survey refused', good.replace(
                        first, 'SMMU SURVEY NOT RUN: FORMAT'), expect, False)
                    if expect['s2survey'] in ('tables', 'odd'):
                        expect_result(f'{name} a level-1 entry wrong', re.sub(
                            r'SMMU SURVEY L1 000 (.)', r'SMMU SURVEY L1 000 X', good), expect, False)
                        expect_result(f'{name} an empty-row count wrong', re.sub(
                            r'EMPTY ROWS (\d+)', lambda m: f'EMPTY ROWS {int(m[1]) + 1}', good), expect, False)
                        expect_result(f'{name} level 1 pointing elsewhere', good.replace(
                            'GIB 01 0000000040001003', 'GIB 01 0000000040002003'), expect, False)
                        expect_result(f'{name} a level-2 row missing', re.sub(
                            r'SMMU SURVEY L2 [0-9A-F]{8} .*\n', '', good, count=1), expect, False)
                else:
                    expect_result(f'{name} survey unasked', good.replace(
                        'USB SERROR NONE', 'SMMU SURVEY NOT RUN: NO STAGE 2\nUSB SERROR NONE'), expect, False)
                if not expect.get('s2walk'):
                    expect_result(f'{name} walk unasked', good.replace(
                        'USB SERROR NONE', 'SMMU WALK NOT RUN: NO STAGE 2\nUSB SERROR NONE'), expect, False)
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
