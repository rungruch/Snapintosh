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

    def test_entropy_stage_has_one_entry(self):
        m = usb.build_layout('entropy', self.out(), self.efi, self.sdboot, self.slb, self.tcb,
                             self.dtb)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertEqual(m['stage'], 'entropy')
        self.assertEqual((out / 'EFI/systemd/drivers/slbounceaa64.efi').read_bytes(),
                         self.slb.read_bytes())
        entries = sorted((out / 'loader/entries').iterdir())
        self.assertEqual([e.name for e in entries], ['el2qual-16-rng.conf'])
        text = entries[0].read_text()
        self.assertIn('options nopac nosmp rng\n', text)        # read only, no VHE exit, no USB
        self.assertNotIn('emulate', text)
        self.assertNotIn('usb', text)
        self.assertIn('default el2qual-16-rng.conf', (out / 'loader/loader.conf').read_text())
        self.refused('entropy needs --slbounce', stage='entropy', slbounce=None)

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

    def test_vhe_exit_stage_has_its_two_entries(self):
        m = usb.build_layout('vhe-exit', self.out(), self.efi, self.sdboot, self.slb, self.tcb,
                             self.dtb)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertEqual(m['stage'], 'vhe-exit')
        self.assertEqual((out / 'EFI/systemd/drivers/slbounceaa64.efi').read_bytes(),
                         self.slb.read_bytes())
        self.assertEqual((out / 'tcblaunch.exe').read_bytes(), self.tcb.read_bytes())
        entries = sorted((out / 'loader/entries').iterdir())
        self.assertEqual([e.name for e in entries],
                         ['el2qual-6-vhe-exit.conf', 'el2qual-7-vhe-exit-full.conf'])
        texts = [e.read_text() for e in entries]
        self.assertIn('options nopac nosmp exit-vhe\n', texts[0])    # the switch alone first
        self.assertIn('options exit-vhe\n', texts[1])
        for text in texts:
            self.assertIn('devicetree /dtb/gaokun3-el2.dtb\n', text)
            self.assertNotIn('emulate-', text)                        # never the QEMU-only options
        self.assertIn('default el2qual-6-vhe-exit.conf', (out / 'loader/loader.conf').read_text())
        usb.build_layout('stage2', self.out('s2'), self.efi, self.sdboot, self.slb, self.tcb,
                         self.dtb)
        for e in (self.out('s2') / 'loader/entries').iterdir():
            self.assertNotIn('exit-vhe', e.read_text())

    def test_vhe_exit_stage_input_rules(self):
        self.refused('vhe-exit needs --slbounce', stage='vhe-exit', slbounce=None)
        self.refused('vhe-exit needs --systemd-boot', stage='vhe-exit', systemd_boot=None)
        self.refused('vhe-exit needs --dtb, or --always-switch', stage='vhe-exit', dtb=None)

    def test_usb_inventory_holds_only_the_probe(self):
        probe = self.file('q1n1-usb-probe-hold.efi', pe(payload=b'usb-probe'))
        m = usb.build_layout('usb-inventory', self.out(), None, self.sdboot, usb_probe=probe)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertEqual(m['stage'], 'usb-inventory')
        self.assertEqual(sorted(m['files']), sorted([
            'EFI/BOOT/BOOTAA64.EFI', 'EFI/q1n1/q1n1-usb-probe-hold.efi', 'loader/loader.conf',
            'loader/entries/q1n1-usb-inventory.conf']))
        # Nothing that could attempt a Secure Launch, and no el2qual.
        self.assertFalse(any(k in rel for rel in m['files']
                             for k in ('slbounce', 'tcblaunch', 'dtb/', 'el2qual')))
        text = (out / 'loader/entries/q1n1-usb-inventory.conf').read_text()
        self.assertIn('efi /EFI/q1n1/q1n1-usb-probe-hold.efi\n', text)
        self.assertNotIn('devicetree', text)
        self.assertNotIn('options', text)
        self.assertIn('default q1n1-usb-inventory.conf', (out / 'loader/loader.conf').read_text())

    def test_usb_inventory_input_rules(self):
        probe = self.file('q1n1-usb-probe-hold.efi', pe(payload=b'usb-probe'))
        for extra in (dict(slbounce=self.slb), dict(tcblaunch=self.tcb), dict(dtb=self.dtb),
                      dict(always_switch=True), dict(efi=self.efi)):
            args = dict(usb_probe=probe, systemd_boot=self.sdboot, efi=None)
            args.update(extra)
            with self.assertRaises(usb.LayoutError):
                usb.build_layout('usb-inventory', self.out('x'), **args)
        with self.assertRaises(usb.LayoutError):
            usb.build_layout('usb-inventory', self.out('y'), None, self.sdboot)       # no probe
        with self.assertRaises(usb.LayoutError):
            usb.build_layout('usb-inventory', self.out('z'), None, None, usb_probe=probe)
        with self.assertRaises(usb.LayoutError):                                     # wrong stage
            usb.build_layout('stage2', self.out('w'), self.efi, self.sdboot, self.slb, self.tcb,
                             self.dtb, usb_probe=probe)
        with self.assertRaises(usb.LayoutError):                                     # not a PE app
            usb.build_layout('usb-inventory', self.out('v'), None, self.sdboot, usb_probe=self.slb)

    def vmapple_bundle2(self, name='bundle2'):
        """A version 2 bundle: the synthetic guest plus a monitor chunk."""
        import importlib.util
        root = Path(__file__).resolve().parents[1]
        sys.path.insert(0, str(root / 'probes'))
        sys.path.insert(0, str(root / 'tools' / 'vmapple-smoke'))
        import classicload
        def load(mod, path):
            spec = importlib.util.spec_from_file_location(mod, root / path)
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
            return m
        classic = load('classic_fixtures_usb2', 'tools/test-vmapple-classic.py')
        stick = load('vmapple_stick_usb2', 'tools/vmapple-stick.py')
        kc = self.file('kernelcache2.synthetic', b'')
        classic.synthetic_kernelcache(kc)
        tree = self.file('devicetree2.synthetic', classic._fixtures.synthetic_tree())
        from dataclasses import replace
        layout = replace(classicload.QEMU_MONITOR_LAYOUT, drop_nodes=())
        image, seed_pa, seed_chunk = stick.deferred_seed_image(kc, tree, layout)
        manifest, files = stick.bundle_bytes(
            image, seed_pa, seed_chunk,
            monitor=(b'\x00' * 4096, 0x40200000, 0x43000000, (0x81000000, 0x9D200000),
                     (0xC6200000, 1600, 6400, 2400, 160)))
        out = Path(self.tmp.name) / name
        for rel, data in files.items():
            (out / rel).parent.mkdir(parents=True, exist_ok=True)
            (out / rel).write_bytes(data)
        (out / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True))
        return out

    def test_vmapple_h2_stage(self):
        q1n1 = self.file('q1n1.efi', pe(payload=b'--vmapple-enter VHE EXIT  DT SIMPLE-FB'))
        bundle = self.vmapple_bundle2()
        m = usb.build_layout('vmapple-h2', self.out(), q1n1, self.sdboot, self.slb,
                             self.tcb, self.dtb, bundle=bundle)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertIn('EFI/systemd/drivers/slbounceaa64.efi', m['files'])
        self.assertIn('tcblaunch.exe', m['files'])
        self.assertEqual(m['monitor_entry_pa'], 0x40200000)
        self.assertEqual(m['params_pa'], 0x43000000)
        self.assertTrue(m['bundle_qualification'])
        text = (out / 'loader/entries/q1n1-vmapple-h2.conf').read_text()
        self.assertIn('options --vmapple-enter --exit-vhe --hold-at-monitor --gic-hold\n', text)
        gic_text = (out / 'loader/entries/q1n1-vmapple-h2-gic.conf').read_text()
        self.assertIn('options --vmapple-enter --exit-vhe --hold-at-monitor\n', gic_text)

    def test_vmapple_h2_refusals(self):
        q1n1 = self.file('q1n1.efi', pe(payload=b'--vmapple-enter VHE EXIT  DT SIMPLE-FB'))
        plain = self.file('q1n1-plain.efi', pe(payload=b'--vmapple-place VHE EXIT  DT SIMPLE-FB'))
        with self.assertRaises(usb.LayoutError):                     # a version 1 bundle
            usb.build_layout('vmapple-h2', self.out('a'), q1n1, self.sdboot, self.slb,
                             self.tcb, self.dtb, bundle=self.vmapple_bundle('v1bundle'))
        with self.assertRaises(usb.LayoutError):                     # no entering mode built in
            usb.build_layout('vmapple-h2', self.out('b'), plain, self.sdboot, self.slb,
                             self.tcb, self.dtb, bundle=self.vmapple_bundle2('b2'))
        with self.assertRaises(usb.LayoutError):                     # no slbounce
            usb.build_layout('vmapple-h2', self.out('c'), q1n1, self.sdboot, None,
                             self.tcb, self.dtb, bundle=self.vmapple_bundle2('b3'))
        with self.assertRaises(usb.LayoutError):                     # no bundle
            usb.build_layout('vmapple-h2', self.out('d'), q1n1, self.sdboot, self.slb,
                             self.tcb, self.dtb)

    def vmapple_bundle(self, name='bundle'):
        """A real bundle from the synthetic kernelcache and tree."""
        import importlib.util
        root = Path(__file__).resolve().parents[1]
        sys.path.insert(0, str(root / 'probes'))
        sys.path.insert(0, str(root / 'tools' / 'vmapple-smoke'))
        import classicload
        def load(mod, path):
            spec = importlib.util.spec_from_file_location(mod, root / path)
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
            return m
        classic = load('classic_fixtures_usb', 'tools/test-vmapple-classic.py')
        stick = load('vmapple_stick_usb', 'tools/vmapple-stick.py')
        kc = self.file('kernelcache.synthetic', b'')
        classic.synthetic_kernelcache(kc)
        tree = self.file('devicetree.synthetic', classic._fixtures.synthetic_tree())
        from dataclasses import replace
        layout = replace(classicload.QEMU_MONITOR_LAYOUT, drop_nodes=())
        image, seed_pa, seed_chunk = stick.deferred_seed_image(kc, tree, layout)
        manifest, files = stick.bundle_bytes(image, seed_pa, seed_chunk)
        out = Path(self.tmp.name) / name
        for rel, data in files.items():
            (out / rel).parent.mkdir(parents=True, exist_ok=True)
            (out / rel).write_bytes(data)
        (out / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True))
        return out

    def test_vmapple_h1_stage(self):
        q1n1 = self.file('q1n1.efi', pe(payload=b'--vmapple-place PLACED AND VERIFIED'))
        bundle = self.vmapple_bundle()
        m = usb.build_layout('vmapple-h1', self.out(), q1n1, self.sdboot, dtb=self.dtb,
                             bundle=bundle)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertIn('Snapintosh/vmapple/bundle.bin', m['files'])
        self.assertIn('Snapintosh/vmapple/chunk0.bin', m['files'])
        self.assertIn('EFI/q1n1/q1n1.efi', m['files'])
        self.assertFalse(any(k in rel for rel in m['files'] for k in ('slbounce', 'tcblaunch')))
        text = (out / 'loader/entries/q1n1-vmapple-h1.conf').read_text()
        self.assertIn('options --vmapple-place --hold\n', text)
        self.assertIn('devicetree /dtb/', text)
        self.assertEqual((out / 'Snapintosh/vmapple/bundle.bin').read_bytes(),
                         (bundle / 'Snapintosh/vmapple/bundle.bin').read_bytes())

    def test_vmapple_h1_refusals(self):
        q1n1 = self.file('q1n1.efi', pe(payload=b'--vmapple-place PLACED AND VERIFIED'))
        plain = self.file('q1n1-plain.efi', pe(payload=b'q1n1 VHE EXIT  DT SIMPLE-FB'))
        bundle = self.vmapple_bundle()
        with self.assertRaises(usb.LayoutError):                     # no stick option built in
            usb.build_layout('vmapple-h1', self.out('a'), plain, self.sdboot, dtb=self.dtb,
                             bundle=bundle)
        for extra in (dict(slbounce=self.slb), dict(tcblaunch=self.tcb), dict(always_switch=True)):
            with self.assertRaises(usb.LayoutError):
                usb.build_layout('vmapple-h1', self.out('b'), q1n1, self.sdboot, dtb=self.dtb,
                                 bundle=bundle, **extra)
        with self.assertRaises(usb.LayoutError):                     # no bundle
            usb.build_layout('vmapple-h1', self.out('c'), q1n1, self.sdboot, dtb=self.dtb)
        with self.assertRaises(usb.LayoutError):                     # no device tree
            usb.build_layout('vmapple-h1', self.out('d'), q1n1, self.sdboot, bundle=bundle)
        chunk = bundle / 'Snapintosh/vmapple/chunk0.bin'
        chunk.write_bytes(b'\x01' + chunk.read_bytes()[1:])
        with self.assertRaises(usb.LayoutError):                     # a chunk off its hash
            usb.build_layout('vmapple-h1', self.out('e'), q1n1, self.sdboot, dtb=self.dtb,
                             bundle=bundle)
        with self.assertRaises(usb.LayoutError):                     # --bundle elsewhere
            usb.build_layout('stage2', self.out('f'), self.efi, self.sdboot, self.slb, self.tcb,
                             self.dtb, bundle=self.vmapple_bundle('other'))

    def test_q1n1_foothold_stage(self):
        q1n1 = self.file('q1n1.efi', pe(payload=b'q1n1 VHE EXIT  DT SIMPLE-FB'))
        m = usb.build_layout('q1n1-foothold', self.out(), q1n1, self.sdboot, self.slb, self.tcb,
                             self.dtb)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertEqual((out / 'EFI/q1n1/q1n1.efi').read_bytes(), q1n1.read_bytes())
        self.assertFalse(any('el2qual' in rel for rel in m['files']))
        self.assertEqual((out / 'tcblaunch.exe').read_bytes(), self.tcb.read_bytes())
        self.assertIn('EFI/systemd/drivers/slbounceaa64.efi', m['files'])
        entries = sorted((out / 'loader/entries').iterdir())
        self.assertEqual([e.name for e in entries],
                         ['q1n1-1-foothold.conf', 'q1n1-2-foothold-vhe-exit.conf'])
        texts = [e.read_text() for e in entries]
        self.assertIn('options --el2\n', texts[0])                 # the foothold alone first
        self.assertIn('options --el2 --exit-vhe\n', texts[1])
        for text in texts:
            self.assertIn('efi /EFI/q1n1/q1n1.efi\n', text)
            self.assertIn('devicetree /dtb/gaokun3-el2.dtb\n', text)
            self.assertNotIn('emulate', text)                       # never the QEMU-only options
        self.assertIn('default q1n1-1-foothold.conf', (out / 'loader/loader.conf').read_text())

    def test_q1n1_foothold_refuses_other_builds(self):
        for payload in (b'q1n1 default build', b'q1n1 VHE EXIT  only', b'q1n1 DT SIMPLE-FB only'):
            q1n1 = self.file('q1n1.efi', pe(payload=payload))
            with self.assertRaises(usb.LayoutError):
                usb.build_layout('q1n1-foothold', self.out('x'), q1n1, self.sdboot, self.slb,
                                 self.tcb, self.dtb)
        good = self.file('q1n1-good.efi', pe(payload=b'q1n1 VHE EXIT  DT SIMPLE-FB'))
        with self.assertRaises(usb.LayoutError):                    # slbounce is required
            usb.build_layout('q1n1-foothold', self.out('y'), good, self.sdboot, None, self.tcb,
                             self.dtb)
        with self.assertRaises(usb.LayoutError):                    # so is the gate's tree
            usb.build_layout('q1n1-foothold', self.out('z'), good, self.sdboot, self.slb, self.tcb)

    def test_usb_read_stage_has_its_eight_entries(self):
        m = usb.build_layout('usb-read', self.out(), self.efi, self.sdboot, self.slb, self.tcb,
                             self.dtb)
        out = self.out()
        self.assert_manifest_complete(out, m)
        self.assertEqual(m['stage'], 'usb-read')
        self.assertIn('EFI/systemd/drivers/slbounceaa64.efi', m['files'])
        entries = sorted((out / 'loader/entries').iterdir())
        names = [e.name for e in entries]
        self.assertEqual(sorted(names), sorted(['el2qual-8-usb.conf', 'el2qual-9-usb-smmu.conf',
                                                'el2qual-10-usb-smmu-map.conf',
                                                'el2qual-11-usb-s2-walk.conf',
                                                'el2qual-12-usb-s2-survey.conf',
                                                'el2qual-13-usb-s2-fixed.conf',
                                                'el2qual-14-usb-s2-bypass-plan.conf',
                                                'el2qual-15-usb-halt.conf']))
        texts = {e.name: e.read_text() for e in entries}
        self.assertIn('options nopac nosmp usb\n', texts['el2qual-8-usb.conf'])  # no SMMU, no VHE exit first
        # Entry 9 stays what runs 17, 19 and 20 ran; the map is entry 10's alone.
        self.assertIn('options nopac nosmp exit-vhe usb-smmu\n', texts['el2qual-9-usb-smmu.conf'])
        self.assertIn('options nopac nosmp exit-vhe usb-smmu-map\n', texts['el2qual-10-usb-smmu-map.conf'])
        # Entry 10 stays what run 21 ran; the walk is entry 11's alone.
        self.assertIn('options nopac nosmp exit-vhe usb-smmu-map usb-s2-walk\n',
                      texts['el2qual-11-usb-s2-walk.conf'])
        # Entry 11 stays what run 22 ran; the survey (which implies the
        # walk) is entry 12's alone.
        self.assertIn('options nopac nosmp exit-vhe usb-smmu-map usb-s2-survey\n',
                      texts['el2qual-12-usb-s2-survey.conf'])
        # The fixed range and memory map are entry 13's alone.
        self.assertIn('options nopac nosmp exit-vhe usb-smmu-map usb-s2-fixed\n',
                      texts['el2qual-13-usb-s2-fixed.conf'])
        # Entry 14 plans the SMMU write; no entry performs one.
        self.assertIn('options nopac nosmp exit-vhe usb-smmu-map usb-s2-bypass-plan\n',
                      texts['el2qual-14-usb-s2-bypass-plan.conf'])
        # Entry 15 reads the xHCI halt state; the controller alone, no VHE exit.
        self.assertIn('options nopac nosmp usb-halt\n', texts['el2qual-15-usb-halt.conf'])
        for text in texts.values():
            self.assertNotIn('bypass-write', text)
        texts = list(texts.values())
        for text in texts:
            self.assertIn('efi /EFI/el2qual/el2qual.efi\n', text)
            self.assertIn('devicetree /dtb/gaokun3-el2.dtb\n', text)
            self.assertNotIn('emulate', text)
        self.assertIn('default el2qual-8-usb.conf', (out / 'loader/loader.conf').read_text())
        self.refused('usb-read needs --slbounce', stage='usb-read', slbounce=None)

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
