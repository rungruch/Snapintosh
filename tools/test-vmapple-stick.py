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
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'vmapple-smoke'))
sys.path.insert(0, str(ROOT / 'probes'))
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
        image = classicload.build_classic_image(
            kc, dt_path, layout, entropy=lambda n: b'\xa5' * n)

        stick.validate_chunks(image, layout)
        check(True, 'the synthetic image chunks validate')
        bad = classicload.ClassicImage(image.payload, image.payload_pa, image.entry_pa,
                                       image.boot_args_pa, image.dt_blob, image.info,
                                       chunks=image.chunks + ((image.chunks[0][0] + 0x4000, b'x'),))
        check(refuses(stick.validate_chunks, bad, layout), 'overlapping chunks are refused')
        out = tmp / 'bundle'
        out.mkdir()
        manifest, files = stick.bundle_bytes(image)
        for name, data in files.items():
            (out / name).write_bytes(data)
        (out / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True))
        check(stick.check(Args(out=str(out))) == 0, 'check agrees with build')
        (out / 'chunk0.bin').write_bytes(b'\x00' + files['chunk0.bin'][1:])
        check(refuses(stick.check, Args(out=str(out))), 'a tampered chunk is refused')
        (out / 'chunk0.bin').write_bytes(files['chunk0.bin'])
        sha = stick.qualification_hash(manifest)
        check(stick.check_boot(Args(out=str(out), qualification_sha256=sha)) == 0,
              'the matching qualification hash is accepted')
        check(refuses(stick.check_boot, Args(out=str(out), qualification_sha256='0' * 64)),
              'a mismatched qualification hash refuses the boot')
    print(f'PASS test-vmapple-stick: {checks} checks')


if __name__ == '__main__':
    main()
