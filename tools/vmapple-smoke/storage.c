/* SPDX-License-Identifier: MIT */
/*
 * VMApple Modern VirtIO Block Controller and PCIe Storage Mediation (Phase 4).
 *
 * Implements the paravirtualized block device contract for VirtualMac2,1:
 * - PCIe ECAM configuration header at BDF 00:01.0 (Apple 0x106b:0x1a00).
 * - Modern VirtIO 1.0 capabilities (Common, Notify, ISR, Device Config) in BAR0.
 * - Apple variant identification: max_secure_erase_sectors = 1 (Root Volume).
 * - Split virtqueue request processing: READ, WRITE, FLUSH, and APPLE_BARRIER.
 * - Backdoor interface (BDIF) panic coredump sink at 0x30010408.
 *
 * Designed for freestanding EL2 execution on gaokun3 and host-side test harnesses.
 */

#include "storage.h"

#ifndef TEST_HARNESS
#include "monitor.h"
#else
int monitor_inject_virq(unsigned intid);
void mon_clean_range(uint64_t pa, uint64_t size);
void mon_uart_putc(char c);
#endif

static void log_str(const char *s)
{
    while (*s) mon_uart_putc(*s++);
}

static void log_hex(uint64_t v)
{
    const char h[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) mon_uart_putc(h[(v >> (4 * i)) & 15]);
}

/* ---- VirtIO 1.0 Modern PCI Capability Types ------------------------------ */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4

/* BAR0 Internal Layout Offsets */
#define BAR0_OFF_COMMON             0x0000u
#define BAR0_SIZE_COMMON            0x0038u
#define BAR0_OFF_NOTIFY             0x1000u
#define BAR0_SIZE_NOTIFY            0x0004u
#define BAR0_OFF_ISR                0x2000u
#define BAR0_SIZE_ISR               0x0001u
#define BAR0_OFF_DEVICE             0x3000u
#define BAR0_SIZE_DEVICE            0x0048u

/* VirtIO Features */
#define VIRTIO_BLK_F_SIZE_MAX       (1ull << 1)
#define VIRTIO_BLK_F_SEG_MAX        (1ull << 2)
#define VIRTIO_BLK_F_BLK_SIZE       (1ull << 6)
#define VIRTIO_BLK_F_FLUSH          (1ull << 9)
#define VIRTIO_BLK_F_ZONED          (1ull << 17) /* Forces config size >= 0x40 */
#define VIRTIO_F_VERSION_1          (1ull << 32)

/* VirtIO Device Status */
#define VIRTIO_STAT_ACKNOWLEDGE     1u
#define VIRTIO_STAT_DRIVER          2u
#define VIRTIO_STAT_DRIVER_OK       4u
#define VIRTIO_STAT_FEATURES_OK     8u

/* VirtIO Block Request Types */
#define VIRTIO_BLK_T_IN             0u
#define VIRTIO_BLK_T_OUT            1u
#define VIRTIO_BLK_T_FLUSH          4u

/* VirtIO Block Completion Status */
#define VIRTIO_BLK_S_OK             0u
#define VIRTIO_BLK_S_IOERR          1u
#define VIRTIO_BLK_S_UNSUPP         2u

/* Split Virtqueue Flags */
#define VRING_DESC_F_NEXT           1u
#define VRING_DESC_F_WRITE          2u
#define VRING_DESC_F_INDIRECT       4u

#define QUEUE_MAX_SIZE              128u

/* /arm-io/pcie interrupt-base (64) plus the INTx swizzle for device 1 INTA. */
#define STORAGE_INTX_INTID_DEFAULT  65u

/* Queue descriptor */
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

/* Queue available ring */
struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_MAX_SIZE];
};

/* Queue used ring entry */
struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

/* Queue used ring */
struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[QUEUE_MAX_SIZE];
};

/* Block Request Header */
struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
};

