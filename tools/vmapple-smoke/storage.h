/* SPDX-License-Identifier: MIT */
#ifndef SNAPINTOSH_VMAPPLE_STORAGE_H
#define SNAPINTOSH_VMAPPLE_STORAGE_H

#include <stdint.h>

/* PCIe Topology constants for VirtualMac2,1 */
#define MON_PCIE_CFG_BASE   0x40000000ull
#define MON_PCIE_CFG_SIZE   0x10000000ull  /* 256 MiB ECAM */
#define MON_PCIE_BAR0_BASE  0x50000000ull  /* 32-bit MMIO window from DT ranges */
#define MON_PCIE_BAR0_SIZE  0x00010000ull  /* 64 KiB BAR0 window */

/* PCI identifiers for VirtIO Block device */
#define PCI_VENDOR_ID_APPLE             0x106b
#define PCI_DEVICE_ID_APPLE_VIRTIO_BLK  0x1a00
#define PCI_VENDOR_ID_VIRTIO            0x1af4
#define PCI_DEVICE_ID_VIRTIO_BLK        0x1042
#define PCI_CLASS_STORAGE_SCSI          0x010000

/* Apple VirtIO Block variants (stored in max_secure_erase_sectors at offset 0x3c) */
#define VM_APPLE_VIRTIO_BLK_VARIANT_UNSPECIFIED 0
#define VM_APPLE_VIRTIO_BLK_VARIANT_ROOT        1
#define VM_APPLE_VIRTIO_BLK_VARIANT_AUX         2

/* Apple proprietary VirtIO block request type */
#define VIRTIO_BLK_T_APPLE_BARRIER              0x10000

/* Storage controller configuration and buffer attachment */
void storage_init(const void *disk_data, uint64_t disk_size, int read_only);
void storage_init_params(uint64_t params_pa);
void storage_set_mem_access(void (*r)(void *, uint64_t, uint64_t),
                            void (*w)(uint64_t, const void *, uint64_t));

/* Access dispatchers for monitor.c */
int storage_pcie_cfg_access(uint64_t off, unsigned size, int write, uint64_t *value);
int storage_bar0_access(uint64_t off, unsigned size, int write, uint64_t *value);
int storage_bdif_cmd(uint64_t devid, uint64_t cmd_pa);

/* Query whether a root storage device is currently active */
int storage_is_active(void);

#endif /* SNAPINTOSH_VMAPPLE_STORAGE_H */
