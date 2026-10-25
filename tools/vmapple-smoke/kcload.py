# SPDX-License-Identifier: MIT
"""Lay out a VirtualMac2,1 guest for the Route B QEMU smoke test.

Builds, from a locally obtained kernelcache (never committed) and the
vma2macosap device tree template (also never committed), the image the EL2
stub drops into:

  - the kernelcache's segments, laid out linearly at PA = vmaddr - mhVA +
    GUEST_RAM_BASE, where mhVA is the lowest segment address. XNU's classic
    entry (osfmk/arm64/start.s) locates the mach header with a PC-relative
    adrp, which with the MMU off yields the physical load address, so this
    linear placement is the contract; boot_args.virtBase is the VA twin of
    physBase, and KVA = PA - physBase + virtBase.
  - the template device tree, completed the way the VM boot firmware would:
    placeholders resolved, /chosen/dram-base and dram-size added (arm_init
    panics without them), /chosen/random-seed filled with entropy, the
    counter rate filled in, and the arm-io ranges,
    PL011 and GICv3 register windows moved from the VirtualMac platform map
    onto QEMU virt's (UART at 0x09000000, GICD at 0x08000000, GICR at
    0x080A0000). The rest of the device map is left as the template has it;
    the drivers for it are kext-stage and their probe results are part of
    what the smoke test observes.
  - a boot_args (revision 1, version 2, the 0x480-byte arm64 structure from
    pexpert/arm64/boot.h) above the kernel image, with the device tree, and
    topOfKernelData just past both, so XNU treats [kernel end .. top) as
    loader data and everything above it as available memory.

Only facts from the published structure layouts and from XNU's documented
entry contract are used; this is an independent implementation for the
gaokun3 port's Phase 0C smoke test, not a copy of any other loader.
"""
import json
import os
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'probes'))
import appledt                                                  # noqa: E402

# The Mach header VA's 32 MiB block offset is 0x1004000. start.s initially
# maps in 32 MiB blocks, so physBase needs the same full offset, not merely the
# 16 KiB page offset, or a KVA points into __LINKEDIT rather than __TEXT_EXEC.
GUEST_RAM_BASE = 0x45004000
GUEST_MEM_SIZE = 0x1C000000             # 448 MiB; QEMU gets 576 MiB
DT_PA = 0x4A000000
BOOT_ARGS_PA = 0x4A010000
TOP_OF_KERNEL_DATA = 0x4A020000         # 16 KiB aligned, above all loader data

# QEMU virt's device map, as offsets under arm-io with an identity range
# whose parent base is 0x08000000 (the GIC distributor base).
ARMIO_PARENT_BASE = 0x08000000
QEMU_UART_OFF = 0x01000000              # PL011 at 0x09000000
QEMU_GICD_OFF = 0x00000000              # GICD at 0x08000000
QEMU_GICR_OFF = 0x000A0000              # GICR at 0x080A0000
QEMU_GICR_SIZE = 0x100000               # 8 PEs x 0x20000

# The counter rate of QEMU TCG's -cpu max on the host this harness was
# validated on. The stub prints CNTFRQ_EL0 before entering the guest and the
# harness warns if it differs; an inaccurate timebase scales delays but does
# not stop early boot.
TIMEBASE_HZ = 1_000_000_000

# `-v` keeps ordinary boot messages visible; serial=3 selects the PL011 output
# console. `debug=0x8` is not needed for this check.
# `-noprogress`: the smoke boot_args describe no framebuffer, and with a zero
# display size PE_init_iokit's progress-image centring loop
# (pexpert/arm/pe_init.c, "*delta -= 50 * scale" until the image fits half
# the display) never ends on a release kernel. The real loader passes the
# simple-framebuffer instead.
CMDLINE = '-v serial=3 -noprogress'
BOOT_ARGS_SIZE = 0x480

# XNU's early PRNG asks the loader for 4 * SHA-512 = 256 bytes on arm64
# (osfmk/prng/prng_random.c, BOOTLOADER_ENTROPY_REQUEST_SIZE) and reads them
# from /chosen/random-seed on non-SPTM kernels (pexpert/gen/pe_gen.c). The
# VMAPPLE template carries that property as 256 zero bytes for the VM
# firmware to fill; XNU counts an all-zero seed as no seed and panics
# "Expected 256 seed bytes from bootloader, but got 0".
RANDOM_SEED_BYTES = 256