/* State of the VirtIO Block Device */
struct virtio_blk_state {
    /* Backing storage */
    const uint8_t *disk_data;
    uint64_t disk_pa;
    uint64_t disk_size;
    int is_readonly;
    int is_active;

    /* PCI Config & Features */
    uint32_t bar0_addr;
    uint16_t pci_cmd;
    /* INTx INTID. The VirtualMac ADT's interrupt cells are GIC INTIDs
     * (pl061-buttons: 37) and /arm-io/pcie interrupt-base is 64; IOPCIFamily
     * swizzles INTA of device 1 to 65 and writes it to config 0x3c (QEMU
     * smoke 2026-10-30, OBSERVED). Delivery follows what the kernel wrote. */
    uint8_t irq_line;
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint64_t driver_features;
    uint8_t device_status;
    uint8_t isr_status;

    /* Virtqueue 0 State */
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_enable;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
    uint16_t last_avail_idx;

    /* Statistics & Diagnostics */
    uint32_t reads_completed;
    uint32_t writes_completed;
    uint32_t bytes_transferred;
    uint32_t bar0_accesses;
    uint32_t vq_requests;
    uint32_t last_irq;
};

static struct virtio_blk_state blk_dev;

/* Memory access abstraction: on hardware EL2, guest PA is identity mapped.
 * Test harnesses can override these function pointers if needed. */
typedef void (*mem_copy_fn)(void *dst, const void *src, uint64_t len);

static void default_guest_read(void *dst, uint64_t guest_pa, uint64_t len)
{
    mon_clean_range(guest_pa, len);
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)guest_pa;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++) d[i] = src[i];
}

static void default_guest_write(uint64_t guest_pa, const void *src, uint64_t len)
{
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)guest_pa;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < len; i++) dst[i] = s[i];
    mon_clean_range(guest_pa, len);
}

static void (*guest_read_fn)(void *, uint64_t, uint64_t) = default_guest_read;
static void (*guest_write_fn)(uint64_t, const void *, uint64_t) = default_guest_write;

void storage_set_mem_access(void (*r)(void *, uint64_t, uint64_t),
                            void (*w)(uint64_t, const void *, uint64_t))
{
    guest_read_fn = r ? r : default_guest_read;
    guest_write_fn = w ? w : default_guest_write;
}

void storage_init(const void *disk_data, uint64_t disk_size, int read_only)
{
    blk_dev.disk_data = (const uint8_t *)disk_data;
    blk_dev.disk_pa = (uint64_t)(uintptr_t)disk_data;
    blk_dev.disk_size = disk_size;
    blk_dev.is_readonly = read_only;
    blk_dev.is_active = (disk_data != 0 && disk_size > 0);

    blk_dev.bar0_addr = (uint32_t)MON_PCIE_BAR0_BASE;
    blk_dev.pci_cmd = 0x06; /* Bus Master | Memory Space */
    blk_dev.irq_line = STORAGE_INTX_INTID_DEFAULT;
    blk_dev.device_features_sel = 0;
    blk_dev.driver_features_sel = 0;
    blk_dev.driver_features = 0;
    blk_dev.device_status = 0;
    blk_dev.isr_status = 0;

    blk_dev.queue_select = 0;
    blk_dev.queue_size = QUEUE_MAX_SIZE;
    blk_dev.queue_enable = 0;
    blk_dev.queue_desc = 0;
    blk_dev.queue_driver = 0;
    blk_dev.queue_device = 0;
    blk_dev.last_avail_idx = 0;

    blk_dev.reads_completed = 0;
    blk_dev.writes_completed = 0;
    blk_dev.bytes_transferred = 0;
    blk_dev.bar0_accesses = 0;
    blk_dev.vq_requests = 0;
    blk_dev.last_irq = 0;
}

void storage_init_params(uint64_t params_pa)
{
    if (!params_pa) return;
    uint64_t disk_pa = *(const volatile uint64_t *)(uintptr_t)(params_pa + 0x58);
    uint64_t disk_size = *(const volatile uint64_t *)(uintptr_t)(params_pa + 0x60);
    blk_dev.disk_pa = disk_pa;
    blk_dev.disk_size = disk_size;
    if (disk_pa && disk_size) {
        storage_init((const void *)(uintptr_t)disk_pa, disk_size, 0);
        blk_dev.disk_pa = disk_pa;
    }
}

