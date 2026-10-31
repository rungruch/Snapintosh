#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Route B QEMU smoke test (gaokun3 Phase 0C).

Builds the EL2 stub (stub.S) and runs a guest under it on QEMU's `virt`
machine (TCG, `-cpu max,sve=off,sme=off`: PAuth and the 16 KiB granule,
no SVE/SME or Apple CPU features),
to answer, as far as early boot reaches, the two Phase 0C overturn conditions
for Route B:

  1. does the VirtualMac kernel hit Apple CPU behaviour EL2 cannot supply, and
  2. can its PAC hypercalls be served on standard PAuth (collapsed key
     domains; see docs/GAOKUN3-PHASE0B.md B6).

  python3 tools/vmapple-smoke/smoke.py selftest
      Build the stub and the synthetic EL1 mini-kernel (minikernel.S, no Apple
      material) and prove the stub's services end to end. Needs only QEMU and
      clang; this is the committed, always-runnable check.

  python3 tools/vmapple-smoke/smoke.py boot --kernelcache PATH
      Load a locally obtained VirtualMac2,1 kernelcache (never committed) at
      EL1 under the stub, build boot_args and a device tree, and report how far
      it gets before it prints, panics or faults. With --mmio-trace, also
      list every device window the guest touched and which QEMU region
      answered it (build/vmapple-smoke/mmio.tsv).

