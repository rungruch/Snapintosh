#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble the files for an el2qual USB stick (docs/GAOKUN3-QUALIFICATION.md).

  python3 tools/el2qual-usb.py stage1
  python3 tools/el2qual-usb.py stage1b --systemd-boot F --dtb F
  python3 tools/el2qual-usb.py stage2  --systemd-boot F --slbounce F --tcblaunch F
                                       (--dtb F | --always-switch)
  python3 tools/el2qual-usb.py thermal (the same inputs as stage2)
  python3 tools/el2qual-usb.py vhe-exit (the same inputs as stage2)

This only builds a directory tree, by default build/el2qual-usb/<stage>/.
It never writes to a disk or a mounted volume: copying the tree onto a
FAT32 stick is a step the operator does by hand, after reading the plan.
A destination inside the repository is accepted only under build/, which
is ignored, so tcblaunch.exe (a Microsoft binary) can never be committed.

  stage1   el2qual alone as \\EFI\\BOOT\\BOOTAA64.EFI, run at the firmware's
           level (EL1 on the gaokun3). No Secure Launch is attempted.
  stage1b  systemd-boot with the device tree and the stage 2 entries, but
           no slbounce: el2qual reports whether slbounce's gate would open.
  stage2   stage1b plus slbounce and tcblaunch.exe: the EL2 measurement.
  thermal  stage2's files with one entry only: the Phase 1 tsens read
           (load option "thermal"; docs/GAOKUN3-PHASE1-GUARDS.md).
  vhe-exit stage2's files with the two Phase 2 VHE-exit entries only (load
           option "exit-vhe"; docs/GAOKUN3-PHASE2.md).