int storage_is_active(void)
{
    return blk_dev.is_active;
}

/* ---- PCI Configuration Header Access (BDF 00:01.0) ----------------------- */
static uint8_t pci_cfg_byte(uint32_t off)
{
    /* PCI Header Type 00h (Standard Endpoint) */
    switch (off) {
    /* Vendor ID: 0x1af4 (VirtIO) */
    case 0x00: return (uint8_t)(PCI_VENDOR_ID_VIRTIO & 0xFF);
    case 0x01: return (uint8_t)(PCI_VENDOR_ID_VIRTIO >> 8);
    /* Device ID: 0x1042 (Modern VirtIO Block) */
    case 0x02: return (uint8_t)(PCI_DEVICE_ID_VIRTIO_BLK & 0xFF);
    case 0x03: return (uint8_t)(PCI_DEVICE_ID_VIRTIO_BLK >> 8);
    /* Command: Bus Master (0x4) | Memory Space (0x2) */
    case 0x04: return (uint8_t)(blk_dev.pci_cmd & 0xFF);
    case 0x05: return (uint8_t)(blk_dev.pci_cmd >> 8);
    /* Status: Capabilities List (0x10) */
    case 0x06: return 0x10;
    case 0x07: return 0x00;
    /* Revision ID: 0x01 */
    case 0x08: return 0x01;
    /* Class Code: 0x010000 (SCSI / Mass Storage) */
    case 0x09: return 0x00;
    case 0x0a: return 0x00;
    case 0x0b: return 0x01;
    /* Cache Line Size: 16 (64 bytes) */
    case 0x0c: return 0x10;
    case 0x0d: return 0x00; /* Latency Timer */
    case 0x0e: return 0x00; /* Header Type 00h */
    case 0x0f: return 0x00; /* BIST */

    /* BAR0: 32-bit Memory, non-prefetchable */
    case 0x10: return (uint8_t)(blk_dev.bar0_addr & 0xFF);
    case 0x11: return (uint8_t)((blk_dev.bar0_addr >> 8) & 0xFF);
    case 0x12: return (uint8_t)((blk_dev.bar0_addr >> 16) & 0xFF);
    case 0x13: return (uint8_t)((blk_dev.bar0_addr >> 24) & 0xFF);

    /* Subsystem Vendor ID: 0x1af4 (VirtIO) */
    case 0x2c: return (uint8_t)(PCI_VENDOR_ID_VIRTIO & 0xFF);
    case 0x2d: return (uint8_t)(PCI_VENDOR_ID_VIRTIO >> 8);
    /* Subsystem ID: 0x0002 (VirtIO Block) */
    case 0x2e: return 0x02;
    case 0x2f: return 0x00;

    /* Capabilities Pointer: 0x40 */
    case 0x34: return 0x40;

    /* Interrupt Line: the INTID the kernel programmed (see irq_line) */
    case 0x3c: return blk_dev.irq_line;
    case 0x3d: return 0x01; /* INTA */

    /* ---- Capability 1: COMMON_CFG at 0x40 (len 16) ---- */
    case 0x40: return 0x09; /* Vendor-Specific Cap */
    case 0x41: return 0x50; /* Next Cap: 0x50 */
    case 0x42: return 16;   /* Cap Length */
    case 0x43: return VIRTIO_PCI_CAP_COMMON_CFG;
    case 0x44: return 0;    /* BAR 0 */
    case 0x48: return (uint8_t)(BAR0_OFF_COMMON & 0xFF);
    case 0x49: return (uint8_t)(BAR0_OFF_COMMON >> 8);
    case 0x4c: return (uint8_t)(BAR0_SIZE_COMMON & 0xFF);
    case 0x4d: return (uint8_t)(BAR0_SIZE_COMMON >> 8);

    /* ---- Capability 2: NOTIFY_CFG at 0x50 (len 20) ---- */
    case 0x50: return 0x09;
    case 0x51: return 0x64; /* Next Cap: 0x64 */
    case 0x52: return 20;
    case 0x53: return VIRTIO_PCI_CAP_NOTIFY_CFG;
    case 0x54: return 0;    /* BAR 0 */
    case 0x58: return (uint8_t)(BAR0_OFF_NOTIFY & 0xFF);
    case 0x59: return (uint8_t)(BAR0_OFF_NOTIFY >> 8);
    case 0x5c: return (uint8_t)(BAR0_SIZE_NOTIFY & 0xFF);
    case 0x5d: return (uint8_t)(BAR0_SIZE_NOTIFY >> 8);
    case 0x60: return 0x04; /* notify_off_multiplier = 4 */

    /* ---- Capability 3: ISR_CFG at 0x64 (len 16) ---- */
    case 0x64: return 0x09;
    case 0x65: return 0x74; /* Next Cap: 0x74 */
    case 0x66: return 16;
    case 0x67: return VIRTIO_PCI_CAP_ISR_CFG;
    case 0x68: return 0;    /* BAR 0 */
    case 0x6c: return (uint8_t)(BAR0_OFF_ISR & 0xFF);
    case 0x6d: return (uint8_t)(BAR0_OFF_ISR >> 8);
    case 0x70: return (uint8_t)(BAR0_SIZE_ISR & 0xFF);
    case 0x71: return (uint8_t)(BAR0_SIZE_ISR >> 8);

    /* ---- Capability 4: DEVICE_CFG at 0x74 (len 16) ---- */
    case 0x74: return 0x09;
    case 0x75: return 0x00; /* End of capabilities */
    case 0x76: return 16;
    case 0x77: return VIRTIO_PCI_CAP_DEVICE_CFG;
    case 0x78: return 0;    /* BAR 0 */
    case 0x7c: return (uint8_t)(BAR0_OFF_DEVICE & 0xFF);
    case 0x7d: return (uint8_t)(BAR0_OFF_DEVICE >> 8);
    case 0x80: return (uint8_t)(BAR0_SIZE_DEVICE & 0xFF);
    case 0x81: return (uint8_t)(BAR0_SIZE_DEVICE >> 8);

    default: return 0;
    }
}

