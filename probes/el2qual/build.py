#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build el2qual.efi, the Phase 0A CPU qualification UEFI application.

  python3 probes/el2qual/build.py [--llvm DIR] [--out DIR]

The image is PE/COFF (aarch64-unknown-windows), linked by lld-link as an EFI
application, so no ELF-to-PE conversion step is needed. That target is LLP64:
every source here uses fixed-width types, and probes/tgran16.c's u64 is
unsigned long long for the same reason.

LLVM is found from --llvm, then SNAP_LLVM, then Homebrew's keg (as
probes/run.py uses), then PATH. lld-link is used when present; otherwise
ld.lld in its link flavor (llvm-mingw ships only the latter).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'probes' / 'el2qual'
HOMEBREW = Path('/opt/homebrew/opt/llvm/bin')
EXE = '.exe' if os.name == 'nt' else ''

CFLAGS = ['--target=aarch64-unknown-windows', '-std=gnu11', '-ffreestanding', '-fshort-wchar',
          '-mgeneral-regs-only', '-mno-stack-arg-probe', '-fno-stack-protector', '-O1',
          '-Wall', '-Wextra', '-Werror', f'-I{SRC}']

# (source, object name, extra flags). tgran16.c is built twice: the _EL12
# variant is the one that reaches EL1 once E2H is set, as slbounce leaves it.
UNITS = [
    (SRC / 'el2qual.c', 'el2qual.o', []),
    (SRC / 'usbstage.c', 'usbstage.o', []),
    (SRC / 'rng.c', 'rng.o', []),
    (SRC / 'report.c', 'report.o', []),
    (SRC / 'firmware.c', 'firmware.o', []),
    (SRC / 'el2qual-asm.S', 'el2qual-asm.o', []),
    (SRC / 'thermal.c', 'thermal.o', []),
    (SRC / 'vhe.c', 'vhe.o', []),
    (SRC / 'pagemap.c', 'pagemap.o', []),
    (SRC / 'usbprobe.c', 'usbprobe.o', []),
    (SRC / 's2walk.c', 's2walk.o', []),
    (SRC / 's2bypass.c', 's2bypass.o', []),
    # The Phase 1 guard itself, unchanged: the "thermal" option runs it live.
    (ROOT / 'compat' / 'sc8280xp-thermal.c', 'sc8280xp-thermal.o', []),
    (ROOT / 'probes' / 'tgran16.c', 'tgran16.o', []),
    (ROOT / 'probes' / 'tgran16.c', 'tgran16-el12.o',
     ['-DTGRAN16_EL12', '-Dtgran16_probe=tgran16_probe_el12']),
]


def llvm_dir(explicit=None):
    for candidate in (explicit, os.environ.get('SNAP_LLVM')):
        if candidate:
            return Path(candidate)
    if (HOMEBREW / 'clang').exists():
        return HOMEBREW
    found = shutil.which('clang')
    if not found:
        raise SystemExit('clang not found: pass --llvm DIR or set SNAP_LLVM')
    return Path(found).parent


def linker(llvm):
    for name in ('lld-link', ):
        for where in (llvm, Path('/opt/homebrew/opt/lld/bin')):
            if (where / (name + EXE)).exists():
                return [str(where / (name + EXE))]
    if (llvm / ('ld.lld' + EXE)).exists():
        return [str(llvm / ('ld.lld' + EXE)), '-flavor', 'link']
    raise SystemExit(f'no lld-link or ld.lld beside {llvm}')


def build(out_dir=None, llvm=None):
    """Compile and link; returns the path of el2qual.efi."""
    llvm = llvm_dir(llvm)
    out = Path(out_dir) if out_dir else ROOT / 'build' / 'el2qual'
    out.mkdir(parents=True, exist_ok=True)
    clang = str(llvm / ('clang' + EXE))
    objects = []
    for source, name, extra in UNITS:
        obj = out / name
        subprocess.run([clang, *CFLAGS, *extra, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    efi = out / 'el2qual.efi'
    link(llvm, efi, objects)
    return efi


def link(llvm, efi, objects):
    # /Brepro: no timestamp in the PE header, so identical inputs give an
    # identical image and builds can be compared by hash.
    subprocess.run([*linker(llvm), '/subsystem:efi_application', '/entry:efi_main',
                    '/nodefaultlib', '/Brepro', f'/out:{efi}', *objects], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--llvm', help='directory holding clang and lld-link/ld.lld')
    ap.add_argument('--out', help='output directory (default build/el2qual)')
    a = ap.parse_args()
    efi = build(a.out, a.llvm)
    digest = hashlib.sha256(efi.read_bytes()).hexdigest()
    print(f'{efi.relative_to(ROOT) if efi.is_relative_to(ROOT) else efi}  '
          f'{efi.stat().st_size} bytes  sha256 {digest}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
