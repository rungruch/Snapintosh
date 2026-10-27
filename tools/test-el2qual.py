#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for probes/el2qual's report, firmware-reader and VHE hand-off code.

  python3 tools/test-el2qual.py [--cc CLANG]

1. probes/el2qual/report.c's ID register table must name exactly the
   encodings probes/idregs.h reads, in the same order and under the same
   names, so the two cannot drift.
2. tools/test-el2qual.c is built with AddressSanitizer and UBSan where the
   host has their runtimes, otherwise with UBSan in trap mode (no runtime;
   e.g. llvm-mingw on Arm64 Windows ships no ASan), and run. The mode used
   is printed, because the two are not the same evidence.
3. el2q_exit_vhe in probes/el2qual/el2qual-asm.S must perform the VHE exit
   in the order its comment requires. QEMU's TCG does not fault on a missing
   barrier or a misordered write, so a passing QEMU run cannot show this;
   the order is checked here instead, and deliberately reordered copies of
   the routine must be rejected.
4. Deliberately broken copies of probes/el2qual/pagemap.c, each removing
   one of its safety properties (break-before-make steps, the regime's
   leaf bits, refusals, never rewriting a valid entry), must each make
   tools/test-el2qual.c fail.
5. The same for probes/el2qual/s2walk.c: the table mapping check, the
   SError checks, stopping at a fault, reading each descriptor once, the
   format refusals and the page classes, and the survey's choice of
   table, bound and map rows; and for the memory-map scan in
   probes/el2qual/firmware.c that reports the candidate region.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EL2Q = ROOT / 'probes' / 'el2qual'
EXE = '.exe' if os.name == 'nt' else ''


def idregs_table():
    """(crm, op2, name) for each case of probes/idregs.h's id_register()."""
    text = (ROOT / 'probes' / 'idregs.h').read_text()
    body = text.split('static uint64_t id_register', 1)[1].split('default:', 1)[0]
    rows = []
    for case, crm, op2, name in re.findall(
            r'case 0x([0-9a-f]+): __asm__ volatile\("mrs %0, s3_0_c0_c(\d)_(\d)"[^/]*/\* (\w+) \*/',
            body):
        assert int(case, 16) == (int(crm) << 3 | int(op2)), f'idregs.h case {case} != c{crm}_{op2}'
        rows.append((int(crm), int(op2), name))
    return rows


def report_table():
    text = (EL2Q / 'report.c').read_text()
    body = text.split('el2q_idregs[EL2Q_ID_COUNT] = {', 1)[1].split('};', 1)[0]
    return [(int(c), int(o), n) for c, o, _, n in
            re.findall(r'\{(\d), (\d), ([01]), "(\w+)"\}', body)]


def short_name(name):
    """idregs.h spells registers in full; the report drops _EL1 and, for the
    AArch64 ones, the ID_ prefix so they fit its columns."""
    name = name.removesuffix('_EL1')
    return name.removeprefix('ID_') if name.startswith('ID_AA64') else name


def check_tables():
    want = [(c, o, short_name(n)) for c, o, n in idregs_table()]
    got = report_table()
    count = int(re.search(r'#define EL2Q_ID_COUNT\s+(\d+)', (EL2Q / 'report.h').read_text())[1])
    problems = []
    if len(want) != count:
        problems.append(f'idregs.h reads {len(want)} registers, EL2Q_ID_COUNT is {count}')
    if got != want:
        for i, (a, b) in enumerate(zip(want, got)):
            if a != b:
                problems.append(f'row {i}: idregs.h {a}, report.c {b}')
        if len(got) != len(want):
            problems.append(f'report.c has {len(got)} rows, idregs.h {len(want)}')
    return problems


EXIT_REGS = ('tcr_el2', 'ttbr0_el2', 'cptr_el2', 'cnthctl_el2')


def exit_vhe_body(asm):
    """The instructions of el2q_exit_vhe, comments and labels removed, as
    (mnemonic, [operands]) pairs."""
    body = asm.split('el2q_exit_vhe:', 1)[1].split('\n.global', 1)[0].split('/* ----', 1)[0]
    insns = []
    for line in body.splitlines():
        line = re.sub(r'/\*.*?\*/|//.*', '', line).strip()
        if not line or line.endswith(':') or line.startswith('.'):
            continue
        op, _, rest = line.partition(' ')
        insns.append((op.lower(), [a.strip().lower() for a in rest.split(',')] if rest.strip() else []))
    return insns