QEMU is found from --qemu, then SNAP_QEMU, then PATH; LLVM from --llvm, then
SNAP_LLVM, then Homebrew's keg, then PATH.
"""
import argparse
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'probes'))
import classicload                                              # noqa: E402
HOMEBREW = pathlib.Path('/opt/homebrew/opt/llvm/bin')

STUB_PA = 0x40200000
PARAMS_PA = 0x43000000
FAULT_PA = 0x43001000
MINIKERNEL_PA = 0x45000000
TIDCP_LOG_PA = 0x43002000
TIDCP_MAX = 64
SMOKE_PARAMS_MAGIC = 0x564D41504C450002
SMOKE_FAULT_MAGIC = 0x5641554C54
PARAM_FLAG_TRACE = 1
PARAM_FLAG_MONITOR = 2
PARAM_FLAG_PASSTHRU = 8
PARAM_FLAG_GICPT = 16
PARAM_FLAG_UARTPT = 32
PARAM_FLAG_BASELINE = 64
PARAM_FLAG_HALT_ON_OFF = 128
PARAM_FLAG_FRAMEBUFFER = 256
PARAM_FLAG_NO_GUEST = 512
PARAM_FLAG_GIC_HOLD = 1024


def tool(name, explicit, env, *extra_dirs):
    for cand in (explicit, os.environ.get(env)):
        if cand:
            p = pathlib.Path(cand)
            return str(p / name) if p.is_dir() else str(p)
    for d in extra_dirs:
        if (pathlib.Path(d) / name).exists():
            return str(pathlib.Path(d) / name)
    found = shutil.which(name)
    if not found:
        raise SystemExit(f'{name} not found: pass the right --flag or set {env}')
    return found


def llvm_dir(explicit):
    for cand in (explicit, os.environ.get('SNAP_LLVM')):
        if cand:
            return pathlib.Path(cand)
    if (HOMEBREW / 'clang').exists():
        return HOMEBREW
    found = shutil.which('clang')
    if not found:
        raise SystemExit('clang not found: pass --llvm or set SNAP_LLVM')
    return pathlib.Path(found).parent


CFLAGS = ['--target=aarch64-none-elf', '-ffreestanding', '-nostdlib', '-mgeneral-regs-only',
          '-mstrict-align', '-march=armv8.5-a+pauth', '-Wall', '-Wextra', '-Werror', '-O1',
          '-c', f'-I{HERE}']


def build_image(llvm, out, src, linked, base, defines=()):
    """Assemble src and link it at `base` with stub.ld; write the ELF and a raw
    binary. The link address must equal the load address, or PC-relative
    literal pools point into the wrong image. Returns (elf_path, bin_path).
    The EL2 stub is always linked with monitor.c, its device models.
    `defines` are -D flags for both (the platform gate)."""
    out.mkdir(parents=True, exist_ok=True)
    clang = str(llvm / 'clang')
    defs = [f'-D{d}' for d in defines]
    obj = out / (src.stem + '.o')
    subprocess.run([clang, *CFLAGS, *defs, str(src), '-o', str(obj)], check=True)
    objs = [str(obj)]
    if src.name == 'stub.S':
        for unit in ('monitor.c', 'panic.c', 'storage.c'):
            obj_c = out / (pathlib.Path(unit).stem + '.o')
            subprocess.run([clang, *CFLAGS, *defs, '-std=c11', str(HERE / unit),
                            '-o', str(obj_c)], check=True)
            objs.append(str(obj_c))
    elf = out / (linked + '.elf')
    ld = tool('ld.lld', None, 'SNAP_LLD', str(llvm), '/opt/homebrew/opt/lld/bin')
    subprocess.run([ld, '-T', str(HERE / 'stub.ld'), f'--defsym=BASE={base:#x}',
                    *objs, '-o', str(elf)], check=True)
    binf = out / (linked + '.bin')
    objcopy = tool('llvm-objcopy', None, 'SNAP_OBJCOPY', str(llvm), '/opt/homebrew/opt/llvm/bin')
    subprocess.run([objcopy, '-O', 'binary', str(elf), str(binf)], check=True)
    return elf, binf


def params_blob(entry, x0, flags, s2_range=(0, 0), disk=(0, 0)):
    """The stub's parameter page (layout.h smoke_params). s2_range is the
    guest RAM the monitor's stage 2 maps; the stub refuses an empty or
    misaligned one only when the monitor runs."""
    b = bytearray(0x68)
    struct.pack_into('<QQQQQQ', b, 0, entry, x0, flags, SMOKE_PARAMS_MAGIC, *s2_range)
    if disk[0] and disk[1]:
        struct.pack_into('<QQ', b, 0x58, disk[0], disk[1])
    return bytes(b)


def s2_range(layout):
    """The 2 MiB-aligned span of guest RAM stage 2 must map: from the
    block holding ram_base through the panic chunk."""
    lo = layout.ram_base & ~0x1FFFFF
    hi = (layout.panic_pa + layout.panic_size + 0x1FFFFF) & ~0x1FFFFF
    return lo, hi


def _monitor_ask(sock_path, commands, settle=0.4):
    """Send human-monitor commands to a running QEMU over its unix socket and
    return the collected output. Used to see where a silent guest is: EL1
    faults never reach the EL2 stub, so a stuck kernel is invisible on the
    serial line without this."""
    import socket
    import time
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(str(sock_path))
    except OSError:
        return ''
    out = b''
    try:
        time.sleep(0.2)
        try:
            out += s.recv(65536)                        # the "(qemu) " greeting
        except OSError:
            pass
        for cmd in commands:
            s.sendall(cmd.encode() + b'\n')
            time.sleep(settle)
            try:
                out += s.recv(65536)
            except OSError:
                pass
    finally:
        s.close()
    return out.decode('utf-8', 'replace')


def run_qemu(qemu, out, stub_bin, payload_bin, payload_pa, entry_pa, x0, flags, timeout, cpu,
             extra=(), probe_cmds=(), stop_cmds=(), memory='576M', s2=(0, 0),
             disk=(0, 0), disk_path=None):
    """Boot the stub at EL2 with one payload loaded at payload_pa. `extra`
    is appended to QEMU's arguments. With `stop_cmds` [(command, (file,
    size))], the stub halts on a named stop instead of exiting (the split
    console), so the run polls the stub's fault record and, once it
    appears, sends each command to QEMU's monitor, waits for its file, and
    ends the run. Returns (serial_text, exit_code, timed_out,
    monitor_text)."""
    import time
    import kcload                       # the vCPU count the device tree describes
    params = out / 'params.bin'
    params.write_bytes(params_blob(entry_pa, x0, flags, s2, disk))
    mon = out / 'qemu-monitor.sock'
    mon.unlink(missing_ok=True)
    loader_args = [
        '-device', f'loader,file={stub_bin},addr={STUB_PA:#x},force-raw=on',
        '-device', f'loader,file={payload_bin},addr={payload_pa:#x},force-raw=on',
        '-device', f'loader,file={params},addr={PARAMS_PA:#x},force-raw=on',
    ]
    if disk_path and disk[0] and disk[1]:
        loader_args.extend(['-device', f'loader,file={disk_path},addr={disk[0]:#x},force-raw=on'])
    loader_args.extend(['-device', f'loader,addr={STUB_PA:#x},cpu-num=0'])

    cmd = [
        qemu, '-M', 'virt,virtualization=on,gic-version=3', '-cpu', cpu,
        '-m', memory, '-smp', str(kcload.GUEST_CPUS), '-display', 'none', '-serial', 'stdio', '-no-reboot',
        '-semihosting-config', 'enable=on,target=native',
        '-monitor', f'unix:{mon},server,nowait',
        *loader_args,
        *extra,
    ]
    serial_out = out / 'serial.out'
    serial_err = out / 'serial.err'
    with open(serial_out, 'wb') as out_f, open(serial_err, 'wb') as err_f:
        proc = subprocess.Popen(cmd, stdout=out_f, stderr=err_f, cwd=str(ROOT))
        deadline = time.time() + timeout
        timed_out = False
        monitor_text = ''
        next_poll = time.time() + 2
        while proc.poll() is None:
            if time.time() > next_poll:
                next_poll = time.time() + 2
                cur = serial_out.read_text(errors='replace') if serial_out.exists() else ''
                if 'Waiting on' in cur and 'IOMedia' in cur and not disk_path:
                    monitor_text = _monitor_ask(mon, [
                        'info registers',
                        'x/4i $pc',
                        f'xp/4gx {TIDCP_LOG_PA:#x}',
                        f'xp/{4 * TIDCP_MAX}gx {TIDCP_LOG_PA + 0x20:#x}',
                        *probe_cmds,
                    ], settle=1.0 if probe_cmds else 0.4)
                    until = time.time() + 15
                    while time.time() < until and not all(
                            f.exists() and f.stat().st_size >= size for _, (f, size) in stop_cmds):
                        time.sleep(0.2)
                    proc.kill()
                    break
                if disk_path and ('[USERSPACE]' in cur or 'cannot mount root' in cur or 'Failed to mount root' in cur or 'panic' in cur):
                    time.sleep(1.0)
                    monitor_text = _monitor_ask(mon, [
                        'info registers',
                        'x/4i $pc',
                        f'xp/4gx {TIDCP_LOG_PA:#x}',
                        f'xp/{4 * TIDCP_MAX}gx {TIDCP_LOG_PA + 0x20:#x}',
                        *probe_cmds,
                    ], settle=1.0 if probe_cmds else 0.4)
                    until = time.time() + 15
                    while time.time() < until and not all(
                            f.exists() and f.stat().st_size >= size for _, (f, size) in stop_cmds):
                        time.sleep(0.2)
                    proc.kill()
                    break
                if disk_path and 'mount-complete' in cur:
                    # Give Darwin up to 180 seconds in TCG emulation after mount to execute userspace init
                    if not hasattr(proc, 'mount_time'):
                        proc.mount_time = time.time()
                    elif time.time() - proc.mount_time > 180.0:
                        monitor_text = _monitor_ask(mon, [
                            'info registers',
                            'x/4i $pc',
                            'xp/4gx 0x84c0a360',
                            'xp/4wx 0x84bdf2d0',
                            f'pmemsave 0x81000000 0x8000000 "{out / "ram.raw"}"',
                            f'xp/4gx {TIDCP_LOG_PA:#x}',
                            f'xp/{4 * TIDCP_MAX}gx {TIDCP_LOG_PA + 0x20:#x}',
                            *probe_cmds,
                        ], settle=2.0)
                        proc.kill()
                        break
                if stop_cmds:
                    record = _monitor_ask(mon, [f'xp/1gx {FAULT_PA:#x}'], settle=0.2)
                    if f'{SMOKE_FAULT_MAGIC:016x}' in record.lower():
                        monitor_text = _monitor_ask(mon, [c for c, _ in stop_cmds], settle=1.0)
                        # QEMU writes a dump asynchronously to this exchange: wait
                        # for each file to reach its size before ending the run.
                        until = time.time() + 15
                        while time.time() < until and not all(
                                f.exists() and f.stat().st_size >= size for _, (f, size) in stop_cmds):
                            time.sleep(0.2)
                        proc.kill()
                        break
            if time.time() > deadline:
                timed_out = True
                # The guest is still running but silent: ask QEMU where it is.
                monitor_text = _monitor_ask(mon, [
                    'info registers',
                    'x/4i $pc',
                    f'xp/4gx {TIDCP_LOG_PA:#x}',
                    f'xp/{4 * TIDCP_MAX}gx {TIDCP_LOG_PA + 0x20:#x}',
                    *probe_cmds,
                ], settle=1.0 if probe_cmds else 0.4)
                until = time.time() + 15
                while time.time() < until and not all(
                        f.exists() and f.stat().st_size >= size for _, (f, size) in stop_cmds):
                    time.sleep(0.2)
                proc.kill()
                break
            time.sleep(0.1)
        proc.wait()
    out_b = serial_out.read_bytes() if serial_out.exists() else b''
    err_b = serial_err.read_bytes() if serial_err.exists() else b''

    def dec(b):
        return (b or b'').decode('utf-8', 'replace') if isinstance(b, (bytes, bytearray)) else (b or '')
    return dec(out_b) + dec(err_b), proc.returncode, timed_out, monitor_text


def selftest(args):
    llvm = llvm_dir(args.llvm)
    qemu = tool('qemu-system-aarch64', args.qemu, 'SNAP_QEMU')
    out = ROOT / 'build' / 'vmapple-smoke'
    _, stub_bin = build_image(llvm, out, HERE / 'stub.S', 'stub', STUB_PA)
    _, mk_bin = build_image(llvm, out, HERE / 'minikernel.S', 'minikernel', MINIKERNEL_PA)
    text, code, timed_out, _mon = run_qemu(qemu, out, stub_bin, mk_bin, MINIKERNEL_PA,
                                           MINIKERNEL_PA, 0,
                                           PARAM_FLAG_TRACE if args.trace else 0,
                                           args.timeout, args.cpu)
    sys.stdout.write(text)
    want = [
        'EL2: entering guest at EL1',
        'MK: running at EL1',
        'MK: DAIF debug remains masked',
        'MK: pac hypercalls ok',
        'MK: pauth sign/auth ok',
        'MK: impdef sysreg trapped and raz',
        'MK: done ok',
    ]
    problems = [line for line in want if line not in text]
    if 'MK: FAIL' in text:
        problems.append('mini-kernel reported FAIL')
    if timed_out:
        problems.append(f'QEMU did not exit within {args.timeout}s')
    if 'EL2: unexpected exception' in text:
        problems.append('the stub recorded an unexpected EL2 exception')
    for p in problems:
        print(f'FAIL: {p}')
    if problems:
        print('FAIL vmapple-smoke selftest')
        return 1
    print('PASS vmapple-smoke selftest: EL2 stub serves the PAC hypercalls, '
          'RAZ/WIs Apple IMPDEF registers, and runs PAuth natively at EL1')
    return 0


def boot(args):
    import kcload
    llvm = llvm_dir(args.llvm)
    qemu = tool('qemu-system-aarch64', args.qemu, 'SNAP_QEMU')
    out = ROOT / 'build' / 'vmapple-smoke'
    _, stub_bin = build_image(llvm, out, HERE / 'stub.S', 'stub', STUB_PA)
    kc = pathlib.Path(args.kernelcache)
    if not kc.exists():
        print(f'SKIP: no kernelcache at {kc} (bring your own; never committed)')
        return 0
    flags = PARAM_FLAG_TRACE if args.trace else 0
    layout = None
    probe_cmds = []
    stop_cmds = []
    screen = out / 'screen.raw'
    # QEMU echoes monitor input a character at a time; a short path keeps
    # the command quick. QEMU runs from the repository root.
    screen_arg = screen.relative_to(ROOT) if screen.is_relative_to(ROOT) else screen
    if args.framebuffer and not args.monitor:
        print('--framebuffer needs --monitor')
        return 2
    if args.monitor:
        flags |= PARAM_FLAG_MONITOR
        layout = kcload.MONITOR_LAYOUT
        if args.gic_passthrough:
            flags |= PARAM_FLAG_GICPT
        if args.uart_passthrough:
            flags |= PARAM_FLAG_UARTPT
        if args.baseline_answers:
            flags |= PARAM_FLAG_BASELINE
        if args.passthrough:
            flags |= PARAM_FLAG_PASSTHRU
            from dataclasses import replace as _replace2
            layout = _replace2(kcload._LAYOUT, drop_nodes=kcload.MONITOR_LAYOUT.drop_nodes)
        if args.framebuffer:
            # The split console (Phase 3 P4, decision 3): the guest is told
            # about the top rows; the monitor draws its band below them.
            flags |= PARAM_FLAG_FRAMEBUFFER
            layout = classicload.split_screen(layout, classicload.QEMU_SPLIT_SCREEN,
                                              classicload.QEMU_MONITOR_ROWS)
            base, _, height, stride = classicload.QEMU_SPLIT_SCREEN
            screen.unlink(missing_ok=True)
            dump = f'pmemsave {base:#x} {height * stride} {screen_arg}'
            probe_cmds = [dump]
            stop_cmds = [(dump, (screen, height * stride))]
        if args.keep_nodes:
            from dataclasses import replace as _replace
            layout = _replace(layout, drop_nodes=())
        if args.minimal_nodes:
            from dataclasses import replace as _replace4
            layout = _replace4(layout, drop_nodes=layout.drop_nodes + (
                '/device-tree/arm-io/pl031-rtc', '/device-tree/arm-io/pl061-buttons',
                '/device-tree/arm-io/pvpanic'))
        if args.drop_pcie:
            from dataclasses import replace as _replace3
            layout = _replace3(layout, drop_nodes=layout.drop_nodes + ('/device-tree/arm-io/pcie',))
    disk = (0, 0)
    disk_path = None
    if getattr(args, 'root_disk', None):
        disk_path = pathlib.Path(args.root_disk).resolve()
        if not disk_path.exists():
            print(f'FAIL: root disk not found: {disk_path}')
            print('To generate a minimal 32 MiB APFS test root image on macOS:')
            print(f'  diskutil image create blank --format RAW --size 32m --fs APFS --volumeName TestRoot {disk_path}')
            return 1
        disk = (0xa0000000, disk_path.stat().st_size)
        from dataclasses import replace as _replace_boot
        # The template's arm-io window, so the PCIe memory range (and BAR0
        # in it) translates; see ClassicLayout.armio_size. Set boot_uuid so
        # /chosen/boot-uuid and /chosen/apfs-preboot-uuid match the APFS root
        # volume for IOPlatformExpert::getBootMedia().
        layout = _replace_boot(layout, armio_size=0x70000000,
                               boot_uuid='CC453B28-C3F1-49AD-B0A5-389C17A60F40')
        if getattr(args, 'boot_args', None):
            layout = _replace_boot(layout, cmdline=args.boot_args)
        else:
            layout = _replace_boot(layout, cmdline=layout.cmdline + ' amfi_get_out_of_my_way=1 -s -nobsdmgroot rd=disk0s1')
    payload, payload_pa, entry_pa, x0 = kcload.lay_out(kc, out, layout)
    mmio_log = out / 'mmio-trace.log'
    extra = []
    if args.mmio_trace:
        mmio_log.unlink(missing_ok=True)
        extra = ['-trace', 'memory_region_ops_read', '-trace', 'memory_region_ops_write',
                 '-D', str(mmio_log)]
    text, code, timed_out, monitor_text = run_qemu(qemu, out, stub_bin, payload,
                                                   payload_pa, entry_pa, x0,
                                                   flags, args.timeout, args.cpu, extra,
                                                   probe_cmds, stop_cmds,
                                                   # The monitor layout's RAM and screen
                                                   # reach 0x9e258000; QEMU RAM starts at
                                                   # 0x40000000.
                                                   memory='2048M' if (args.monitor or disk_path) else '576M',
                                                   s2=s2_range(layout) if args.monitor else (0, 0),
                                                   disk=disk, disk_path=disk_path)
    try:
        import fcntl
        fl = fcntl.fcntl(1, fcntl.F_GETFL)
        if fl & os.O_NONBLOCK:
            fcntl.fcntl(1, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)
    except Exception:
        pass
    sys.stdout.write(text)
    sys.stdout.flush()
    print('--- smoke boot result ---')
    if timed_out:
        print(f'NO EXIT within {args.timeout}s; last serial above')
    pc = kcload.monitor_pc(monitor_text)
    if pc is not None:
        print(f'guest stopped with PC {pc:#x}; code around it:')
        kcload.disasm_around(kc, pc)
    if monitor_text:
        (out / 'monitor.txt').write_text(monitor_text)
    kcload.report_tidcp(text, monitor_text, out)
    if args.monitor:
        return report_monitor(text, mmio_log if args.mmio_trace else None, out,
                              screen=screen if args.framebuffer else None,
                              root_disk=disk_path is not None)
    if args.mmio_trace:
        kcload.report_mmio(mmio_log.read_text(errors='replace') if mmio_log.exists() else '',
                           (out / 'devicetree.vma2smoke').read_bytes(), out)
    return kcload.report(text, out)


# The monitor's band, as monitor.c draws it: background, then glyphs from
# probes/font5x7.h at scale 2 in 12x20-pixel cells from x 8, line n at y
# 6 + 20n. Colours are the 0x00RRGGBB words monitor.c stores.
BAND_BG, BAND_TEXT, BAND_STOP = 0x00202020, 0x00FFFFFF, 0x00FF5050
BAND_BANNER = 'SNAPINTOSH MONITOR: STAGE 2 DEFAULT-DENY'
BAND_NOTE = 0x00FFFF40
BAND_MAILBOX = 'MAILBOX 4FFFF8: REQUEST ABSORBED, NO HOST'


def font5x7():
    """The glyph table from probes/font5x7.h, so the harness and the monitor
    draw from one source."""
    import re
    text = (ROOT / 'probes' / 'font5x7.h').read_text()
    body = text[text.index('font5x7[64][7] = {'):text.index('};')]
    rows = re.findall(r'\{(0x[0-9a-f]{2}(?:,0x[0-9a-f]{2}){6})\}', body)
    if len(rows) != 64:
        raise SystemExit(f'font5x7.h: expected 64 glyphs, found {len(rows)}')
    return [[int(v, 16) for v in r.split(',')] for r in rows]


def expected_band(width, rows, lines):
    """The band's 32-bit words for `lines` [(text, colour)], row-major."""
    font = font5x7()
    px = [BAND_BG] * (width * rows)
    for n, (s, colour) in enumerate(lines):
        for j, ch in enumerate(s.upper()):
            c = ord(ch)
            g = font[c - 0x20] if 0x20 <= c <= 0x5F else font[ord('?') - 0x20]
            x0, y0 = 8 + j * 12, 6 + n * 20
            for row in range(7):
                for bit in range(5):
                    if g[row] & (0x10 >> bit):
                        for d in range(4):
                            x, y = x0 + bit * 2 + (d & 1), y0 + row * 2 + (d >> 1)
                            if x < width and y < rows:
                                px[y * width + x] = colour
    return px


