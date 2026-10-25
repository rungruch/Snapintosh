#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for compat/sc8280xp-thermal.c, the gaokun3 tsens guard. No hardware.

  python3 tools/test-sc8280xp-thermal.py [--cc CLANG] [--dtb PATH]

1. The guard's controller table (compat/sc8280xp-thermal.c) and the test's
   own transcription (tools/test-sc8280xp-thermal.c) must agree, and must
   match the gaokun3 DTB: four "qcom,sc8280xp-tsens" nodes, their TM/SROT
   reg windows and #qcom,sensors. The DTB check also requires that every
   thermal zone on a tsens names a sensor the guard reads, that no other
   soc node covers a word the guard may read, and that the 95 C limit is
   below every tsens zone's critical trip. The DTB is a local build output
   (build/, not distributed); without it this part is SKIP.
2. tools/test-sc8280xp-thermal.c is built with ASan+UBSan, or UBSan in trap
   mode where the host has no ASan runtime, and run. The mode is printed.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'compat' / 'sc8280xp-thermal.c'
HDR = ROOT / 'compat' / 'sc8280xp-thermal.h'
TEST = ROOT / 'tools' / 'test-sc8280xp-thermal.c'
DTB = ROOT / 'build' / 'el2qual-usb' / 'stage2' / 'dtb' / 'sc8280xp-huawei-gaokun3-el2.dtb'
EXE = '.exe' if os.name == 'nt' else ''
ROW = re.compile(r'\{UINT64_C\((0x[0-9a-f]+)\), UINT64_C\((0x[0-9a-f]+)\), (\w+)\}')


def define(text, name):
    m = re.search(rf'#define {name}\s+(\w+)', text)
    return int(m[1].rstrip('uU'), 0)


def table(path):
    """[(tm, srot, sensors)] in controller order, as the C file spells it."""
    text = path.read_text()
    rows = []
    for tm, srot, count in ROW.findall(text):
        rows.append((int(tm, 16), int(srot, 16),
                     define(text, count) if count.startswith('TSENS') else int(count.rstrip('u'))))
    return rows


def allowed_words(rows):
    """Every word the guard may read: SROT+0, SROT+4, TM+0xe4 and TM+0xa0+4n."""
    out = set()
    for tm, srot, count in rows:
        out |= {srot, srot + 4, tm + 0xe4} | {tm + 0xa0 + 4 * n for n in range(count)}
    return out


# -- a minimal flattened-device-tree reader (devicetree spec, structure block) --

def fdt(blob):
    magic, _total, off_struct, off_strings = struct.unpack_from('>4I', blob)
    if magic != 0xd00dfeed:
        raise ValueError('not a flattened device tree')
    pos, stack, root = off_struct, [], None
    while True:
        (token,) = struct.unpack_from('>I', blob, pos)
        pos += 4
        if token == 1:                                  # FDT_BEGIN_NODE
            end = blob.index(b'\0', pos)
            node = dict(name=blob[pos:end].decode(), props={}, children=[])
            pos = (end + 4) & ~3
            (stack[-1]['children'] if stack else [None]).append(node)
            root = root or node
            stack.append(node)
        elif token == 2:                                # FDT_END_NODE
            stack.pop()
        elif token == 3:                                # FDT_PROP
            length, nameoff = struct.unpack_from('>II', blob, pos)
            pos += 8
            name = blob[off_strings + nameoff:blob.index(b'\0', off_strings + nameoff)].decode()
            stack[-1]['props'][name] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:                                # FDT_NOP
            continue
        elif token == 9:                                # FDT_END
            return root
        else:
            raise ValueError(f'FDT token {token:#x} at {pos - 4:#x}')


def cells(value):
    return list(struct.unpack(f'>{len(value) // 4}I', value))


def strings(value):
    return [s.decode() for s in value.split(b'\0') if s]


def child(node, name):
    return next((c for c in node['children'] if c['name'] == name), None)