Every input is checked before anything is written (PE machine and
subsystem, FDT header), and a manifest records the SHA-256 of each file
placed, so a photographed result can be tied to exactly what ran.
"""
import argparse
import hashlib
import json
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / 'build' / 'el2qual-usb'

IMAGE_FILE_MACHINE_ARM64 = 0xAA64
SUBSYSTEM_EFI_APPLICATION = 10
SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER = 11
SUBSYSTEM_EFI_RUNTIME_DRIVER = 12

# systemd-boot loads \EFI\systemd\drivers\*<arch>.efi only (drivers.c checks
# the suffix EFI_MACHINE_TYPE_NAME ".efi"); for AArch64 that is "aa64". The
# slbounce README's example name, slbouncea64.efi, does not end in it.
SLBOUNCE_NAME = 'slbounceaa64.efi'

# Run in this order, each adding one step, so a hang is attributable to the
# step the previous run did not take: the EL2 switch alone, then PAuth on
# the boot CPU, then the other CPUs via PSCI, then both. PAuth, the
# go/no-go, comes before PSCI so its answer cannot be lost to a secondary
# that never returns. systemd-boot sorts entries by file name.
ENTRIES = [
    ('el2qual-1-boot-cpu.conf', 'el2qual 1: boot CPU, no PAuth', 'nopac nosmp'),
    ('el2qual-2-boot-cpu-pauth.conf', 'el2qual 2: boot CPU and PAuth', 'nosmp'),
    ('el2qual-3-all-cpus.conf', 'el2qual 3: all CPUs, no PAuth', 'nopac'),
    ('el2qual-4-full.conf', 'el2qual 4: all CPUs and PAuth', ''),
]

# The Phase 1 tsens read (docs/GAOKUN3-PHASE1-GUARDS.md): the boot CPU alone
# and no PAuth, the steps run 9 already cleared, then the thermal stage. One
# entry, so nothing else on the stick can be chosen by mistake.
THERMAL_ENTRIES = [
    ('el2qual-5-thermal.conf', 'el2qual 5: tsens read only (boot CPU, no PAuth)',
     'nopac nosmp thermal'),
]

# The Phase 2 VHE exit (docs/GAOKUN3-PHASE2.md), in order. The first adds
# only the switch to steps run 9 cleared (boot CPU, no PAuth); the second
# measures everything in non-VHE EL2, PAuth and every CPU via PSCI. Only
# these two, so no stage2 entry can be picked by mistake.
VHE_EXIT_ENTRIES = [
    ('el2qual-6-vhe-exit.conf', 'el2qual 6: VHE exit, boot CPU, no PAuth', 'nopac nosmp exit-vhe'),
    ('el2qual-7-vhe-exit-full.conf', 'el2qual 7: VHE exit, all CPUs and PAuth', 'exit-vhe'),
]
STAGE_ENTRIES = {'stage1b': ENTRIES, 'stage2': ENTRIES, 'thermal': THERMAL_ENTRIES,
                 'vhe-exit': VHE_EXIT_ENTRIES}


class LayoutError(Exception):
    pass


def pe_info(path):
    """(machine, subsystem) of a PE image, or LayoutError if it is not one."""
    data = Path(path).read_bytes()
    if len(data) < 0x40 or data[:2] != b'MZ':
        raise LayoutError(f'{path}: not a PE image (no MZ header)')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe + 24 + 70 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise LayoutError(f'{path}: not a PE image (no PE signature)')
    machine = struct.unpack_from('<H', data, pe + 4)[0]
    subsystem = struct.unpack_from('<H', data, pe + 24 + 68)[0]
    return machine, subsystem


def require_pe(path, what, subsystems=None):
    machine, subsystem = pe_info(path)
    if machine != IMAGE_FILE_MACHINE_ARM64:
        raise LayoutError(f'{what} {path}: machine {machine:#06x}, not ARM64 (0xaa64)')
    if subsystems and subsystem not in subsystems:
        raise LayoutError(f'{what} {path}: PE subsystem {subsystem}, want one of '
                          f'{sorted(subsystems)}')


def require_dtb(path):
    data = Path(path).read_bytes()
    if len(data) < 40 or data[:4] != b'\xd0\x0d\xfe\xed':
        raise LayoutError(f'device tree {path}: no FDT magic')
    total = struct.unpack_from('>I', data, 4)[0]
    if total != len(data):
        raise LayoutError(f'device tree {path}: header totalsize {total}, file {len(data)} bytes')


MANIFEST = 'el2qual-usb-manifest.json'


def check_destination(out, replace=False):
    out = Path(out).resolve()
    if out == Path(out.anchor):
        raise LayoutError(f'{out}: refusing a filesystem root; copy the tree onto the '
                          'stick by hand')
    if out.is_relative_to(ROOT) and not out.is_relative_to(ROOT / 'build'):
        raise LayoutError(f'{out}: inside the repository but not under build/, where '
                          'tcblaunch.exe could be committed')
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        # Only a tree this script wrote may be replaced, never an arbitrary
        # directory (or a mounted stick) someone pointed --out at.
        if not replace or not (out / MANIFEST).is_file():
            raise LayoutError(f'{out}: exists and is not an empty directory'
                              + ('' if replace else ' (use --replace)')
                              + (' or a previous el2qual-usb tree' if replace else ''))
    return out


def entry_text(title, options, dtb_name):
    lines = [f'title {title}', 'efi /EFI/el2qual/el2qual.efi']
    if options:
        lines.append(f'options {options}')
    if dtb_name:
        lines.append(f'devicetree /dtb/{dtb_name}')
    return '\n'.join(lines) + '\n'


def build_layout(stage, out, efi, systemd_boot=None, slbounce=None, tcblaunch=None,
                 dtb=None, always_switch=False, replace=False):
    """Validate every input, then write the tree; returns the manifest."""
    if stage not in ('stage1', 'stage1b', 'stage2', 'thermal', 'vhe-exit'):
        raise LayoutError(f'unknown stage {stage}')
    switches = stage in ('stage2', 'thermal', 'vhe-exit')
    require_pe(efi, 'el2qual', {SUBSYSTEM_EFI_APPLICATION})
    if stage != 'stage1':
        if not systemd_boot:
            raise LayoutError(f'{stage} needs --systemd-boot')
        require_pe(systemd_boot, 'systemd-boot', {SUBSYSTEM_EFI_APPLICATION})
    if stage == 'stage1b' and not dtb:
        raise LayoutError('stage1b exists to check the device tree gate: it needs --dtb')
    if switches:
        if not slbounce or not tcblaunch:
            raise LayoutError(f'{stage} needs --slbounce and --tcblaunch')
        require_pe(slbounce, 'slbounce', {SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER,
                                          SUBSYSTEM_EFI_RUNTIME_DRIVER})
        require_pe(tcblaunch, 'tcblaunch.exe')
        # The gaokun3's firmware supplies no device tree, so without one
        # slbounce's zap-shader gate stays closed and it silently does not
        # switch: a wasted boot, not a dangerous one.
        if not dtb and not always_switch:
            raise LayoutError(f'{stage} needs --dtb, or --always-switch to state that '
                              'slbounce was built with SLBOUNCE_ALWAYS_SWITCH=1')
        if dtb and always_switch:
            raise LayoutError('give --dtb or --always-switch, not both')
    if stage == 'stage1' and any((systemd_boot, slbounce, tcblaunch, dtb, always_switch)):
        raise LayoutError('stage1 runs el2qual alone; it takes no other inputs')
    if dtb:
        require_dtb(dtb)

    out = check_destination(out, replace)
    if out.exists() and any(out.iterdir()):
        shutil.rmtree(out)

    placed = {}
    def place(src, rel):
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dest)
        placed[rel] = hashlib.sha256(dest.read_bytes()).hexdigest()

    def write(rel, text):
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(text.encode())
        placed[rel] = hashlib.sha256(dest.read_bytes()).hexdigest()

    out.mkdir(parents=True, exist_ok=True)
    if stage == 'stage1':
        place(efi, 'EFI/BOOT/BOOTAA64.EFI')
    else:
        place(systemd_boot, 'EFI/BOOT/BOOTAA64.EFI')
        place(efi, 'EFI/el2qual/el2qual.efi')
        dtb_name = Path(dtb).name if dtb else None
        if dtb:
            place(dtb, f'dtb/{dtb_name}')
        entries = STAGE_ENTRIES[stage]
        write('loader/loader.conf', f'timeout 15\ndefault {entries[0][0]}\neditor no\n')
        for name, title, options in entries:
            write(f'loader/entries/{name}', entry_text(title, options, dtb_name))
        if switches:
            place(slbounce, f'EFI/systemd/drivers/{SLBOUNCE_NAME}')
            # slbounce opens tcblaunch.exe from the root of its own volume.
            place(tcblaunch, 'tcblaunch.exe')
    manifest = {'stage': stage, 'always_switch': bool(always_switch), 'files': placed}
    (out / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('stage', choices=['stage1', 'stage1b', 'stage2', 'thermal', 'vhe-exit'])
    ap.add_argument('--out', help='destination directory (default build/el2qual-usb/<stage>)')
    ap.add_argument('--efi', help='a built el2qual.efi (default: build it now)')
    ap.add_argument('--llvm', help='passed to probes/el2qual/build.py')
    ap.add_argument('--systemd-boot', help='systemd-bootaa64.efi')
    ap.add_argument('--slbounce', help='slbounce.efi (an EFI boot service driver)')
    ap.add_argument('--tcblaunch', help='tcblaunch.exe, old enough for slbounce')
    ap.add_argument('--dtb', help='device tree whose Adreno zap-shader is disabled')
    ap.add_argument('--always-switch', action='store_true',
                    help='slbounce was built with SLBOUNCE_ALWAYS_SWITCH=1 (no --dtb)')
    ap.add_argument('--replace', action='store_true', help='replace an existing tree')
    a = ap.parse_args()
    efi = a.efi
    if not efi:
        sys.path.insert(0, str(ROOT / 'probes' / 'el2qual'))
        import build as el2qual_build
        efi = el2qual_build.build(llvm=a.llvm)
    out = Path(a.out) if a.out else DEFAULT_OUT / a.stage
    try:
        manifest = build_layout(a.stage, out, efi, a.systemd_boot, a.slbounce, a.tcblaunch,
                                a.dtb, a.always_switch, a.replace)
    except (LayoutError, OSError) as error:
        print(f'REFUSED: {error}')
        return 1
    print(f'{a.stage} layout in {out}')
    for rel, digest in manifest['files'].items():
        print(f'  {digest[:16]}  {rel}')
    print('Copy the whole tree onto an empty FAT32 USB stick; then follow '
          'docs/GAOKUN3-QUALIFICATION.md.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
