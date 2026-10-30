# gaokun3 Phase 4: Storage Mediation and Root Filesystem Mount (Design)

**Status, 2026-10-30: Phase 4 INITIATED.** Phase 3 passed completely on hardware
in Run 47 (qualification `784D548102EAF6B4`): the stock 26A428 VirtualMac2,1
kernel bootstrapped through memory management (`pmap_startup`), physical GICv3,
virtual timer, PAC QARMA5, context switching (with SSBS/DIT neutralized), IOKit
driver matching, and BSD domain init (`L2TP DOMAIN INIT COMPLETE`), reaching
`vfs_mountroot()`. Detecting no storage media, the kernel recorded
`missing-boot-media` to NVRAM and idled live at EL1 with no panic, no freeze,
and no restart (VERIFIED, Run 47). Phase 4 begins the storage mediation to
mount the macOS 27 root filesystem.

Evidence classes follow AGENTS.md: VERIFIED, OBSERVED, INFERRED,
EXPERIMENTAL, and UNIMPLEMENTED. Unmarked statements about the plan are
proposals.

---

## 1. Goal & Acceptance Criteria

Phase 4 bridges the kernel from the `missing-boot-media` root-mount wait to an
active mounted filesystem running userspace (`launchd`).

### Acceptance Criteria
1. **IOBlockStorageDevice & IOMedia Publication:**
   The EL2 monitor models a VirtIO block storage device on the guest PCIe bus.
   `AppleVirtIOPCITransport` matches on the PCI device, attaches
   `AppleVirtIOBlockStorageDevice`, and publishes an `IOMedia` object for the
   system disk and its APFS container.
2. **Root Filesystem Mount (`vfs_mountroot`):**
   `vfs_mountroot()` locates the system volume by UUID or boot-uuid, probes
   the APFS container superblock via `AppleAPFS`, and mounts `/` read-only.
3. **Userspace Transition:**
   The kernel launches `/sbin/launchd` (or single-user `/bin/sh` with `-s`).
   The guest console prints userspace output.

---

## 2. Storage Architecture in Route B (VirtualMac2,1)

### Contrast with Route A (Zenbook A16 / J700)
- **Route A (J700):** Bare-metal Apple Silicon kernel expects an Apple ANS2 NVMe
  controller. Snapintosh mediates this with `compat/nvme-host.c` translating
  Apple NVMe commands to the real NVMe controller.
- **Route B (VirtualMac2,1):** The VirtualMac guest kernel has no ANS2 driver.
  Instead, storage is designed around paravirtualized standards:
  1. **Primary System Root Storage: VirtIO Modern Block Device on PCIe.**
     - Driven by `com.apple.driver.AppleVirtIO` (`AppleVirtIOBlockStorageDevice`
       via `AppleVirtIOPCITransport`).
     - Matches Apple VirtIO PCI vendor `0x106b`, device `0x1a00`
       (`PCI_DEVICE_ID_APPLE_VIRTIO_BLK`), or standard VirtIO `0x1af4:0x0002`.
     - Uses VirtIO 1.0+ Modern capabilities (`COMMON_CFG`, `NOTIFY_CFG`,
       `ISR_CFG`, `DEVICE_CFG`) mapped in PCIe BAR0.
     - Differentiates the root volume using Apple's variant tag:
       `max_secure_erase_sectors = 1` (`VM_APPLE_VIRTIO_BLK_VARIANT_ROOT`).
  2. **Secondary Crash/Panic Storage: Backdoor Mailbox (`bdif` / `AppleVirtualPlatformUtilStorage`).**
     - Fixed MMIO window at `0x30000000` (`DEVID_ROOT` at `0x30000000`, `DEVID_AUX`
       at `0x30010000`).
     - Used exclusively by `AppleVirtualPlatformUtilStorage.cpp` for kernel panic
       coredumps (`kPaniclogBlockDeviceID` at `0x30010000`).
     - In Run 45, the panicked kernel attempted to write the coredump to `0x30010408`
       (`BDIF AUX CMD`). Wiring this to absorb or buffer panic dumps prevents
       unmodelled halts during recovery.