def _segments(kc):
    """(vmaddr, file offset, file size) for each LC_SEGMENT_64, and the entry
    virtual address, from the kernelcache's main Mach-O header."""
    data = kc.read_bytes()
    magic, ncmds = struct.unpack_from('<I', data, 0)[0], struct.unpack_from('<I', data, 16)[0]
    if magic != 0xFEEDFACF:
        raise SystemExit(f'{kc}: not a 64-bit Mach-O (kernelcache expected)')
    segs, entry, p = [], None, 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, p)
        if cmd == 0x19:                                     # LC_SEGMENT_64
            va, vs, fo, fs = struct.unpack_from('<QQQQ', data, p + 24)
            segs.append((va, fo, fs, vs))
        elif cmd == 5:                                      # LC_UNIXTHREAD
            entry = struct.unpack_from('<Q', data, p + 16 + 32 * 8)[0]
        p += size
    if not segs or entry is None:
        raise SystemExit(f'{kc}: no segments or no LC_UNIXTHREAD entry')
    return data, segs, entry


def _complete_dt_tree(dt, out=None, entropy=os.urandom):
    """Complete an already-parsed VMAPPLE ADT for QEMU. Kept separate from the
    file wrapper so tools/test-vmapple-smoke.py can exercise it with a wholly
    synthetic tree, never Apple material. When `out` is provided, save the
    generated tree there for the host-only smoke run. `entropy(n)` supplies
    the boot seed; the seeded tree is written to `out` only, never committed."""
    resolved = dt.resolve_placeholders()

    seed = dt.get('/device-tree/chosen', 'random-seed')
    if seed is None or len(seed) < RANDOM_SEED_BYTES:
        raise SystemExit(f'device tree: /chosen/random-seed has '
                         f'{"no" if seed is None else len(seed)} bytes, XNU needs {RANDOM_SEED_BYTES}')
    fill = entropy(len(seed))
    if len(fill) != len(seed) or not any(fill):
        raise SystemExit('device tree: the entropy source returned no usable random-seed')
    dt.set('/device-tree/chosen', 'random-seed', fill)

    dt.add('/device-tree/chosen', 'dram-base', struct.pack('<Q', GUEST_RAM_BASE))
    dt.add('/device-tree/chosen', 'dram-size', struct.pack('<Q', GUEST_MEM_SIZE))
    dt.set_u32('/device-tree', 'clock-frequency', TIMEBASE_HZ)
    changed = dt.set_all('timebase-frequency', struct.pack('<Q', TIMEBASE_HZ), under='/device-tree/cpus')
    if not changed:
        raise SystemExit('device tree: no cpu timebase-frequency properties found')

    armio = '/device-tree/arm-io'
    dt.set(armio, 'ranges', struct.pack('<QQQ', 0, ARMIO_PARENT_BASE, 0x40000000))
    dt.set(armio + '/uart0', 'reg', struct.pack('<QQ', QEMU_UART_OFF, 0x1000))
    gic = dt.get(armio + '/gic', 'reg')
    if len(gic) != 48:
        raise SystemExit(f'device tree: gic reg has {len(gic)} bytes, expected 48')
    dt.set(armio + '/gic', 'reg',
           struct.pack('<QQQQ', QEMU_GICD_OFF, 0x10000, QEMU_GICR_OFF, QEMU_GICR_SIZE) + gic[32:])

    blob = dt.serialize()
    if out is not None:
        (out / 'devicetree.vma2smoke').write_bytes(blob)
    return blob, resolved


def _complete_dt(dt_path, out):
    """Read the local VMAPPLE template and complete it for QEMU."""
    return _complete_dt_tree(appledt.DeviceTree(pathlib.Path(dt_path).read_bytes()), out)


def _boot_args(virt_base, dt_va, dt_len):
    b = bytearray(BOOT_ARGS_SIZE)
    struct.pack_into('<HH', b, 0, 1, 2)                    # Revision, Version
    struct.pack_into('<QQQQ', b, 8, virt_base, GUEST_RAM_BASE,
                     GUEST_MEM_SIZE, TOP_OF_KERNEL_DATA)
    # Boot_Video at 0x28 left zero: no framebuffer; the serial line is the
    # console for this test.
    struct.pack_into('<I', b, 0x58, 0)                    # machineType
    struct.pack_into('<Q', b, 0x60, dt_va)
    struct.pack_into('<I', b, 0x68, dt_len)
    line = CMDLINE.encode()
    b[0x6c:0x6c + len(line)] = line
    struct.pack_into('<Q', b, 0x478, GUEST_MEM_SIZE)      # memSizeActual
    return bytes(b)


