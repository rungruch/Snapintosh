#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble the files for an el2qual USB stick (docs/GAOKUN3-QUALIFICATION.md).

  python3 tools/el2qual-usb.py stage1
  python3 tools/el2qual-usb.py stage1b --systemd-boot F --dtb F
  python3 tools/el2qual-usb.py stage2  --systemd-boot F --slbounce F --tcblaunch F
                                       (--dtb F | --always-switch)
  python3 tools/el2qual-usb.py thermal (the same inputs as stage2)
  python3 tools/el2qual-usb.py vhe-exit (the same inputs as stage2)
  python3 tools/el2qual-usb.py usb-read (the same inputs as stage2)
  python3 tools/el2qual-usb.py usb-inventory --systemd-boot F --usb-probe F
  python3 tools/el2qual-usb.py q1n1-foothold --q1n1 F (and stage2's other inputs)
  python3 tools/el2qual-usb.py vmapple-h1 --systemd-boot F --q1n1 F --dtb F --bundle DIR
  python3 tools/el2qual-usb.py vmapple-h2 --systemd-boot F --q1n1 F --slbounce F --tcblaunch F --dtb F --bundle DIR

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
  usb-read stage2's files with the eight USB entries only (load options
           "usb", "usb-smmu", "usb-smmu-map", "usb-s2-walk", "usb-s2-survey",
           "usb-s2-fixed", "usb-s2-bypass-plan", "usb-halt";
           docs/GAOKUN3-USB-DEVICE-MODE.md).
  entropy  stage2's files with one entry only: the Phase 3 entropy check
           (load option "rng"; docs/GAOKUN3-PHASE3.md, step P7).
  q1n1-foothold  stage2's files with q1n1 (its -DQ1N1_VHE_EXIT
           -DQ1N1_DT_FRAMEBUFFER build) in place of el2qual: its --el2
           foothold under slbounce, then the same after --exit-vhe.
  vmapple-h1  Phase 3 step H1 (docs/GAOKUN3-PHASE3.md): systemd-boot, the
           device tree, q1n1's -DQ1N1_VMAPPLE_STICK build and a VirtualMac
           bundle from tools/vmapple-stick.py. One entry, --vmapple-place
           --hold: q1n1 places and checks the bundle before
           ExitBootServices, fills its seed from UEFI's RAW RNG, reports,
           releases it and waits for a key. It never calls
           ExitBootServices, so there is no slbounce or tcblaunch.exe.
  vmapple-h2  Phase 3 step H2: as vmapple-h1 plus slbounce and
           tcblaunch.exe, one entry --vmapple-enter --exit-vhe: q1n1
           keeps a version 2 bundle (with the gaokun3 monitor) through
           ExitBootServices and enters the monitor; the guest is not
           entered. Booting it writes the real GIC (decision 4).
  usb-inventory  systemd-boot and q1n1's holding USB protocol inventory
           (q1n1-usb-probe-hold.efi) only: no slbounce, tcblaunch.exe or
           device tree, so nothing on it can attempt a Secure Launch.

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
# The read-only USB stage (docs/GAOKUN3-USB-DEVICE-MODE.md), in order, each
# adding one thing so a hang names it: the clock controller's status before
# and after ExitBootServices, and USB0 only if that says it is powered; then
# the same after the VHE exit, with apps_smmu read last; then the same again
# through el2qual's own map of the SMMU window, the one entry that writes
# (RAM only: one table page of its own and one descriptor); then that, plus
# the read-only walk of the stage-2 tables, whose reads the security
# controller may refuse; then that, plus a read-only survey of the stage
# 2's top two levels; then the walk over a fixed range in the identity
# region the survey found, with UEFI's memory map there; then the plan for
# the S2CR bypass, which writes nothing. Boot CPU and no PAuth, the steps run 9 cleared.
USB_READ_ENTRIES = [
    ('el2qual-8-usb.conf', 'el2qual 8: USB read (read only)', 'nopac nosmp usb'),
    ('el2qual-9-usb-smmu.conf', 'el2qual 9: USB and SMMU read after the VHE exit (read only)',
     'nopac nosmp exit-vhe usb-smmu'),
    ('el2qual-10-usb-smmu-map.conf',
     'el2qual 10: as 9, through its own map of the SMMU window (SMMU read only)',
     'nopac nosmp exit-vhe usb-smmu-map'),
    ('el2qual-11-usb-s2-walk.conf',
     'el2qual 11: as 10, then a read-only walk of the stage-2 tables',
     'nopac nosmp exit-vhe usb-smmu-map usb-s2-walk'),
    ('el2qual-12-usb-s2-survey.conf',
     'el2qual 12: as 11, then a read-only survey of the stage 2',
     'nopac nosmp exit-vhe usb-smmu-map usb-s2-survey'),
    ('el2qual-13-usb-s2-fixed.conf',
     'el2qual 13: as 10, then a fixed range in the identity region, and its memory map',
     'nopac nosmp exit-vhe usb-smmu-map usb-s2-fixed'),
    ('el2qual-14-usb-s2-bypass-plan.conf',
     'el2qual 14: the S2CR bypass plan, nothing written',
     'nopac nosmp exit-vhe usb-smmu-map usb-s2-bypass-plan'),
    ('el2qual-15-usb-halt.conf',
     'el2qual 15: USB0 xHCI halt state from the measured CAPLENGTH (read only)',
     'nopac nosmp usb-halt'),
]
# Phase 3 step P7 (docs/GAOKUN3-PHASE3.md): whether the firmware offers an
# entropy source a Route B boot could take its seed from. The check runs
# before ExitBootServices and is read only; one entry, so nothing else on
# the stick can be chosen by mistake.
ENTROPY_ENTRIES = [
    ('el2qual-16-rng.conf', 'el2qual 16: UEFI RNG protocol check (read only)', 'nopac nosmp rng'),
]
STAGE_ENTRIES = {'stage1b': ENTRIES, 'stage2': ENTRIES, 'thermal': THERMAL_ENTRIES,
                 'vhe-exit': VHE_EXIT_ENTRIES, 'usb-read': USB_READ_ENTRIES,
                 'entropy': ENTROPY_ENTRIES}