def check_exit_sequence(insns):
    """Problems with the VHE exit's order (see the comment on el2q_exit_vhe):
    loads only before the MMU goes off; the MMU turned off from the current
    SCTLR_EL2 less M, after a DSB and followed by an ISB; TLBI ALLE2 and a
    DSB while E2H is still set; HCR_EL2 once, then an ISB; TCR, TTBR0, CPTR
    and CNTHCTL once each in the new layouts; TLBI ALLE2, DSB, ISB; the
    planned SCTLR_EL2; ISB; return. No other system register is written."""
    ops = [op for op, _ in insns]
    msr = [(i, args[0]) for i, (op, args) in enumerate(insns) if op == 'msr' and args]
    sctlr = [i for i, reg in msr if reg == 'sctlr_el2']
    if len(sctlr) != 2:
        return [f'{len(sctlr)} writes to sctlr_el2, wanted 2 (MMU off, then the plan)']
    off, on = sctlr
    problems = []
    memory = [i for i, op in enumerate(ops) if re.match(r'(ld|st)[a-z]*$', op) and op not in ('stlr',)]
    if any(off <= i <= on for i in memory) or any(i > on for i in memory):
        problems.append('a memory access while the MMU is off or after the switch')
    # MMU off: bic rX, rX, #1 of an mrs rX, sctlr_el2, then dsb, msr, isb.
    src = insns[off][1][1]
    made = [i for i in range(off) if insns[i][0] == 'bic' and insns[i][1][0] == src]
    read = [i for i in range(off) if insns[i][0] == 'mrs' and insns[i][1][1:] == ['sctlr_el2']]
    if not made or insns[made[-1]][1][2] != '#1' or not read or insns[read[-1]][1][0] != insns[made[-1]][1][1]:
        problems.append('the first sctlr_el2 write is not the current value less M')
    last_mem = max([i for i in memory if i < off], default=-1)
    if 'dsb' not in ops[last_mem + 1:off]:
        problems.append('no DSB between the last memory access and turning the MMU off')
    if ops[off + 1:off + 2] != ['isb']:
        problems.append('no ISB straight after turning the MMU off')
    hcr = [i for i, reg in msr if reg == 'hcr_el2']
    if len(hcr) != 1:
        return problems + [f'{len(hcr)} writes to hcr_el2, wanted 1']
    hcr = hcr[0]
    tlbis = [i for i, (op, args) in enumerate(insns) if op == 'tlbi']
    if any(insns[i][1] != ['alle2'] for i in tlbis):
        problems.append('a TLBI other than ALLE2')
    first = [i for i in tlbis if off < i < hcr]
    if not first or 'dsb' not in ops[first[-1] + 1:hcr]:
        problems.append('no TLBI ALLE2 and DSB between the MMU going off and the HCR_EL2 write')
    if ops[hcr + 1:hcr + 2] != ['isb']:
        problems.append('no ISB straight after the HCR_EL2 write')
    second = [i for i in tlbis if hcr < i < on]
    for reg in EXIT_REGS:
        at = [i for i, r in msr if r == reg]
        if len(at) != 1:
            problems.append(f'{len(at)} writes to {reg}, wanted 1')
        elif not hcr + 1 < at[0] < (second[0] if second else on):
            problems.append(f'{reg} written outside the window between HCR_EL2 and the second TLBI')
    if not second:
        problems.append('no TLBI ALLE2 after the new controls')
    elif ops[second[-1] + 1:on] != ['dsb', 'isb']:
        problems.append('the second TLBI is not followed by exactly DSB, ISB before the MMU goes on')
    if ops[on + 1:] != ['isb', 'ret']:
        problems.append('the planned SCTLR_EL2 is not followed by ISB and RET')
    others = [reg for i, reg in msr if reg not in ('sctlr_el2', 'hcr_el2') + EXIT_REGS]
    if others:
        problems.append(f'writes to {", ".join(others)}, which the exit does not plan')
    if insns[on][1][1] != 'x2' or not any(op == 'ldp' and args[1] == 'x2' for op, args in insns):
        problems.append('the MMU-on SCTLR_EL2 is not the planned value loaded at entry')
    return problems