int storage_pcie_cfg_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    unsigned bus = (unsigned)((off >> 20) & 0xFF);
    unsigned dev = (unsigned)((off >> 15) & 0x1F);
    unsigned fn  = (unsigned)((off >> 12) & 0x07);
    uint32_t reg = (uint32_t)(off & 0xFFF);

    /* Device 1 on Bus 0 is our VirtIO Block device */
    if (blk_dev.is_active && bus == 0 && dev == 1 && fn == 0) {
        log_str("EL2: pcie cfg reg ");
        log_hex(reg);
        log_str(" sz ");
        log_hex(size);
        if (write) {
            log_str(" wr ");
            log_hex(*value);
            log_str("\n");
            uint64_t v = *value;
            if (reg == 0x04) {
                blk_dev.pci_cmd = (uint16_t)v;
            } else if (reg == 0x3c) {
                blk_dev.irq_line = (uint8_t)v;
            } else if (reg >= 0x10 && reg < 0x14) {
                /* BAR0 probe or base assignment: mask to 64 KiB alignment */
                blk_dev.bar0_addr = (uint32_t)v & ~(MON_PCIE_BAR0_SIZE - 1);
            }
            return 1;
        }
        uint64_t res = 0;
        for (unsigned i = 0; i < size; i++) {
            res |= ((uint64_t)pci_cfg_byte(reg + i)) << (8 * i);
        }
        *value = res;
        log_str(" rd ");
        log_hex(res);
        log_str("\n");
        return 1;
    }

    /* All other slots answer all-ones (absent PCI device) */
    if (!write) *value = (size == 8) ? ~0ull : ((1ull << (8 * size)) - 1);
    return 1;
}

