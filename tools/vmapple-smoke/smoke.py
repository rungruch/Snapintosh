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
HOMEBREW = pathlib.Path('/opt/homebrew/opt/llvm/bin')

STUB_PA = 0x40200000
PARAMS_PA = 0x43000000
FAULT_PA = 0x43001000
MINIKERNEL_PA = 0x45000000
TIDCP_LOG_PA = 0x43002000
TIDCP_MAX = 64
SMOKE_PARAMS_MAGIC = 0x564D41504C450001
SMOKE_FAULT_MAGIC = 0x5641554C54
PARAM_FLAG_TRACE = 1
PARAM_FLAG_MONITOR = 2
PARAM_FLAG_ABSENT_FIXED = 4
PARAM_FLAG_PASSTHRU = 8
PARAM_FLAG_GICPT = 16
PARAM_FLAG_UARTPT = 32
PARAM_FLAG_BASELINE = 64
PARAM_FLAG_HALT_ON_OFF = 128


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
          '-march=armv8.5-a+pauth', '-Wall', '-Wextra', '-Werror', '-O1', '-c', f'-I{HERE}']


def build_image(llvm, out, src, linked, base):
    """Assemble src and link it at `base` with stub.ld; write the ELF and a raw
    binary. The link address must equal the load address, or PC-relative
    literal pools point into the wrong image. Returns (elf_path, bin_path).
    The EL2 stub is always linked with monitor.c, its device models."""
    out.mkdir(parents=True, exist_ok=True)
    clang = str(llvm / 'clang')
    obj = out / (src.stem + '.o')
    subprocess.run([clang, *CFLAGS, str(src), '-o', str(obj)], check=True)
    objs = [str(obj)]
    if src.name == 'stub.S':
        mon = out / 'monitor.o'
        subprocess.run([clang, *CFLAGS, '-std=c11', str(HERE / 'monitor.c'),
                        '-o', str(mon)], check=True)
        objs.append(str(mon))
    elf = out / (linked + '.elf')
    ld = tool('ld.lld', None, 'SNAP_LLD', str(llvm), '/opt/homebrew/opt/lld/bin')
    subprocess.run([ld, '-T', str(HERE / 'stub.ld'), f'--defsym=BASE={base:#x}',
                    *objs, '-o', str(elf)], check=True)
    binf = out / (linked + '.bin')
    objcopy = tool('llvm-objcopy', None, 'SNAP_OBJCOPY', str(llvm), '/opt/homebrew/opt/llvm/bin')
    subprocess.run([objcopy, '-O', 'binary', str(elf), str(binf)], check=True)
    return elf, binf


def params_blob(entry, x0, flags):
    b = bytearray(0x20)
    struct.pack_into('<QQQQ', b, 0, entry, x0, flags, SMOKE_PARAMS_MAGIC)
    return bytes(b)


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
             extra=(), probe_cmds=()):
    """Boot the stub at EL2 with one payload loaded at payload_pa. `extra`
    is appended to QEMU's arguments. Returns (serial_text, exit_code,
    timed_out, monitor_text)."""
    import time
    import kcload                       # the vCPU count the device tree describes
    params = out / 'params.bin'
    params.write_bytes(params_blob(entry_pa, x0, flags))
    mon = out / 'qemu-monitor.sock'
    mon.unlink(missing_ok=True)
    cmd = [
        qemu, '-M', 'virt,virtualization=on,gic-version=3', '-cpu', cpu,
        '-m', '576M', '-smp', str(kcload.GUEST_CPUS), '-display', 'none', '-serial', 'stdio', '-no-reboot',
        '-semihosting-config', 'enable=on,target=native',
        '-monitor', f'unix:{mon},server,nowait',
        '-device', f'loader,file={stub_bin},addr={STUB_PA:#x},force-raw=on',
        '-device', f'loader,file={payload_bin},addr={payload_pa:#x},force-raw=on',
        '-device', f'loader,file={params},addr={PARAMS_PA:#x},force-raw=on',
        '-device', f'loader,addr={STUB_PA:#x},cpu-num=0',
        *extra,
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.time() + timeout
    timed_out = False
    monitor_text = ''
    while proc.poll() is None:
        if time.time() > deadline:
            timed_out = True
            # The guest is still running but silent: ask QEMU where it is.
            monitor_text = _monitor_ask(mon, [
                'info registers',
                'x/4i $pc',
                f'xp/4gx {TIDCP_LOG_PA:#x}',
                f'xp/{4 * TIDCP_MAX}gx {TIDCP_LOG_PA + 0x20:#x}',
                *probe_cmds,
            ])
            proc.kill()
            break
        time.sleep(0.1)
    out_b, err_b = proc.communicate()

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
    if args.framebuffer and not args.monitor:
        print('--framebuffer needs --monitor')
        return 2
    if args.monitor:
        flags |= PARAM_FLAG_MONITOR
        if args.absent_fixed:
            flags |= PARAM_FLAG_ABSENT_FIXED
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
            from dataclasses import replace as _replace5
            layout = _replace5(layout, video=(FB_PA, FB_WIDTH, FB_HEIGHT, FB_WIDTH * 4, 32))
            probe_cmds = [f'xp/512gx {FB_PA:#x}']
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
                                                   probe_cmds)
    sys.stdout.write(text)
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
                              fb_text=monitor_text if args.framebuffer else None)
    if args.mmio_trace:
        kcload.report_mmio(mmio_log.read_text(errors='replace') if mmio_log.exists() else '',
                           (out / 'devicetree.vma2smoke').read_bytes(), out)
    return kcload.report(text, out)


