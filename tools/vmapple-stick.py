#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and check a VirtualMac stick bundle for q1n1 (Phase 3 P5).

Host and QEMU only. A bundle is what is copied to the boot stick for
q1n1 to place: the classic loader's chunks (one file per chunk), the
completed device tree, and a manifest recording every chunk's physical
address, size and hash, the entry and boot_args addresses, the guest
profile and the layout. The gaokun3 chain boots from q1n1 (decision 2),
not from tools/cycle.py. Loading it on hardware is Phase 3's H steps and
is not done here.

The output directory is the stick's tree: Snapintosh/vmapple/ holds
bundle.bin, the binary manifest q1n1's -DQ1N1_VMAPPLE_STICK reads
(q1n1 platform/uefi/vmapple-stick.h), and chunkN.bin; manifest.json and
devicetree.bin beside them are for the host. The boot seed is never on
the stick: the chunks carry a zero /chosen/random-seed slot, the
manifests record where it is, and q1n1 fills it from UEFI's RAW RNG
before ExitBootServices (Phase 0C condition 3).

Checks mirror the discipline the boot workflow uses elsewhere:

- `build` refuses a layout whose chunks overlap, are unaligned, or
  leave the layout's guest window, and a kernelcache whose profile
  hashes do not match (when guestprofile can run).
- `check` re-reads a bundle and refuses any chunk whose bytes no
  longer hash to the manifest (prepare/check agreement).
- `qualify` prints the bundle's qualification hash, and `check-boot`
  refuses a boot whose presented hash does not match it. There is no
  boot in this tool; the refusal is the host half of the rule that a
  boot never runs without a matching qualification.