/* ---- Virtqueue Processing ----------------------------------------------- */
static void process_virtqueue(void)
{
    if (!blk_dev.queue_enable || !blk_dev.queue_desc || !blk_dev.queue_driver || !blk_dev.queue_device)
        return;

    uint16_t avail_idx = 0;
    guest_read_fn(&avail_idx, blk_dev.queue_driver + 2, 2);

    uint16_t used_idx = 0;
    guest_read_fn(&used_idx, blk_dev.queue_device + 2, 2);

    while (blk_dev.last_avail_idx != avail_idx) {
        blk_dev.vq_requests++;
        uint16_t head_idx = 0;
        uint64_t elem_offset = blk_dev.queue_driver + 4 + (blk_dev.last_avail_idx % blk_dev.queue_size) * 2;
        guest_read_fn(&head_idx, elem_offset, 2);

        /* Walk descriptor chain */
        struct virtio_blk_req_hdr req_hdr;
        uint16_t cur_desc = head_idx;
        struct vring_desc desc;
        uint8_t req_status = VIRTIO_BLK_S_OK;
        uint32_t total_data_bytes = 0;

        /* Step 1: Read Request Header */
        guest_read_fn(&desc, blk_dev.queue_desc + cur_desc * sizeof(struct vring_desc), sizeof(desc));
        if (desc.len >= sizeof(req_hdr)) {
            guest_read_fn(&req_hdr, desc.addr, sizeof(req_hdr));
        } else {
            req_status = VIRTIO_BLK_S_IOERR;
        }

        /* Step 2: Traverse data and status descriptors */
        uint64_t disk_offset = req_hdr.sector * 512ull;
        while (desc.flags & VRING_DESC_F_NEXT) {
            cur_desc = desc.next;
            guest_read_fn(&desc, blk_dev.queue_desc + cur_desc * sizeof(struct vring_desc), sizeof(desc));

            /* Check if this is the final status descriptor */
            if (!(desc.flags & VRING_DESC_F_NEXT) && (desc.flags & VRING_DESC_F_WRITE)) {
                break;
            }

            /* Handle data transfer */
            if (req_status == VIRTIO_BLK_S_OK) {
                if (req_hdr.type == VIRTIO_BLK_T_IN) {
                    /* READ: Copy from disk image to guest memory */
                    if (disk_offset + desc.len <= blk_dev.disk_size) {
                        guest_write_fn(desc.addr, blk_dev.disk_data + disk_offset, desc.len);
                        disk_offset += desc.len;
                        total_data_bytes += desc.len;
                        blk_dev.reads_completed++;
                    } else {
                        req_status = VIRTIO_BLK_S_IOERR;
                    }
                } else if (req_hdr.type == VIRTIO_BLK_T_OUT) {
                    /* WRITE: If read-only, report error; otherwise ignore or update */
                    if (blk_dev.is_readonly) {
                        req_status = VIRTIO_BLK_S_IOERR;
                    } else {
                        blk_dev.writes_completed++;
                        total_data_bytes += desc.len;
                    }
                } else if (req_hdr.type == VIRTIO_BLK_T_FLUSH || req_hdr.type == VIRTIO_BLK_T_APPLE_BARRIER) {
                    /* Flush and barrier are successful no-ops */
                    req_status = VIRTIO_BLK_S_OK;
                } else {
                    req_status = VIRTIO_BLK_S_UNSUPP;
                }
            }
        }

        /* Step 3: Write status byte into the last descriptor */
        if (desc.len >= 1 && (desc.flags & VRING_DESC_F_WRITE)) {
            guest_write_fn(desc.addr, &req_status, 1);
        }
        log_str("EL2: storage: vq req type ");
        log_hex(req_hdr.type);
        log_str(" sector ");
        log_hex(req_hdr.sector);
        log_str(" bytes ");
        log_hex(total_data_bytes);
        log_str(" status ");
        log_hex(req_status);
        log_str("\n");

        /* Step 4: Record used element */
        struct vring_used_elem used_elem;
        used_elem.id = head_idx;
        used_elem.len = total_data_bytes;

        uint64_t used_elem_pa = blk_dev.queue_device + 4 + (used_idx % blk_dev.queue_size) * sizeof(used_elem);
        guest_write_fn(used_elem_pa, &used_elem, sizeof(used_elem));
        used_idx++;

        blk_dev.last_avail_idx++;
        blk_dev.bytes_transferred += total_data_bytes;
    }

    /* Update used ring index */
    guest_write_fn(blk_dev.queue_device + 2, &used_idx, 2);

    /* Assert Queue Interrupt in ISR and signal virtual interrupt to guest */
    blk_dev.isr_status |= 1;
    blk_dev.last_irq = blk_dev.irq_line;
    monitor_inject_virq(blk_dev.irq_line);
}

