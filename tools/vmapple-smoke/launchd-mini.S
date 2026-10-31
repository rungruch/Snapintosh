/* SPDX-License-Identifier: MIT */
/*
 * Freestanding ARM64 Darwin userspace init program (/sbin/launchd).
 *
 * Implements direct XNU BSD system calls without dyld, libSystem, or libc:
 * - SYS_write (4): writes greeting banner to stdout/stderr
 * - SYS_open (5): opens /dev/console (O_WRONLY = 1, mode = 0) and writes banner
 * - Idle loop (wfe): remains active at EL0 without exiting or causing a panic.
 */

.global _start
.align 4

.text
_start:
    /* 1. Attempt write to stdout (fd 1) */
    mov     x0, #1                      /* fd = 1 (stdout) */
    adr     x1, banner_msg
    mov     x2, #BANNER_LEN
    mov     x16, #4                     /* SYS_write = 4 */
    svc     #0x80

    /* 2. Attempt write to stderr (fd 2) */
    mov     x0, #2                      /* fd = 2 (stderr) */
    adr     x1, banner_msg
    mov     x2, #BANNER_LEN
    mov     x16, #4                     /* SYS_write = 4 */
    svc     #0x80

    /* 3. Attempt to open /dev/console (O_WRONLY = 1, mode = 0) */
    adr     x0, dev_console
    mov     x1, #1                      /* O_WRONLY = 1 */
    mov     x2, #0                      /* mode = 0 */
    mov     x16, #5                     /* SYS_open = 5 */
    svc     #0x80
    b.cs    skip_console

    /* open succeeded: x0 is console fd; write banner to it */
    mov     x19, x0
    mov     x0, x19
    adr     x1, banner_msg
    mov     x2, #BANNER_LEN
    mov     x16, #4                     /* SYS_write = 4 */
    svc     #0x80

skip_console:
    /* 4. Steady-state idle loop at EL0: remain alive without crashing */
idle:
    wfe
    b       idle

.align 4
dev_console:
    .asciz  "/dev/console"

.align 4
banner_msg:
    .ascii  "\n"
    .ascii  "================================================================\n"
    .ascii  "[USERSPACE] Hello from macOS 27 userspace!\n"
    .ascii  "[USERSPACE] PID 1 (/sbin/launchd) running on Qualcomm SC8280XP\n"
    .ascii  "[USERSPACE] Transition from kernel to userspace SUCCESSFUL!\n"
    .ascii  "================================================================\n"
    .ascii  "\n"
banner_end:
.equ BANNER_LEN, banner_end - banner_msg