"""
import hashlib
import json
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'probes'))
import classicload


def _sha(data):
    return hashlib.sha256(data).hexdigest()


STICK_DIR = 'Snapintosh/vmapple'
SEED_BYTES = 256
Q1N1_VMB_MAGIC = 0x31424D56314E3151          # "Q1N1VMB1"
Q1N1_VMB_MAX = 8
Q1N1_VMB_PATH_BYTES = 64


def deferred_seed_image(kernelcache, dt_path, layout):
    """The image with an empty /chosen/random-seed slot, and where it is.

    The loader refuses an all-zero seed, so the image is built twice with
    two different fills; the bytes that differ are exactly the slot. Any
    other difference, or a slot that is not one 256-byte run inside one
    chunk, is refused. Returns (image, seed_pa, seed_chunk)."""
    from dataclasses import replace
    a = classicload.build_classic_image(kernelcache, dt_path, layout,
                                        entropy=lambda n: b'\x5a' * n)
    b = classicload.build_classic_image(kernelcache, dt_path, layout,
                                        entropy=lambda n: b'\xa5' * n)
    if [(pa, len(d)) for pa, d in a.chunks] != [(pa, len(d)) for pa, d in b.chunks]:
        refusal('the seed changes the chunk layout')
    found = []
    for i, ((pa, da), (_, db)) in enumerate(zip(a.chunks, b.chunks)):
        diff = [j for j in range(len(da)) if da[j] != db[j]] if da != db else []
        if diff:
            found.append((i, pa, diff))
    if len(found) != 1:
        refusal(f'the seed should differ in one chunk, not {len(found)}')
    index, pa, diff = found[0]
    if len(diff) != SEED_BYTES or diff[-1] - diff[0] != SEED_BYTES - 1:
        refusal(f'the seed slot is {len(diff)} bytes, not one {SEED_BYTES}-byte run')
    start = diff[0]
    chunks = list(a.chunks)
    data = bytearray(chunks[index][1])
    data[start:start + SEED_BYTES] = bytes(SEED_BYTES)
    chunks[index] = (pa, bytes(data))
    image = replace(a, chunks=tuple(chunks),
                    payload=chunks[0][1] if index == 0 else a.payload,
                    dt_blob=bytes(SEED_BYTES).join(a.dt_blob.split(b'\x5a' * SEED_BYTES, 1)))
    return image, pa + start, index


def q1n1_bundle(manifest, qualification):
    """bundle.bin: q1n1's struct q1n1_vmb_manifest, then one 112-byte
    record per chunk. Version 1 is the 96-byte header H1 placed. Version 2
    (a `monitor` entry in the manifest) extends it to 176 bytes with the
    monitor's entry and parameter page, the stage-2 RAM range and the
    screen split, for the H2 hand-off; --vmapple-place still accepts both,
    the entering mode requires version 2. Version 3 (a `ramdisk` entry)
    extends version 2 to 192 bytes with the disk's address and size,
    which q1n1 writes to the parameter page at +0x58/+0x60."""
    chunks = manifest['chunks']
    if not 0 < len(chunks) <= Q1N1_VMB_MAX:
        refusal(f'{len(chunks)} chunks; q1n1 places 1 to {Q1N1_VMB_MAX}')
    seed = manifest['seed']
    monitor = manifest.get('monitor')
    ramdisk = manifest.get('ramdisk')
    if monitor is None:
        body = struct.pack('<8Q32s', Q1N1_VMB_MAGIC, 1, len(chunks), 112, manifest['entry_pa'],
                           manifest['boot_args_pa'], seed['pa'], seed['chunk'],
                           bytes.fromhex(qualification))
    else:
        fb = monitor['framebuffer']
        head = (Q1N1_VMB_MAGIC, 3 if ramdisk else 2, len(chunks), 112,
                manifest['entry_pa'], manifest['boot_args_pa'],
                seed['pa'], seed['chunk'], monitor['entry_pa'],
                monitor['params_pa'], monitor['s2_lo'], monitor['s2_hi'],
                fb['pa'], fb['width'], fb['stride'], fb['guest_rows'],
                fb['band_rows'], 0)
        if ramdisk is None:
            body = struct.pack('<18Q32s', *head, bytes.fromhex(qualification))
        else:
            body = struct.pack('<20Q32s', *head, ramdisk['pa'], ramdisk['size'],
                               bytes.fromhex(qualification))
    for c in chunks:
        path = '\\' + c['file'].replace('/', '\\')
        raw = path.encode('ascii')
        if len(raw) >= Q1N1_VMB_PATH_BYTES:
            refusal(f'chunk path {path} is too long for q1n1')
        body += struct.pack('<2Q32s64s', c['pa'], c['size'], bytes.fromhex(c['sha256']),
                            raw.ljust(Q1N1_VMB_PATH_BYTES, b'\0'))
    return body


def bundle_bytes(image, seed_pa, seed_chunk, monitor=None, ramdisk=None):
    """(manifest_dict, {relative path: bytes}) for a ClassicImage whose seed
    slot is empty (deferred_seed_image). `monitor` is (data, pa, params_pa,
    s2_range, framebuffer) for a version 2 bundle: the monitor image as one
    more chunk, entered at its first byte. framebuffer is
    (pa, width, stride, guest_rows, band_rows) or None. `ramdisk` is
    (data, pa) for a Phase 4 P3 RAM-backed root disk: one more chunk,
    recorded in the manifest, and bundle.bin version 3 carries its
    address and size for q1n1 to write to the parameter page
    (+0x58/+0x60). A ramdisk without a monitor is refused: the monitor
    is what mediates it as a VirtIO block device."""
    files = {}
    chunks = []
    for i, (pa, data) in enumerate(image.chunks):
        name = f'{STICK_DIR}/chunk{i}.bin'
        files[name] = data
        chunks.append({'file': name, 'pa': pa, 'size': len(data), 'sha256': _sha(data)})
    files['devicetree.bin'] = image.dt_blob
    manifest = {
        'format': 'snapintosh-vmapple-stick-2',
        'profile': image.info['profile'],
        'layout': image.info['layout'],
        'entry_pa': image.entry_pa,
        'boot_args_pa': image.boot_args_pa,
        'chunks': chunks,
        'seed': {'pa': seed_pa, 'chunk': seed_chunk, 'size': SEED_BYTES,
                 'source': 'q1n1 fills it from the UEFI RNG protocol, RAW only'},
        'devicetree': {'size': len(image.dt_blob), 'sha256': _sha(image.dt_blob)},
        'info': image.info,
    }
    if monitor is not None:
        data, pa, params_pa, (s2_lo, s2_hi), fb = monitor
        if pa % 0x1000 or params_pa % 0x1000:
            refusal('the monitor image and parameter page must be page aligned')
        placed = sorted((c['pa'], c['pa'] + c['size']) for c in chunks)
        for lo, hi in placed + [(params_pa, params_pa + 0x1000)]:
            if pa < hi and lo < pa + len(data):
                refusal(f'the monitor image at {pa:#x} overlaps {lo:#x}')
        if params_pa < pa + len(data) and pa < params_pa + 0x1000:
            refusal('the parameter page overlaps the monitor image')
        name = f'{STICK_DIR}/chunk{len(chunks)}.bin'
        files[name] = data
        chunks.append({'file': name, 'pa': pa, 'size': len(data), 'sha256': _sha(data)})
        fb_pa, fb_width, fb_stride, fb_guest, fb_band = fb or (0, 0, 0, 0, 0)
        manifest['monitor'] = {
            'chunk': len(chunks) - 1, 'entry_pa': pa, 'params_pa': params_pa,
            's2_lo': s2_lo, 's2_hi': s2_hi,
            'framebuffer': {'pa': fb_pa, 'width': fb_width, 'stride': fb_stride,
                            'guest_rows': fb_guest, 'band_rows': fb_band},
        }
    if ramdisk is not None:
        data, pa = ramdisk
        if monitor is None:
            refusal('a ramdisk bundle needs a monitor to mediate the disk')
        if not data:
            refusal('a ramdisk chunk cannot be empty')
        if pa % 0x4000:
            refusal(f'the ramdisk at {pa:#x} is not 16 KiB aligned')
        placed = sorted((c['pa'], c['pa'] + c['size']) for c in chunks)
        if monitor is not None:
            placed.append((manifest['monitor']['params_pa'],
                           manifest['monitor']['params_pa'] + 0x1000))
        for lo, hi in placed:
            if pa < hi and lo < pa + len(data):
                refusal(f'the ramdisk at {pa:#x} overlaps {lo:#x}')
        name = f'{STICK_DIR}/chunk{len(chunks)}.bin'
        files[name] = data
        chunks.append({'file': name, 'pa': pa, 'size': len(data), 'sha256': _sha(data)})
        manifest['ramdisk'] = {'chunk': len(chunks) - 1, 'pa': pa, 'size': len(data)}
    files[f'{STICK_DIR}/bundle.bin'] = q1n1_bundle(manifest, qualification_hash(manifest))
    return manifest, files


def refusal(text):
    raise SystemExit(f'vmapple-stick: {text}')


def validate_chunks(image, layout):
    placed = sorted(image.chunks)
    for (pa, data), (next_pa, _) in zip(placed, placed[1:]):
        if pa + len(data) > next_pa:
            refusal(f'chunks overlap at {pa:#x}')
    floor = layout.ram_base
    ceiling = layout.panic_pa + layout.panic_size
    for pa, data in image.chunks:
        if pa % 0x4000:
            refusal(f'chunk at {pa:#x} is not 16 KiB aligned')
        if not (floor <= pa and pa + len(data) <= ceiling):
            refusal(f'chunk at {pa:#x} leaves the guest window '
                    f'({floor:#x}..{ceiling:#x})')


def qualification_hash(manifest):
    body = json.dumps(manifest, sort_keys=True).encode()
    return hashlib.sha256(body).hexdigest()


def build(args):
    dt_path = ROOT / 'downloads' / 'extracted' / 'vma2macosap' / 'DeviceTree.vma2macosap'
    if args.layout == 'qemu-monitor':
        layout = classicload.QEMU_MONITOR_LAYOUT
    elif args.layout == 'gaokun3':
        def regions(text):
            out = []
            for item in text.split(','):
                if not item:
                    continue
                base, _, size = item.partition(':')
                out.append((int(base, 0), int(size, 0)))
            return out
        reserved = regions(args.reserved)
        if not reserved:
            refusal('the gaokun3 layout needs --reserved base:size,... from the SLB tree')
        layout = classicload.derive_gaokun3_layout(
            reserved, firmware_reserved=regions(args.uefi_reserved))
    else:  # pragma: no cover - argparse choices guard this
        refusal(f'unknown layout {args.layout}')
    if args.boot_args or args.ramdisk:
        from dataclasses import replace
        if args.boot_args:
            layout = replace(layout, cmdline=args.boot_args)
        if args.ramdisk:
            # As smoke.py --root-disk: the PCIe memory range sits at
            # arm-io offset 0x40000000, which the baseline window cannot
            # translate, so BAR0 never maps (see ClassicLayout.armio_size).
            layout = replace(layout, armio_size=0x70000000)
    image, seed_pa, seed_chunk = deferred_seed_image(args.kernelcache, dt_path, layout)
    validate_chunks(image, layout)
    monitor = None
    if args.monitor:
        if args.monitor_pa is None or args.params_pa is None:
            refusal('--monitor needs --monitor-pa and --params-pa')
        s2 = (layout.ram_base & ~0x1FFFFF,
              (layout.panic_pa + layout.panic_size + 0x1FFFFF) & ~0x1FFFFF)
        fb = None
        if layout.video is not None and layout.monitor_band is not None:
            vbase, vwidth, vrows, vstride, _ = layout.video
            _, _, brows, _ = layout.monitor_band
            fb = (vbase, vwidth, vstride, vrows, brows)
        monitor = (pathlib.Path(args.monitor).read_bytes(), args.monitor_pa,
                   args.params_pa, s2, fb)
    ramdisk = None
    if args.ramdisk:
        data = pathlib.Path(args.ramdisk).read_bytes()
        occupied = [(pa, len(chunk)) for pa, chunk in image.chunks]
        if monitor is not None:
            occupied += [(monitor[1], len(monitor[0])), (monitor[2], 0x1000)]
        if layout.video is not None:
            vbase, _, vrows, vstride, _ = layout.video
            occupied.append((vbase, vrows * vstride))
        if layout.monitor_band is not None:
            bbase, _, brows, bstride = layout.monitor_band
            occupied.append((bbase, brows * bstride))
        ramdisk = (data, classicload.place_ramdisk(layout, len(data), occupied))
    manifest, files = bundle_bytes(image, seed_pa, seed_chunk, monitor, ramdisk)
    out = pathlib.Path(args.out)
    for name, data in files.items():
        (out / name).parent.mkdir(parents=True, exist_ok=True)
        (out / name).write_bytes(data)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True))
    print(f'wrote {out}: layout {layout.name}, {len(manifest["chunks"])} chunks, '
          f'entry {image.entry_pa:#x}')
    print(f'qualification sha256: {qualification_hash(manifest)}')
    return 0


def check(args):
    out = pathlib.Path(args.out)
    try:
        manifest = json.loads((out / 'manifest.json').read_text())
    except (OSError, ValueError) as e:
        refusal(f'no readable manifest in {out}: {e}')
    for chunk in manifest.get('chunks', []):
        data = (out / chunk['file']).read_bytes() if (out / chunk['file']).exists() else None
        if data is None or len(data) != chunk['size'] or _sha(data) != chunk['sha256']:
            refusal(f"chunk {chunk['file']} does not match its manifest hash")
    dt = out / 'devicetree.bin'
    want = manifest.get('devicetree', {})
    if not dt.exists() or _sha(dt.read_bytes()) != want.get('sha256'):
        refusal('devicetree.bin does not match its manifest hash')
    placed = sorted((c['pa'], c['size']) for c in manifest['chunks'])
    for (pa, size), (next_pa, _) in zip(placed, placed[1:]):
        if pa + size > next_pa:
            refusal(f'chunks overlap at {pa:#x}')
    seed = manifest.get('seed', {})
    try:
        holder = manifest['chunks'][seed['chunk']]
        offset = seed['pa'] - holder['pa']
        slot = (out / holder['file']).read_bytes()[offset:offset + SEED_BYTES]
    except (KeyError, IndexError, TypeError, OSError):
        refusal('the manifest does not locate the seed slot')
    if offset < 0 or len(slot) != SEED_BYTES or any(slot):
        refusal('the seed slot is not empty: a seed must never come from the stick')
    binary = out / STICK_DIR / 'bundle.bin'
    if not binary.exists() or binary.read_bytes() != q1n1_bundle(manifest,
                                                                 qualification_hash(manifest)):
        refusal('bundle.bin does not match manifest.json')
    print(f'PASS vmapple-stick check: {out} qualification sha256: '
          f'{qualification_hash(manifest)}')
    return 0


def check_boot(args):
    out = pathlib.Path(args.out)
    try:
        manifest = json.loads((out / 'manifest.json').read_text())
    except (OSError, ValueError) as e:
        refusal(f'no readable manifest in {out}: {e}')
    want = qualification_hash(manifest)
    if args.qualification_sha256 != want:
        refusal('boot refused: the presented qualification hash does not '
                f'match this bundle ({want})')
    print('PASS vmapple-stick check-boot: qualification matches '
          '(host check only; no boot is performed by this tool)')
    return 0


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--kernelcache', required=True)
    b.add_argument('--layout', choices=('qemu-monitor', 'gaokun3'), required=True)
    b.add_argument('--reserved', default='',
                   help='gaokun3: comma list of base:size reserved regions from the SLB tree')
    b.add_argument('--uefi-reserved', default='',
                   help='gaokun3: comma list of base:size regions UEFI\'s memory map does not '
                        'offer (from el2qual\'s recorded map, e.g. run 24\'s 0xc6200000:0x2400000)')
    b.add_argument('--monitor', help='monitor image (raw binary) for a version 2 bundle')
    b.add_argument('--monitor-pa', type=lambda v: int(v, 0),
                   help='physical address the monitor image is placed and entered at')
    b.add_argument('--params-pa', type=lambda v: int(v, 0),
                   help='physical address of the parameter page q1n1 writes for the monitor')
    b.add_argument('--ramdisk', help='raw root-disk image placed above the panic chunk '
                                     'as one more bundle chunk (Phase 4 P3)')
    b.add_argument('--boot-args', help='override the layout kernel command line '
                                       '(e.g. to name the root device with --ramdisk)')
    b.add_argument('--out', required=True)
    c = sub.add_parser('check')
    c.add_argument('--out', required=True)
    q = sub.add_parser('check-boot')
    q.add_argument('--out', required=True)
    q.add_argument('--qualification-sha256', required=True)
    args = ap.parse_args()
    return {'build': build, 'check': check, 'check-boot': check_boot}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