def sequence_mutations(insns):
    """Deliberately wrong copies of the exit, each of which must be refused."""
    def find(op, *args):
        return next(i for i, (o, a) in enumerate(insns) if o == op and a[:len(args)] == list(args))
    def drop(i):
        return insns[:i] + insns[i + 1:]
    def swap(i, j):
        out = list(insns)
        out[i], out[j] = out[j], out[i]
        return out
    hcr = find('msr', 'hcr_el2')
    tcr = find('msr', 'tcr_el2')
    tlbis = [i for i, (o, _) in enumerate(insns) if o == 'tlbi']
    sctlr = [i for i, (o, a) in enumerate(insns) if o == 'msr' and a[0] == 'sctlr_el2']
    off_isb = sctlr[0] + 1
    return {
        'HCR after TCR': swap(hcr, tcr),
        'no ISB after HCR': drop(hcr + 1),
        'no TLBI before HCR': drop(tlbis[0]),
        'second TLBI before the new controls': swap(tlbis[1], find('msr', 'cnthctl_el2')),
        'no DSB after the second TLBI': drop(tlbis[1] + 1),
        'MMU back on before the second TLBI': swap(sctlr[1], tlbis[1]),
        'a load with the MMU off': insns[:off_isb + 1] + [('ldr', ['x9', '[x0]'])] + insns[off_isb + 1:],
        'MMU off with the planned value': [(o, ['sctlr_el2', 'x2'] if i == sctlr[0] else a)
                                           for i, (o, a) in enumerate(insns)],
        'no CPTR write': drop(find('msr', 'cptr_el2')),
        'no DSB before the MMU goes off': drop(sctlr[0] - 1),
        'no ISB after the MMU goes off': drop(off_isb),
        'an unplanned VBAR write': insns[:tcr] + [('msr', ['vbar_el2', 'x9'])] + insns[tcr:],
        'TLBI VMALLE1 instead': [(o, ['vmalle1'] if i == tlbis[1] else a) for i, (o, a) in enumerate(insns)],
        'no ISB before RET': drop(len(insns) - 2),
    }


def check_exit_vhe():
    insns = exit_vhe_body((EL2Q / 'el2qual-asm.S').read_text())
    problems = [f'el2q_exit_vhe: {p}' for p in check_exit_sequence(insns)]
    for name, wrong in sequence_mutations(insns).items():
        if not check_exit_sequence(wrong):
            problems.append(f'the order check accepted a deliberately wrong copy: {name}')
    return problems, len(sequence_mutations(insns))


def compile_test(cc, out, sanitizers, pagemap=None, s2walk=None, firmware=None):
    cmd = [cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitizers,
           f'-I{EL2Q}', str(ROOT / 'tools' / 'test-el2qual.c'),
           str(ROOT / 'tools' / 'test-el2qual-vhe.c'), str(ROOT / 'tools' / 'test-el2qual-usbprobe.c'),
           str(ROOT / 'tools' / 'test-el2qual-pagemap.c'), str(ROOT / 'tools' / 'test-el2qual-s2walk.c'),
           str(EL2Q / 'report.c'), str(EL2Q / 'usbprobe.c'), str(pagemap or EL2Q / 'pagemap.c'),
           str(s2walk or EL2Q / 's2walk.c'),
           str(firmware or EL2Q / 'firmware.c'), str(EL2Q / 'thermal.c'), str(EL2Q / 'vhe.c'),
           str(ROOT / 'compat' / 'sc8280xp-thermal.c'), '-o', str(out)]
    return cmd, subprocess.run(cmd, capture_output=True, text=True)


# (name, text in pagemap.c, replacement): each must make the suite fail.
PAGEMAP_MUTATIONS = [
    ('the slot written while building',
     '    out->cloned = l3 != 0;\n',
     '    out->cloned = l3 != 0;\n    hw->write64(hw->ctx, l2_slot, (fresh & PT_OA) | 3);\n'),
    ('PXN, RES0, as execute-never in the EL2 regime',
     'PT_RO_EL2 | PT_XN)', 'PT_RO_EL2 | PT_PXN)'),
    ('AP[1] clear in the EL2 regime', '#define PT_RO_EL2  (3ull << 6)', '#define PT_RO_EL2  (2ull << 6)'),
    ('the table limits dropped', '(l2_desc & PT_TABLE_ATTRS) | ', ''),
    ('an invalid non-zero level-2 entry replaced', 'if (l2_desc && (l2_desc & 3) != 3) {', 'if (0) {'),
    ('no break', '    hw->write64(hw->ctx, io->l2_slot, 0);\n', ''),
    ('no TLBI', '    hw->sync(hw->ctx, EL2Q_PM_TLBI, 0, 0);\n', ''),
    ('the clone not cleaned first', '    hw->sync(hw->ctx, EL2Q_PM_CLEAN, io->table, 0x1000);\n', ''),
    ('no ISB', '    hw->sync(hw->ctx, EL2Q_PM_ISB, 0, 0);\n', ''),
    ('a valid entry rewritten', 'if (i >= first && i < first + count && !e)',
     'if (i >= first && i < first + count)'),
    ('a non-identity entry accepted', '(e & PT_OA) != page', '((e & PT_OA) != page && 0)'),
    ('a Normal MAIR byte taken as Device', '& 0xf3ull', '& 0xf0ull'),
    ('the start level ignored', 'unsigned start = 4 - levels;', 'unsigned start = 0;'),
]


