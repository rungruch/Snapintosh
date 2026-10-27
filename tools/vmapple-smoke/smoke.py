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
    literal pools point into the wrong image. Returns (elf_path, bin_path)."""
    out.mkdir(parents=True, exist_ok=True)
    clang = str(llvm / 'clang')
    obj = out / (src.stem + '.o')
    subprocess.run([clang, *CFLAGS, str(src), '-o', str(obj)], check=True)
    elf = out / (linked + '.elf')
    ld = tool('ld.lld', None, 'SNAP_LLD', str(llvm), '/opt/homebrew/opt/lld/bin')
    subprocess.run([ld, '-T', str(HERE / 'stub.ld'), f'--defsym=BASE={base:#x}',
                    str(obj), '-o', str(elf)], check=True)
    binf = out / (linked + '.bin')
    objcopy = tool('llvm-objcopy', None, 'SNAP_OBJCOPY', str(llvm), '/opt/homebrew/opt/llvm/bin')
    subprocess.run([objcopy, '-O', 'binary', str(elf), str(binf)], check=True)
    return elf, binf


def params_blob(entry, x0, trace):
    b = bytearray(0x20)
    struct.pack_into('<QQQQ', b, 0, entry, x0, PARAM_FLAG_TRACE if trace else 0,
                     SMOKE_PARAMS_MAGIC)
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


def run_qemu(qemu, out, stub_bin, payload_bin, payload_pa, entry_pa, x0, trace, timeout, cpu,
             extra=()):
    """Boot the stub at EL2 with one payload loaded at payload_pa. `extra`
    is appended to QEMU's arguments. Returns (serial_text, exit_code,
    timed_out, monitor_text)."""
    import time
    import kcload                       # the vCPU count the device tree describes
    params = out / 'params.bin'
    params.write_bytes(params_blob(entry_pa, x0, trace))
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
                                           MINIKERNEL_PA, 0, args.trace, args.timeout, args.cpu)
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
    payload, payload_pa, entry_pa, x0 = kcload.lay_out(kc, out)
    mmio_log = out / 'mmio-trace.log'
    extra = []
    if args.mmio_trace:
        mmio_log.unlink(missing_ok=True)
        extra = ['-trace', 'memory_region_ops_read', '-trace', 'memory_region_ops_write',
                 '-D', str(mmio_log)]
    text, code, timed_out, monitor_text = run_qemu(qemu, out, stub_bin, payload,
                                                   payload_pa, entry_pa, x0,
                                                   args.trace, args.timeout, args.cpu, extra)
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
    if args.mmio_trace:
        kcload.report_mmio(mmio_log.read_text(errors='replace') if mmio_log.exists() else '',
                           (out / 'devicetree.vma2smoke').read_bytes(), out)
    return kcload.report(text, out)


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
    a = ap.parse_args()
    return selftest(a) if a.cmd == 'selftest' else boot(a)


if __name__ == '__main__':
    sys.exit(main())
