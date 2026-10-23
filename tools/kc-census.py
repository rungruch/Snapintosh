#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Census of the instructions a nested-virtualization-free guest cannot run.

Phase 0B of the gaokun3 port (docs/GAOKUN3-PHASE0B.md, questions A1-A7 and
B5-B6) asks how many places in an XNU kernel depend on EL2 semantics the
Snapdragon 8cx Gen 3 cannot provide (no FEAT_NV/NV2, no FEAT_FGT): EL2
register moves, EL2 TLBI/AT, `mrs CurrentEL`, `eret`, hypervisor/secure
monitor calls, Apple's guarded-execution words and Apple's IMPLEMENTATION
DEFINED registers. It also counts the architectural counter and timer
registers, which decide whether a guest can run on the gaokun3's 19.2 MHz
counter without FEAT_ECV (A7, B7).

    tools/kc-census.py /System/Library/Kernels/kernel.release.t8140
    tools/kc-census.py <kernelcache> --sites build/phase0b/sites.tsv

The scan is linear over every executable segment, so data words embedded in
text are counted too and every figure is an upper bound (the same method and
limit as docs/APPLE-ENCODINGS.md). For an MH_FILESET kernelcache each site is
attributed to the fileset entry whose own segment covers it, because the
top-level segments hold all kexts' code. Nothing is written but the optional
site list; keep outputs under build/ (ignored).
"""
import argparse
import bisect
import pathlib
import struct
import subprocess
import sys
from collections import Counter

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_FILESET_ENTRY = 0x80000035
VM_PROT_EXECUTE = 4

# Apple guarded execution. ALT_CALL is the undefined encoding every call stub
# carries next to GENTER (see tools/kc-analyze.py).
GENTER = 0x00201420
GEXIT = 0x00201400
ALT_CALL = 0xD4E00000
APPLE_BLOCK = (0x00201000, 0x00202000)   # Apple's reserved 0x00201xxx block

ERET_OPCODES = {0xD69F03E0: 'eret', 0xD69F0BFF: 'eretaa', 0xD69F0FFF: 'eretab'}
MRS_CURRENTEL = (0xD5384240, 0xFFFFFFE0)   # mrs Xt, CurrentEL (op0 3 op1 0 c4 c2 2)

# Arm reserves CRn 11 and 15 of op0 3 (and of SYS) for IMPLEMENTATION DEFINED
# use, whatever op1 says; Apple's registers live there.
IMPDEF_CRN = (11, 15)

# (op0, op1, CRn, CRm, op2). Only encodings checked against the Arm ARM
# register index; the ones added after the first census (the FGT, SME, GIC
# list, counter and timer registers, ACTLR_EL12, SCXTNUM_EL12) are also
# anchored to LLVM's encodings in tools/test-kc-census.py. Anything else prints in the generic
# s<op0>_<op1>_c<n>_c<m>_<op2> form that LLVM and lldb use for unnamed registers.
SYSREG_NAMES = {
    (3, 4, 0, 0, 0): 'VPIDR_EL2', (3, 4, 0, 0, 5): 'VMPIDR_EL2',
    (3, 4, 1, 0, 0): 'SCTLR_EL2', (3, 4, 1, 0, 1): 'ACTLR_EL2',
    (3, 4, 1, 1, 0): 'HCR_EL2', (3, 4, 1, 1, 1): 'MDCR_EL2',
    (3, 4, 1, 1, 2): 'CPTR_EL2', (3, 4, 1, 1, 3): 'HSTR_EL2',
    (3, 4, 1, 1, 4): 'HFGRTR_EL2', (3, 4, 1, 1, 5): 'HFGWTR_EL2',
    (3, 4, 1, 1, 6): 'HFGITR_EL2',
    (3, 4, 1, 1, 7): 'HACR_EL2', (3, 4, 1, 2, 0): 'ZCR_EL2',
    (3, 4, 1, 2, 2): 'HCRX_EL2', (3, 4, 1, 2, 5): 'SMPRIMAP_EL2',
    (3, 4, 2, 0, 0): 'TTBR0_EL2', (3, 4, 2, 0, 1): 'TTBR1_EL2',
    (3, 4, 2, 0, 2): 'TCR_EL2', (3, 4, 2, 1, 0): 'VTTBR_EL2',
    (3, 4, 2, 1, 2): 'VTCR_EL2', (3, 4, 2, 2, 0): 'VNCR_EL2',
    (3, 4, 3, 0, 0): 'DACR32_EL2',
    (3, 4, 3, 1, 4): 'HDFGRTR_EL2', (3, 4, 3, 1, 5): 'HDFGWTR_EL2',
    (3, 4, 4, 0, 0): 'SPSR_EL2', (3, 4, 4, 0, 1): 'ELR_EL2',
    (3, 4, 4, 1, 0): 'SP_EL1',
    (3, 4, 5, 0, 1): 'IFSR32_EL2', (3, 4, 5, 1, 0): 'AFSR0_EL2',
    (3, 4, 5, 1, 1): 'AFSR1_EL2', (3, 4, 5, 2, 0): 'ESR_EL2',
    (3, 4, 5, 2, 3): 'VSESR_EL2', (3, 4, 5, 3, 0): 'FPEXC32_EL2',
    (3, 4, 5, 6, 0): 'TFSR_EL2',
    (3, 4, 6, 0, 0): 'FAR_EL2', (3, 4, 6, 0, 4): 'HPFAR_EL2',
    (3, 4, 10, 2, 0): 'MAIR_EL2', (3, 4, 10, 3, 0): 'AMAIR_EL2',
    (3, 4, 12, 0, 0): 'VBAR_EL2',
    (3, 4, 13, 0, 1): 'CONTEXTIDR_EL2', (3, 4, 13, 0, 2): 'TPIDR_EL2',
    (3, 4, 13, 0, 7): 'SCXTNUM_EL2',
    (3, 4, 14, 0, 3): 'CNTVOFF_EL2', (3, 4, 14, 1, 0): 'CNTHCTL_EL2',
    (3, 4, 14, 2, 0): 'CNTHP_TVAL_EL2', (3, 4, 14, 2, 1): 'CNTHP_CTL_EL2',
    (3, 4, 14, 2, 2): 'CNTHP_CVAL_EL2', (3, 4, 14, 3, 0): 'CNTHV_TVAL_EL2',
    (3, 4, 14, 3, 1): 'CNTHV_CTL_EL2', (3, 4, 14, 3, 2): 'CNTHV_CVAL_EL2',
    (3, 4, 12, 11, 0): 'ICH_HCR_EL2', (3, 4, 12, 11, 1): 'ICH_VTR_EL2',
    (3, 4, 12, 11, 2): 'ICH_MISR_EL2', (3, 4, 12, 11, 3): 'ICH_EISR_EL2',
    (3, 4, 12, 11, 5): 'ICH_ELRSR_EL2', (3, 4, 12, 11, 7): 'ICH_VMCR_EL2',
    # The _EL12 / _EL02 aliases a VHE host uses to reach EL1/EL0 state.
    (3, 5, 1, 0, 0): 'SCTLR_EL12', (3, 5, 1, 0, 1): 'ACTLR_EL12',
    (3, 5, 1, 0, 2): 'CPACR_EL12', (3, 5, 1, 2, 6): 'SMCR_EL12',
    (3, 5, 2, 0, 0): 'TTBR0_EL12', (3, 5, 2, 0, 1): 'TTBR1_EL12',
    (3, 5, 2, 0, 2): 'TCR_EL12',
    (3, 5, 4, 0, 0): 'SPSR_EL12', (3, 5, 4, 0, 1): 'ELR_EL12',
    (3, 5, 5, 1, 0): 'AFSR0_EL12', (3, 5, 5, 1, 1): 'AFSR1_EL12',
    (3, 5, 5, 2, 0): 'ESR_EL12', (3, 5, 6, 0, 0): 'FAR_EL12',
    (3, 5, 10, 2, 0): 'MAIR_EL12', (3, 5, 10, 3, 0): 'AMAIR_EL12',
    (3, 5, 12, 0, 0): 'VBAR_EL12', (3, 5, 13, 0, 1): 'CONTEXTIDR_EL12',
    (3, 5, 13, 0, 7): 'SCXTNUM_EL12', (3, 5, 14, 1, 0): 'CNTKCTL_EL12',
    (3, 5, 14, 2, 0): 'CNTP_TVAL_EL02', (3, 5, 14, 2, 1): 'CNTP_CTL_EL02',
    (3, 5, 14, 2, 2): 'CNTP_CVAL_EL02', (3, 5, 14, 3, 0): 'CNTV_TVAL_EL02',
    (3, 5, 14, 3, 1): 'CNTV_CTL_EL02', (3, 5, 14, 3, 2): 'CNTV_CVAL_EL02',
    # The EL0/EL1 counter and timer registers.
    (3, 3, 14, 0, 0): 'CNTFRQ_EL0', (3, 3, 14, 0, 1): 'CNTPCT_EL0',
    (3, 3, 14, 0, 2): 'CNTVCT_EL0', (3, 3, 14, 0, 5): 'CNTPCTSS_EL0',
    (3, 3, 14, 0, 6): 'CNTVCTSS_EL0',
    (3, 3, 14, 2, 0): 'CNTP_TVAL_EL0', (3, 3, 14, 2, 1): 'CNTP_CTL_EL0',
    (3, 3, 14, 2, 2): 'CNTP_CVAL_EL0', (3, 3, 14, 3, 0): 'CNTV_TVAL_EL0',
    (3, 3, 14, 3, 1): 'CNTV_CTL_EL0', (3, 3, 14, 3, 2): 'CNTV_CVAL_EL0',
    (3, 0, 14, 1, 0): 'CNTKCTL_EL1',
}
for _n in range(4):
    SYSREG_NAMES[(3, 4, 12, 8, _n)] = f'ICH_AP0R{_n}_EL2'
    SYSREG_NAMES[(3, 4, 12, 9, _n)] = f'ICH_AP1R{_n}_EL2'
for _n in range(16):   # list registers 0-7 at CRm 12, 8-15 at CRm 13
    SYSREG_NAMES[(3, 4, 12, 12 + _n // 8, _n % 8)] = f'ICH_LR{_n}_EL2'


def sysreg_name(key):
    name = SYSREG_NAMES.get(key)
    return name or 's%d_%d_c%d_c%d_%d' % key


def decode_system(word):
    """Fields of an MSR/MRS (register) or SYS/SYSL word, or None.

    Bits 31:22 are 1101010100. L (bit 21) selects the read direction. Bits
    20:19 are the op0 field: 0 is MSR (immediate), 1 is SYS/SYSL, and 2 or 3
    is MSR/MRS (register) where op0 is bit 19 plus 2.
    """
    if (word & 0xFFC00000) != 0xD5000000:
        return None
    field = (word >> 19) & 3
    if field == 0:
        return None
    return {
        'reg': field >= 2,
        'op0': 1 if field == 1 else 2 + (field & 1),
        'read': bool(word & 0x00200000),
        'op1': (word >> 16) & 7,
        'crn': (word >> 12) & 15,
        'crm': (word >> 8) & 15,
        'op2': (word >> 5) & 7,
    }


def sysop_name(f):
    """Name an op0 1 system instruction from its CRn/CRm."""
    if f['crn'] == 8:
        return 'TLBI'
    if f['crn'] == 7 and f['crm'] == 8:
        return 'AT'
    if f['crn'] == 7:
        return 'DC/IC'
    return 'SYS'


def classify(word):
    """Classify one instruction word; None when it is of no interest.

    Returns (kind, label, rw). The kinds are mutually exclusive, impdef taking
    precedence over el2-reg/el12-reg because Arm gives CRn 11 and 15 to the
    vendor whatever op1 is.
    """
    if word == GENTER:
        return 'genter', 'genter', ''
    if word == GEXIT:
        return 'gexit', 'gexit', ''
    if word == ALT_CALL:
        return 'alt-call', 'alt-call', ''
    if APPLE_BLOCK[0] <= word < APPLE_BLOCK[1]:
        return 'apple-block', '0x%08x' % word, ''
    if word in ERET_OPCODES:
        return 'eret', ERET_OPCODES[word], ''
    if (word & MRS_CURRENTEL[1]) == MRS_CURRENTEL[0]:
        return 'currentel', 'CurrentEL', 'R'
    if (word & 0xFFE0001F) == 0xD4000002:
        return 'hvc', 'hvc #%d' % ((word >> 5) & 0xFFFF), ''
    if (word & 0xFFE0001F) == 0xD4000003:
        return 'smc', 'smc #%d' % ((word >> 5) & 0xFFFF), ''
    f = decode_system(word)
    if f is None:
        return None
    rw = 'R' if f['read'] else 'W'
    key = (f['op0'], f['op1'], f['crn'], f['crm'], f['op2'])
    if f['reg']:
        if f['crn'] in IMPDEF_CRN:
            return 'impdef-reg', sysreg_name(key), rw
        if f['op1'] == 4:
            return 'el2-reg', sysreg_name(key), rw
        if f['op1'] == 5:
            return 'el12-reg', sysreg_name(key), rw
        # Generic timer registers: CRn 14 at op1 3, and CNTKCTL_EL1 at op1 0.
        # The EL2 and _EL02 ones are already counted above.
        if f['crn'] == 14 and f['op1'] in (0, 3):
            return 'counter-reg', sysreg_name(key), rw
        return None
    if f['crn'] in IMPDEF_CRN:
        return 'impdef-sys', 'sys #%d,c%d,c%d,#%d' % (f['op1'], f['crn'], f['crm'], f['op2']), rw
    if f['op1'] in (4, 5):
        label = '%s op1=%d c%d c%d #%d' % (sysop_name(f), f['op1'], f['crn'], f['crm'], f['op2'])
        return 'el2-sysop', label, rw
    return None


KINDS = (
    ('el2-reg', 'A1  EL2 register MRS/MSR (op1 4)'),
    ('el12-reg', 'A1  _EL12/_EL02 alias MRS/MSR (op1 5)'),
    ('el2-sysop', 'A2  EL2 system instructions (TLBI/AT/SYS, op1 4/5)'),
    ('currentel', 'A3  mrs CurrentEL'),
    ('eret', 'A4  ERET / ERETAA / ERETAB'),
    ('hvc', 'A5  HVC'),
    ('smc', 'A5  SMC'),
    ('genter', 'A5  genter'),
    ('alt-call', 'A5  alt-call 0xd4e00000'),
    ('gexit', 'A5  gexit'),
    ('counter-reg', 'A7  counter and timer MRS/MSR (CRn 14, op1 0/3)'),
    ('apple-block', 'B5  other 0x00201xxx words (AMX and kin)'),
    ('impdef-reg', 'B5  IMPLEMENTATION DEFINED MRS/MSR (CRn 11/15)'),
    ('impdef-sys', 'B5  IMPLEMENTATION DEFINED SYS/SYSL (CRn 11/15)'),
)


# -- Mach-O ------------------------------------------------------------------

def parse_header(data, off=0):
    magic, _, _, _, ncmds = struct.unpack_from('<IiiII', data, off)
    if magic != MH_MAGIC_64:
        raise ValueError('not a 64-bit Mach-O at 0x%x' % off)
    segments, entries, p = [], [], off + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, p)
        if cmd == LC_SEGMENT_64:
            name = data[p + 8:p + 24].rstrip(b'\0').decode()
            vmaddr, vmsize, fileoff, filesize, _, initprot = struct.unpack_from('<QQQQii', data, p + 24)
            segments.append({'name': name, 'vmaddr': vmaddr, 'vmsize': vmsize,
                             'fileoff': fileoff, 'filesize': filesize, 'initprot': initprot})
        elif cmd == LC_FILESET_ENTRY:
            vmaddr, fileoff, strx = struct.unpack_from('<QQI', data, p + 8)
            end = data.index(b'\0', p + strx)
            entries.append((data[p + strx:end].decode(), fileoff))
        p += size
    return segments, entries


def executable(seg):
    return bool(seg['initprot'] & VM_PROT_EXECUTE) and seg['filesize'] > 0


def build_owner_index(data, entries):
    """Sorted (start, end, entry) ranges from each entry's own segments.

    A fileset kernelcache's top-level segments span every kext; only the
    entry's embedded Mach-O header says which part of them is its own.
    """
    ranges = []
    for name, off in entries:
        segs, _ = parse_header(data, off)
        for s in segs:
            if executable(s):
                ranges.append((s['vmaddr'], s['vmaddr'] + s['vmsize'], name))
    ranges.sort()
    return [r[0] for r in ranges], ranges


def owner_of(index, va):
    starts, ranges = index
    i = bisect.bisect_right(starts, va) - 1
    if i >= 0 and va < ranges[i][1]:
        return ranges[i][2]
    return '?'


# -- symbols -----------------------------------------------------------------

def load_symbols(path):
    """Text-symbol addresses and names from `nm -n`; empty when stripped."""
    try:
        proc = subprocess.run(['nm', '-n', str(path)], capture_output=True, text=True)
    except OSError:
        return [], []
    if proc.returncode != 0:
        return [], []
    addrs, names = [], []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ('t', 'T'):
            try:
                addrs.append(int(parts[0], 16))
            except ValueError:
                continue
            names.append(parts[2])
    return addrs, names


def symbol_at(syms, va, index=None):
    """The nearest text symbol at or below va as name+offset, or '-'.

    With a fileset's owner index only a symbol of the site's own entry
    counts: 26A428's com.apple.kernel carries no text symbols at all, and the
    nearest one below its code belongs to whichever kext sits under it.
    """
    addrs, names = syms
    i = bisect.bisect_right(addrs, va) - 1
    if i < 0 or (index and owner_of(index, addrs[i]) != owner_of(index, va)):
        return '-'
    return '%s+0x%x' % (names[i], va - addrs[i])


# -- scan --------------------------------------------------------------------

def scan_words(words, base_va, segment, emit):
    """Classify every word; emit(va, segment, kind, label, rw) per hit."""
    for i, word in enumerate(words):
        hit = classify(word)
        if hit:
            emit(base_va + 4 * i, segment, hit[0], hit[1], hit[2])


def scan_image(data):
    """All sites of an image as (va, segment, owner, kind, label, rw)."""
    segments, entries = parse_header(data)
    index = build_owner_index(data, entries) if entries else None
    sites = []

    def emit(va, segment, kind, label, rw):
        owner = owner_of(index, va) if index else 'kernel'
        sites.append((va, segment, owner, kind, label, rw))

    for s in segments:
        if not executable(s):
            continue
        n = min(s['filesize'], s['vmsize']) // 4
        words = struct.unpack_from('<%dI' % n, data, s['fileoff'])
        scan_words(words, s['vmaddr'], s['name'], emit)
    return segments, entries, sites


def report(path, segments, entries, sites, syms, index, top):
    print('Image: %s (%s)' % (path.name, 'MH_FILESET, %d entries' % len(entries) if entries else 'plain Mach-O'))
    for s in segments:
        if executable(s):
            print('  exec segment %-18s VA 0x%016x +0x%x' % (s['name'], s['vmaddr'], s['filesize']))
    print('All counts are upper bounds: a linear scan also matches data in text.')
    for kind, title in KINDS:
        hits = [s for s in sites if s[3] == kind]
        print('\n%s: %d' % (title, len(hits)))
        if not hits:
            continue
        by_label = Counter((s[4], s[5]) for s in hits)
        for (label, rw), n in by_label.most_common(top):
            print('   %-44s %-1s %6d' % (label, rw, n))
        if len(by_label) > top:
            print('   ... %d more distinct' % (len(by_label) - top))
        if entries:
            print('   by owner: ' + ', '.join('%s %d' % kv for kv in Counter(s[2] for s in hits).most_common(6)))
        if syms[0] and kind in ('currentel', 'eret', 'el2-sysop'):
            for fn, n in Counter(symbol_at(syms, s[0], index).split('+')[0] for s in hits).most_common(top):
                print('   in %-42s %6d' % (fn, n))


def write_sites(out, sites, syms, index):
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w') as fh:
        fh.write('va\tsegment\towner\tkind\tlabel\trw\tsymbol\n')
        for va, segment, owner, kind, label, rw in sites:
            fh.write('0x%x\t%s\t%s\t%s\t%s\t%s\t%s\n' % (
                va, segment, owner, kind, label, rw, symbol_at(syms, va, index) if syms[0] else '-'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', type=pathlib.Path, help='raw Mach-O kernel or kernelcache (not IM4P)')
    ap.add_argument('--sites', type=pathlib.Path, help='write every site as TSV (keep under build/)')
    ap.add_argument('--top', type=int, default=12, help='rows per class (default 12)')
    args = ap.parse_args()
    if not args.image.is_file():
        sys.exit('not a file: %s' % args.image)
    data = args.image.read_bytes()
    try:
        segments, entries, sites = scan_image(data)
    except (ValueError, struct.error) as e:
        sys.exit('%s: %s (IM4P input must be decompressed first)' % (args.image, e))
    syms = load_symbols(args.image)
    index = build_owner_index(data, entries) if entries else None
    report(args.image, segments, entries, sites, syms, index, args.top)
    if args.sites:
        write_sites(args.sites, sites, syms, index)


if __name__ == '__main__':
    main()