```
       +-------------------------------------------------------+
       |             macOS 27 Guest Kernel (EL1)               |
       |                                                       |
       |   +--------------------+     +--------------------+   |
       |   |     AppleAPFS      |     | AppleVirtualPlatf. |   |
       |   +---------+----------+     |    (UtilStorage)   |   |
       |             |                +---------+----------+   |
       |   +---------v----------+               |              |
       |   | AppleVirtIOBlock   |               |              |
       |   +---------+----------+               |              |
       |   | AppleVirtIOPCI     |               |              |
       +---|---------+----------|---------------|--------------+
           | PCIe ECAM/BAR0     |               | MMIO 0x30010408
           | 0x40000000/0x5000..|               | (kPaniclogBlockDev)
       +---v--------------------v---------------v--------------+
       |             Snapintosh EL2 Monitor                     |
       |                                                       |
       |   +--------------------+     +--------------------+   |
       |   | VirtIO-Blk Model   |     | BDIF Panic Sink    |   |
       |   | (tools/vmapple-    |     | (absorb/save       |   |
       |   |  smoke/storage.c)  |     |  panic coredump)   |   |
       |   +---------+----------+     +--------------------+   |
       +-------------|-----------------------------------------+
                     |
       +-------------v-----------------------------------------+
       | Backing Medium:                                       |
       | Stage 4A: RAM-backed APFS image (non-destructive)     |
       | Stage 4B: Physical internal NVMe / USB mass storage   |
       +-------------------------------------------------------+
```

---

## 3. Hardware Contracts & Device Tree Topology

### PCIe Host Controller (`/device-tree/arm-io/pcie`)
The VirtualMac template device tree describes the PCIe host controller:
- **Compatible:** `pcie,vmapple1`
- **Device Type:** `pci`
- **ECAM Configuration Window (`reg`):**
  Parent PA `0x40000000`, size `0x10000000` (256 MiB).
  Addressing: `offset = (bus << 20) | (dev << 15) | (func << 12) | (reg & 0xFFF)`.
- **MMIO Space (`ranges`):**
  Flags `0x02000000` (32-bit non-prefetchable memory),
  PCI address `0x50000000`, parent PA `0x50000000`, size `0x2fff0000` (~768 MiB).
- **Bus Range:** Bus 0 to Bus 8 (`bus-range = [0, 8]`).

### VirtIO Block PCI Device Specification
Placed at Bus 0, Device 1, Function 0 (`BDF 00:01.0`):
- **ECAM Offset:** `0x40008000`.
- **PCI Configuration Header:**
  - `Vendor ID`: `0x106b` (Apple)
  - `Device ID`: `0x1a00` (`PCI_DEVICE_ID_APPLE_VIRTIO_BLK`)
  - `Command`: `0x0006` (Bus Master + Memory Space Enable)
  - `Status`: `0x0010` (Capabilities List present)
  - `Revision ID`: `0x01`
  - `Class Code`: `0x010000` (Mass Storage Controller - SCSI)
  - `Header Type`: `0x00` (Standard endpoint)
  - `BAR0`: `0x50000000` (32-bit Memory BAR, size 16 KiB)
  - `Capabilities Pointer`: `0x40`
- **PCI Capabilities (VirtIO Modern 1.0):**
  1. `VIRTIO_PCI_CAP_COMMON_CFG` (type 1): BAR 0, offset `0x0000`, size `0x0038`.
  2. `VIRTIO_PCI_CAP_NOTIFY_CFG` (type 2): BAR 0, offset `0x1000`, size `0x0004`, multiplier 4.
  3. `VIRTIO_PCI_CAP_ISR_CFG` (type 3): BAR 0, offset `0x2000`, size `0x0001`.
  4. `VIRTIO_PCI_CAP_DEVICE_CFG` (type 4): BAR 0, offset `0x3000`, size `0x0040`.
- **VirtIO Block Configuration Register Block (`DEVICE_CFG`):**
  - `capacity` (offset `0x00`): 64-bit sector count (512-byte sectors).
  - `size_max` (offset `0x08`): max segment size (e.g. 1 MiB).
  - `seg_max` (offset `0x0c`): max segments (e.g. 128).
  - `blk_size` (offset `0x14`): 512 (or 4096).
  - `max_secure_erase_sectors` (offset `0x2c`): `1` (`VM_APPLE_VIRTIO_BLK_VARIANT_ROOT`).

---

## 4. Safety & Non-Negotiable Rules

1. **Strict NVMe Partition Safety (AGENTS.md Rule 5):**
   The internal NVMe on the gaokun3 holds Windows 11 and firmware recovery partitions.
   Installing to or altering internal NVMe partitions is destructive and strictly
   forbidden without prior written authorization, a complete sector backup, and a
   tested recovery procedure.
2. **RAM-First Validation (Stage 4A):**
   Initial Phase 4 validation uses a RAM-backed disk image loaded as a bundle chunk
   by q1n1 from the USB stick. This involves zero risk to internal storage.
3. **Compilation Unit Limits (AGENTS.md Section 1):**
   `tools/vmapple-smoke/monitor.c` is currently 1,282 physical lines (50.4 KiB),
   directly at the 50 KiB limit.
   All Phase 4 storage logic must reside in a dedicated compilation unit
   `tools/vmapple-smoke/storage.c` and header `tools/vmapple-smoke/storage.h`,
   with its own host test suite `tools/test-vmapple-storage.py`.
