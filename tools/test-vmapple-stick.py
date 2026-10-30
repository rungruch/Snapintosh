#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for tools/vmapple-stick.py (Phase 3 P5): bundle build,
check agreement, tamper refusal and the qualification-hash boot refusal.
Fixtures are the classic loader test's synthetic kernelcache and tree;
no Apple material and no hardware.
"""
import importlib.util
import json
import pathlib
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'vmapple-smoke'))
sys.path.insert(0, str(ROOT / 'probes'))
import appledt
import classicload

_spec = importlib.util.spec_from_file_location('stick_tool', ROOT / 'tools' / 'vmapple-stick.py')
stick = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stick)

_cspec = importlib.util.spec_from_file_location('classic_fixtures', ROOT / 'tools' / 'test-vmapple-classic.py')
_classic_test = importlib.util.module_from_spec(_cspec)
_cspec.loader.exec_module(_classic_test)
_fixtures = _classic_test._fixtures

checks = 0


def check(condition, text):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(f'FAIL: {text}')


def refuses(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except SystemExit:
        return True
    return False


class Args:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        kc = tmp / 'kernelcache.synthetic'
        _classic_test.synthetic_kernelcache(kc)
        dt_path = tmp / 'devicetree.synthetic'
        dt_path.write_bytes(_fixtures.synthetic_tree())
        from dataclasses import replace
        layout = replace(classicload.QEMU_MONITOR_LAYOUT, drop_nodes=())
        image, seed_pa, seed_chunk = stick.deferred_seed_image(kc, dt_path, layout)
        holder_pa, holder = image.chunks[seed_chunk]
        slot = holder[seed_pa - holder_pa:seed_pa - holder_pa + 256]
        check(len(slot) == 256 and not any(slot), 'the bundle carries an empty 256-byte seed slot')
        check(b'\x5a' * 256 not in image.dt_blob and b'\xa5' * 256 not in image.dt_blob,
              'no build-time fill is left in the tree')

        stick.validate_chunks(image, layout)
        check(True, 'the synthetic image chunks validate')
        bad = classicload.ClassicImage(image.payload, image.payload_pa, image.entry_pa,
                                       image.boot_args_pa, image.dt_blob, image.info,
                                       chunks=image.chunks + ((image.chunks[0][0] + 0x4000, b'x'),))
        check(refuses(stick.validate_chunks, bad, layout), 'overlapping chunks are refused')
        out = tmp / 'bundle'
        manifest, files = stick.bundle_bytes(image, seed_pa, seed_chunk)
        for name, data in files.items():
            (out / name).parent.mkdir(parents=True, exist_ok=True)
            (out / name).write_bytes(data)
        (out / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True))
        check(stick.check(Args(out=str(out))) == 0, 'check agrees with build')

        binary = files['Snapintosh/vmapple/bundle.bin']
        count = len(manifest['chunks'])
        check(len(binary) == 96 + 112 * count, 'bundle.bin is q1n1\'s 96-byte header and '
              '112-byte records')
        magic, version, n, entry_size, entry, boot_args, spa, schunk = \
            struct.unpack_from('<8Q', binary, 0)
        check((magic, version, n, entry_size) == (0x31424D56314E3151, 1, count, 112),
              'bundle.bin header: magic Q1N1VMB1, version 1, the chunk count, 112-byte records')
        check((entry, boot_args, spa, schunk) == (image.entry_pa, image.boot_args_pa,
                                                   seed_pa, seed_chunk),
              'bundle.bin carries the entry, boot_args and seed slot')
        check(binary[64:96] == bytes.fromhex(stick.qualification_hash(manifest)),
              'bundle.bin carries the qualification hash')
        pa, size, digest, path = struct.unpack_from('<2Q32s64s', binary, 96)
        check((pa, size, digest.hex()) == (manifest['chunks'][0]['pa'],
                                           manifest['chunks'][0]['size'],
                                           manifest['chunks'][0]['sha256'])
              and path.rstrip(b'\0') == b'\\Snapintosh\\vmapple\\chunk0.bin',
              'bundle.bin records chunk 0 with q1n1\'s absolute path')

        mon_data = bytes(range(256)) * 8
        manifest_v2, files_v2 = stick.bundle_bytes(
            image, seed_pa, seed_chunk,
            monitor=(mon_data, 0x40200000, 0x43000000, (0x81000000, 0x9D200000),
                     (0x9E000000, 1024, 4096, 512, 88)))
        binary_v2 = files_v2['Snapintosh/vmapple/bundle.bin']
        count_v2 = len(manifest_v2['chunks'])
        check(len(binary_v2) == 176 + 112 * count_v2,
              'version 2 bundle.bin is a 176-byte header and 112-byte records')
        fields = struct.unpack_from('<18Q', binary_v2, 0)
        check(fields[1] == 2 and fields[8] == 0x40200000 and fields[9] == 0x43000000
              and fields[10:12] == (0x81000000, 0x9D200000)
              and fields[12:17] == (0x9E000000, 1024, 4096, 512, 88),
              'version 2 header carries the monitor entry, parameter page, stage-2 '
              'range and screen split')
        check(binary_v2[144:176] == bytes.fromhex(stick.qualification_hash(manifest_v2)),
              'version 2 bundle.bin carries the qualification hash')
        check(files_v2[manifest_v2['chunks'][-1]['file']] == mon_data
              and manifest_v2['monitor']['chunk'] == count_v2 - 1,
              'the monitor image is the last chunk')
        check(refuses(stick.bundle_bytes, image, seed_pa, seed_chunk,
                      (mon_data, image.chunks[0][0], 0x43000000, (0x81000000, 0x9D200000), None)),
              'a monitor image overlapping a guest chunk is refused')
        check(refuses(stick.bundle_bytes, image, seed_pa, seed_chunk,
                      (mon_data, 0x40200000, 0x40200000, (0x81000000, 0x9D200000), None)),
              'a parameter page overlapping the monitor image is refused')
        check(binary[64:96] == bytes.fromhex(stick.qualification_hash(manifest))
              and struct.unpack_from('<Q', binary, 8)[0] == 1,
              'a bundle without a monitor stays version 1, byte for byte')

        disk_data = bytes(range(256)) * 64
        disk_pa = classicload.place_ramdisk(layout, len(disk_data),
                                            occupied=[(pa, s) for pa, s in
                                                      [(c[0], len(c[1])) for c in image.chunks]])
        check(refuses(stick.bundle_bytes, image, seed_pa, seed_chunk,
                      None, (disk_data, disk_pa)),
              'a ramdisk without a monitor is refused: nothing mediates it')
        mon_rd = (mon_data, 0x40200000, 0x43000000, (0x81000000, 0x9D200000), None)
        manifest_rd, files_rd = stick.bundle_bytes(image, seed_pa, seed_chunk,
                                                   monitor=mon_rd,
                                                   ramdisk=(disk_data, disk_pa))
        rd = manifest_rd['ramdisk']
        check(files_rd[manifest_rd['chunks'][rd['chunk']]['file']] == disk_data
              and (rd['pa'], rd['size']) == (disk_pa, len(disk_data)),
              'the ramdisk is a bundle chunk recorded in the manifest')
        binary_rd = files_rd['Snapintosh/vmapple/bundle.bin']
        count_rd = len(manifest_rd['chunks'])
        check(len(binary_rd) == 192 + 112 * count_rd
              and struct.unpack_from('<Q', binary_rd, 8)[0] == 3
              and struct.unpack_from('<2Q', binary_rd, 144) == (disk_pa, len(disk_data))
              and binary_rd[160:192] == bytes.fromhex(stick.qualification_hash(manifest_rd)),
              'a ramdisk bundle is version 3: disk address and size before the '
              'qualification hash')
        out_rd = tmp / 'bundle-ramdisk'
        for name, data in files_rd.items():
            (out_rd / name).parent.mkdir(parents=True, exist_ok=True)
            (out_rd / name).write_bytes(data)
        (out_rd / 'manifest.json').write_text(json.dumps(manifest_rd, sort_keys=True))
        check(stick.check(Args(out=str(out_rd))) == 0, 'check agrees with a ramdisk bundle')
        check(refuses(stick.bundle_bytes, image, seed_pa, seed_chunk,
                      mon_rd, (disk_data, disk_pa + 0x1000)),
              'an unaligned ramdisk address is refused')
        check(refuses(stick.bundle_bytes, image, seed_pa, seed_chunk,
                      mon_rd, (disk_data, image.chunks[0][0])),
              'a ramdisk overlapping a guest chunk is refused')
        check(refuses(stick.bundle_bytes, image, seed_pa, seed_chunk,
                      mon_rd, (b'', disk_pa)),
              'an empty ramdisk is refused')
        check('ramdisk' not in manifest, 'a bundle without a ramdisk is unchanged')

        chunk0 = out / 'Snapintosh' / 'vmapple' / 'chunk0.bin'
        chunk0.write_bytes(b'\x00' + files['Snapintosh/vmapple/chunk0.bin'][1:])
        check(refuses(stick.check, Args(out=str(out))), 'a tampered chunk is refused')
        chunk0.write_bytes(files['Snapintosh/vmapple/chunk0.bin'])
        bundle = out / 'Snapintosh' / 'vmapple' / 'bundle.bin'
        bundle.write_bytes(binary[:40] + b'\xff' * 8 + binary[48:])
        check(refuses(stick.check, Args(out=str(out))), 'a bundle.bin that disagrees is refused')
        bundle.write_bytes(binary)
        name = manifest['chunks'][seed_chunk]['file']
        seeded = bytearray(files[name])
        seeded[seed_pa - holder_pa] = 1
        (out / name).write_bytes(bytes(seeded))
        manifest2 = json.loads(json.dumps(manifest))
        manifest2['chunks'][seed_chunk]['sha256'] = stick._sha(bytes(seeded))
        (out / 'manifest.json').write_text(json.dumps(manifest2, sort_keys=True))
        check(refuses(stick.check, Args(out=str(out))), 'a bundle carrying a seed is refused')
        (out / name).write_bytes(files[name])
        (out / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True))

        real_dt = ROOT / 'downloads' / 'extracted' / 'vma2macosap' / 'DeviceTree.vma2macosap'
        if not real_dt.exists():
            print('SKIP test-vmapple-stick armio build: downloads/extracted/vma2macosap absent')
        else:
            mon_file = tmp / 'monitor.bin'
            mon_file.write_bytes(mon_data)
            disk_file = tmp / 'ramdisk.raw'
            disk_file.write_bytes(disk_data)
            for name, ramdisk_arg, want in (('plain', None, 0x40000000),
                                            ('ramdisk', str(disk_file), 0x70000000)):
                out_b = tmp / f'build-{name}'
                rc = stick.build(Args(kernelcache=str(kc), layout='qemu-monitor',
                                      reserved='', uefi_reserved='',
                                      monitor=str(mon_file), monitor_pa=0x40200000,
                                      params_pa=0x43000000, ramdisk=ramdisk_arg,
                                      boot_args=None, out=str(out_b)))
                ranges = appledt.DeviceTree((out_b / 'devicetree.bin').read_bytes()).get(
                    '/device-tree/arm-io', 'ranges')
                got = struct.unpack('<QQQ', ranges)[2]
                check(rc == 0 and got == want,
                      f'a {name} stick build writes the {want:#x} arm-io window: {got:#x}')

        sha = stick.qualification_hash(manifest)
        check(stick.check_boot(Args(out=str(out), qualification_sha256=sha)) == 0,
              'the matching qualification hash is accepted')
        check(refuses(stick.check_boot, Args(out=str(out), qualification_sha256='0' * 64)),
              'a mismatched qualification hash refuses the boot')
        nothing = classicload.build_classic_image(kc, dt_path, layout,
                                                  entropy=lambda n: b'\xa5' * n)
        from unittest import mock
        with mock.patch.object(classicload, 'build_classic_image', return_value=nothing):
            check(refuses(stick.deferred_seed_image, kc, dt_path, layout),
                  'a seed slot that cannot be located is refused')
    print(f'PASS test-vmapple-stick: {checks} checks')


if __name__ == '__main__':
    main()