def lay_out(kc, out):
    """Build the guest image. Returns (payload path, payload PA, entry PA,
    boot_args PA)."""
    out.mkdir(parents=True, exist_ok=True)
    data, segs, entry_va = _segments(pathlib.Path(kc))
    mh_va = min(va for va, _, _, _ in segs)
    span = max(va + vs for va, _, _, vs in segs) - mh_va
    if GUEST_RAM_BASE + span > DT_PA:
        raise SystemExit(f'kernelcache spans {span:#x}; does not fit below {DT_PA:#x}')

    dt_path = ROOT / 'downloads' / 'extracted' / 'vma2macosap' / 'DeviceTree.vma2macosap'
    if not dt_path.exists():
        raise SystemExit(f'no device tree template at {dt_path}')
    dt_blob, resolved = _complete_dt(dt_path, out)
    dt_va = DT_PA - GUEST_RAM_BASE + mh_va                # KVA in the physmap

    payload = bytearray(TOP_OF_KERNEL_DATA - GUEST_RAM_BASE)
    for va, fo, fs, _vs in segs:
        payload[va - mh_va:va - mh_va + fs] = data[fo:fo + fs]
    payload[DT_PA - GUEST_RAM_BASE:DT_PA - GUEST_RAM_BASE + len(dt_blob)] = dt_blob
    args = _boot_args(mh_va, dt_va, len(dt_blob))
    payload[BOOT_ARGS_PA - GUEST_RAM_BASE:BOOT_ARGS_PA - GUEST_RAM_BASE + len(args)] = args

    blob_path = out / 'vmapple-guest.bin'
    blob_path.write_bytes(payload)
    entry_pa = entry_va - mh_va + GUEST_RAM_BASE

    layout = {
        'mh_va': f'{mh_va:#x}', 'entry_va': f'{entry_va:#x}', 'entry_pa': f'{entry_pa:#x}',
        'phys_base': f'{GUEST_RAM_BASE:#x}', 'mem_size': f'{GUEST_MEM_SIZE:#x}',
        'dt_pa': f'{DT_PA:#x}', 'dt_va': f'{dt_va:#x}', 'dt_len': len(dt_blob),
        'boot_args_pa': f'{BOOT_ARGS_PA:#x}', 'top_of_kernel_data': f'{TOP_OF_KERNEL_DATA:#x}',
        'placeholders_resolved': resolved, 'timebase_hz': TIMEBASE_HZ,
        'cmdline': CMDLINE,
    }
    (out / 'layout.json').write_text(json.dumps(layout, indent=2))
    return blob_path, GUEST_RAM_BASE, entry_pa, BOOT_ARGS_PA


# What progress looks like on the serial line, earliest first. XNU's first
# sign of life is the PAC SET_INITIAL_STATE hypercall from common_start,
# issued before the guest MMU is on; the stub logs it when --trace is given.
# kprintf output before the serial console switch goes to the early buffer
# only, so the first UART-visible kernel line is the platform chatter or the
# banner.
MILESTONES = [
    ('stub entered', 'EL2: entering guest at EL1'),
    ('counter rate printed', 'EL2: cntfrq'),
    ('guest reached common_start (PAC hvc)', 'EL2: hvc 00000000c1000000'),
    ('kernel kprintf on the UART', 'Kernel'),
    ('kernel banner', 'Darwin Kernel Version'),
]

_PC_RE = None


def monitor_pc(monitor_text):
    """The guest's PC from `info registers` output, or None. The aarch64
    human monitor prints R15=... for the PC."""
    import re
    for pat in (r'R15=([0-9a-fA-F]+)', r'PC=([0-9a-fA-Fx]+)', r'pc\s+([0-9a-fA-Fx]+)'):
        m = re.search(pat, monitor_text)
        if m:
            try:
                return int(m.group(1), 16)
            except ValueError:
                pass
    return None


