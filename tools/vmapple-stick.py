#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and check a VirtualMac stick bundle for q1n1 (Phase 3 P5).

Host and QEMU only. A bundle is what will later be written to the boot
stick for q1n1 to preload: the classic loader's chunks (one file per
chunk), the completed device tree, and a manifest recording every
chunk's physical address, size and hash, the entry and boot_args
addresses, the guest profile and the layout. The gaokun3 chain boots
from q1n1, not from tools/cycle.py, and q1n1's chainload takes a raw
image, so the bundle is deliberately just chunks plus this manifest;
loading it on hardware is Phase 3's H steps and is not done here.

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
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'probes'))
import classicload


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def bundle_bytes(image):
    """(manifest_dict, {filename: bytes}) for a ClassicImage."""
    files = {}
    chunks = []
    for i, (pa, data) in enumerate(image.chunks):
        name = f'chunk{i}.bin'
        files[name] = data
        chunks.append({'file': name, 'pa': pa, 'size': len(data), 'sha256': _sha(data)})
    files['devicetree.bin'] = image.dt_blob
    manifest = {
        'format': 'snapintosh-vmapple-stick-1',
        'profile': image.info['profile'],
        'layout': image.info['layout'],
        'entry_pa': image.entry_pa,
        'boot_args_pa': image.boot_args_pa,
        'chunks': chunks,
        'devicetree': {'size': len(image.dt_blob), 'sha256': _sha(image.dt_blob)},
        'info': image.info,
    }
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
        reserved = []
        for item in args.reserved.split(','):
            if not item:
                continue
            base, _, size = item.partition(':')
            reserved.append((int(base, 0), int(size, 0)))
        if not reserved:
            refusal('the gaokun3 layout needs --reserved base:size,... from the SLB tree')
        layout = classicload.derive_gaokun3_layout(reserved)
    else:  # pragma: no cover - argparse choices guard this
        refusal(f'unknown layout {args.layout}')
    image = classicload.build_classic_image(args.kernelcache, dt_path, layout)
    validate_chunks(image, layout)
    manifest, files = bundle_bytes(image)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
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
