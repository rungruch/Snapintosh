# SPDX-License-Identifier: MIT
"""Smoke-harness view of the classic VirtualMac loader.

The loader itself lives in probes/xnuboot.py (build_classic_image and its
ClassicLayout; docs/GAOKUN3-PHASE3.md P2): there is one classic loader, and
this module is the QEMU smoke's adapter over it. It re-exports the QEMU
layout's constants and the loader entry points under the names the smoke
and its host test have always used (raising SystemExit, as a command-line
loader should), and keeps the smoke-only run analysis below: milestones,
the TIDCP record and the MMIO-trace report.

Only facts from the published structure layouts and from XNU's documented
entry contract are used; this is an independent implementation for the
gaokun3 port, not a copy of any other loader.
"""
import json
import os
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'probes'))
import appledt                                                  # noqa: E402
import classicload                                              # noqa: E402

_LAYOUT = classicload.QEMU_CLASSIC_LAYOUT

# The Mach header VA's 32 MiB block offset is 0x1004000. start.s initially
# maps in 32 MiB blocks, so physBase needs the same full offset, not merely
# the 16 KiB page offset, or a KVA points into __LINKEDIT rather than
# __TEXT_EXEC.
GUEST_RAM_BASE = _LAYOUT.ram_base
GUEST_MEM_SIZE = _LAYOUT.mem_size              # 448 MiB; QEMU gets 576 MiB
DT_PA = _LAYOUT.dt_pa                          # the NVRAM bank alone is 64 KiB
BOOT_ARGS_PA = _LAYOUT.boot_args_pa
TOP_OF_KERNEL_DATA = _LAYOUT.top_of_kernel_data
PANIC_PA = _LAYOUT.panic_pa
PANIC_SIZE = _LAYOUT.panic_size
PANIC_LOG_SIZE = _LAYOUT.panic_log_size

# QEMU virt's device map, as offsets under arm-io with an identity range
# whose parent base is 0x08000000 (the GIC distributor base).
ARMIO_PARENT_BASE = _LAYOUT.armio_parent_base
QEMU_UART_OFF = _LAYOUT.uart_off               # PL011 at 0x09000000
QEMU_GICD_OFF = _LAYOUT.gicd_off               # GICD at 0x08000000
QEMU_GICR_OFF = _LAYOUT.gicr_off               # GICR at 0x080A0000

# The vCPUs QEMU runs (smoke.py passes this as -smp). The VMAPPLE template
# lists 32 CPU nodes, cpu0 "running" and the rest "waiting"; left at 32,
# AppleARMGICv3 panics on the redistributor count.
GUEST_CPUS = _LAYOUT.guest_cpus
GICR_FRAME_SIZE = 0x20000               # RD_base + SGI_base, 64 KiB each (GICv3)
QEMU_GICR_SIZE = _LAYOUT.gicr_size

# The counter rate of QEMU TCG's -cpu max on the host this harness was
# validated on. The stub prints CNTFRQ_EL0 before entering the guest and
# the harness warns if it differs.
TIMEBASE_HZ = _LAYOUT.timebase_hz

CMDLINE = _LAYOUT.cmdline
BOOT_ARGS_SIZE = classicload.BOOT_ARGS_SIZE
RANDOM_SEED_BYTES = classicload.CLASSIC_RANDOM_SEED_BYTES
NVRAM_BANK_SIZE = _LAYOUT.nvram_bank_size
NVRAM_SYSTEM_SIZE = _LAYOUT.nvram_system_size
NVRAM_HEADER_SIG = classicload.CLASSIC_NVRAM_HEADER_SIG
NVRAM_PARTITION_SIG = classicload.CLASSIC_NVRAM_PARTITION_SIG
CHRP_BLOCK = classicload.CLASSIC_CHRP_BLOCK


def _refuse(fn, *args, **kwargs):
    """Run a classic-loader function, refusing the way a loader CLI does."""
    try:
        return fn(*args, **kwargs)
    except ValueError as e:
        raise SystemExit(str(e)) from None


def _chrp_header(sig, length, name):
    return _refuse(classicload._classic_chrp_header, sig, length, name)


