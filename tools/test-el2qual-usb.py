#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/el2qual-usb.py: the layouts it writes and every input or
destination it must refuse, without writing anything, before a stick is
ever made. Uses synthetic PE and DTB files; nothing here touches a disk
other than temporary directories."""
import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('el2qual_usb', ROOT / 'tools' / 'el2qual-usb.py')
usb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb)


def pe(machine=0xAA64, subsystem=10, payload=b''):
    """A minimal PE header: what el2qual-usb.py reads, and nothing more."""
    data = bytearray(0x40 + 24 + 240)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x40)
    data[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<H', data, 0x44, machine)
    struct.pack_into('<H', data, 0x40 + 24, 0x20b)
    struct.pack_into('<H', data, 0x40 + 24 + 68, subsystem)
    return bytes(data) + payload


def dtb(extra=0, magic=b'\xd0\x0d\xfe\xed'):
    data = bytearray(64)
    data[:4] = magic
    struct.pack_into('>I', data, 4, len(data) + extra)
    return bytes(data)


class Layout(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.inputs = self.dir / 'in'
        self.inputs.mkdir()
        self.efi = self.file('el2qual.efi', pe(payload=b'el2qual'))
        self.sdboot = self.file('systemd-bootaa64.efi', pe(payload=b'sd-boot'))
        self.slb = self.file('slbounce.efi', pe(subsystem=11, payload=b'slbounce'))
        self.tcb = self.file('tcblaunch.exe', pe(subsystem=16, payload=b'tcb'))
        self.dtb = self.file('gaokun3-el2.dtb', dtb())

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, name, data):
        path = self.inputs / name
        path.write_bytes(data)
        return path

    def out(self, name='out'):
        return self.dir / name

    def refused(self, needle, **kw):
        out = kw.pop('out', self.out())
        args = dict(stage='stage2', out=out, efi=self.efi, systemd_boot=self.sdboot,
                    slbounce=self.slb, tcblaunch=self.tcb, dtb=self.dtb)
        args.update(kw)
        existed = Path(out).exists()
        with self.assertRaises(usb.LayoutError) as caught:
            usb.build_layout(**args)
        self.assertIn(needle, str(caught.exception))
        if not existed:
            self.assertFalse(Path(out).exists(), 'a refused layout wrote files')

    def assert_manifest_complete(self, out, manifest):
        on_disk = {p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file()}
        self.assertEqual(on_disk, set(manifest['files']) | {usb.MANIFEST})
        for rel, digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((out / rel).read_bytes()).hexdigest(), digest)
        self.assertEqual(json.loads((out / usb.MANIFEST).read_text()), manifest)

    def test_stage1_is_el2qual_alone(self):
        m = usb.build_layout('stage1', self.out(), self.efi)
        self.assertEqual(set(m['files']), {'EFI/BOOT/BOOTAA64.EFI'})
        self.assertEqual((self.out() / 'EFI/BOOT/BOOTAA64.EFI').read_bytes(), self.efi.read_bytes())
        self.assert_manifest_complete(self.out(), m)

    def test_stage2_with_dtb(self):
        m = usb.build_layout('stage2', self.out(), self.efi, self.sdboot, self.slb, self.tcb,
                             self.dtb)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertEqual((out / 'EFI/BOOT/BOOTAA64.EFI').read_bytes(), self.sdboot.read_bytes())
        driver = out / 'EFI/systemd/drivers/slbounceaa64.efi'
        self.assertEqual(driver.read_bytes(), self.slb.read_bytes())
        self.assertTrue(driver.name.lower().endswith('aa64.efi'))   # systemd-boot's filter
        self.assertEqual((out / 'tcblaunch.exe').read_bytes(), self.tcb.read_bytes())
        self.assertIn('default el2qual-1-boot-cpu.conf', (out / 'loader/loader.conf').read_text())
        entries = sorted((out / 'loader/entries').iterdir())
        self.assertEqual([e.name for e in entries], [n for n, _, _ in usb.ENTRIES])
        texts = [e.read_text() for e in entries]
        self.assertIn('options nopac nosmp\n', texts[0])
        self.assertIn('options nosmp\n', texts[1])      # PAuth before PSCI
        self.assertIn('options nopac\n', texts[2])
        self.assertNotIn('options', texts[3])
        for text in texts:
            self.assertIn('efi /EFI/el2qual/el2qual.efi\n', text)
            self.assertIn('devicetree /dtb/gaokun3-el2.dtb\n', text)
        self.assertFalse(m['always_switch'])

    def test_stage2_always_switch_has_no_devicetree(self):
        m = usb.build_layout('stage2', self.out(), self.efi, self.sdboot, self.slb, self.tcb,
                             always_switch=True)
        self.assertTrue(m['always_switch'])
        self.assertFalse(any('dtb/' in rel for rel in m['files']))
        for e in (self.out() / 'loader/entries').iterdir():
            self.assertNotIn('devicetree', e.read_text())

    def test_thermal_stage_has_one_entry(self):
        m = usb.build_layout('thermal', self.out(), self.efi, self.sdboot, self.slb, self.tcb,
                             self.dtb)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertEqual(m['stage'], 'thermal')
        self.assertEqual((out / 'EFI/systemd/drivers/slbounceaa64.efi').read_bytes(),
                         self.slb.read_bytes())
        self.assertEqual((out / 'tcblaunch.exe').read_bytes(), self.tcb.read_bytes())
        entries = sorted((out / 'loader/entries').iterdir())
        self.assertEqual([e.name for e in entries], ['el2qual-5-thermal.conf'])
        text = entries[0].read_text()
        self.assertIn('options nopac nosmp thermal\n', text)
        self.assertIn('devicetree /dtb/gaokun3-el2.dtb\n', text)
        self.assertIn('default el2qual-5-thermal.conf', (out / 'loader/loader.conf').read_text())
        # No stage2 entry can be picked by mistake, and stage2 has no thermal one.
        usb.build_layout('stage2', self.out('s2'), self.efi, self.sdboot, self.slb, self.tcb,
                         self.dtb)
        for e in (self.out('s2') / 'loader/entries').iterdir():
            self.assertNotIn('thermal', e.read_text())

    def test_thermal_stage_input_rules(self):
        self.refused('thermal needs --slbounce', stage='thermal', slbounce=None)
        self.refused('thermal needs --systemd-boot', stage='thermal', systemd_boot=None)
        self.refused('thermal needs --dtb, or --always-switch', stage='thermal', dtb=None)

    def test_stage1b_has_no_slbounce(self):
        m = usb.build_layout('stage1b', self.out(), self.efi, self.sdboot, dtb=self.dtb)
        self.assertFalse(any('slbounce' in rel or 'tcblaunch' in rel for rel in m['files']))
        self.assertIn('dtb/gaokun3-el2.dtb', m['files'])

    # ---- Inputs it must refuse ----

    def test_refuses_wrong_architecture(self):
        self.refused('not ARM64', efi=self.file('x64.efi', pe(machine=0x8664)))

    def test_refuses_non_pe(self):
        self.refused('not a PE image', tcblaunch=self.file('t.exe', b'hello' * 20))
        self.refused('no PE signature', tcblaunch=self.file('t2.exe', b'MZ' + bytes(200)))

    def test_refuses_slbounce_that_is_not_a_driver(self):
        self.refused('PE subsystem 10', slbounce=self.file('s.efi', pe(subsystem=10)))

    def test_refuses_driver_as_boot_manager(self):
        self.refused('PE subsystem 11', systemd_boot=self.file('b.efi', pe(subsystem=11)))

    def test_refuses_bad_device_tree(self):
        self.refused('no FDT magic', dtb=self.file('bad.dtb', dtb(magic=b'\0\0\0\0')))
        self.refused('totalsize', dtb=self.file('short.dtb', dtb(extra=8)))

    def test_stage2_needs_a_gate_answer(self):
        self.refused('--always-switch', dtb=None)
        self.refused('not both', always_switch=True)

    def test_stage_input_rules(self):
        self.refused('takes no other inputs', stage='stage1')
        self.refused('needs --dtb', stage='stage1b', dtb=None, slbounce=None, tcblaunch=None)
        self.refused('needs --systemd-boot', systemd_boot=None)
        self.refused('needs --slbounce', slbounce=None)

    # ---- Destinations it must refuse ----

    def test_refuses_repository_outside_build(self):
        self.refused('not under build/', out=ROOT / 'docs' / 'el2qual-usb-test')

    def test_refuses_filesystem_root(self):
        self.refused('filesystem root', out=Path(self.dir.anchor))

    def test_refuses_non_empty_directory(self):
        other = self.out('other')
        other.mkdir()
        (other / 'keep.txt').write_text('mine')
        self.refused('use --replace', out=other)
        self.refused('previous el2qual-usb tree', out=other, replace=True)
        self.assertEqual((other / 'keep.txt').read_text(), 'mine')

    def test_replaces_its_own_tree(self):
        usb.build_layout('stage1', self.out(), self.efi)
        self.refused('use --replace', stage='stage1', systemd_boot=None, slbounce=None,
                     tcblaunch=None, dtb=None)
        m = usb.build_layout('stage1b', self.out(), self.efi, self.sdboot, dtb=self.dtb,
                             replace=True)
        self.assert_manifest_complete(self.out(), m)

    def test_real_el2qual_image(self):
        built = ROOT / 'build' / 'el2qual' / 'el2qual.efi'
        if not built.exists():
            self.skipTest('build/el2qual/el2qual.efi not built')
        machine, subsystem = usb.pe_info(built)
        self.assertEqual((machine, subsystem), (0xAA64, 10))


if __name__ == '__main__':
    unittest.main(verbosity=2 if '-v' in sys.argv else 1)