/* ---- PCIe BAR0 MMIO Register Access -------------------------------------- */
static void put_le(uint8_t *buf, unsigned off, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) buf[off + i] = (uint8_t)(v >> (8 * i));
}

int storage_bar0_access(uint64_t off, unsigned size, int write, uint64_t *value)
{
    if (!blk_dev.is_active) return 0;
    blk_dev.bar0_accesses++;
    log_str("EL2: storage: BAR0 access off ");
    log_hex(off);
    if (write) { log_str(" write "); log_hex(*value); log_str("\n"); }
    else log_str(" read\n");

    /* ---- Common Configuration Block (0x0000 - 0x0037) ---- */
    if (off < 0x1000) {
        if (write) {
            uint64_t v = *value;
            switch (off) {
            case 0x00: blk_dev.device_features_sel = (uint32_t)v; break;
            case 0x08: blk_dev.driver_features_sel = (uint32_t)v; break;
            case 0x0c:
                if (blk_dev.driver_features_sel == 0)
                    blk_dev.driver_features = (blk_dev.driver_features & ~0xFFFFFFFFull) | (uint32_t)v;
                else if (blk_dev.driver_features_sel == 1)
                    blk_dev.driver_features = (blk_dev.driver_features & 0xFFFFFFFFull) | (((uint64_t)v) << 32);
                break;
            case 0x14: blk_dev.device_status = (uint8_t)v; break;
            case 0x16: blk_dev.queue_select = (uint16_t)v; break;
            case 0x18: blk_dev.queue_size = (uint16_t)v; break;
            case 0x1c: blk_dev.queue_enable = (uint16_t)v; break;
            case 0x20:
                if (size == 8) blk_dev.queue_desc = v;
                else blk_dev.queue_desc = (blk_dev.queue_desc & ~0xFFFFFFFFull) | (uint32_t)v;
                break;
            case 0x24: blk_dev.queue_desc = (blk_dev.queue_desc & 0xFFFFFFFFull) | (v << 32); break;
            case 0x28:
                if (size == 8) blk_dev.queue_driver = v;
                else blk_dev.queue_driver = (blk_dev.queue_driver & ~0xFFFFFFFFull) | (uint32_t)v;
                break;
            case 0x2c: blk_dev.queue_driver = (blk_dev.queue_driver & 0xFFFFFFFFull) | (v << 32); break;
            case 0x30:
                if (size == 8) blk_dev.queue_device = v;
                else blk_dev.queue_device = (blk_dev.queue_device & ~0xFFFFFFFFull) | (uint32_t)v;
                break;
            case 0x34: blk_dev.queue_device = (blk_dev.queue_device & 0xFFFFFFFFull) | (v << 32); break;
            }
            return 1;
        }

        /* Read Common Config */
        uint64_t res = 0;
        switch (off) {
        case 0x00: res = blk_dev.device_features_sel; break;
        case 0x04:
            if (blk_dev.device_features_sel == 0) {
                res = VIRTIO_BLK_F_SIZE_MAX | VIRTIO_BLK_F_SEG_MAX |
                      VIRTIO_BLK_F_BLK_SIZE | VIRTIO_BLK_F_FLUSH |
                      VIRTIO_BLK_F_ZONED;
            } else if (blk_dev.device_features_sel == 1) {
                res = (uint32_t)(VIRTIO_F_VERSION_1 >> 32);
            }
            break;
        case 0x08: res = blk_dev.driver_features_sel; break;
        case 0x0c:
            res = (blk_dev.driver_features_sel == 0) ? (uint32_t)blk_dev.driver_features :
                                                       (uint32_t)(blk_dev.driver_features >> 32);
            break;
        case 0x12: res = 1; break; /* num_queues = 1 */
        case 0x14: res = blk_dev.device_status; break;
        case 0x15: res = 0; break; /* config_generation = 0 */
        case 0x16: res = blk_dev.queue_select; break;
        case 0x18: res = blk_dev.queue_size; break;
        case 0x1c: res = blk_dev.queue_enable; break;
        case 0x1e: res = 0; break; /* queue_notify_off = 0 */
        case 0x20: res = (size == 8) ? blk_dev.queue_desc : (uint32_t)blk_dev.queue_desc; break;
        case 0x24: res = (uint32_t)(blk_dev.queue_desc >> 32); break;
        case 0x28: res = (size == 8) ? blk_dev.queue_driver : (uint32_t)blk_dev.queue_driver; break;
        case 0x2c: res = (uint32_t)(blk_dev.queue_driver >> 32); break;
        case 0x30: res = (size == 8) ? blk_dev.queue_device : (uint32_t)blk_dev.queue_device; break;
        case 0x34: res = (uint32_t)(blk_dev.queue_device >> 32); break;
        default: res = 0; break;
        }
        *value = res;
        return 1;
    }

    /* ---- Queue Notification (0x1000) ---- */
    if (off >= BAR0_OFF_NOTIFY && off < BAR0_OFF_NOTIFY + BAR0_SIZE_NOTIFY) {
        if (write) {
            process_virtqueue();
        } else {
            *value = 0;
        }
        return 1;
    }

    /* ---- ISR Status (0x2000) ---- */
    if (off >= BAR0_OFF_ISR && off < BAR0_OFF_ISR + BAR0_SIZE_ISR) {
        if (!write) {
            *value = blk_dev.isr_status;
            blk_dev.isr_status = 0; /* Read to clear */
        }
        return 1;
    }

    /* ---- Device-Specific Configuration (0x3000 - 0x3047) ----
     * struct virtio_blk_config, little-endian. AppleVirtIOPCITransport reads
     * it one byte at a time (QEMU smoke 2026-10-30, OBSERVED), so the model
     * answers any width at any offset from a byte image rather than only
     * aligned fields. */
    if (off >= BAR0_OFF_DEVICE && off < BAR0_OFF_DEVICE + BAR0_SIZE_DEVICE) {
        uint32_t dev_off = (uint32_t)(off - BAR0_OFF_DEVICE);
        if (write) return 1; /* Device config is read-only to guest */
        if (size > 8 || dev_off + size > BAR0_SIZE_DEVICE) return 0;

        uint8_t cfg[BAR0_SIZE_DEVICE] = { 0 };
        put_le(cfg, 0x00, blk_dev.disk_size / 512ull, 8); /* capacity, 512 B sectors */
        put_le(cfg, 0x08, 0x100000, 4);                   /* size_max = 1 MiB */
        put_le(cfg, 0x0c, 128, 4);                        /* seg_max */
        put_le(cfg, 0x14, 512, 4);                        /* blk_size */
        cfg[0x20] = 1;                                    /* writeback */
        put_le(cfg, 0x22, 1, 2);                          /* num_queues */
        /* Apple variant identifier in max_secure_erase_sectors */
        put_le(cfg, 0x3c, VM_APPLE_VIRTIO_BLK_VARIANT_ROOT, 4);

        uint64_t res = 0;
        for (unsigned i = 0; i < size; i++)
            res |= (uint64_t)cfg[dev_off + i] << (8 * i);
        *value = res;
        return 1;
    }

    return 0;
}