4. **Preserve Baseline Behavior (AGENTS.md Rule 3):**
   The VirtIO storage model must be compile-time gated or default to empty bus
   when storage is not provided, ensuring baseline Phase 3 behavior remains
   identical.

---

## 5. Phase 4 Work Breakdown & Milestones

| Step | Milestone | Scope & Evidence | Status |
| --- | --- | --- | --- |
| **P1** | **VirtIO Block Controller Host Model** | Implement `storage.c`/`storage.h` with PCIe ECAM decoding, BAR0 registers, Modern VirtIO queue processing, and Apple variant tagging. Host test suite (`test-vmapple-storage.py`) with 100% PASS under ASan/UBSan. | **COMPLETE** (44 checks PASS) |
| **P2** | **QEMU Smoke Boot with Root Disk** | Wire `storage.c` into `smoke.py`. Boot stock 26A428 kernel with a minimal test APFS disk under QEMU. Verify `AppleVirtIOBlockStorageDevice` matching, BAR0 mapping, virtqueue requests, and APFS root mount. | **COMPLETE** (mount-complete TestRoot, PASS) |
| **P3** | **Stick Bundle Ramdisk Provisioning** | Add ramdisk chunk support to `probes/classicload.py`, `tools/vmapple-stick.py`, and q1n1 `--vmapple-place`. Memory layout verification. | **COMPLETE** (host + QEMU rehearsal: `place_ramdisk` above the panic chunk, bundle.bin version 3, q1n1 places and enters, `mount-complete volume TestRoot` through the stick path; a stick build with a ramdisk must also widen the `/arm-io` window to `0x70000000` or BAR0 never maps) |
| **P4** | **Hardware Qualification on gaokun3** | Boot on the tablet with the RAM-backed root disk. Verify `vfs_mountroot()` success and single-user shell or launchd transition. | Pending P3 |

---

## 6. Step P2 Key Findings & Engineering Breakthroughs (VERIFIED)

During Step P2 QEMU smoke boot validation with `test-root.raw`, the stock macOS 27 kernel successfully mounted the APFS root volume (`TestRoot`). Three critical architectural contracts were resolved:

1. **/arm-io Ranges Window (`ClassicLayout.armio_size`):**
   - In `probes/classicload.py`, the layout previously truncated `/arm-io` ranges to `0x40000000`.
   - The PCIe memory window begins at arm-io offset `0x40000000` (`ranges = 0x50000000` via parent `0x40000000`, size `0x2fff0000`).
   - With an `armio` size of `0x40000000`, `IOPCIBridge::getDTNubAddressing()` could not translate the BAR0 range in `assigned-addresses`, failing with `kIOReturnNotFound` and creating zero `IODeviceMemory` objects.
   - The baseline default stays `0x40000000` (AGENTS.md Rule 3). Only the opt-in `--root-disk` path in `smoke.py` widens it to the template's `0x70000000`, which allowed full address translation; `AppleVirtIOPCITransport` successfully mapped BAR0 into kernel memory.

2. **Byte-Wise Device Configuration Space Reads:**
   - Disassembly and runtime tracing revealed `AppleVirtIOPCITransport` reads `virtio_blk_config` one byte at a time (e.g. 1-byte read of `max_secure_erase_sectors` at offset `0x3c` for root-variant detection).
   - Replaced coarse 32-bit/64-bit word access with a byte-accurate buffer in `storage_bar0_access()`, enabling byte-wise reading of `capacity`, `blk_size`, and Apple's variant tag.

3. **PCIe INTx Interrupt Delivery:**
   - Apple's PCIe bridge swizzles device 1 INTA to GIC INTID 65 (interrupt-base 64 + 1) and writes the assigned INTID to PCI config offset `0x3c` (`irq_line`).
   - Storage model now tracks writes to `0x3c` and injects virtual IRQ 65 upon virtqueue request completion (`monitor_inject_virq(blk_dev.irq_line)`).

4. **APFS Mount Verified in QEMU:**
   - Kernel log:
     ```
     nx_mount:1482: disk1 checkpoint search: largest xid 2, best xid 2 @ 3
     handle_mount:893: disk1s1 vol-uuid: 51EB8A19-CC0A-438E-A0E0-37BD22D66872 block size: 4096 block count: 7803
     apfs_log_op_with_proc:3279: disk1s1 mount-complete volume TestRoot, requested by: kernel_task (pid 0)
     RESULT: PASS (monitor: APFS root volume mounted via VirtIO block controller)
     ```

