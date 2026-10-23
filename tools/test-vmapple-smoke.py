#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for tools/vmapple-smoke's loader-only data transforms.

The fixture below is a synthetic Apple device tree built from public format
facts. It contains no Apple code, firmware or strings. QEMU/PAuth execution is
covered separately by `python3 tools/vmapple-smoke/smoke.py selftest`.
"""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'vmapple-smoke'))
sys.path.insert(0, str(ROOT / 'probes'))
import appledt
import kcload

checks = 0


def check(condition, text):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(f'FAIL: {text}')


def prop(name, value=b'', placeholder=False):
    raw = name.encode().ljust(32, b'\0')
    length = len(value) | (appledt.PLACEHOLDER if placeholder else 0)
    return raw + struct.pack('<I', length) + value + b'\0' * ((-len(value)) % 4)


def node(name, properties=(), children=()):
    props = [prop('name', name.encode() + b'\0'), *properties]
    return struct.pack('<II', len(props), len(children)) + b''.join(props) + b''.join(children)


def synthetic_tree():
    cpu0 = node('cpu0', [prop('timebase-frequency', struct.pack('<Q', 0))])
    cpus = node('cpus', [], [cpu0])
    uart = node('uart0', [prop('reg', struct.pack('<QQ', 0, 0x1000))])
    gic = node('gic', [prop('reg', bytes(48))])
    armio = node('arm-io', [prop('ranges', bytes(24))], [uart, gic])
    chosen = node('chosen')
    return node('device-tree', [
        prop('clock-frequency', struct.pack('<I', 0)),
        prop('machine-identity', b'recipe', placeholder=True),
    ], [chosen, armio, cpus])


def test_dt_completion():
    tree = appledt.DeviceTree(synthetic_tree())
    blob, resolved = kcload._complete_dt_tree(tree)
    out = appledt.DeviceTree(blob)
    check(resolved == 1, 'one placeholder was resolved')
    check(not out.nodes['/device-tree']['machine-identity'].placeholder, 'placeholder flag cleared')
    check(out.get('/device-tree/chosen', 'dram-base') == struct.pack('<Q', kcload.GUEST_RAM_BASE),
          'dram-base added')
    check(out.get('/device-tree/chosen', 'dram-size') == struct.pack('<Q', kcload.GUEST_MEM_SIZE),
          'dram-size added')
    check(out.get('/device-tree', 'clock-frequency') == struct.pack('<I', kcload.TIMEBASE_HZ),
          'root timebase set')
    check(out.get('/device-tree/cpus/cpu0', 'timebase-frequency') == struct.pack('<Q', kcload.TIMEBASE_HZ),
          'cpu timebase set')
    check(out.get('/device-tree/arm-io', 'ranges') == struct.pack('<QQQ', 0, kcload.ARMIO_PARENT_BASE, 0x40000000),
          'arm-io ranges rewritten')
    check(out.get('/device-tree/arm-io/uart0', 'reg') == struct.pack('<QQ', kcload.QEMU_UART_OFF, 0x1000),
          'PL011 range rewritten')
    expected_gic = struct.pack('<QQQQ', kcload.QEMU_GICD_OFF, 0x10000,
                               kcload.QEMU_GICR_OFF, kcload.QEMU_GICR_SIZE) + bytes(16)
    check(out.get('/device-tree/arm-io/gic', 'reg') == expected_gic, 'GIC ranges rewritten')


def test_boot_args():
    virt = 0xfffffe0007004000
    dtva = virt + 0x5000000
    b = kcload._boot_args(virt, dtva, 0x1234)
    check(len(b) == 0x480, 'boot_args is 0x480 bytes')
    check(struct.unpack_from('<HH', b, 0) == (1, 2), 'revision 1 version 2')
    check(struct.unpack_from('<QQQQ', b, 8) == (virt, kcload.GUEST_RAM_BASE,
          kcload.GUEST_MEM_SIZE, kcload.TOP_OF_KERNEL_DATA), 'base and memory fields')
    check(struct.unpack_from('<Q', b, 0x60)[0] == dtva, 'device tree VA at 0x60')
    check(struct.unpack_from('<I', b, 0x68)[0] == 0x1234, 'device tree length at 0x68')
    check(b[0x6c:0x6c + len(kcload.CMDLINE)] == kcload.CMDLINE.encode(), 'command line at 0x6c')
    check(struct.unpack_from('<Q', b, 0x478)[0] == kcload.GUEST_MEM_SIZE, 'memSizeActual at 0x478')


def test_layout_alignment():
    link = 0xfffffe0007004000
    check(kcload.GUEST_RAM_BASE & 0x1ffffff == link & 0x1ffffff, '32 MiB block offsets match')
    check(kcload.TOP_OF_KERNEL_DATA & 0x3fff == 0, 'topOfKernelData is 16 KiB aligned')
    check(kcload.GUEST_RAM_BASE < kcload.DT_PA < kcload.BOOT_ARGS_PA < kcload.TOP_OF_KERNEL_DATA,
          'loader ranges ordered')


def main():
    test_dt_completion()
    test_boot_args()
    test_layout_alignment()
    print(f'PASS test-vmapple-smoke: {checks} checks')


if __name__ == '__main__':
    main()
