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


def cpu(n, state=None, reg=None, children=()):
    return node(f'cpu{n}', [prop('reg', struct.pack('<I', n if reg is None else reg)),
                            prop('state', (state or ('running' if n == 0 else 'waiting')).encode() + b'\0'),
                            prop('timebase-frequency', struct.pack('<Q', 0))], children)


def synthetic_tree(seed=bytes(256), cpus=None):
    # Like the template: several CPUs, cpu0 running, one with a child node.
    if cpus is None:
        cpus = [cpu(0), cpu(1, children=[node('cache')]), cpu(2), cpu(3)]
    cpus = node('cpus', [], cpus)
    uart = node('uart0', [prop('reg', struct.pack('<QQ', 0, 0x1000))])
    gic = node('gic', [prop('reg', bytes(48))])
    armio = node('arm-io', [prop('ranges', bytes(24))], [uart, gic])
    chosen = node('chosen', [] if seed is None else [prop('random-seed', seed)])
    pram = node('pram', [prop('reg', bytes(16))])
    return node('device-tree', [
        prop('clock-frequency', struct.pack('<I', 0)),
        prop('machine-identity', b'recipe', placeholder=True),
    ], [chosen, armio, cpus, pram])


def refuses(tree, entropy=lambda n: b'\xa5' * n):
    try:
        kcload._complete_dt_tree(appledt.DeviceTree(tree), entropy=entropy)
    except SystemExit:
        return True
    return False


def test_dt_completion():
    tree = appledt.DeviceTree(synthetic_tree())
    blob, resolved = kcload._complete_dt_tree(tree, entropy=lambda n: bytes(range(1, 256)) + b'\x01' * (n - 255))
    out = appledt.DeviceTree(blob)
    check(out.get('/device-tree/chosen', 'random-seed') == bytes(range(1, 256)) + b'\x01',
          'random-seed placeholder filled with entropy')
    check(out.get('/device-tree/chosen', 'nvram-bank-size') == struct.pack('<I', kcload.NVRAM_BANK_SIZE),
          'nvram-bank-size set')
    check(out.get('/device-tree/chosen', 'nvram-bank-count') == struct.pack('<I', 1), 'one NVRAM bank')
    check(out.get('/device-tree/chosen', 'nvram-current-bank') == struct.pack('<I', 0), 'bank 0 current')
    check(out.get('/device-tree/chosen', 'nvram-proxy-data') == kcload.nvram_image(),
          'nvram-proxy-data is the bank, exactly nvram-bank-size long')
    kept = out.children['/device-tree/cpus']
    check(len(kept) == kcload.GUEST_CPUS, f'/cpus trimmed to {kcload.GUEST_CPUS}: {kept}')
    check(all(struct.unpack('<I', out.get(c, 'reg'))[0] < kcload.GUEST_CPUS for c in kept),
          'the kept CPUs are the low regs')
    check(not [n for n in tree.nodes if n.startswith('/device-tree/cpus/cpu1')],
          'a dropped CPU and its children leave the tree, not just its serialization')
    check(kcload.QEMU_GICR_SIZE == kcload.GUEST_CPUS * 0x20000, 'one GICv3 redistributor frame per vCPU')
    check(refuses(synthetic_tree(cpus=[])), 'a tree with no CPUs is refused')
    check(refuses(synthetic_tree(cpus=[cpu(1, reg=1)])), 'a tree without CPU reg 0 is refused')
    check(refuses(synthetic_tree(cpus=[cpu(0, state='waiting'), cpu(1)])),
          'a boot CPU that is not running is refused')
    check(refuses(synthetic_tree(cpus=[cpu(0), cpu(5, reg=0)])), 'two CPUs with reg 0 are refused')
    check(refuses(synthetic_tree(seed=None)), 'a tree without /chosen/random-seed is refused')
    check(refuses(synthetic_tree(seed=bytes(64))), 'a random-seed shorter than XNU requests is refused')
    check(refuses(synthetic_tree(), entropy=lambda n: bytes(n)), 'an all-zero seed is refused')
    check(refuses(synthetic_tree(), entropy=lambda n: b'\x01' * (n - 1)), 'a short entropy fill is refused')
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
    check(out.get('/device-tree/pram', 'reg') == struct.pack('<QQ', kcload.PANIC_PA, kcload.PANIC_SIZE),
          'pram reg points at the panic region (published XNU check_for_panic_log)')
    check(out.get('/device-tree/chosen', 'embedded-panic-log-size')
          == struct.pack('<I', kcload.PANIC_LOG_SIZE), 'panic log size published in /chosen')