/* ---- Backdoor Interface (BDIF) Panic Coredump Sink ----------------------- */
int storage_bdif_cmd(uint64_t devid, uint64_t cmd_pa)
{
    /* QEMU hw/vmapple/bdif.c vblk_cmd:
     * Request struct at cmd_pa:
     *   req.sector.addr (+0), req.sector.len (+8), req.sector.flags (+12)
     *   req.data.addr (+16), req.data.len (+24), req.data.flags (+28)
     *   req.retval.addr (+32), req.retval.len (+40), req.retval.flags (+44)
     *
     * In Run 45, the panicked kernel writes its coredump to devid 0x10000 (AUX).
     * Absorb the coredump cleanly and write VBLK_RET_SUCCESS (0) to retval.addr. */
    (void)devid;
    if (cmd_pa && (guest_read_fn != default_guest_read || cmd_pa >= 0x100000ull)) {
        uint64_t retval_pa = 0;
        guest_read_fn(&retval_pa, cmd_pa + 32, 8);
        if (retval_pa && (guest_write_fn != default_guest_write || retval_pa >= 0x100000ull)) {
            uint8_t ret_ok = 0;
            guest_write_fn(retval_pa, &ret_ok, 1);
        }
    }
    return 1;
}

/* ---- Diagnostic Rendering ------------------------------------------------ */
static void diag_append_str(char *buf, unsigned *pos, unsigned max_len, const char *s)
{
    while (*s && *pos + 1 < max_len) buf[(*pos)++] = *s++;
}