# Phase 2's USB question (docs/GAOKUN3-PHASE2.md): which USB protocols the
# gaokun3's firmware installs. q1n1's passive inventory, built to hold its
# screen, since a boot menu redraws over an application that returns.
USB_PROBE_PATH = 'EFI/q1n1/q1n1-usb-probe-hold.efi'
USB_INVENTORY_ENTRY = ('q1n1-usb-inventory.conf', 'q1n1: USB protocol inventory (read only, no EL2 switch)')
STAGES = ('stage1', 'stage1b', 'stage2', 'thermal', 'vhe-exit', 'usb-inventory', 'q1n1-foothold',
          'usb-read', 'entropy', 'vmapple-h1', 'vmapple-h2')

# q1n1's first boot on the gaokun3 (docs/GAOKUN3-PHASE2.md), in order: its
# --el2 foothold under slbounce's VHE state, drawing on the device tree's
# framebuffer; then the same after leaving VHE. Only the gaokun3 build can
# draw there, so the image must carry both features (q1n1_features).
Q1N1_PATH = 'EFI/q1n1/q1n1.efi'
Q1N1_FOOTHOLD_ENTRIES = [
    ('q1n1-1-foothold.conf', 'q1n1 1: EL2 foothold (no VHE exit)', '--el2'),
    ('q1n1-2-foothold-vhe-exit.conf', 'q1n1 2: EL2 foothold after the VHE exit', '--el2 --exit-vhe'),
]
STAGE_ENTRIES['q1n1-foothold'] = Q1N1_FOOTHOLD_ENTRIES
# Report strings only the -DQ1N1_VHE_EXIT and -DQ1N1_DT_FRAMEBUFFER code
# contains (q1n1 platform/uefi/main.c).
Q1N1_MARKERS = {b'VHE EXIT ': '-DQ1N1_VHE_EXIT', b'DT SIMPLE-FB': '-DQ1N1_DT_FRAMEBUFFER'}