# The same for s2walk.c. The read limit has no mutation: with each
# descriptor read once, no table shape reaches it.
S2WALK_MUTATIONS = [
    ('a table page read without its mapping check', 'if (!table_readable(par, page)) {', 'if (!table_readable(par, page) && 0) {'),
    ('an outer non-cacheable table page accepted', 'write_back(attr >> 4) && ', ''),
    ('a table page not inner shareable accepted', ' && (par & PAR_SH_IS) == PAR_SH_IS', ''),
    ('a table page at another address accepted', '!((par ^ page) & PAR_PA) && ', '!((par ^ page) & PAR_PA & 0) && '),
    ('non-cacheable taken as write-back', '((nibble & 0xc) == 0x4 && (nibble & 3))', '(nibble & 0xc) == 0x4'),
    ('no SError check after a read', 'if (w->hw->serror(w->hw->ctx)) {', 'if (0) {'),
    ('an SError already pending ignored', '    if (hw->serror(hw->ctx)) return EL2Q_S2W_PENDING;\n', ''),
    ('the walk continued past a fault', '    if (esr) {', '    if (0) {'),
    ('a descriptor read again', 'if (w->desc_valid[level] && w->desc_at[level] == address) {', 'if (0) {'),
    ('the access flag ignored', 'if (!(desc & S2_AF) || (oa >> oas)) return', 'if ((oa >> oas)) return'),
    ('a leaf past the output size accepted', 'if (!(desc & S2_AF) || (oa >> oas)) return', 'if (!(desc & S2_AF) || ((oa >> oas) && 0)) return'),
    ('S2AP ignored', '(desc & S2_AP_RW) == S2_AP_RW ? EL2Q_S2W_RW : EL2Q_S2W_LIMITED', 'EL2Q_S2W_RW'),
    ('a block compared page by page', 'oa != (ipa & ~(size - 1))', 'oa != ipa'),
    ('a reserved level-3 encoding taken as a page', '(level == 3 && type != S2_TYPE) || ', ''),
    ('a level-0 block accepted', ' || (level == 0 && type == S2_BLOCK)', ''),
    ('a table past the output size followed', 'if (table >> oas) {', 'if (0) {'),
    ('concatenated start tables assumed', 'input > below + 9 || ', ''),
    ('an input larger than the output accepted', ' || input > oas)', ')'),
    ('the root alignment ignored', '(root & (entries * 8 - 1)) || ', ''),
    ('VA64 ignored', '!(bank->cba2r & 1) || ', ''),
    ('the granule ignored', '((bank->tcr >> 14) & 3) || ', ''),
    ('SL0 ignored', 'unsigned start = 2 - sl0,', 'unsigned start = 1,'),
    ('translation off walked anyway', '    if (!(bank->sctlr & 1)) return EL2Q_S2W_NOT_NEEDED;\n', ''),
    ('an arena above 4 GiB accepted', 'arena + EL2Q_S2W_ARENA_SIZE > (1ull << 32)', '0'),
    ('CnP taken as an address bit', '#define TTBR_BADDR 0x0000fffffffffffeull', '#define TTBR_BADDR 0x0000ffffffffffffull'),
    # The survey.
    ('a level-2 table surveyed under an entry that is not a table', "if (out->l1[out->gib] != 'T') {", 'if (0) {'),
    ('a survey started below level 1', '    if (f.start != 1) return out->state = EL2Q_S2W_FORMAT;\n', ''),
    ('a table past the output size shown as a table', "(desc & S2_OA) >> oas ? 'X' : 'T'", "'T'"),
    ('the survey bounded by the walk\'s limit', '.limit = EL2Q_S2W_SURVEY_MAX_READS', '.limit = EL2Q_S2W_MAX_READS'),
    ('the wrong level-1 entry kept as the arena\'s', 'if (i == out->gib) {', 'if (i == 0) {'),
    ('the level-2 table address with its low bits', 'out->l2_table = out->gib_desc & S2_OA;',
     'out->l2_table = out->gib_desc;'),
    ('level-2 entries classed as pages', '((uint64_t)out->gib << 30) + (i << 21)', '((uint64_t)out->gib << 30) + (i << 12)'),
    ('empty rows printed', "if (row_all(s->l2, r, '.') || row_all(s->l2, r, '-')) continue;",
     "if (row_all(s->l2, r, '-')) continue;"),
    ('unread rows printed', "if (row_all(s->l2, r, '.') || row_all(s->l2, r, '-')) continue;",
     "if (row_all(s->l2, r, '.')) continue;"),
    ('a row labelled with the wrong address', '((uint64_t)r * ROW << 21)', '((uint64_t)r * ROW << 20)'),
    # The candidate region's lines.
    ('"AND 0 MORE" printed', 'index == shown + 1 && r->count > shown', 'index == shown + 1'),
]