def disasm_around(kc, addr):
    """Print the instructions around addr, which may be a KVA (inside the
    kernel image, slide 0) or a physical address (V=P land). Maps back into
    the kernelcache and disassembles with llvm-mc; diagnostics only, output
    goes to the log, never committed."""
    import subprocess
    data, segs, _ = _segments(pathlib.Path(kc))
    mh_va = min(va for va, _, _, _ in segs)
    foff = None
    for va, fo, fs, vs in segs:
        if va <= addr < va + vs and addr - va < fs:
            foff = fo + (addr - va)
            break
    if foff is None and GUEST_RAM_BASE <= addr < TOP_OF_KERNEL_DATA:
        foff = addr - GUEST_RAM_BASE
        for va, fo, fs, _ in segs:
            if va - mh_va <= foff < va - mh_va + fs:
                foff = fo + (foff - (va - mh_va))
                break
        else:
            foff = None
    if foff is None:
        print(f'  (address {addr:#x} is not in the kernel image)')
        return
    start = max(foff - 32, 0) & ~3
    words = struct.unpack_from('<12I', data, start)
    src = '\n'.join(' '.join(f'0x{b:02x}' for b in struct.pack('<I', w)) for w in words)
    llvm = ROOT / 'build' / 'vmapple-smoke'
    mc = 'llvm-mc'
    for cand in ('/opt/homebrew/opt/llvm/bin/llvm-mc',):
        if pathlib.Path(cand).exists():
            mc = cand
    out = subprocess.run([mc, '--disassemble', '-triple=arm64-apple-macos', '-mattr=+all'],
                         input=src, capture_output=True, text=True)
    lines = [l for l in out.stdout.splitlines() if l.strip()]
    for i, l in enumerate(lines[:12]):
        mark = '>' if start + 4 * i == foff else ' '
        print(f'  {mark} {l.strip()}')


def report(text, out):
    """Summarize a boot run's serial log against the milestones."""
    problems = []
    m = None
    for line in text.splitlines():
        if line.startswith('EL2: cntfrq'):
            try:
                hz = int(line.split()[-1], 16)
            except ValueError:
                hz = -1
            if hz != TIMEBASE_HZ:
                print(f'WARN: guest CNTFRQ is {hz} Hz, the device tree says {TIMEBASE_HZ}; '
                      'delays are mis-scaled but boot continues')
            m = line
    reached = [name for name, marker in MILESTONES if marker in text]
    for name, marker in MILESTONES:
        print(('  ok  ' if name in reached else '  --  ') + name)
    if 'EL2: unexpected exception' in text:
        problems.append('the stub recorded an unexpected EL2 exception (esr/elr in the log above)')
    if 'panic' in text:
        for line in text.splitlines():
            if 'panic' in line:
                print('guest panic line:', line[:160])
                break
        problems.append('the guest panicked')
    if 'MK: FAIL' in text or 'EL2: bad params' in text:
        problems.append('stub-side failure')
    if len(reached) < len(MILESTONES):
        first_missing = next(name for name, _ in MILESTONES if name not in reached)
        problems.append(f'boot stopped before: {first_missing}')
    if problems:
        print('RESULT: NOT RUN as a pass -- ' + '; '.join(problems))
        return 1
    print('RESULT: PASS (early boot reached the kernel banner under the EL2 stub)')
    return 0


# ---- B5: implementation-defined registers the guest actually touches -----
#
# HCR_EL2.TIDCP traps EL1 accesses to the CRn 11 and 15 system-register space
# to EL2. The smoke stub RAZ/WIs them and records each one; these helpers turn
# that record into names. The ISS layout for EC 0x18 (trapped MSR/MRS) is from
# the Arm ARM: bit 0 direction (0 write, 1 read), 4:1 CRm, 9:5 Rt, 13:10 CRn,
# 16:14 Op1, 19:17 Op2, 21:20 Op0.

# The two encodings docs/GAOKUN3-PHASE0B.md (B5) records the monitor as already
# knowing, so a run can separate "new to the monitor" from "already handled".
KNOWN_IMPDEF = {
    's3_4_c15_c1_3': 'AMX_STATE_T_EL1 (compat/fast-trap.h)',
    's3_4_c15_c1_4': 'answered 0 (probes/sysregzero.py)',
}

TIDCP_LINE = 'EL2: tidcp '