def q1n1_features(path):
    data = Path(path).read_bytes()
    missing = [define for marker, define in Q1N1_MARKERS.items() if marker not in data]
    if missing:
        raise LayoutError(f'{path}: not the gaokun3 q1n1 build (lacks {" and ".join(missing)}); '
                          'build it with make uefi OUT=build/uefi-gaokun3 '
                          'Q1N1_DEFINES="-DQ1N1_VHE_EXIT -DQ1N1_DT_FRAMEBUFFER"')


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


def entry_text(title, options, dtb_name, efi='/EFI/el2qual/el2qual.efi'):
    lines = [f'title {title}', f'efi {efi}']
    if options:
        lines.append(f'options {options}')
    if dtb_name:
        lines.append(f'devicetree /dtb/{dtb_name}')
    return '\n'.join(lines) + '\n'


# Phase 3 step H1 (docs/GAOKUN3-PHASE3.md): q1n1 places and checks the
# VirtualMac bundle, then holds its report on screen. Report strings only the
# -DQ1N1_VMAPPLE_STICK code contains (q1n1 platform/uefi/vmapple-stick.c).
VMAPPLE_H1_ENTRY = ('q1n1-vmapple-h1.conf',
                    'q1n1: VirtualMac bundle placement check (H1, nothing entered)',
                    '--vmapple-place --hold')
VMAPPLE_MARKERS = {b'--vmapple-place': '-DQ1N1_VMAPPLE_STICK',
                   b'PLACED AND VERIFIED': '-DQ1N1_VMAPPLE_STICK'}
BUNDLE_DIR = 'Snapintosh/vmapple'

# Phase 3 step H2 (docs/GAOKUN3-PHASE3.md): as H1, plus slbounce and
# tcblaunch.exe so q1n1 leaves the firmware at EL2, keeps a version 2
# bundle, and enters the monitor (the guest is not entered). Two entries
# share one stick: the default halts after stage-2 setup before any GIC
# access (--gic-hold), and the second runs monitor_init's GIC setup and
# halts before the guest (decision 4).
VMAPPLE_H2_ENTRIES = (
    ('q1n1-vmapple-h2.conf',
     'q1n1: enter the VirtualMac monitor, stage 2 only (H2a, no GIC access)',
     '--vmapple-enter --exit-vhe --hold-at-monitor --gic-hold'),
    ('q1n1-vmapple-h2-gic.conf',
     'q1n1: enter the VirtualMac monitor with GIC setup (H2b, guest not entered)',
     '--vmapple-enter --exit-vhe --hold-at-monitor'),
)
VMAPPLE_H2_ENTRY = VMAPPLE_H2_ENTRIES[0]
VMAPPLE_H2_MARKERS = {b'--vmapple-enter': '-DQ1N1_VMAPPLE_STICK',
                      b'VHE EXIT ': '-DQ1N1_VHE_EXIT',
                      b'DT SIMPLE-FB': '-DQ1N1_DT_FRAMEBUFFER'}


def check_bundle(bundle):
    """The bundle's own check (tools/vmapple-stick.py check): every chunk's
    hash, bundle.bin against manifest.json, the empty seed slot. Returns
    the files to place, relative to the stick's root."""
    import contextlib
    import importlib.util
    import io
    spec = importlib.util.spec_from_file_location('vmapple_stick', ROOT / 'tools' / 'vmapple-stick.py')
    stick = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stick)
    class Args:
        out = str(bundle)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            stick.check(Args())
    except SystemExit as refusal:
        raise LayoutError(f'bundle {bundle}: {refusal}') from None
    files = sorted(p for p in (Path(bundle) / BUNDLE_DIR).iterdir() if p.is_file())
    if not any(p.name == 'bundle.bin' for p in files):
        raise LayoutError(f'bundle {bundle}: no {BUNDLE_DIR}/bundle.bin')
    import json as _json
    try:
        manifest = _json.loads((Path(bundle) / 'manifest.json').read_text())
    except (OSError, ValueError) as error:
        raise LayoutError(f'bundle {bundle}: no readable manifest.json: {error}') from None
    return files, manifest


