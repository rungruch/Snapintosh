#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for the classic VirtualMac loader in probes/classicload.py (P2).

Fixtures are synthetic: a hand-built Mach-O header and device tree from
public format facts, with no Apple material. The QEMU smoke's adapter
(tools/vmapple-smoke/kcload.py) must produce byte-identical output to the
xnuboot path it delegates to, given the same entropy source.
"""
import importlib.util
import os
import pathlib
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'vmapple-smoke'))
sys.path.insert(0, str(ROOT / 'probes'))
import appledt
import classicload
import kcload

_spec = importlib.util.spec_from_file_location('smoke_fixtures', ROOT / 'tools' / 'test-vmapple-smoke.py')
_fixtures = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_fixtures)

checks = 0


def check(condition, text):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(f'FAIL: {text}')


def refuses(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except ValueError:
        return True
    return False


def synthetic_kernelcache(path):
    """A minimal 64-bit Mach-O: one segment and an LC_UNIXTHREAD entry."""
    seg_va = 0xfffffe0007004000
    header = struct.pack('<IIIIIIII', 0xFEEDFACF, 0x0100000C, 0, 2, 2, 72 + 288, 0, 0)
    seg = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__TEXT', seg_va, 0x8000, 0, 0x400, 0, 0, 0, 0)
    thread = struct.pack('<II', 5, 288) + bytes(16) + struct.pack('<32Q', *([0] * 31 + [seg_va + 0x480]))
    blob = header + seg + thread
    blob = blob.ljust(0x400, b'\0')
    pathlib.Path(path).write_bytes(blob)
    return seg_va


def test_profile():
    if sys.version_info < (3, 10):
        print('SKIP test-vmapple-classic profile: guestprofile needs Python 3.10+')
        return
    import xnuboot
    profile = xnuboot.get_profile('vma2macosap-26A428')
    check(profile.abi == 'classic-vmapple-v1', 'the vma2 profile has its own ABI')
    check(profile.monitors == () and profile.frame_type_info_va == 0,
          'no SPTM/TXM contract is claimed for the classic guest')
    check(refuses(xnuboot.configure_guest, 'vma2macosap-26A428'),
          'the SPTM loader path refuses the classic profile')
    check(xnuboot.GUEST_PROFILE.name == 'j700ap-26A428',
          'the refusal left the J700 profile selected')


def test_classic_image(tmp):
    kc = tmp / 'kernelcache.synthetic'
    seg_va = synthetic_kernelcache(kc)
    dt_path = tmp / 'devicetree.synthetic'
    dt_path.write_bytes(_fixtures.synthetic_tree())
    entropy = lambda n: bytes(range(1, 256)) + b'\x01' * (n - 255)
    image = classicload.build_classic_image(kc, dt_path, classicload.QEMU_CLASSIC_LAYOUT, entropy=entropy)
    layout = classicload.QEMU_CLASSIC_LAYOUT
    check(image.payload_pa == layout.ram_base, 'payload at RAM base')
    check(image.entry_pa == seg_va + 0x480 - seg_va + layout.ram_base, 'entry PA keeps the segment offset')
    check(image.boot_args_pa == layout.boot_args_pa, 'x0 is the boot_args PA')
    dt = appledt.DeviceTree(image.dt_blob)
    check(dt.get('/device-tree/pram', 'reg') == struct.pack('<QQ', layout.panic_pa, layout.panic_size),
          'panic region published through /pram')
    check(dt.get('/device-tree/chosen', 'embedded-panic-log-size')
          == struct.pack('<I', layout.panic_log_size), 'panic log size published')
    check(dict(image.chunks)[layout.panic_pa] == bytes(layout.panic_size),
          'the panic region is a zeroed chunk of its own')
    check(not (layout.ram_base <= layout.panic_pa < layout.ram_base + layout.mem_size),
          'the panic region sits outside the reported guest DRAM')
    from dataclasses import replace as _replace
    check(refuses(lambda: _replace(layout, panic_pa=layout.top_of_kernel_data).validate()),
          'a panic region inside the reported DRAM is refused')
    args = image.payload[layout.boot_args_pa - layout.ram_base:
                         layout.boot_args_pa - layout.ram_base + classicload.BOOT_ARGS_SIZE]
    check(bytes(args) == kcload._boot_args(seg_va, layout.dt_pa - layout.ram_base + seg_va,
                                           len(image.dt_blob)),
          'classic boot_args match the smoke adapter byte for byte')
    blob, _ = kcload._complete_dt_tree(appledt.DeviceTree(_fixtures.synthetic_tree()), entropy=entropy)
    check(blob == image.dt_blob, 'the completed tree matches the smoke adapter byte for byte')

    bad = tmp / 'kernelcache.bad'
    bad.write_bytes(b'\x00' * 64)
    check(refuses(classicload.build_classic_image, bad, dt_path, layout, entropy),
          'a non-Mach-O kernelcache is refused')
    check(refuses(classicload.build_classic_image, kc, dt_path, layout, lambda n: bytes(n)),
          'an all-zero seed is refused')
    nodt = tmp / 'devicetree.nopram'
    tree = appledt.DeviceTree(_fixtures.synthetic_tree())
    del tree.nodes['/device-tree/pram']
    tree.children['/device-tree'].remove('/device-tree/pram')
    nodt.write_bytes(tree.serialize())
    check(refuses(classicload.build_classic_image, kc, nodt, layout, entropy),
          'a tree without /pram is refused (no panic region)')
    from dataclasses import replace
    check(refuses(lambda: replace(layout, panic_log_size=layout.panic_size + 1).validate()),
          'a panic log larger than its region is refused')


def test_monitor_layout(tmp):
    kc = tmp / 'kernelcache.synthetic'
    synthetic_kernelcache(kc)
    dt_path = tmp / 'devicetree.synthetic2'
    dt_path.write_bytes(_fixtures.synthetic_tree())
    entropy = lambda n: b'\xa5' * n
    layout = classicload.QEMU_MONITOR_LAYOUT
    check(layout.armio_parent_base == 0x10000000, 'the monitor layout keeps the template map')
    # A layout whose drop list names an absent node is refused.
    from dataclasses import replace
    bad = replace(layout, drop_nodes=('/device-tree/arm-io/gfx',))
    check(refuses(classicload.build_classic_image, kc, dt_path, bad, entropy),
          'dropping a node the tree does not have is refused')
    image = classicload.build_classic_image(kc, dt_path, replace(layout, drop_nodes=()),
                                            entropy=entropy)
    dt = appledt.DeviceTree(image.dt_blob)
    check(dt.get('/device-tree/arm-io', 'ranges')
          == struct.pack('<QQQ', 0, 0x10000000, 0x40000000),
          'arm-io ranges keep the template parent under the monitor layout')
    check(dt.get('/device-tree/arm-io/uart0', 'reg')
          == struct.pack('<QQ', 0x10010000, 0x1000), 'the PL011 stays at its template IPA')


# Reserved regions measured from the gaokun3's SLB device tree
# (build/phase1/gaokun3-el2.dts, reserved-memory) plus its UEFI GOP
# framebuffer at 0xc6200000 (0xfa0000 bytes), which firmware is using.
GAOKUN3_RESERVED = [
    (0x80000000, 0x860000), (0x80860000, 0x20000), (0x80880000, 0x80000),
    (0x80900000, 0x200000), (0x80b00000, 0x100000), (0x83b00000, 0xf00000),
    (0x84a00000, 0x800000), (0x85b00000, 0xc00000), (0x86700000, 0x500000),
    (0x86c00000, 0x2000000), (0x88c00000, 0x1500000), (0x8a100000, 0x1e00000),
    (0x8bf00000, 0x2000), (0x8c600000, 0x1e00000), (0xaeb00000, 0x16600000),
    (0xc6200000, 0xfa0000),
]


def test_gaokun3_layout():
    layout = classicload.derive_gaokun3_layout(GAOKUN3_RESERVED)
    check(layout.name == 'gaokun3' and layout.timebase_hz == 19_200_000,
          'the gaokun3 layout carries the 19.2 MHz counter rate')
    check(layout.guest_cpus == 8, 'the gaokun3 layout starts all 8 cores')
    check(layout.ram_base & 0x1FFFFFF == 0x1004000,
          'ram_base keeps the kernelcache header offset')
    for base, size in GAOKUN3_RESERVED:
        check(not (layout.ram_base < base + size
                   and base < layout.ram_base + layout.mem_size + layout.panic_size),
              f'the guest window avoids reserved region {base:#x}')
    check(layout.panic_pa == layout.ram_base + layout.mem_size,
          'the panic chunk sits just above the reported DRAM')
    qemu = classicload.split_screen(classicload.QEMU_MONITOR_LAYOUT,
                                    classicload.QEMU_SPLIT_SCREEN, classicload.QEMU_MONITOR_ROWS)
    check(qemu.video == (0x62000000, 1024, 512, 4096, 32)
          and qemu.monitor_band == (0x62200000, 1024, 88, 4096),
          'the QEMU split matches tools/vmapple-smoke/layout.h (512 + 88 rows)')
    import re
    header = (ROOT / 'tools' / 'vmapple-smoke' / 'layout.h').read_text()
    mon = {k: int(v, 0) for k, v in re.findall(r'#define (MON_FB_\w+)\s+(0x[0-9a-fA-F]+|\d+)', header)}
    base, width, height, stride = classicload.QEMU_SPLIT_SCREEN
    check(mon == {'MON_FB_PA': base, 'MON_FB_WIDTH': width, 'MON_FB_STRIDE': stride,
                  'MON_FB_GUEST_ROWS': height - classicload.QEMU_MONITOR_ROWS,
                  'MON_FB_BAND_ROWS': classicload.QEMU_MONITOR_ROWS},
          'layout.h and classicload describe the same QEMU screen')
    check(refuses(classicload.split_screen, classicload.QEMU_MONITOR_LAYOUT,
                  (0x60000000, 1024, 600, 4096), 88),
          'a framebuffer inside guest DRAM is refused')
    check(refuses(classicload.derive_gaokun3_layout, [(0x80000000, 16 << 30)]),
          'a fully reserved DRAM is refused')
    check(refuses(classicload.derive_gaokun3_layout, GAOKUN3_RESERVED, 0x80000000 +
                  (16 << 30), 32 << 30), 'a guest larger than the DRAM is refused')


def test_real_inputs(tmp):
    kc = ROOT / 'downloads' / 'extracted' / 'vma2macosap' / 'kernelcache.release.vma2'
    dt = ROOT / 'downloads' / 'extracted' / 'vma2macosap' / 'DeviceTree.vma2macosap'
    if not kc.exists() or not dt.exists():
        print('SKIP test-vmapple-classic real inputs: downloads/extracted/vma2macosap absent')
        return
    if sys.version_info < (3, 10):
        print('SKIP test-vmapple-classic profile hashes: guestprofile needs Python 3.10+')
    else:
        import xnuboot
        profile = xnuboot.get_profile('vma2macosap-26A428')
        measured = profile.hashes(kc.parent)
        check(profile.validate(measured) == [], f'profile hashes match the local bundle: {measured}')
    saved = os.urandom
    os.urandom = lambda n: b'\xa5' * n
    try:
        mine = classicload.build_classic_image(kc, dt, classicload.QEMU_CLASSIC_LAYOUT)
        out = tmp / 'smoke-out'
        payload, payload_pa, entry_pa, x0 = kcload.lay_out(kc, out)
    finally:
        os.urandom = saved
    check(payload.read_bytes() == mine.payload, 'smoke lay_out is byte-identical to the xnuboot loader')
    check((payload_pa, entry_pa, x0) == (mine.payload_pa, mine.entry_pa, mine.boot_args_pa),
          'smoke lay_out returns the xnuboot addresses')


def main():
    test_profile()
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        test_classic_image(tmp)
        test_monitor_layout(tmp)
        test_gaokun3_layout()
        test_real_inputs(tmp)
    print(f'PASS test-vmapple-classic: {checks} checks')


if __name__ == '__main__':
    main()