# The memory-map scan for the candidate region (el2q_memmap_region in
# firmware.c).
FIRMWARE_MUTATIONS = [
    ('a descriptor starting at the end counted', 'if (start >= end || start + (pages << 12) <= base) continue;',
     'if (start > end || start + (pages << 12) <= base) continue;'),
    ('a descriptor ending at the base counted', 'if (start >= end || start + (pages << 12) <= base) continue;',
     'if (start >= end || start + (pages << 12) < base) continue;'),
    ('a descriptor without pages counted', 'if (!pages || pages > UINT64_MAX >> 12 || start + (pages << 12) < start) continue;',
     'if (pages > UINT64_MAX >> 12 || start + (pages << 12) < start) continue;'),
    ('a wrapping descriptor counted', 'if (!pages || pages > UINT64_MAX >> 12 || start + (pages << 12) < start) continue;',
     'if (!pages || pages > UINT64_MAX >> 12) continue;'),
    ('the entries left unsorted', 'while (n && out->e[n - 1].start > start) {', 'while (0 && n) {'),
    ('a higher entry let into a full list', '        if (n == EL2Q_S2W_MAP_ENTRIES && start >= out->e[n - 1].start) continue;\n', ''),
    ('the attribute read from the page count', 'le64(m + off + 32)}', 'le64(m + off + 24)}'),
    ('a short descriptor size accepted', 'if (!m || desc_size < 40 || !map_size) return;', 'if (!m || !map_size) return;'),
]


def check_mutations(cc, flags, tmp, name, mutations):
    """Problems: mutations of probes/el2qual/<name> that do not apply, or
    that the suite accepts."""
    source = (EL2Q / name).read_text()
    problems = []
    for i, (what, old, new) in enumerate(mutations):
        if source.count(old) != 1:
            problems.append(f'{what}: its text is not in {name} exactly once')
            continue
        mutant = Path(tmp) / f'{Path(name).stem}-{i}.c'
        mutant.write_text(source.replace(old, new))
        exe = Path(tmp) / (f'mutant-{i}' + EXE)
        cmd, built = compile_test(cc, exe, flags, **{Path(name).stem: mutant})
        if built.returncode:
            problems.append(f'{what}: does not build')
            continue
        if subprocess.run([str(exe)], capture_output=True, text=True).returncode == 0:
            problems.append(f'{what}: accepted')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', help='host clang (default: $CC, then clang on PATH)')
    a = ap.parse_args()
    cc = a.cc or os.environ.get('CC') or shutil.which('clang')
    if not cc:
        print('NOT RUN: no host clang (pass --cc or set CC)')
        return 2

    problems = check_tables()
    for p in problems:
        print(f'FAIL table: {p}')
    if not problems:
        print(f'PASS table: report.c names the {len(report_table())} encodings idregs.h reads')
    order, wrong = check_exit_vhe()
    for p in order:
        print(f'FAIL vhe exit order: {p}')
    if not order:
        print(f'PASS vhe exit order: el2q_exit_vhe follows its required order; '
              f'{wrong} deliberately reordered copies are rejected')
    problems += order

    modes = [('ASan+UBSan', ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']),
             ('UBSan trap (no ASan runtime on this host)',
              ['-fsanitize=undefined', '-fsanitize-trap=undefined'])]
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ('test-el2qual' + EXE)
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
        verdict = 'PASS' if ran.returncode == 0 else f'FAIL (exit {ran.returncode})'
        print(f'{verdict} test-el2qual.c [{mode}]')
        if ran.returncode == 0:
            for name, mutations in (('pagemap.c', PAGEMAP_MUTATIONS), ('s2walk.c', S2WALK_MUTATIONS),
                                    ('firmware.c', FIRMWARE_MUTATIONS)):
                mutants = check_mutations(cc, flags, tmp, name, mutations)
                for m in mutants:
                    print(f'FAIL {Path(name).stem} mutation: {m}')
                if not mutants:
                    print(f'PASS {Path(name).stem} mutations: {len(mutations)} deliberately broken copies '
                          f'of {name} are rejected')
                problems += mutants
    return 1 if problems or ran.returncode else 0


if __name__ == '__main__':
    sys.exit(main())