def build_vmapple_h1(out, systemd_boot, q1n1, dtb, bundle, replace, extra):
    if extra:
        raise LayoutError('vmapple-h1 takes --systemd-boot, --q1n1, --dtb and --bundle only: '
                          'it never leaves the firmware, so no slbounce or tcblaunch.exe')
    if not all((systemd_boot, q1n1, dtb, bundle)):
        raise LayoutError('vmapple-h1 needs --systemd-boot, --q1n1, --dtb and --bundle')
    require_pe(systemd_boot, 'systemd-boot', {SUBSYSTEM_EFI_APPLICATION})
    require_pe(q1n1, 'q1n1', {SUBSYSTEM_EFI_APPLICATION})
    data = Path(q1n1).read_bytes()
    missing = sorted({d for m, d in VMAPPLE_MARKERS.items() if m not in data})
    if missing:
        raise LayoutError(f'{q1n1}: not a q1n1 build with {" ".join(missing)}')
    require_dtb(dtb)
    chunks, _manifest = check_bundle(bundle)
    out = check_destination(out, replace)
    if out.exists() and any(out.iterdir()):
        shutil.rmtree(out)
    placed = {}
    def put(rel, payload):
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(payload)
        placed[rel] = hashlib.sha256(payload).hexdigest()
    put('EFI/BOOT/BOOTAA64.EFI', Path(systemd_boot).read_bytes())
    put(Q1N1_PATH, data)
    dtb_name = Path(dtb).name
    put(f'dtb/{dtb_name}', Path(dtb).read_bytes())
    for path in chunks:
        put(f'{BUNDLE_DIR}/{path.name}', path.read_bytes())
    name, title, options = VMAPPLE_H1_ENTRY
    put('loader/loader.conf', f'timeout 15\ndefault {name}\neditor no\n'.encode())
    put(f'loader/entries/{name}', entry_text(title, options, dtb_name, '/' + Q1N1_PATH).encode())
    import json as _json
    qualification = _json.loads((Path(bundle) / 'manifest.json').read_text())
    manifest = {'stage': 'vmapple-h1', 'always_switch': False, 'files': placed,
                'bundle_entry_pa': qualification['entry_pa']}
    (out / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def check_bundle_v2(bundle, manifest):
    """An H2 bundle must be version 2 with a monitor to enter: the binary
    header's version, a monitor entry inside its own chunk, a parameter
    page, and a described screen split (the gaokun3 panel)."""
    import struct as _struct
    binary = (Path(bundle) / BUNDLE_DIR / 'bundle.bin').read_bytes()
    if len(binary) < 16 or _struct.unpack_from('<Q', binary, 8)[0] != 2:
        raise LayoutError(f'bundle {bundle}: not a version 2 bundle (no monitor to enter); '
                          'rebuild it with tools/vmapple-stick.py --monitor')
    monitor = manifest.get('monitor') or {}
    chunks = manifest.get('chunks') or []
    index = monitor.get('chunk')
    if not isinstance(index, int) or not 0 <= index < len(chunks) \
            or chunks[index].get('pa') != monitor.get('entry_pa'):
        raise LayoutError(f'bundle {bundle}: its monitor entry is not the monitor chunk')
    if not monitor.get('params_pa'):
        raise LayoutError(f'bundle {bundle}: no parameter page for the monitor')
    fb = monitor.get('framebuffer') or {}
    if not all(fb.get(k) for k in ('pa', 'width', 'stride', 'guest_rows', 'band_rows')):
        raise LayoutError(f'bundle {bundle}: no screen split described for the monitor')
    return monitor


def build_vmapple_h2(out, systemd_boot, q1n1, slbounce, tcblaunch, dtb, bundle,
                     replace, extra):
    if extra:
        raise LayoutError('vmapple-h2 takes --systemd-boot, --q1n1, --slbounce, '
                          '--tcblaunch, --dtb and --bundle only')
    if not all((systemd_boot, q1n1, slbounce, tcblaunch, dtb, bundle)):
        raise LayoutError('vmapple-h2 needs --systemd-boot, --q1n1, --slbounce, '
                          '--tcblaunch, --dtb and --bundle')
    require_pe(systemd_boot, 'systemd-boot', {SUBSYSTEM_EFI_APPLICATION})
    require_pe(q1n1, 'q1n1', {SUBSYSTEM_EFI_APPLICATION})
    require_pe(slbounce, 'slbounce', {SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER,
                                      SUBSYSTEM_EFI_RUNTIME_DRIVER})
    require_pe(tcblaunch, 'tcblaunch.exe')
    data = Path(q1n1).read_bytes()
    missing = sorted({d for m, d in VMAPPLE_H2_MARKERS.items() if m not in data})
    if missing:
        raise LayoutError(f'{q1n1}: not a q1n1 build with {" ".join(missing)}')
    require_dtb(dtb)
    chunks, bundle_manifest = check_bundle(bundle)
    monitor = check_bundle_v2(bundle, bundle_manifest)
    out = check_destination(out, replace)
    if out.exists() and any(out.iterdir()):
        shutil.rmtree(out)
    placed = {}
    def put(rel, payload):
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(payload)
        placed[rel] = hashlib.sha256(payload).hexdigest()
    put('EFI/BOOT/BOOTAA64.EFI', Path(systemd_boot).read_bytes())
    put(Q1N1_PATH, data)
    put(f'EFI/systemd/drivers/{SLBOUNCE_NAME}', Path(slbounce).read_bytes())
    put('tcblaunch.exe', Path(tcblaunch).read_bytes())
    dtb_name = Path(dtb).name
    put(f'dtb/{dtb_name}', Path(dtb).read_bytes())
    for path in chunks:
        put(f'{BUNDLE_DIR}/{path.name}', path.read_bytes())
    default_name = VMAPPLE_H2_ENTRIES[0][0]
    put('loader/loader.conf', f'timeout 15\ndefault {default_name}\neditor no\n'.encode())
    for name, title, options in VMAPPLE_H2_ENTRIES:
        put(f'loader/entries/{name}', entry_text(title, options, dtb_name, '/' + Q1N1_PATH).encode())
    import json as _json
    manifest = {'stage': 'vmapple-h2', 'always_switch': False, 'files': placed,
                'bundle_entry_pa': bundle_manifest['entry_pa'],
                'monitor_entry_pa': monitor['entry_pa'],
                'params_pa': monitor['params_pa'],
                'bundle_qualification': hashlib.sha256(
                    _json.dumps(bundle_manifest, sort_keys=True).encode()).hexdigest()}
    (out / MANIFEST).write_text(_json.dumps(manifest, indent=2) + '\n')
    return manifest


def build_layout(stage, out, efi, systemd_boot=None, slbounce=None, tcblaunch=None,
                 dtb=None, always_switch=False, replace=False, usb_probe=None, bundle=None):
    """Validate every input, then write the tree; returns the manifest."""
    if stage not in STAGES:
        raise LayoutError(f'unknown stage {stage}')
    if stage == 'vmapple-h1':
        return build_vmapple_h1(out, systemd_boot, efi, dtb, bundle, replace,
                                any((slbounce, tcblaunch, always_switch, usb_probe)))
    if stage == 'vmapple-h2':
        return build_vmapple_h2(out, systemd_boot, efi, slbounce, tcblaunch, dtb,
                                bundle, replace, any((always_switch, usb_probe)))
    if bundle:
        raise LayoutError('--bundle belongs to the vmapple-h1 and vmapple-h2 stages only')
    if stage == 'usb-inventory':
        return build_usb_inventory(out, systemd_boot, usb_probe, replace,
                                   any((slbounce, tcblaunch, dtb, always_switch, efi)))
    if usb_probe:
        raise LayoutError('--usb-probe belongs to the usb-inventory stage only')
    switches = stage in ('stage2', 'thermal', 'vhe-exit', 'q1n1-foothold', 'usb-read', 'entropy')
    q1n1 = stage == 'q1n1-foothold'
    require_pe(efi, 'q1n1' if q1n1 else 'el2qual', {SUBSYSTEM_EFI_APPLICATION})
    if q1n1:
        q1n1_features(efi)
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
        place(efi, Q1N1_PATH if q1n1 else 'EFI/el2qual/el2qual.efi')
        dtb_name = Path(dtb).name if dtb else None
        if dtb:
            place(dtb, f'dtb/{dtb_name}')
        entries = STAGE_ENTRIES[stage]
        write('loader/loader.conf', f'timeout 15\ndefault {entries[0][0]}\neditor no\n')
        for name, title, options in entries:
            write(f'loader/entries/{name}', entry_text(title, options, dtb_name,
                                                       '/' + Q1N1_PATH if q1n1 else '/EFI/el2qual/el2qual.efi'))
        if switches:
            place(slbounce, f'EFI/systemd/drivers/{SLBOUNCE_NAME}')
            # slbounce opens tcblaunch.exe from the root of its own volume.
            place(tcblaunch, 'tcblaunch.exe')
    manifest = {'stage': stage, 'always_switch': bool(always_switch), 'files': placed}
    (out / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def build_usb_inventory(out, systemd_boot, usb_probe, replace, extra):
    if extra:
        raise LayoutError('usb-inventory takes only --systemd-boot and --usb-probe: '
                          'no slbounce, tcblaunch.exe, device tree or el2qual')
    if not systemd_boot or not usb_probe:
        raise LayoutError('usb-inventory needs --systemd-boot and --usb-probe')
    require_pe(systemd_boot, 'systemd-boot', {SUBSYSTEM_EFI_APPLICATION})
    require_pe(usb_probe, 'q1n1-usb-probe-hold.efi', {SUBSYSTEM_EFI_APPLICATION})
    out = check_destination(out, replace)
    if out.exists() and any(out.iterdir()):
        shutil.rmtree(out)
    placed = {}
    def put(rel, data):
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        placed[rel] = hashlib.sha256(data).hexdigest()
    put('EFI/BOOT/BOOTAA64.EFI', Path(systemd_boot).read_bytes())
    put(USB_PROBE_PATH, Path(usb_probe).read_bytes())
    name, title = USB_INVENTORY_ENTRY
    put('loader/loader.conf', f'timeout 15\ndefault {name}\neditor no\n'.encode())
    put(f'loader/entries/{name}', entry_text(title, '', None, '/' + USB_PROBE_PATH).encode())
    manifest = {'stage': 'usb-inventory', 'always_switch': False, 'files': placed}
    (out / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('stage', choices=list(STAGES))
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
    ap.add_argument('--usb-probe', help="q1n1's q1n1-usb-probe-hold.efi (usb-inventory only)")
    ap.add_argument('--q1n1', help="q1n1's gaokun3 build of q1n1.efi (q1n1-foothold, "
                    'vmapple-h1, vmapple-h2)')
    ap.add_argument('--bundle', help='a tools/vmapple-stick.py bundle directory '
                    '(vmapple-h1, vmapple-h2 only)')
    a = ap.parse_args()
    efi = a.efi
    if a.stage in ('q1n1-foothold', 'vmapple-h1', 'vmapple-h2'):
        if efi or not a.q1n1:
            print(f'REFUSED: {a.stage} takes --q1n1 (not --efi)')
            return 1
        efi = a.q1n1
    elif a.q1n1:
        print('REFUSED: --q1n1 belongs to the q1n1-foothold, vmapple-h1 and '
              'vmapple-h2 stages only')
        return 1
    elif a.stage == 'usb-inventory':
        pass
    elif not efi:
        sys.path.insert(0, str(ROOT / 'probes' / 'el2qual'))
        import build as el2qual_build
        efi = el2qual_build.build(llvm=a.llvm)
    out = Path(a.out) if a.out else DEFAULT_OUT / a.stage
    try:
        manifest = build_layout(a.stage, out, efi, a.systemd_boot, a.slbounce, a.tcblaunch,
                                a.dtb, a.always_switch, a.replace, a.usb_probe, a.bundle)
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