def regs(node, address_cells=2, size_cells=2):
    c = cells(node['props'].get('reg', b''))
    step = address_cells + size_cells
    join = lambda words: sum(w << (32 * (len(words) - 1 - i)) for i, w in enumerate(words))
    return [(join(c[i:i + address_cells]), join(c[i + address_cells:i + step]))
            for i in range(0, len(c) - step + 1, step)]


def check_dtb(path, rows, limit_mc):
    problems, notes = [], []
    root = fdt(path.read_bytes())
    soc = child(root, 'soc@0')
    if not soc:
        return ['no /soc@0 node'], notes
    if cells(soc['props']['#address-cells']) != [2] or cells(soc['props']['#size-cells']) != [2]:
        return ['/soc@0 is not #address-cells = #size-cells = 2'], notes
    nodes = [c for c in soc['children']
             if 'qcom,sc8280xp-tsens' in strings(c['props'].get('compatible', b''))]
    found = {}
    for n in nodes:
        compat = strings(n['props']['compatible'])
        reg = regs(n)
        count = cells(n['props']['#qcom,sensors'])[0]
        if 'qcom,tsens-v2' not in compat:
            problems.append(f'{n["name"]}: compatible {compat} lacks qcom,tsens-v2')
        if len(reg) != 2 or reg[0][1] != 0x1ff or reg[1][1] != 8:
            problems.append(f'{n["name"]}: reg {[(hex(a), hex(s)) for a, s in reg]} is not <TM 0x1ff>, <SROT 0x8>')
            continue
        found[reg[0][0]] = dict(name=n['name'], tm=reg[0][0], srot=reg[1][0], sensors=count,
                                phandle=cells(n['props']['phandle'])[0])
    if len(nodes) != 4:
        problems.append(f'{len(nodes)} qcom,sc8280xp-tsens nodes, expected 4')
    labels = {}
    symbols = child(root, '__symbols__')
    if symbols:
        for label, value in symbols['props'].items():
            labels[strings(value)[0]] = label
    for b, (tm, srot, count) in enumerate(rows):
        node = found.get(tm)
        if not node:
            problems.append(f'tsens{b}: no DT node with TM {tm:#x}')
            continue
        if (node['srot'], node['sensors']) != (srot, count):
            problems.append(f'tsens{b}: C has SROT {srot:#x}/{count} sensors, DT {node["srot"]:#x}/{node["sensors"]}')
        label = labels.get(f'/soc@0/{node["name"]}')
        if symbols and label != f'tsens{b}':
            problems.append(f'tsens{b}: DT label for {node["name"]} is {label}')
        if 0xa0 + 4 * count > 0x1ff:
            problems.append(f'tsens{b}: {count} status words overrun the 0x1ff TM window')
    # A word the guard may read must belong to its tsens node and no other device.
    words = allowed_words(rows)
    for n in soc['children']:
        if n['name'] in {f['name'] for f in found.values()}:
            continue
        for base, size in regs(n):
            for w in sorted(words):
                if base <= w < base + size:
                    problems.append(f'{n["name"]} reg {base:#x}+{size:#x} covers guard word {w:#x}')
    # Thermal zones on a tsens must name a sensor the guard reads.
    by_phandle = {found[tm]['phandle']: (b, found[tm]) for b, (tm, _, _) in enumerate(rows) if tm in found}
    critical = []
    zones = child(root, 'thermal-zones')
    for z in zones['children'] if zones else []:
        ref = cells(z['props'].get('thermal-sensors', b''))
        if not ref or ref[0] not in by_phandle:
            continue
        b, node = by_phandle[ref[0]]
        sensor = ref[1] if len(ref) > 1 else 0
        if sensor >= node['sensors']:
            problems.append(f'{z["name"]}: tsens{b} sensor {sensor} is not among the {node["sensors"]} the guard reads')
        trips = child(z, 'trips')
        for t in trips['children'] if trips else []:
            temp = struct.unpack('>i', t['props']['temperature'])[0]
            kind = strings(t['props']['type'])[0]
            if kind == 'critical':
                critical.append((temp, z['name']))
            elif temp < limit_mc:
                notes.append(f'{z["name"]} (tsens{b} sensor {sensor}) has a {kind} trip at {temp} mC, '
                             f'below the guard limit; nothing acts on it under the monitor')
    if not critical:
        problems.append('no critical trip on any tsens zone')
    elif limit_mc >= min(critical)[0]:
        problems.append(f'limit {limit_mc} mC is not below {min(critical)[1]} critical {min(critical)[0]} mC')
    else:
        notes.append(f'limit {limit_mc} mC is below the lowest tsens critical trip ({min(critical)[0]} mC)')
    return problems, notes