def chrp_accepts(image, bank_size):
    """The CHRP handler's acceptance rules (iokit/Kernel/IONVRAMCHRPHandler.cpp),
    written out independently of kcload: a version-1 "nvram" header whose
    checksum and Adler-32 match, then the partitions it walks. Returns the
    (offset, size) of the common and system partitions, or None if refused."""
    if len(image) != bank_size or image[4:9] != b'nvram':
        return None
    total = image[0] + sum(image[2:16])
    while total > 0xff:
        total = (total & 0xff) + (total >> 8)
    if total != image[1]:
        return None
    low, high = 1, 0
    for byte in image[20:]:
        low = (low + byte) % 65521
        high = (high + low) % 65521
    if struct.unpack_from('<I', image, 16)[0] != (high << 16 | low):
        return None
    found, off = {}, 0
    while off < bank_size:
        length = struct.unpack_from('<H', image, off + 2)[0] * 16
        if length < 16 or off + length > bank_size:
            break
        name = image[off + 4:off + 16].rstrip(b'\0')
        if name in (b'common', b'system'):
            found[name] = (off + 16, length - 16)
        off += length
    return found


def test_nvram():
    image = kcload.nvram_image()
    parts = chrp_accepts(image, kcload.NVRAM_BANK_SIZE)
    check(parts is not None, 'the NVRAM bank passes the CHRP checksum and Adler-32')
    check(set(parts) == {b'common', b'system'}, f'common and system partitions found: {parts}')
    check(all(image[o] == 0 for o, _ in parts.values()), 'both partitions start empty')
    check(sum(size + 16 for _, size in parts.values()) + 32 == kcload.NVRAM_BANK_SIZE,
          'the header and partitions fill the bank')
    for where in (1, 6, 17, 100, len(image) - 1):
        bad = bytearray(image)
        bad[where] ^= 0x40
        check(chrp_accepts(bytes(bad), kcload.NVRAM_BANK_SIZE) is None,
              f'the acceptance model refuses a bank with byte {where:#x} changed')
    try:
        kcload.nvram_image(bank_size=0x3000)
        check(False, 'a bank too small for the system partition is refused')
    except ValueError:
        check(True, 'a bank too small for the system partition is refused')
    # The real template is about 60 KiB before the bank is added.
    check(kcload.NVRAM_BANK_SIZE + 0x10000 <= kcload.BOOT_ARGS_PA - kcload.DT_PA,
          'the device tree with its NVRAM bank fits below boot_args')


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


def test_tidcp_decoder():
    # s3_4_c15_c10_6, the Apple counter alias the A16 monitor emulates, read.
    iss = (3 << 20) | (6 << 17) | (4 << 14) | (15 << 10) | (9 << 5) | (10 << 1) | 1
    name, direction, rt = kcload.decode_tidcp(0x18 << 26 | iss)
    check(name == 's3_4_c15_c10_6', f'decoded name, got {name}')
    check(direction == 'read', 'decoded direction')
    check(rt == 9, 'decoded Rt')
    # The same register written, with a different Rt, is the same register.
    iss_w = (3 << 20) | (6 << 17) | (4 << 14) | (15 << 10) | (2 << 5) | (10 << 1)
    name_w, direction_w, rt_w = kcload.decode_tidcp(iss_w)
    check(name_w == name and direction_w == 'write' and rt_w == 2, 'write form')
    # Serial lines are deduplicated by register and direction, order kept.
    serial = ('EL2: tidcp %016x\n' % (0x18 << 26 | iss)) * 2 + \
             'EL2: tidcp %016x\n' % iss_w + 'EL2: hvc 00000000c1000000\n'
    check(kcload.tidcp_from_serial(serial) == [('s3_4_c15_c10_6', 'read'),
                                               ('s3_4_c15_c10_6', 'write')],
          'serial set in first-seen order')
    check(kcload.tidcp_from_serial('EL2: tidcp zz\n') == [], 'a bad line is ignored')