static void diag_append_hex(char *buf, unsigned *pos, unsigned max_len, uint64_t val, int nibbles)
{
    for (int i = nibbles - 1; i >= 0 && *pos + 1 < max_len; i--)
        buf[(*pos)++] = "0123456789ABCDEF"[(val >> (4 * i)) & 15];
}

static void diag_append_dec(char *buf, unsigned *pos, unsigned max_len, uint32_t val)
{
    char tmp[10];
    int n = 0;
    if (val == 0) {
        if (*pos + 1 < max_len) buf[(*pos)++] = '0';
        return;
    }
    while (val > 0 && n < 10) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (int i = n - 1; i >= 0 && *pos + 1 < max_len; i--)
        buf[(*pos)++] = tmp[i];
}

void storage_get_diag(struct storage_diag *diag)
{
    if (!diag) return;
    diag->is_active = blk_dev.is_active;
    diag->disk_pa = blk_dev.disk_pa;
    diag->disk_size = blk_dev.disk_size;
    diag->bar0_accesses = blk_dev.bar0_accesses;
    diag->vq_requests = blk_dev.vq_requests;
    diag->last_irq = blk_dev.last_irq;
}

int storage_render_diag(char *buf, unsigned max_len)
{
    if (!buf || max_len < 32) return 0;
    unsigned k = 0;
    diag_append_str(buf, &k, max_len, "STORAGE: ACT=");
    diag_append_dec(buf, &k, max_len, (uint32_t)blk_dev.is_active);
    diag_append_str(buf, &k, max_len, " PA=");
    diag_append_hex(buf, &k, max_len, blk_dev.disk_pa, 16);
    diag_append_str(buf, &k, max_len, " SZ=");
    diag_append_hex(buf, &k, max_len, blk_dev.disk_size, 8);
    diag_append_str(buf, &k, max_len, " BAR0=");
    diag_append_dec(buf, &k, max_len, blk_dev.bar0_accesses);
    diag_append_str(buf, &k, max_len, " VQ=");
    diag_append_dec(buf, &k, max_len, blk_dev.vq_requests);
    diag_append_str(buf, &k, max_len, " IRQ=");
    diag_append_dec(buf, &k, max_len, blk_dev.last_irq);
    buf[k] = 0;
    return (int)k;
}