def nvram_image(bank_size=NVRAM_BANK_SIZE, system_size=NVRAM_SYSTEM_SIZE):
    return classicload.classic_nvram_image(bank_size, system_size)


def _segments(kc):
    """(data, segments, entry VA) for the kernelcache at path `kc`."""
    data = pathlib.Path(kc).read_bytes()
    try:
        segs, entry = classicload.classic_segments(data)
    except ValueError as e:
        raise SystemExit(f'{kc}: {e}') from None
    return data, segs, entry


def _trim_cpus(dt, count):
    return _refuse(classicload.classic_trim_cpus, dt, count)


def _complete_dt_tree(dt, out=None, entropy=os.urandom):
    """Complete an already-parsed VMAPPLE ADT for QEMU. Kept separate from
    the file wrapper so tools/test-vmapple-smoke.py can exercise it with a
    wholly synthetic tree, never Apple material."""
    blob, resolved = _refuse(classicload.classic_complete_dt, dt, _LAYOUT, entropy)
    if out is not None:
        (out / 'devicetree.vma2smoke').write_bytes(blob)
    return blob, resolved


def _complete_dt(dt_path, out):
    """Read the local VMAPPLE template and complete it for QEMU."""
    return _complete_dt_tree(appledt.DeviceTree(pathlib.Path(dt_path).read_bytes()), out)


def _boot_args(virt_base, dt_va, dt_len):
    return bytes(classicload.boot_args(virt_base, GUEST_RAM_BASE, GUEST_MEM_SIZE,
                                       TOP_OF_KERNEL_DATA, dt_va, dt_len, CMDLINE))


MONITOR_LAYOUT = classicload.QEMU_MONITOR_LAYOUT


def lay_out(kc, out, layout=None):
    """Build the guest image. Returns (payload path, payload PA, entry PA,
    boot_args PA). `layout` defaults to the Phase 0C QEMU-device layout;
    the Phase 3 monitor layout keeps the template's own device map."""
    out.mkdir(parents=True, exist_ok=True)
    dt_path = ROOT / 'downloads' / 'extracted' / 'vma2macosap' / 'DeviceTree.vma2macosap'
    if not dt_path.exists():
        raise SystemExit(f'no device tree template at {dt_path}')
    image = _refuse(classicload.build_classic_image, kc, dt_path, layout or _LAYOUT)
    blob_path = classicload.write_classic_image(image, out)
    return blob_path, image.payload_pa, image.entry_pa, image.boot_args_pa


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


# ---- gaokun3 Phase 3 step P1: the device windows the guest touches -------
#
# _complete_dt_tree moves arm-io's ranges so the PL011 and the GICv3 land on
# QEMU virt's own. Every other template window moves with them, onto
# whatever QEMU has at the moved address. With QEMU 11.1.2 that is mostly
# gpex's PCIe MMIO window, which answered every read with all-ones in the
# 2026-10-27 trace, so a boot that gets past those devices has not shown
# what they need. On the gaokun3 there is no such window: stage 2 is
# default-deny, and a touch stops the run. This report turns QEMU's MMIO
# trace into the list of windows touched, in first-touch order, with the
# QEMU region that answered each.

# The windows the smoke places on QEMU's model of the same device on
# purpose, as (node, reg index) -> QEMU region-name prefix.
MODELLED_BY_QEMU = {
    ('uart0', 0): 'pl011',
    ('gic', 0): 'gicv3_dist',
    ('gic', 1): 'gicv3_redist',
}

# QEMU's log trace backend. The report needs the absolute physical address
# in `addr`, which QEMU 11.1.2 prints: the PL011's data register traces as
# addr 0x9000000. A QEMU that printed region offsets instead would show its
# accesses as "no tree node" rather than hide them.
_MMIO_TRACE_RE = re.compile(
    r"memory_region_ops_(read|write) cpu \d+ mr 0x[0-9a-f]+ addr (0x[0-9a-f]+) "
    r"value (0x[0-9a-f]+) size (\d+) name '([^']*)'")


def parse_mmio_trace(text):
    """[(direction, pa, value, size, region)] from QEMU's
    memory_region_ops_read/write trace lines, in order. Other lines,
    including QEMU's own log messages, are skipped."""
    out = []
    for line in text.splitlines():
        m = _MMIO_TRACE_RE.search(line)
        if m:
            out.append((m[1], int(m[2], 16), int(m[3], 16), int(m[4]), m[5]))
    return out


