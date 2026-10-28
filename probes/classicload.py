#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The classic loader for a VirtualMac2,1 guest (Route B).

probes/xnuboot.py re-exports this module, so there is one loader with two
entry paths: the SPTM path for the J700 guest and this classic path. It is
kept free of the SPTM import chain so the QEMU smoke harness
(tools/vmapple-smoke) can use it on a stock host Python.

The classic entry contract is B8 (docs/GAOKUN3-PHASE0B.md): EL1, x0 the
physical address of boot_args, MMU and caches off; the kernel builds its
own tables. boot_args is the revision-1/version-2, 0x480-byte arm64
structure from pexpert/arm64/boot.h:

  0x00 Revision u16      0x02 Version u16
  0x08 virtBase          0x10 physBase       0x18 memSize
  0x20 topOfKernelData   0x28 Video[6]       0x58 machineType u32
  0x60 deviceTreeP       0x68 deviceTreeLength u32
  0x6c CommandLine[1024] 0x470 bootFlags     0x478 memSizeActual
"""
from dataclasses import dataclass
from typing import Optional, Tuple
import json
import os
import pathlib
import struct

from appledt import DeviceTree

BOOT_ARGS_SIZE = 0x480
BOOT_LINE_LENGTH = 1024
BOOT_ARGS_REVISION = 1
BOOT_ARGS_VERSION = 2
VIDEO_DISPLAY_MODE = 0          # text mode: XNU paints glyphs itself


def boot_args(virt_base, phys_base, mem_size, top_of_kernel_data,
              devicetree_va, dt_len, cmdline, video=None):
    """Pack one boot_args. `video` is (base, width, height, stride, depth)
    or None for no framebuffer."""
    b = bytearray(BOOT_ARGS_SIZE)
    struct.pack_into('<HH', b, 0, BOOT_ARGS_REVISION, BOOT_ARGS_VERSION)
    struct.pack_into('<QQQQ', b, 8, virt_base, phys_base, mem_size,
                     top_of_kernel_data)
    if video is not None:
        vbase, vwidth, vheight, vstride, vdepth = video
        struct.pack_into('<6Q', b, 0x28, vbase, VIDEO_DISPLAY_MODE, vstride,
                         vwidth, vheight, vdepth)
    struct.pack_into('<I', b, 0x58, 0)                  # machineType
    struct.pack_into('<Q', b, 0x60, devicetree_va)
    struct.pack_into('<I', b, 0x68, dt_len)
    line = cmdline.encode()
    if len(line) >= BOOT_LINE_LENGTH or b'\0' in line:
        raise ValueError('boot command line is too long or contains a NUL')
    b[0x6c:0x6c + len(line)] = line
    struct.pack_into('<Q', b, 0x478, mem_size)          # memSizeActual
    return b


# ---------------------------------------------------------------------------
# The classic loader (Route B, docs/GAOKUN3-PHASE3.md P2).
#
# The VirtualMac2,1 kernel takes the classic XNU entry (B8): EL1, x0 the
# physical address of boot_args, MMU and caches off, and it builds its own
# tables. There is no SPTM boot structure, frame table or loader-built kernel
# page tables. This path therefore shares only boot_args() and appledt with
# the SPTM path above; everything else is layout, tree completion and linear
# segment placement. The kernelcache's physical base keeps its Mach header's
# 32 MiB-block offset, and PA = VA - mhVA + ram_base throughout.
#
# The VM firmware's duties this loader performs (Phase 0C condition 4):
# placeholders resolved, /chosen/dram-base and dram-size, 256 bytes of real
# entropy in /chosen/random-seed (the caller supplies the source; an empty
# or all-zero fill is refused), an empty valid NVRAM bank as
# /chosen/nvram-proxy-data, /cpus trimmed to the CPUs actually started, the
# counter rate, and the panic region: published XNU (xnu-12377.121.6,
# pexpert/arm/pe_init.c, check_for_panic_log) reads the region's physical
# base from /pram's reg[0] and its usable size from
# /chosen/embedded-panic-log-size, and leaves gPanicBase unset when either
# is missing. The template's /pram reg is all zero and it carries no
# log-size property, so both are supplied here.

CLASSIC_RANDOM_SEED_BYTES = 256
CLASSIC_NVRAM_HEADER_SIG = 0x5A
CLASSIC_NVRAM_PARTITION_SIG = 0x70
CLASSIC_CHRP_BLOCK = 16


@dataclass(frozen=True)
class ClassicLayout:
    """Where a classic guest lives, and the rates it is told about.

    All addresses are physical. `video` is a boot_args.Video tuple
    (base, width, height, stride_bytes, depth) or None for no framebuffer.
    """
    name: str
    ram_base: int
    mem_size: int
    dt_pa: int
    boot_args_pa: int
    top_of_kernel_data: int
    panic_pa: int
    panic_size: int
    panic_log_size: int
    timebase_hz: int
    guest_cpus: int
    armio_parent_base: int
    uart_off: int
    gicd_off: int
    gicr_off: int
    cmdline: str
    nvram_bank_size: int = 0x10000
    nvram_system_size: int = 0x3000
    video: Optional[Tuple[int, int, int, int, int]] = None
    # Device-tree nodes the completed tree leaves out (Phase 3 decision 5,
    # option b): a VM without that device.
    drop_nodes: Tuple[str, ...] = ()

    @property
    def gicr_size(self):
        return self.guest_cpus * 0x20000    # RD_base + SGI_base per CPU

    def validate(self):
        top = self.ram_base + self.mem_size
        if not (self.ram_base < self.dt_pa < self.boot_args_pa
                < self.top_of_kernel_data <= top):
            raise ValueError('classic layout: loader ranges are not ordered inside guest RAM')
        if self.top_of_kernel_data & 0x3fff:
            raise ValueError('classic layout: topOfKernelData is not 16 KiB aligned')
        # The panic region is firmware-carved memory, as iBoot's is: it must
        # not lie inside the DRAM the guest is told it owns. Supplying it
        # inside DRAM regressed the QEMU boot to a pre-console halt
        # (2026-10-27, OBSERVED); outside DRAM the same boot passes.
        if self.ram_base <= self.panic_pa < top or self.ram_base < self.panic_pa + self.panic_size <= top:
            raise ValueError('classic layout: panic region overlaps the reported guest DRAM')
        if self.panic_pa < self.top_of_kernel_data:
            raise ValueError('classic layout: panic region overlaps the loader data')
        if not 0 < self.panic_log_size <= self.panic_size:
            raise ValueError('classic layout: panic log size exceeds its region')
        if self.guest_cpus < 1 or self.timebase_hz < 1:
            raise ValueError('classic layout: no CPUs or no counter rate')
        return self


# QEMU virt, as the Route B smoke has always run it: UART at 0x09000000,
# GICD at 0x08000000, GICR at 0x080A0000, under an arm-io range whose parent
# base is the GICD base. The other template devices keep their offsets, so
# the range moves them too; that is not a model of them.
QEMU_CLASSIC_LAYOUT = ClassicLayout(
    name='qemu-virt', ram_base=0x45004000, mem_size=0x1C000000,
    dt_pa=0x4A000000, boot_args_pa=0x4A040000, top_of_kernel_data=0x4A050000,
    panic_pa=0x61004000, panic_size=0x100000, panic_log_size=0x80000,
    timebase_hz=1_000_000_000, guest_cpus=1,
    armio_parent_base=0x08000000, uart_off=0x01000000, gicd_off=0x00000000,
    gicr_off=0x000A0000, cmdline='-v serial=3 -noprogress').validate()


# QEMU virt with the Route B monitor (Phase 3 P3): the tree keeps the
# VirtualMac template's own physical map (arm-io parent 0x10000000), so the
# guest's device IPAs are the template addresses and every one of them is a
# monitor model behind stage 2, never a QEMU device. The paravirtual
# devices with nodes are left out of the tree (decision 5, option b,
# recommended; the decision itself is the owner's and still open): the
# paravirtual GPU and IOSurface mapper, the AES engine, and the avp,*
# platform devices other than the published-contract RTC and buttons,
# which are modelled. The two fixed addresses have no node to drop; the
# monitor stops on them by default (option c).
QEMU_MONITOR_LAYOUT = ClassicLayout(
    name='qemu-virt-monitor', ram_base=0x45004000, mem_size=0x1C000000,
    dt_pa=0x4A000000, boot_args_pa=0x4A040000, top_of_kernel_data=0x4A050000,
    panic_pa=0x61004000, panic_size=0x100000, panic_log_size=0x80000,
    timebase_hz=1_000_000_000, guest_cpus=1,
    armio_parent_base=0x10000000, uart_off=0x10010000, gicd_off=0x00000000,
    gicr_off=0x00010000, cmdline='-v serial=3 -noprogress',
    drop_nodes=('/device-tree/arm-io/gfx',
                '/device-tree/arm-io/iosurface-mapper',
                '/device-tree/arm-io/aes',
                '/device-tree/arm-io/avp-rtc',
                '/device-tree/arm-io/avp-ctrr',
                '/device-tree/arm-io/avp-sealed-registers')).validate()


def _classic_chrp_header(sig, length, name):
    """One 16-byte CHRP partition header; its checksum folds carries back."""
    if length % CLASSIC_CHRP_BLOCK or not CLASSIC_CHRP_BLOCK <= length <= 0xFFFF * CLASSIC_CHRP_BLOCK:
        raise ValueError(f'CHRP partition length {length:#x}')
    body = struct.pack('<H12s', length // CLASSIC_CHRP_BLOCK, name.encode())
    total = sig + sum(body)
    while total > 0xFF:
        total = (total & 0xFF) + (total >> 8)
    return struct.pack('<BB', sig, total) + body


def classic_nvram_image(bank_size=0x10000, system_size=0x3000):
    """An empty version-1 Apple NVRAM bank (header, system, common).

    The CHRP handler requires this image; without a valid one it reads
    through a null image and the boot stops. Empty means each partition's
    first byte is NUL, where the kernel stops reading variables."""
    import zlib
    common_size = bank_size - 32 - system_size
    if common_size < CLASSIC_CHRP_BLOCK:
        raise ValueError(f'NVRAM bank {bank_size:#x} too small for a {system_size:#x} system partition')
    image = bytearray(bank_size)
    image[0:16] = _classic_chrp_header(CLASSIC_NVRAM_HEADER_SIG, 32, 'nvram')
    struct.pack_into('<I', image, 20, 1)                    # generation
    image[32:48] = _classic_chrp_header(CLASSIC_NVRAM_PARTITION_SIG, system_size, 'system')
    off = 32 + system_size
    image[off:off + 16] = _classic_chrp_header(CLASSIC_NVRAM_PARTITION_SIG, common_size, 'common')
    struct.pack_into('<I', image, 16, zlib.adler32(bytes(image[20:])))
    return bytes(image)


def classic_segments(data):
    """(segments, entry VA) from a kernelcache's main Mach-O header.

    Each segment is (vmaddr, file offset, file size, virtual size)."""
    if len(data) < 32:
        raise ValueError('kernelcache is too short for a Mach-O header')
    magic, ncmds = struct.unpack_from('<II', data, 0)[0], struct.unpack_from('<I', data, 16)[0]
    if magic != 0xFEEDFACF:
        raise ValueError('not a 64-bit Mach-O (kernelcache expected)')
    segs, entry, p = [], None, 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, p)
        if cmd == 0x19:                                     # LC_SEGMENT_64
            va, vs, fo, fs = struct.unpack_from('<QQQQ', data, p + 24)
            segs.append((va, fo, fs, vs))
        elif cmd == 5:                                      # LC_UNIXTHREAD
            entry = struct.unpack_from('<Q', data, p + 16 + 32 * 8)[0]
        p += size
    if not segs or entry is None:
        raise ValueError('no segments or no LC_UNIXTHREAD entry')
    return segs, entry


def classic_trim_cpus(dt, count):
    """Keep only the CPU nodes with reg below count; refuse the rest.

    The boot CPU must be reg 0 and "running". A template with fewer CPUs
    than the layout starts is refused rather than padded, because a node
    for each extra CPU would have to be invented."""
    cpus = '/device-tree/cpus'
    kept, dropped = [], []
    for path in dt.children.get(cpus, []):
        reg = dt.get(path, 'reg')
        if reg is None or len(reg) != 4:
            raise ValueError(f'device tree: {path} has no 4-byte reg')
        (kept if struct.unpack('<I', reg)[0] < count else dropped).append(path)
    regs = sorted(struct.unpack('<I', dt.get(p, 'reg'))[0] for p in kept)
    if regs != list(range(count)):
        raise ValueError(f'device tree: CPU regs {regs} do not cover the {count} vCPUs started')
    boot = next(p for p in kept if dt.get(p, 'reg') == bytes(4))
    if (dt.get(boot, 'state') or b'').rstrip(b'\0') != b'running':
        raise ValueError(f'device tree: boot CPU {boot} is not "running"')
    for path in dropped:
        doomed = [path]
        while doomed:
            node = doomed.pop()
            doomed += dt.children.pop(node, [])
            del dt.nodes[node]
    dt.children[cpus] = kept
    return len(dropped)


def classic_drop_nodes(dt, paths):
    """Remove nodes (and their subtrees) from a parsed tree, by path."""
    for path in paths:
        if path not in dt.nodes:
            raise ValueError(f'device tree: cannot drop absent node {path}')
        parent, _, _ = path.rpartition('/')
        doomed = [path]
        while doomed:
            node = doomed.pop()
            doomed += dt.children.pop(node, [])
            del dt.nodes[node]
        dt.children[parent].remove(path)


def classic_complete_dt(dt, layout, entropy):
    """Complete a parsed VirtualMac template for `layout`.

    Returns (blob, placeholders_resolved). `entropy(n)` supplies the boot
    seed; the seeded tree is a build artifact and is never committed."""
    layout.validate()
    resolved = dt.resolve_placeholders()

    seed = dt.get('/device-tree/chosen', 'random-seed')
    if seed is None or len(seed) < CLASSIC_RANDOM_SEED_BYTES:
        raise ValueError(f'device tree: /chosen/random-seed has '
                         f'{"no" if seed is None else len(seed)} bytes, XNU needs '
                         f'{CLASSIC_RANDOM_SEED_BYTES}')
    fill = entropy(len(seed))
    if len(fill) != len(seed) or not any(fill):
        raise ValueError('device tree: the entropy source returned no usable random-seed')
    dt.set('/device-tree/chosen', 'random-seed', fill)

    dt.put('/device-tree/chosen', 'nvram-bank-size', struct.pack('<I', layout.nvram_bank_size))
    dt.put('/device-tree/chosen', 'nvram-bank-count', struct.pack('<I', 1))
    dt.put('/device-tree/chosen', 'nvram-current-bank', struct.pack('<I', 0))
    dt.put('/device-tree/chosen', 'nvram-proxy-data',
           classic_nvram_image(layout.nvram_bank_size, layout.nvram_system_size))

    if '/device-tree/pram' not in dt.nodes:
        raise ValueError('device tree: no /pram node for the panic region')
    dt.set('/device-tree/pram', 'reg',
           struct.pack('<QQ', layout.panic_pa, layout.panic_size))
    dt.put('/device-tree/chosen', 'embedded-panic-log-size',
           struct.pack('<I', layout.panic_log_size))

    dt.add('/device-tree/chosen', 'dram-base', struct.pack('<Q', layout.ram_base))
    dt.add('/device-tree/chosen', 'dram-size', struct.pack('<Q', layout.mem_size))
    classic_trim_cpus(dt, layout.guest_cpus)
    dt.set_u32('/device-tree', 'clock-frequency', layout.timebase_hz)
    changed = dt.set_all('timebase-frequency', struct.pack('<Q', layout.timebase_hz),
                         under='/device-tree/cpus')
    if not changed:
        raise ValueError('device tree: no cpu timebase-frequency properties found')

    armio = '/device-tree/arm-io'
    dt.set(armio, 'ranges', struct.pack('<QQQ', 0, layout.armio_parent_base, 0x40000000))
    dt.set(armio + '/uart0', 'reg', struct.pack('<QQ', layout.uart_off, 0x1000))
    gic = dt.get(armio + '/gic', 'reg')
    if len(gic) != 48:
        raise ValueError(f'device tree: gic reg has {len(gic)} bytes, expected 48')
    dt.set(armio + '/gic', 'reg',
           struct.pack('<QQQQ', layout.gicd_off, 0x10000, layout.gicr_off,
                       layout.gicr_size) + gic[32:])
    classic_drop_nodes(dt, layout.drop_nodes)
    return dt.serialize(), resolved


def derive_gaokun3_layout(reserved, dramatic_top=0x80000000 + (16 << 30),
                          guest_mem_size=0x60000000, timebase_hz=19_200_000):
    """Derive the gaokun3 ClassicLayout from the SLB reserved regions.

    `reserved` is a list of (base, size) no-map regions from the machine's
    own device tree (Phase 3 P6); the guest's DRAM, its loader data and
    its panic chunk must all sit in one free gap of the first DRAM
    window, and the layout refuses any overlap: interiors must not meet,
    while a reserved region that ends where the guest begins is adjacent,
    not overlapping. The kernelcache's Mach header keeps a 32 MiB-block
    offset of 0x1004000, so ram_base must match it.

    The payload windows mirror the QEMU layouts: the device tree sits
    0x4ffc000 bytes into the window, boot_args 256 KiB later and
    topOfKernelData 64 KiB after that. Device IPAs stay the VirtualMac
    template's; the framebuffer is decision 3, still open, so none is
    published here.
    """
    dt_off = 0x4FFC000
    span = dt_off + 0x50000                     # payload through topOfKernelData
    gaps = []
    cursor = 0x80000000
    for base, size in sorted(reserved):
        if base > cursor:
            gaps.append((cursor, base))
        cursor = max(cursor, base + size)
    if cursor < dramatic_top:
        gaps.append((cursor, dramatic_top))

    def overlaps(a_base, a_size, b_base, b_size):
        return a_base < b_base + b_size and b_base < a_base + a_size

    for gap_base, gap_end in gaps:
        # First address in the gap with the header's 32 MiB-block offset.
        ram_base = gap_base
        want = 0x1004000 & 0x1FFFFFF
        ram_base += (want - (ram_base & 0x1FFFFFF)) & 0x1FFFFFF
        if ram_base + span > gap_end:
            continue
        if ram_base + guest_mem_size + 0x100000 > gap_end:
            continue                                # panic chunk must fit too
        window = (ram_base, guest_mem_size + 0x100000)
        if any(overlaps(*window, rb, rs) for rb, rs in reserved):
            continue
        return ClassicLayout(
            name='gaokun3', ram_base=ram_base, mem_size=guest_mem_size,
            dt_pa=ram_base + dt_off, boot_args_pa=ram_base + dt_off + 0x40000,
            top_of_kernel_data=ram_base + dt_off + 0x50000,
            panic_pa=ram_base + guest_mem_size, panic_size=0x100000,
            panic_log_size=0x80000, timebase_hz=timebase_hz, guest_cpus=8,
            armio_parent_base=0x10000000, uart_off=0x10010000,
            gicd_off=0x00000000, gicr_off=0x00010000,
            cmdline='-v serial=3 -noprogress',
            drop_nodes=QEMU_MONITOR_LAYOUT.drop_nodes).validate()
    raise ValueError('gaokun3 layout: no reserved-free gap fits the guest window')


@dataclass(frozen=True)
class ClassicImage:
    payload: bytes
    payload_pa: int
    entry_pa: int
    boot_args_pa: int
    dt_blob: bytes
    info: dict
    # Every (PA, bytes) a placer must write, including the zeroed panic
    # region beyond the reported DRAM. `payload` is the first chunk.
    chunks: tuple = ()


def build_classic_image(kernelcache, devicetree, layout=QEMU_CLASSIC_LAYOUT,
                        entropy=None):
    """Build a classic guest image from local, never-committed inputs.

    Returns a ClassicImage: the payload chunk holds the kernel segments,
    the completed tree and boot_args; `chunks` adds the zeroed panic
    region beyond the reported DRAM. Entry is at EL1 with
    x0 = boot_args_pa."""
    layout.validate()
    if entropy is None:
        entropy = os.urandom
    data = pathlib.Path(kernelcache).read_bytes()
    segs, entry_va = classic_segments(data)
    mh_va = min(va for va, _, _, _ in segs)
    span = max(va + vs for va, _, _, vs in segs) - mh_va
    if layout.ram_base + span > layout.dt_pa:
        raise ValueError(f'kernelcache spans {span:#x}; does not fit below {layout.dt_pa:#x}')
    if mh_va & 0x1FFFFFF != layout.ram_base & 0x1FFFFFF:
        raise ValueError('kernelcache Mach header and RAM base differ in their 32 MiB-block offset')

    dt_blob, resolved = classic_complete_dt(
        DeviceTree(pathlib.Path(devicetree).read_bytes()), layout, entropy)
    if len(dt_blob) > layout.boot_args_pa - layout.dt_pa:
        raise ValueError(f'device tree is {len(dt_blob):#x} bytes; only '
                         f'{layout.boot_args_pa - layout.dt_pa:#x} fit before boot_args')
    dt_va = layout.dt_pa - layout.ram_base + mh_va

    payload = bytearray(layout.top_of_kernel_data - layout.ram_base)
    for va, fo, fs, _vs in segs:
        payload[va - mh_va:va - mh_va + fs] = data[fo:fo + fs]
    payload[layout.dt_pa - layout.ram_base:
            layout.dt_pa - layout.ram_base + len(dt_blob)] = dt_blob
    args = boot_args(mh_va, layout.ram_base, layout.mem_size,
                     layout.top_of_kernel_data, dt_va, len(dt_blob),
                     cmdline=layout.cmdline, video=layout.video)
    if layout.boot_args_pa + len(args) > layout.top_of_kernel_data:
        raise ValueError('boot_args do not fit below topOfKernelData')
    payload[layout.boot_args_pa - layout.ram_base:
            layout.boot_args_pa - layout.ram_base + len(args)] = args

    info = {
        'profile': 'vma2macosap-26A428', 'layout': layout.name,
        'mh_va': f'{mh_va:#x}', 'entry_va': f'{entry_va:#x}',
        'entry_pa': f'{entry_va - mh_va + layout.ram_base:#x}',
        'phys_base': f'{layout.ram_base:#x}', 'mem_size': f'{layout.mem_size:#x}',
        'dt_pa': f'{layout.dt_pa:#x}', 'dt_va': f'{dt_va:#x}', 'dt_len': len(dt_blob),
        'boot_args_pa': f'{layout.boot_args_pa:#x}',
        'top_of_kernel_data': f'{layout.top_of_kernel_data:#x}',
        'panic_pa': f'{layout.panic_pa:#x}', 'panic_size': f'{layout.panic_size:#x}',
        'panic_log_size': f'{layout.panic_log_size:#x}',
        'placeholders_resolved': resolved, 'timebase_hz': layout.timebase_hz,
        'cmdline': layout.cmdline,
    }
    payload = bytes(payload)
    return ClassicImage(payload, layout.ram_base,
                        entry_va - mh_va + layout.ram_base, layout.boot_args_pa,
                        dt_blob, info,
                        chunks=((layout.ram_base, payload),
                                (layout.panic_pa, bytes(layout.panic_size))))


def write_classic_image(image, out):
    """Write a ClassicImage and its layout record under `out` (ignored)."""
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    blob_path = out / 'vmapple-guest.bin'
    blob_path.write_bytes(image.payload)
    (out / 'devicetree.vma2smoke').write_bytes(image.dt_blob)
    (out / 'layout.json').write_text(json.dumps(image.info, indent=2))
    return blob_path