def compile_test(cc, out, sanitizers):
    cmd = [cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitizers,
           f'-I{ROOT / "compat"}', str(TEST), str(SRC), '-o', str(out)]
    return cmd, subprocess.run(cmd, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', help='host clang (default: $CC, then clang on PATH)')
    ap.add_argument('--dtb', type=Path, default=DTB, help=f'gaokun3 DTB (default: {DTB.relative_to(ROOT)})')
    a = ap.parse_args()
    failed = False

    header = HDR.read_text()
    guard, test = table(SRC), table(TEST)
    problems = []
    if len(guard) != 4 or guard != test:
        problems.append(f'guard table {guard} != test table {test}')
    if sum(r[2] for r in guard) != define(header, 'SC8280XP_THERMAL_SENSORS'):
        problems.append('sensor counts do not add up to SC8280XP_THERMAL_SENSORS')
    for p in problems:
        print(f'FAIL table: {p}')
    if problems:
        failed = True
    else:
        print(f'PASS table: guard and test agree on {len(guard)} controllers, '
              f'{sum(r[2] for r in guard)} sensors, {len(allowed_words(guard))} readable words')

    if not a.dtb.is_file():
        print(f'SKIP dtb: {a.dtb} not present (a local build output; pass --dtb)')
    else:
        limit = define(header, 'SC8280XP_THERMAL_LIMIT_MC')
        dt_problems, notes = check_dtb(a.dtb, guard, limit)
        for n in notes:
            print(f'INFO dtb: {n}')
        for p in dt_problems:
            print(f'FAIL dtb: {p}')
        if dt_problems:
            failed = True
        else:
            print(f'PASS dtb: {a.dtb.name} matches the guard table, zones and limit')

    pin = define(header, 'SC8280XP_THERMAL_VERSION_PIN')
    print(f'INFO pin: production SC8280XP_THERMAL_VERSION_PIN is '
          + ('0 (unmeasured): the guard refuses every poll' if not pin else
             f'{pin:#010x} (read at EL2, run 10)'))

    cc = a.cc or os.environ.get('CC') or shutil.which('clang')
    if not cc:
        print('NOT RUN test-sc8280xp-thermal.c: no host clang (pass --cc or set CC)')
        return 1 if failed else 2
    modes = [('ASan+UBSan', ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']),
             ('UBSan trap (no ASan runtime on this host)',
              ['-fsanitize=undefined', '-fsanitize-trap=undefined'])]
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ('test-sc8280xp-thermal' + EXE)
        for mode, flags in modes:
            cmd, built = compile_test(cc, exe, flags)
            if built.returncode == 0:
                break
        else:
            print(built.stdout + built.stderr)
            print('FAIL build: ' + ' '.join(cmd))
            return 1
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        print((ran.stdout + ran.stderr).rstrip())
        print(('PASS' if ran.returncode == 0 else f'FAIL (exit {ran.returncode})')
              + f' test-sc8280xp-thermal.c [{mode}]')
    return 1 if failed or ran.returncode else 0


if __name__ == '__main__':
    sys.exit(main())