def decode_tidcp(esr):
    """(name, direction, rt) for a TIDCP-trapped access from its ESR or key."""
    iss = esr & 0x3fffff
    crm = (iss >> 1) & 0xf
    rt = (iss >> 5) & 0x1f
    crn = (iss >> 10) & 0xf
    op1 = (iss >> 14) & 0x7
    op2 = (iss >> 17) & 0x7
    op0 = (iss >> 20) & 0x3
    return f's{op0}_{op1}_c{crn}_c{crm}_{op2}', 'read' if iss & 1 else 'write', rt


def tidcp_from_serial(text):
    """(name, direction) pairs in the order the stub first saw them."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(TIDCP_LINE):
            continue
        try:
            esr = int(line[len(TIDCP_LINE):].split()[0], 16)
        except (ValueError, IndexError):
            continue
        name, direction, _rt = decode_tidcp(esr)
        if (name, direction) not in out:
            out.append((name, direction))
    return out


def parse_monitor_words(monitor_text, base):
    """The 64-bit words QEMU's `xp` printed, keyed by physical address. The
    human monitor echoes typed characters, so only address-prefixed lines are
    read, and each is `<addr>: 0x.. 0x..`."""
    words = {}
    for line in monitor_text.splitlines():
        line = line.strip()
        if ':' not in line:
            continue
        addr, _, rest = line.partition(':')
        addr = addr.strip()
        if not addr or any(c not in '0123456789abcdefABCDEF' for c in addr):
            continue
        values = [v for v in rest.split() if v.startswith('0x')]
        if not values:
            continue
        try:
            at = int(addr, 16)
        except ValueError:
            continue
        for i, v in enumerate(values):
            try:
                words[at + 8 * i] = int(v, 16)
            except ValueError:
                pass
    return words


TIDCP_LOG_PA = 0x43002000
TIDCP_LOG_MAGIC = 0x54494443504C4F47
TIDCP_MAX = 64


def tidcp_log(monitor_text):
    """(totals, entries) from the stub's log, or (None, []) if it is absent.
    entries are (name, direction, count, first_elr), busiest first."""
    words = parse_monitor_words(monitor_text, TIDCP_LOG_PA)
    if words.get(TIDCP_LOG_PA) != TIDCP_LOG_MAGIC:
        return None, []
    used = min(words.get(TIDCP_LOG_PA + 0x08, 0), TIDCP_MAX)
    totals = {'used': used,
              'total': words.get(TIDCP_LOG_PA + 0x10, 0),
              'dropped': words.get(TIDCP_LOG_PA + 0x18, 0)}
    entries = []
    for i in range(used):
        at = TIDCP_LOG_PA + 0x20 + i * 32
        key = words.get(at)
        if key is None:
            continue
        name, direction, _rt = decode_tidcp(key)
        entries.append((name, direction, words.get(at + 8, 0), words.get(at + 16, 0)))
    entries.sort(key=lambda e: -e[2])
    return totals, entries


def report_tidcp(text, monitor_text, out=None):
    """Print the run's implementation-defined register set, and save it."""
    seen = tidcp_from_serial(text)
    totals, entries = tidcp_log(monitor_text or '')
    print('--- implementation-defined registers trapped at EL2 (B5) ---')
    if totals is None and not seen:
        # Without the log we cannot tell "no trap" from "not read back", and
        # the difference decides whether this run says anything about B5.
        print('  UNKNOWN: the stub\'s log was not read back, so this run'
              ' measured nothing')
        return
    if totals is not None:
        print(f"  {totals['used']} distinct, {totals['total']} accesses"
              + (f", {totals['dropped']} past the table" if totals['dropped'] else ''))
        if not totals['total']:
            print('  none: the guest reached its stopping point without one,'
                  ' so the stub\'s RAZ/WI never ran')
            return
    elif not entries:
        print(f'  {len(seen)} seen on the serial line; the log was not read back')
    rows = entries or [(n, d, 0, 0) for n, d in seen]
    for name, direction, count, elr in rows:
        note = KNOWN_IMPDEF.get(name, 'NEW to the monitor')
        where = f'  first at {elr:#x}' if elr else ''
        tally = f'{count:>7}' if count else '      ?'
        print(f'  {tally}  {name:<18} {direction:<5}  {note}{where}')
    if out is not None:
        lines = [f'{n}\t{d}\t{c}\t{e:#x}' for n, d, c, e in rows]
        (out / 'tidcp.tsv').write_text('\n'.join(lines) + '\n')