def test_tidcp_log_parse():
    base = kcload.TIDCP_LOG_PA
    iss = (3 << 20) | (6 << 17) | (4 << 14) | (15 << 10) | (10 << 1) | 1
    other = (3 << 20) | (3 << 14) | (15 << 10) | (1 << 1) | 1
    mon = (
        f'(qemu) xp/4gx {base:#x}\n'
        f'{base:016x}: {kcload.TIDCP_LOG_MAGIC:#018x} 0x0000000000000002\n'
        f'{base + 0x10:016x}: 0x0000000000000407 0x0000000000000000\n'
        f'{base + 0x20:016x}: {iss:#018x} 0x0000000000000400\n'
        f'{base + 0x30:016x}: 0xfffffe0009eb0000 0x0000000000000000\n'
        f'{base + 0x40:016x}: {other:#018x} 0x0000000000000007\n'
        f'{base + 0x50:016x}: 0xfffffe0009ec0000 0x0000000000000000\n')
    totals, entries = kcload.tidcp_log(mon)
    check(totals == {'used': 2, 'total': 0x407, 'dropped': 0}, f'log totals, got {totals}')
    check([e[0] for e in entries] == ['s3_4_c15_c10_6', 's3_3_c15_c1_0'],
          f'busiest first, got {[e[0] for e in entries]}')
    check(entries[0][2] == 0x400 and entries[0][3] == 0xfffffe0009eb0000, 'count and ELR')
    check(kcload.tidcp_log('no log here') == (None, []), 'absent log reported as absent')
    check('s3_4_c15_c1_3' in kcload.KNOWN_IMPDEF, 'known-register table is populated')


def test_tidcp_report_distinguishes_empty_from_unreadable(capsys=None):
    import io
    import contextlib
    base = kcload.TIDCP_LOG_PA
    empty = (f'{base:016x}: {kcload.TIDCP_LOG_MAGIC:#018x} 0x0000000000000000\n'
             f'{base + 0x10:016x}: 0x0000000000000000 0x0000000000000000\n')
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        kcload.report_tidcp('', empty)
    out = buf.getvalue()
    check('none:' in out and 'RAZ/WI never ran' in out, f'empty log is stated as empty: {out!r}')
    check('UNKNOWN' not in out, 'a readable empty log is not reported as unknown')
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        kcload.report_tidcp('', 'nothing here')
    out = buf.getvalue()
    check('UNKNOWN' in out, f'an unreadable log is stated as unknown: {out!r}')


def mmio_tree(rtc_offset=0x10050000, ranges=None):
    # The smoke's arm-io ranges, a PL011 and GIC already moved onto QEMU's
    # map, and one device left at a template-style offset.
    if ranges is None:
        ranges = struct.pack('<QQQ', 0, kcload.ARMIO_PARENT_BASE, 0x40000000)
    uart = node('uart0', [prop('reg', struct.pack('<QQ', kcload.QEMU_UART_OFF, 0x1000))])
    gic = node('gic', [prop('reg', struct.pack('<QQQQQQ', kcload.QEMU_GICD_OFF, 0x10000,
                                               kcload.QEMU_GICR_OFF, 0x20000, 0xfff0000, 0x10000))])
    rtc = node('rtc', [prop('reg', struct.pack('<QQ', rtc_offset, 0x1000))])
    return node('device-tree', [], [node('arm-io', [prop('ranges', ranges)], [uart, gic, rtc])])


def trace_line(direction, pa, value, size, region):
    return (f'memory_region_ops_{direction} cpu 0 mr 0x10a0b0c0 addr {pa:#x} '
            f'value {value:#x} size {size} name \'{region}\'')


