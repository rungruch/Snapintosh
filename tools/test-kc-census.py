#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host test for tools/kc-census.py: hand-encoded words, no Apple data.

Every positive word is built by an encoder written here from the Arm ARM
field layout, not by the code under test, and a few are checked against
encodings assembled by LLVM. The near misses must not be counted: a census
that over-reports would silently inflate Route A's patch estimate.
"""
import importlib.util
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('kc_census', ROOT / 'tools' / 'kc-census.py')
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)

failures = []


def check(cond, what):
    print('%s  %s' % ('PASS' if cond else 'FAIL', what))
    if not cond:
        failures.append(what)


def sysword(l, op0, op1, crn, crm, op2, rt=0):
    """MSR/MRS register (op0 2/3) or SYS/SYSL (op0 1) per the Arm ARM."""
    return (0xD5000000 | (l << 21) | ((1 if op0 >= 2 else 0) << 20)
            | ((op0 & 1) << 19) | (op1 << 16) | (crn << 12) | (crm << 8)
            | (op2 << 5) | rt)


def hvc(imm):
    return 0xD4000002 | (imm << 5)


def smc(imm):
    return 0xD4000003 | (imm << 5)


# Anchors: encodings produced by `llvm-mc -triple=arm64`.
check(sysword(1, 3, 4, 1, 1, 0) == 0xD53C1100, 'encoder matches mrs x0, HCR_EL2')
check(sysword(0, 3, 4, 1, 1, 0) == 0xD51C1100, 'encoder matches msr HCR_EL2, x0')
check(sysword(1, 3, 0, 4, 2, 2) == 0xD5384240, 'encoder matches mrs x0, CurrentEL')
check(sysword(0, 1, 4, 8, 3, 0, 31) == 0xD50C831F, 'encoder matches tlbi alle2is')
check(sysword(0, 1, 0, 8, 3, 0, 31) == 0xD508831F, 'encoder matches tlbi vmalle1is')

c = census.classify

# -- positives ---------------------------------------------------------------
check(c(0xD53C1100) == ('el2-reg', 'HCR_EL2', 'R'), 'mrs HCR_EL2')
check(c(0xD51C1100) == ('el2-reg', 'HCR_EL2', 'W'), 'msr HCR_EL2')
check(c(sysword(0, 3, 4, 2, 2, 0, 5)) == ('el2-reg', 'VNCR_EL2', 'W'), 'msr VNCR_EL2')
check(c(sysword(1, 3, 4, 10, 2, 0)) == ('el2-reg', 'MAIR_EL2', 'R'), 'MAIR_EL2 is CRm 2')
check(c(sysword(1, 3, 4, 1, 2, 0)) == ('el2-reg', 'ZCR_EL2', 'R'), 'ZCR_EL2 is CRm 2 op2 0')
check(c(sysword(1, 3, 4, 4, 1, 0)) == ('el2-reg', 'SP_EL1', 'R'), 'op1 4 CRn 4 CRm 1 is SP_EL1')
check(c(sysword(1, 3, 4, 3, 7, 5)) == ('el2-reg', 's3_4_c3_c7_5', 'R'), 'unnamed EL2 register falls back to generic name')
check(c(sysword(1, 3, 5, 4, 0, 1)) == ('el12-reg', 'ELR_EL12', 'R'), 'mrs ELR_EL12')
check(c(sysword(0, 3, 5, 14, 3, 1)) == ('el12-reg', 'CNTV_CTL_EL02', 'W'), 'msr CNTV_CTL_EL02')
check(c(0xD50C831F) == ('el2-sysop', 'TLBI op1=4 c8 c3 #0', 'W'), 'tlbi alle2is is an EL2 system instruction')
check(c(sysword(0, 1, 4, 7, 8, 0, 3))[0:2] == ('el2-sysop', 'AT op1=4 c7 c8 #0'), 'at s1e2r')
check(c(0xD5384240) == ('currentel', 'CurrentEL', 'R'), 'mrs x0, CurrentEL')
check(c(0xD538424F)[0] == 'currentel', 'mrs x15, CurrentEL (any Rt)')
check(c(0xD69F03E0) == ('eret', 'eret', ''), 'eret')
check(c(0xD69F0BFF) == ('eret', 'eretaa', ''), 'eretaa')
check(c(0xD69F0FFF) == ('eret', 'eretab', ''), 'eretab')
check(c(hvc(0)) == ('hvc', 'hvc #0', ''), 'hvc #0')
check(c(hvc(0xFFFF)) == ('hvc', 'hvc #65535', ''), 'hvc #0xffff')
check(c(smc(0)) == ('smc', 'smc #0', ''), 'smc #0')
check(c(smc(1)) == ('smc', 'smc #1', ''), 'smc #1')
check(c(0x00201420)[0] == 'genter', 'genter')
check(c(0x00201400)[0] == 'gexit', 'gexit')
check(c(0xD4E00000)[0] == 'alt-call', 'alt-call')
check(c(0x00201080)[0] == 'apple-block', 'other 0x00201xxx word')
check(c(sysword(1, 3, 0, 15, 2, 7)) == ('impdef-reg', 's3_0_c15_c2_7', 'R'), 'CRn 15 op1 0 is impdef')
check(c(sysword(0, 3, 4, 15, 1, 0))[0] == 'impdef-reg', 'CRn 15 op1 4 is impdef, not el2-reg')
check(c(sysword(0, 3, 5, 11, 0, 0))[0] == 'impdef-reg', 'CRn 11 op1 5 is impdef, not el12-reg')
check(c(sysword(0, 1, 0, 15, 0, 0, 3))[0] == 'impdef-sys', 'SYS with CRn 15 is impdef')

# -- names anchored to LLVM --------------------------------------------------
# Registers a census first printed unnamed (docs/GAOKUN3-PHASE0B.md). Each
# word is `llvm-mc -triple=arm64 -mattr=+all -show-encoding` output (LLVM
# 23.1.4); the encoder must agree with it field by field and the census must
# give LLVM's name.
LLVM_NAMED = (
    (0xD53CCC00, (1, 3, 4, 12, 12, 0), 'el2-reg', 'ICH_LR0_EL2'),
    (0xD51CCCE0, (0, 3, 4, 12, 12, 7), 'el2-reg', 'ICH_LR7_EL2'),
    (0xD53CCD00, (1, 3, 4, 12, 13, 0), 'el2-reg', 'ICH_LR8_EL2'),
    (0xD51CCDE0, (0, 3, 4, 12, 13, 7), 'el2-reg', 'ICH_LR15_EL2'),
    (0xD51C1180, (0, 3, 4, 1, 1, 4), 'el2-reg', 'HFGRTR_EL2'),
    (0xD51C11A0, (0, 3, 4, 1, 1, 5), 'el2-reg', 'HFGWTR_EL2'),
    (0xD51C11C0, (0, 3, 4, 1, 1, 6), 'el2-reg', 'HFGITR_EL2'),
    (0xD51C3180, (0, 3, 4, 3, 1, 4), 'el2-reg', 'HDFGRTR_EL2'),
    (0xD51C31A0, (0, 3, 4, 3, 1, 5), 'el2-reg', 'HDFGWTR_EL2'),
    (0xD51C12A0, (0, 3, 4, 1, 2, 5), 'el2-reg', 'SMPRIMAP_EL2'),
    (0xD53D12C0, (1, 3, 5, 1, 2, 6), 'el12-reg', 'SMCR_EL12'),
    (0xD51DD0E0, (0, 3, 5, 13, 0, 7), 'el12-reg', 'SCXTNUM_EL12'),
    (0xD51D1020, (0, 3, 5, 1, 0, 1), 'el12-reg', 'ACTLR_EL12'),
)
for word, fields, kind, name in LLVM_NAMED:
    rw = 'R' if fields[0] else 'W'
    check(sysword(*fields) == word and c(word) == (kind, name, rw),
          '%s %s matches llvm-mc' % ('mrs' if fields[0] else 'msr', name))

# -- near misses: none may be counted ----------------------------------------
check(c(sysword(1, 3, 0, 1, 0, 0)) is None, 'mrs SCTLR_EL1 (op1 0) is not an EL2 access')
check(c(sysword(1, 3, 3, 13, 0, 2)) is None, 'mrs TPIDR_EL0 (op1 3) is not counted')
check(c(sysword(1, 3, 6, 12, 0, 0)) is None, 'op1 6 (EL3) is not counted')
check(c(sysword(1, 3, 7, 1, 0, 0)) is None, 'op1 7 is not counted')
check(c(0xD50343DF) is None, 'msr daifset, #3 (op0 0, op1 3) is not counted')
check(c(0xD504401F) is None, 'msr-immediate with op1 4 is not an EL2 register access')
check(c(0xD508831F) is None, 'tlbi vmalle1is (op1 0) is not an EL2 sysop')
check(c(0xD50B7B20) is None, 'dc cvac (op1 3) is not counted')
check(c(0xD503201F) is None, 'nop')
check(c(0xD4000001) is None, 'svc #0 is neither hvc nor smc')
check(c(0xD4000004) is None, 'op bits 4 are not hvc/smc')
check(c(0xD4200000) is None, 'brk #0')
check(c(0xD69F03E1) is None, 'eret with a nonzero Rn field is not eret')
check(c(0xD65F03C0) is None, 'ret')
check(c(0xD53842A0) is None, 'CurrentEL encoding with op2 5 is a different register')
check(c(0x00200820) is None, '0x00200820 is outside the 0x00201xxx block')
check(c(0x00202000) is None, 'first word past the Apple block')
check(c(0xD4E00001) is None, 'alt-call with a low bit set')
check(c(0xFFFFFFFF) is None and c(0) is None, 'all-ones and zero')

# -- images ------------------------------------------------------------------


def segment_cmd(name, vmaddr, vmsize, fileoff, filesize, prot):
    return struct.pack('<II16sQQQQiiII', 0x19, 72, name.encode(), vmaddr,
                       vmsize, fileoff, filesize, prot, prot, 0, 0)


def header(ncmds, cmds_size):
    return struct.pack('<IiiIIIII', 0xFEEDFACF, 0x0100000C, 0, 2, ncmds, cmds_size, 0, 0)


def plain_image(words):
    body = struct.pack('<%dI' % len(words), *words)
    cmds = (segment_cmd('__TEXT_EXEC', 0xFFFFFE0007000000, len(body), 0x1000, len(body), 5)
            + segment_cmd('__DATA', 0xFFFFFE0008000000, 0x4000, 0x2000, 0x10, 3))
    img = (header(2, len(cmds)) + cmds).ljust(0x1000, b'\0') + body
    # A data segment must not be scanned even when it holds an eret word.
    return img.ljust(0x2000, b'\0') + struct.pack('<IIII', 0xD69F03E0, 0, 0, 0)


words = [0xD53C1100, 0xD51C1100, 0xD69F03E0, 0xD5384240, hvc(0), smc(0),
         0x00201420, 0xD4E00000, 0xD5384240, 0xD503201F]
_, entries, sites = census.scan_image(plain_image(words))
kinds = [s[3] for s in sites]
check(entries == [] and len(sites) == 9, 'plain image: nine hits, nop ignored (got %d)' % len(sites))
check(kinds.count('currentel') == 2 and kinds.count('eret') == 1, 'plain image: counts per kind')
check(all(s[1] == '__TEXT_EXEC' for s in sites), 'plain image: data segment not scanned')
check(sites[0][0] == 0xFFFFFE0007000000 and sites[2][0] == 0xFFFFFE0007000008, 'plain image: site VAs')
check(all(s[2] == 'kernel' for s in sites), 'plain image: owner is kernel')


def fileset_image():
    # Two entries share one top-level __TEXT_EXEC; each embedded header claims
    # its own half. An eret in each half must be attributed to its own entry.
    va, base = 0xFFFFFE0007000000, 0x3000

    def entry_hdr(start):
        cmd = segment_cmd('__TEXT_EXEC', va + start, 16, base + start, 16, 5)
        return header(1, len(cmd)) + cmd

    def fse(name, fileoff):
        # LC_FILESET_ENTRY: cmd, cmdsize, vmaddr, fileoff, entry_id offset, reserved.
        raw = name.encode() + b'\0'
        size = (32 + len(raw) + 7) & ~7
        return struct.pack('<IIQQII', 0x80000035, size, va, fileoff, 32, 0) + raw.ljust(size - 32, b'\0')

    cmds = (segment_cmd('__TEXT_EXEC', va, 32, base, 32, 5)
            + fse('com.example.a', 0x1000) + fse('com.example.b', 0x2000))
    img = bytearray(0x4000)
    top = header(3, len(cmds)) + cmds
    img[0:len(top)] = top
    img[0x1000:0x1000 + len(entry_hdr(0))] = entry_hdr(0)
    img[0x2000:0x2000 + len(entry_hdr(16))] = entry_hdr(16)
    full = struct.pack('<8I', 0xD69F03E0, 0xD503201F, 0xD503201F, 0xD503201F,
                       0xD503201F, 0xD503201F, 0xD69F0BFF, hvc(7))
    img[base:base + len(full)] = full
    return bytes(img)


_, entries, sites = census.scan_image(fileset_image())
owners = {s[3] + ':' + s[4]: s[2] for s in sites}
check(len(entries) == 2, 'fileset: two entries found')
check(owners.get('eret:eret') == 'com.example.a', 'fileset: first-half eret belongs to entry a')
check(owners.get('eret:eretaa') == 'com.example.b', 'fileset: second-half eretaa belongs to entry b')
check(owners.get('hvc:hvc #7') == 'com.example.b', 'fileset: hvc immediate recovered and attributed')

# A truncated or non-Mach-O input must be refused rather than counted.
for bad, what in ((b'\0' * 64, 'zeros'), (b'\xcf\xfa\xed\xfe', 'header cut short')):
    try:
        census.scan_image(bad)
        check(False, 'refuses %s' % what)
    except (ValueError, struct.error):
        check(True, 'refuses %s' % what)

if failures:
    print('\nFAIL: %d check(s)' % len(failures))
    sys.exit(1)
print('\nPASS')