FB_PA, FB_WIDTH, FB_HEIGHT = 0x60000000, 1024, 600


def report_monitor(text, mmio_log, out, fb_text=None):
    """The Phase 3 P3 verdict: with no QEMU device visible to the guest,
    the boot reaches the root-mount wait (diagnostic absent-fixed gate) or
    stops by name at a fixed address (the default), and QEMU's MMIO trace
    shows only the monitor's own devices."""
    import kcload
    problems = []
    stopped = 'EL2: stop: unmodelled IPA' in text
    if 'EL2: monitor:' not in text:
        problems.append('the monitor did not start')
    if 'EL2: unexpected exception' in text and not stopped:
        # A named stop ends through the fault recorder, which prints this;
        # it is only a failure when nothing named the stop.
        problems.append('the stub recorded an unexpected EL2 exception')
    banner = 'Darwin Kernel Version' in text
    if not banner:
        problems.append('the kernel banner did not print')
    if mmio_log is not None and mmio_log.exists():
        regions = {r[4] for r in kcload.parse_mmio_trace(mmio_log.read_text(errors='replace'))}
        allowed = {'pl011', 'gicv3_redist'}
        stray = {r for r in regions if not any(r.startswith(a) for a in allowed)}
        print(f'QEMU devices touched (by the monitor at EL2 only): {sorted(regions)}')
        if stray:
            problems.append(f'a QEMU device answered outside the monitor: {sorted(stray)}')
    if 'EL2: stop: unmodelled IPA' in text:
        line = next(l for l in text.splitlines() if 'unmodelled IPA' in l)
        print(line.strip())
    if fb_text is not None:
        words = fb_text.count('0x') - fb_text.count('0x0000000000000000')
        print(f'framebuffer at {FB_PA:#x}: {words} non-zero words in the first 4 KiB probed')
        if words < 8:
            problems.append('the framebuffer has no glyph block (nothing painted)')
    if stopped:
        fixed = ('0x0000000030010000' in text or '0x00000000004ff' in text)
        if banner and fixed and 'RESULT' not in text:
            print('RESULT: PASS (monitor: banner reached, then the named '
                  'default-deny stop at a fixed address)')
            return 0 if not problems else 1
        problems.append('the run stopped at an unexpected IPA')
    elif 'Waiting on' in text and 'IOMedia' in text:
        pass                                    # the harness timeout ends it here
    else:
        problems.append('the root-mount wait was not reached')
    for p in problems:
        print(f'FAIL: {p}')
    if problems:
        print('RESULT: FAIL (monitor boot)')
        return 1
    print('RESULT: PASS (monitor: root-mount wait with no QEMU device visible)')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
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
            s.add_argument('--absent-fixed', action='store_true',
                           help='diagnostic only, with --monitor: answer the two fixed '
                                'addresses as absent devices instead of stopping '
                                '(Phase 3 decision 5 option d)')
    a = ap.parse_args()
    return selftest(a) if a.cmd == 'selftest' else boot(a)


if __name__ == '__main__':
    sys.exit(main())