def mmio_windows(dt):
    """[(pa, size, node, index)] for each register window under /arm-io,
    translated through arm-io's single ranges entry."""
    armio = '/device-tree/arm-io'
    ranges = dt.get(armio, 'ranges')
    if ranges is None or len(ranges) != 24:
        raise SystemExit('device tree: arm-io ranges is not one (child, parent, size) entry')
    child, parent, span = struct.unpack('<QQQ', ranges)
    out = []
    for path in dt.children.get(armio, []):
        reg = dt.get(path, 'reg') or b''
        name = path.rsplit('/', 1)[1]
        for i in range(len(reg) // 16):
            off, size = struct.unpack_from('<QQ', reg, 16 * i)
            if not child <= off < child + span:
                raise SystemExit(f'device tree: {name} reg[{i}] {off:#x} is outside arm-io ranges')
            out.append((parent + off - child, size, name, i))
    return out


def _all_ones(value, size):
    # QEMU traces a 4-byte all-ones read as a 64-bit ~0.
    return value in ((1 << (8 * size)) - 1, (1 << 64) - 1)


def report_mmio(trace_text, dt_blob, out=None):
    """Print each device window the guest touched, first touch first, and
    whether QEMU's model of that device answered or something else did.
    Accesses outside every tree window are grouped by 4 KiB page as "no
    tree node": the guest found that address somewhere other than the tree.
    Returns the rows, or None when the trace held no access at all."""
    accesses = parse_mmio_trace(trace_text)
    print('--- device windows the guest touched (QEMU MMIO trace, Phase 3 P1) ---')
    if not accesses:
        print('  UNKNOWN: no trace lines were read, so this run measured nothing')
        return None
    windows = mmio_windows(appledt.DeviceTree(dt_blob))
    touched = {}
    for n, (direction, pa, value, size, region) in enumerate(accesses):
        hit = next((w for w in windows if w[0] <= pa < w[0] + w[1]), None)
        if hit:
            key, label, offset = (hit[2], hit[3]), f'{hit[2]}[{hit[3]}]', pa - hit[0]
        else:
            key, label, offset = ('', pa & ~0xfff), f'(no tree node) {pa & ~0xfff:#010x}', pa & 0xfff
        row = touched.setdefault(key, {'label': label, 'first': n, 'first_offset': offset,
                                       'first_direction': direction, 'reads': 0, 'writes': 0,
                                       'regions': [], 'reads_all_ones': True})
        row['reads' if direction == 'read' else 'writes'] += 1
        if region not in row['regions']:
            row['regions'].append(region)
        if direction == 'read' and not _all_ones(value, size):
            row['reads_all_ones'] = False
    rows = sorted(touched.items(), key=lambda kv: kv[1]['first'])
    report = []
    for key, row in rows:
        want = MODELLED_BY_QEMU.get(key)
        modelled = want is not None and all(r.startswith(want) for r in row['regions'])
        if modelled:
            verdict = 'QEMU model'
        else:
            verdict = 'NOT MODELLED: answered by ' + ', '.join(row['regions'])
            if row['reads'] and row['reads_all_ones']:
                verdict += ', every read all-ones'
        print(f"  {row['label']:<34} R{row['reads']:<6} W{row['writes']:<6} first "
              f"{row['first_direction']} +{row['first_offset']:#x}  {verdict}")
        report.append((row['label'], row['reads'], row['writes'], row['first_direction'],
                       row['first_offset'], modelled, list(row['regions'])))
    unmodelled = sum(1 for r in report if not r[5])
    print(f'  {len(report)} windows touched, {unmodelled} not modelled by QEMU')
    if out is not None:
        lines = [f'{label}\t{reads}\t{writes}\t{d}\t{off:#x}\t{"model" if m else "NOT MODELLED"}\t'
                 f'{",".join(regions)}' for label, reads, writes, d, off, m, regions in report]
        (out / 'mmio.tsv').write_text('\n'.join(lines) + '\n')
    return report