def run_report(trace, tree):
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rows = kcload.report_mmio(trace, tree)
    return rows, buf.getvalue()


def test_mmio_report():
    tree = mmio_tree()
    windows = kcload.mmio_windows(appledt.DeviceTree(tree))
    check((0x09000000, 0x1000, 'uart0', 0) in windows, f'the PL011 window is at 0x09000000: {windows}')
    check((0x17ff0000, 0x10000, 'gic', 2) in windows, 'the third GIC window moves with arm-io ranges')
    check((0x18050000, 0x1000, 'rtc', 0) in windows, 'a template offset moves with arm-io ranges')
    trace = '\n'.join([
        'PL011 data written to disabled UART',
        trace_line('write', 0x09000000, 0x41, 4, 'pl011'),
        trace_line('read', 0x18050000, (1 << 64) - 1, 4, 'gpex_mmio_window'),
        '12345@1700000000.000001:' + trace_line('write', 0x18050010, 1, 4, 'gpex_mmio_window'),
        trace_line('read', 0x08000004, 0x3, 4, 'gicv3_dist'),
        trace_line('write', 0x004ffff8, 0x4aa8fb60, 4, 'virt.flash0'),
        trace_line('read', 0x17ff0008, 0x12345678, 4, 'gpex_mmio_window'),
        trace_line('read', 0x09000018, 0x90, 4, 'pl011'),
    ])
    rows, text = run_report(trace, tree)
    labels = [r[0] for r in rows]
    check(labels == ['uart0[0]', 'rtc[0]', 'gic[0]', '(no tree node) 0x004ff000', 'gic[2]'],
          f'windows in first-touch order: {labels}')
    by = {r[0]: r for r in rows}
    check(by['uart0[0]'][5] and by['gic[0]'][5], 'the PL011 and GICD count as QEMU models')
    check(by['uart0[0]'][1:3] == (1, 1), 'reads and writes are counted per window')
    check(not by['rtc[0]'][5] and by['rtc[0]'][1:3] == (1, 1),
          'a template device answered by the PCIe window is not modelled; the prefixed line counts')
    check('rtc[0]' in text and 'every read all-ones' in text.split('rtc[0]')[1].splitlines()[0],
          'an all-ones answer is called out')
    check(not by['gic[2]'][5] and 'all-ones' not in text.split('gic[2]')[1].splitlines()[0],
          'the third GIC window is not one of the moved ones, and a real value is not all-ones')
    check(by['(no tree node) 0x004ff000'][4] == 0xff8 and not by['(no tree node) 0x004ff000'][5],
          'an address outside every window is reported by page, with its offset')
    check('5 windows touched, 3 not modelled' in text, f'totals line: {text!r}')

    # Deliberately wrong pairings the report must not accept as modelled.
    wrong = trace_line('read', 0x09000000, 0, 4, 'gpex_mmio_window')
    rows, _ = run_report(wrong, tree)
    check(rows[0][0] == 'uart0[0]' and not rows[0][5],
          'the PL011 window answered by something other than the PL011 is not modelled')
    rows, text = run_report('nothing traced here\n', tree)
    check(rows is None and 'UNKNOWN' in text, 'a trace with no accesses is UNKNOWN, not "nothing touched"')
    for bad in (mmio_tree(ranges=bytes(16)), mmio_tree(rtc_offset=0x50000000)):
        try:
            kcload.mmio_windows(appledt.DeviceTree(bad))
            check(False, 'a malformed arm-io ranges or out-of-range reg is refused')
        except SystemExit:
            check(True, 'a malformed arm-io ranges or out-of-range reg is refused')


def main():
    test_dt_completion()
    test_nvram()
    test_boot_args()
    test_layout_alignment()
    test_tidcp_decoder()
    test_tidcp_log_parse()
    test_tidcp_report_distinguishes_empty_from_unreadable()
    test_mmio_report()
    print(f'PASS test-vmapple-smoke: {checks} checks')


if __name__ == '__main__':
    main()