def write_png(path, words, width, height):
    """A PNG of 0x00RRGGBB words, for a person to look at."""
    import zlib
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for w in words[y * width:(y + 1) * width]:
            raw += bytes(((w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    pathlib.Path(path).write_bytes(png)


def check_screen(screen, stop_line, out, mailbox=False, gic_line=None):
    """P4's verdict on the split console. Returns a list of problems."""
    base, width, height, stride = classicload.QEMU_SPLIT_SCREEN
    band_rows = classicload.QEMU_MONITOR_ROWS
    guest_rows = height - band_rows
    if not screen.exists() or screen.stat().st_size != height * stride:
        return ['the screen was not captured (no named stop, or pmemsave failed)']
    data = screen.read_bytes()
    words = list(struct.unpack(f'<{height * stride // 4}I', data))
    words = [words[y * (stride // 4) + x] for y in range(height) for x in range(width)]
    write_png(out / 'screen.png', words, width, height)
    problems = []
    guest = words[:guest_rows * width]
    lit_rows = sum(any(guest[y * width:(y + 1) * width]) for y in range(guest_rows))
    print(f'screen: the guest drew on {lit_rows} of its {guest_rows} rows; '
          f'picture at {out / "screen.png"}')
    if lit_rows < 8:
        problems.append('the guest drew nothing in its framebuffer rows')
    # Band lines by index: 0 banner, 1 stop, 2 mailbox, 3 the GIC as
    # monitor_init read it (monitor.c monitor_gic_report).
    lines = [(BAND_BANNER, BAND_TEXT), (stop_line or '', BAND_STOP),
             (BAND_MAILBOX if mailbox else '', BAND_NOTE), (gic_line or '', BAND_TEXT)]
    want = expected_band(width, band_rows, lines)
    got = words[guest_rows * width:]
    wrong = sum(a != b for a, b in zip(got, want))
    print(f'screen: the monitor band ({band_rows} rows) differs from the expected '
          f'rendering in {wrong} pixels')
    if wrong:
        problems.append(f'the monitor band is not exactly its own text ({wrong} pixels differ)')
    return problems


def report_monitor(text, mmio_log, out, screen=None, root_disk=False):
    """The Phase 3 P3 verdict: with no QEMU device visible to the guest,
    the boot reaches the root-mount wait, or the kernel's console and then a
    named stop at the first contract the monitor does not model (Phase 3's
    acceptance), and QEMU's MMIO trace shows only the monitor's own
    devices. With root_disk (--root-disk), the verdict is stricter: only
    an APFS mount-complete line passes. Driver matching or handle_mount
    alone fail, because both print before mounting finishes."""
    import kcload
    problems = []
    stopped = 'EL2: stop:' in text
    if 'EL2: monitor:' not in text:
        problems.append('the monitor did not start')
    if 'EL2: unexpected exception' in text and not stopped:
        # A named stop ends through the fault recorder, which prints this;
        # it is only a failure when nothing named the stop.
        problems.append('the stub recorded an unexpected EL2 exception')
    banner = 'Darwin Kernel Version' in text
    if not banner:
        problems.append('the kernel banner did not print')
    if 'panic(' in text:
        line = next(l for l in text.splitlines() if 'panic(' in l)
        print('guest panic line:', line[:160])
        problems.append('the guest panicked')
    if mmio_log is not None and mmio_log.exists():
        accesses = kcload.parse_mmio_trace(mmio_log.read_text(errors='replace'))
        regions = {r[4] for r in accesses}
        allowed = {'pl011', 'gicv3_redist'}
        # The monitor enables QEMU's real distributor once at setup
        # (monitor_init: read-modify-write GICD_CTLR for EnGrp0|EnGrp1),
        # because the guest's own distributor writes go to the model.
        # Only that GICD_CTLR setup sequence is allowed; stage 2 never
        # maps the distributor for the guest.
        dist = [r for r in accesses if r[4] == 'gicv3_dist']
        dist_writes = [r for r in dist if r[0] == 'write']
        dist_reads = [r for r in dist if r[0] == 'read']
        if (len(dist_writes) == 1 and dist_writes[0][:2] == ('write', 0x08000000)
                and (dist_writes[0][2] & 0x3) == 0x3 and dist_writes[0][3] == 4
                and all(r[:2] == ('read', 0x08000000) and r[3] == 4 for r in dist_reads)):
            allowed.add('gicv3_dist')
        stray = {r for r in regions if not any(r.startswith(a) for a in allowed)}
        print(f'QEMU devices touched (by the monitor at EL2 only): {sorted(regions)}')
        if stray:
            problems.append(f'a QEMU device answered outside the monitor: {sorted(stray)}')
    if stopped:
        line = next(l for l in text.splitlines() if 'EL2: stop:' in l)
        print(line.strip())
    if screen is not None:
        stop_line = None
        for line in text.splitlines():
            if 'EL2: stop: unmodelled IPA' in line:
                ipa, kind = line.split('unmodelled IPA ')[1].split()[:2]
                stop_line = f'STOP: UNMODELLED IPA {int(ipa, 16):016X} {kind.upper()}'
                break
            if 'EL2: stop: unmodelled system register' in line:
                name, kind = line.split('system register ')[1].split()[:2]
                stop_line = f'STOP: SYSTEM REGISTER {name.upper()} {kind.upper()}'
                break
            if 'EL2: stop: no free list register' in line:
                stop_line = f'STOP: NO FREE LIST REGISTER FOR SGI {int(line.split()[-1]):02d}'
                break
        mailbox = 'EL2: mailbox:' in text
        gic_line = None
        for line in text.splitlines():
            m = re.search(r'EL2: gic: ctlr 0x([0-9a-f]+) -> 0x([0-9a-f]+), redistributor frame (\d+)', line)
            if m:
                gic_line = (f'GIC CTLR {int(m[1], 16):08X} -> {int(m[2], 16):08X}  '
                            f'RD FRAME {int(m[3]) % 10}')
                break
        problems += check_screen(screen, stop_line, out, mailbox, gic_line)
    mounted = 'mount-complete' in text
    if stopped:
        if banner and 'RESULT' not in text:
            if root_disk and not mounted:
                problems.append('root-disk boot stopped before APFS mount-complete')
            elif not root_disk:
                for p in problems:
                    print(f'FAIL: {p}')
                print('RESULT: ' + ('PASS' if not problems else 'FAIL') +
                      ' (monitor: banner reached, then a named stop)')
                return 0 if not problems else 1
        else:
            problems.append('the run stopped before the kernel banner')
    elif mounted:
        pass                                    # APFS root volume mounted (Phase 4 P2)
    elif root_disk:
        problems.append('root-disk boot did not reach APFS mount-complete')
    elif 'Waiting on' in text and 'IOMedia' in text:
        pass                                    # the harness timeout ends it here
    elif 'Got boot device' in text or 'BSD root:' in text or 'AppleVirtIO' in text:
        pass                                    # root disk detected and matched
    else:
        problems.append('the root-mount wait was not reached')
    for p in problems:
        print(f'FAIL: {p}')
    if problems:
        print('RESULT: FAIL (monitor boot)')
        return 1
    if '[USERSPACE]' in text:
        print('RESULT: PASS (userspace: PID 1 /sbin/launchd executed from APFS root)')
    elif mounted:
        print('RESULT: PASS (monitor: APFS root volume mounted via VirtIO block controller)')
    elif 'Got boot device' in text or 'BSD root:' in text or 'AppleVirtIO' in text:
        print('RESULT: PASS (monitor: root disk matched via VirtIO block controller)')
    else:
        print('RESULT: PASS (monitor: root-mount wait with no QEMU device visible)')
    return 0


def build(args):
    """Build the monitor image only, for a stick bundle
    (tools/vmapple-stick.py --monitor). --gaokun3 selects the gaokun3
    platform gate (real GIC and panel addresses, no UART or semihosting);
    the image is linked at --base, which must be its load address."""
    llvm = llvm_dir(args.llvm)
    out = pathlib.Path(args.out)
    defines = ('VMAPPLE_GAOKUN3_MONITOR',) if args.gaokun3 else ()
    _, binf = build_image(llvm, out, HERE / 'stub.S', 'monitor-image', args.base, defines)
    print(f'monitor image: {binf} linked at {args.base:#x}'
          f'{" (gaokun3)" if args.gaokun3 else ""}')
    return 0


def main():
    try:
        import fcntl
        fl = fcntl.fcntl(1, fcntl.F_GETFL)
        if fl & os.O_NONBLOCK:
            fcntl.fcntl(1, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)
    except Exception:
        pass
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--llvm')
    b.add_argument('--gaokun3', action='store_true')
    b.add_argument('--base', type=lambda v: int(v, 0), required=True)
    b.add_argument('--out', required=True)
    for name in ('selftest', 'boot'):
        s = sub.add_parser(name)
        s.add_argument('--llvm')
        s.add_argument('--qemu')
        s.add_argument('--cpu', default='max,sve=off,sme=off',
                       help='QEMU CPU model (default: max with SVE/SME disabled, matching gaokun3)')
        s.add_argument('--trace', action='store_true', help='print EL2 hvc breadcrumbs')
        s.add_argument('--timeout', type=int, default=60)
        if name == 'boot':
            s.add_argument('--kernelcache', required=True)
            s.add_argument('--mmio-trace', action='store_true',
                           help="trace QEMU's MMIO accesses and report which device windows "
                                'the guest touched and what answered (gaokun3 Phase 3 P1)')
            s.add_argument('--framebuffer', action='store_true',
                           help='with --monitor: publish a RAM framebuffer in boot_args '
                                '(Phase 3 P4 split console) and probe it for painted glyphs')
            s.add_argument('--monitor', action='store_true',
                           help='run the Route B monitor (Phase 3 P3): stage 2 default-deny, '
                                'virtual GICv3 and device models, no QEMU device visible')
            s.add_argument('--baseline-answers', action='store_true',
                           help='debug, with --monitor: secondary windows answer as the '
                                'Phase 0C QEMU window did (all-ones reads)')
            s.add_argument('--uart-passthrough', action='store_true',
                           help='debug, with --monitor: stage 2 maps the real QEMU PL011 '
                                'at the template IPA instead of modelling it')
            s.add_argument('--gic-passthrough', action='store_true',
                           help='debug, with --monitor: stage 2 maps the real QEMU GIC '
                                'at the template IPAs instead of modelling it')
            s.add_argument('--minimal-nodes', action='store_true',
                           help='debug, with --monitor: also drop the RTC, buttons and '
                                'pvpanic nodes')
            s.add_argument('--drop-pcie', action='store_true',
                           help='debug, with --monitor: also drop the pcie node')
            s.add_argument('--passthrough', action='store_true',
                           help='debug, with --monitor: stage 2 maps QEMU GIC/PL011 '
                                'instead of modelling them (bisection aid)')
            s.add_argument('--keep-nodes', action='store_true',
                           help='with --monitor: keep the paravirtual device nodes the '
                                'monitor layout otherwise drops (bisection aid)')
            s.add_argument('--root-disk',
                           help='path to raw root disk / APFS image to attach (Phase 4 P2)')
            s.add_argument('--boot-args',
                           help='override kernel command line arguments')
    a = ap.parse_args()
    return {'selftest': selftest, 'boot': boot, 'build': build}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
