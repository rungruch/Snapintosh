/* SPDX-License-Identifier: MIT */
/*
 * Host test suite for VMApple VirtIO Block Storage Controller (Phase 4 P1).
 * Tests PCI configuration enumeration, BAR0 register contracts, Apple variant
 * tagging, VirtIO queue descriptor processing, and BDIF panic sink.
 * Compiled with -fsanitize=address,undefined.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "vmapple-smoke/storage.h"

static unsigned last_virq = 0;
int monitor_inject_virq(unsigned intid)
{
    last_virq = intid;
    return 1;
}

void mon_clean_range(uint64_t pa, uint64_t size)
{
    (void)pa;
    (void)size;
}

void mon_uart_putc(char c)
{
    (void)c;
}

static int checks, failures;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

/* Synthetic guest RAM buffer for testing DMA */
#define GUEST_RAM_SIZE (4 * 1024 * 1024)
static uint8_t guest_ram[GUEST_RAM_SIZE];

static void fake_guest_read(void *dst, uint64_t pa, uint64_t len)
{
    assert(pa + len <= GUEST_RAM_SIZE);
    memcpy(dst, &guest_ram[pa], len);
}

static void fake_guest_write(uint64_t pa, const void *src, uint64_t len)
{
    assert(pa + len <= GUEST_RAM_SIZE);
    memcpy(&guest_ram[pa], src, len);
}

/* Split Virtqueue Structures for Host Test */
struct test_vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct test_vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[128];
};

struct test_vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct test_vring_used {
    uint16_t flags;
    uint16_t idx;
    struct test_vring_used_elem ring[128];
};

struct test_virtio_blk_outhdr {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
};

int main(void)
{
    printf("Starting VMApple storage host tests...\n");

    /* Create synthetic 1 MiB disk with test pattern */
    uint64_t disk_size = 1024 * 1024;
    uint8_t *synthetic_disk = calloc(1, disk_size);
    assert(synthetic_disk != NULL);

    for (size_t i = 0; i < disk_size; i++) {
        synthetic_disk[i] = (uint8_t)((i * 37) & 0xFF);
    }

    storage_set_mem_access(fake_guest_read, fake_guest_write);
    storage_init(synthetic_disk, disk_size, 0);

    CHECK(storage_is_active(), "storage device is active after init");

    /* 1. PCIe ECAM Configuration Space Probing */
    {
        uint64_t val = 0;

        /* Probing Bus 0, Dev 0, Func 0 (Host Bridge or absent): returns all-ones */
        storage_pcie_cfg_access(0x0000, 4, 0, &val);
        CHECK(val == 0xFFFFFFFFu, "PCIe slot 00:00.0 returns all-ones");

        /* Probing Bus 0, Dev 1, Func 0: Offset 0x00 (Vendor/Device ID) */
        storage_pcie_cfg_access(0x8000, 4, 0, &val);
        CHECK((val & 0xFFFF) == PCI_VENDOR_ID_VIRTIO, "Vendor ID matches VirtIO (0x1af4)");
        CHECK(((val >> 16) & 0xFFFF) == PCI_DEVICE_ID_VIRTIO_BLK,
              "Device ID matches VirtIO Block (0x1042)");

        /* Class code at offset 0x08 */
        storage_pcie_cfg_access(0x8008, 4, 0, &val);
        CHECK((val & 0xFF) == 0x01, "Revision ID is 0x01");
        CHECK(((val >> 8) & 0xFFFFFF) == PCI_CLASS_STORAGE_SCSI,
              "Class code matches SCSI Mass Storage (0x010000)");

        /* BAR0 at offset 0x10: initial value */
        storage_pcie_cfg_access(0x8010, 4, 0, &val);
        CHECK(val == MON_PCIE_BAR0_BASE, "BAR0 matches 0x50000000");

        /* Sizing BAR0: write 0xFFFFFFFF, expect 0xFFFF0000 (64 KiB size) */
        val = 0xFFFFFFFFu;
        storage_pcie_cfg_access(0x8010, 4, 1, &val);
        storage_pcie_cfg_access(0x8010, 4, 0, &val);
        CHECK(val == 0xFFFF0000u, "BAR0 sizing probe returns 0xFFFF0000 (64 KiB)");

        /* Reprogramming BAR0 to 0x50000000 */
        val = MON_PCIE_BAR0_BASE;
        storage_pcie_cfg_access(0x8010, 4, 1, &val);
        storage_pcie_cfg_access(0x8010, 4, 0, &val);
        CHECK(val == MON_PCIE_BAR0_BASE, "BAR0 reprogrammed to 0x50000000");

        /* Capabilities Pointer at offset 0x34 */
        storage_pcie_cfg_access(0x8034, 1, 0, &val);
        CHECK(val == 0x40, "Capabilities pointer points to 0x40");

        /* Capability 1: COMMON_CFG at 0x40 */
        storage_pcie_cfg_access(0x8040, 4, 0, &val);
        CHECK((val & 0xFF) == 0x09, "Cap 1 is Vendor-Specific");
        CHECK(((val >> 8) & 0xFF) == 0x50, "Cap 1 next is 0x50");
        CHECK(((val >> 16) & 0xFF) == 16, "Cap 1 len is 16");
        CHECK(((val >> 24) & 0xFF) == 1, "Cap 1 type is COMMON_CFG");

        /* Capability 2: NOTIFY_CFG at 0x50 */
        storage_pcie_cfg_access(0x8050, 4, 0, &val);
        CHECK(((val >> 24) & 0xFF) == 2, "Cap 2 type is NOTIFY_CFG");

        /* Capability 3: ISR_CFG at 0x64 */
        storage_pcie_cfg_access(0x8064, 4, 0, &val);
        CHECK(((val >> 24) & 0xFF) == 3, "Cap 3 type is ISR_CFG");

        /* Capability 4: DEVICE_CFG at 0x74 */
        storage_pcie_cfg_access(0x8074, 4, 0, &val);
        CHECK(((val >> 8) & 0xFF) == 0x00, "Cap 4 is last capability");
        CHECK(((val >> 24) & 0xFF) == 4, "Cap 4 type is DEVICE_CFG");
    }

    /* 2. BAR0 Device Configuration & Apple Variant Tag */
    {
        uint64_t val = 0;

        /* Capacity (sectors): 1 MiB / 512 = 2048 sectors */
        storage_bar0_access(0x3000, 8, 0, &val);
        CHECK(val == 2048, "Device capacity matches 2048 sectors");

        /* Block size */
        storage_bar0_access(0x3014, 4, 0, &val);
        CHECK(val == 512, "Block size is 512");

        /* Apple Variant Identifier at offset 0x303c (max_secure_erase_sectors) */
        storage_bar0_access(0x303c, 4, 0, &val);
        CHECK(val == VM_APPLE_VIRTIO_BLK_VARIANT_ROOT,
              "Apple variant is tagged ROOT (1) at offset 0x3c");

        /* AppleVirtIOPCITransport reads device config a byte at a time. */
        uint64_t cap = 0;
        for (unsigned i = 0; i < 8; i++) {
            uint64_t b = ~0ull;
            storage_bar0_access(0x3000 + i, 1, 0, &b);
            cap |= (b & 0xff) << (8 * i);
            if (b > 0xff) cap = ~0ull;
        }
        CHECK(cap == 2048, "Byte-wise capacity reads assemble 2048 sectors");
        val = ~0ull;
        storage_bar0_access(0x3015, 1, 0, &val);
        CHECK(val == 0x02, "Byte 1 of blk_size is 0x02 (512)");
        val = 0;
        storage_bar0_access(0x303c, 1, 0, &val);
        CHECK(val == VM_APPLE_VIRTIO_BLK_VARIANT_ROOT, "Byte-wise variant read");

        /* Faulty cases: a read that runs past the config block is refused. */
        CHECK(storage_bar0_access(0x3044, 8, 0, &val) == 0,
              "Device config read past 0x48 is rejected");
    }

    /* 3. BAR0 Common Configuration Registers */
    {
        uint64_t val = 0;

        /* Device features: select 0 */
        val = 0;
        storage_bar0_access(0x00, 4, 1, &val);
        storage_bar0_access(0x04, 4, 0, &val);
        CHECK((val & (1 << 6)) != 0, "BLK_SIZE feature offered");
        CHECK((val & (1 << 17)) != 0, "ZONED feature offered to satisfy config size");

        /* Device features: select 1 (VIRTIO_F_VERSION_1) */
        val = 1;
        storage_bar0_access(0x00, 4, 1, &val);
        storage_bar0_access(0x04, 4, 0, &val);
        CHECK((val & 1) != 0, "VERSION_1 feature offered");

        /* Device Status transitions: ACKNOWLEDGE -> DRIVER -> FEATURES_OK -> DRIVER_OK */
        val = 1; storage_bar0_access(0x14, 1, 1, &val);
        val = 2; storage_bar0_access(0x14, 1, 1, &val);
        val = 8; storage_bar0_access(0x14, 1, 1, &val);
        val = 4; storage_bar0_access(0x14, 1, 1, &val);

        storage_bar0_access(0x14, 1, 0, &val);
        CHECK(val == 4, "Device status is DRIVER_OK");

        /* Queue setup: Queue 0 */
        val = 0; storage_bar0_access(0x16, 2, 1, &val);
        storage_bar0_access(0x18, 2, 0, &val);
        CHECK(val == 128, "Default queue size is 128");

        /* Program Queue Addresses in Guest PA */
        uint64_t desc_pa = 0x10000;
        uint64_t avail_pa = 0x11000;
        uint64_t used_pa = 0x12000;

        storage_bar0_access(0x20, 8, 1, &desc_pa);
        storage_bar0_access(0x28, 8, 1, &avail_pa);
        storage_bar0_access(0x30, 8, 1, &used_pa);

        val = 1; storage_bar0_access(0x1c, 2, 1, &val); /* queue_enable = 1 */
    }

    /* 4. Virtqueue Request Execution: READ Sector 2 */
    {
        uint64_t desc_pa = 0x10000;
        uint64_t avail_pa = 0x11000;
        uint64_t used_pa = 0x12000;
        uint64_t hdr_pa = 0x20000;
        uint64_t data_pa = 0x21000;
        uint64_t status_pa = 0x22000;

        /* Write outhdr to guest RAM */
        struct test_virtio_blk_outhdr hdr = {
            .type = 0, /* VIRTIO_BLK_T_IN (READ) */
            .ioprio = 0,
            .sector = 2, /* 1024 bytes into disk */
        };
        fake_guest_write(hdr_pa, &hdr, sizeof(hdr));

        /* Set status byte to 0xFF initially */
        uint8_t status = 0xFF;
        fake_guest_write(status_pa, &status, 1);

        /* Set descriptor 0: Request Header (Read-only for device) */
        struct test_vring_desc d0 = {
            .addr = hdr_pa,
            .len = sizeof(hdr),
            .flags = 1, /* NEXT */
            .next = 1,
        };
        fake_guest_write(desc_pa + 0 * sizeof(d0), &d0, sizeof(d0));

        /* Set descriptor 1: Data Buffer (512 bytes, Device-write) */
        struct test_vring_desc d1 = {
            .addr = data_pa,
            .len = 512,
            .flags = 1 | 2, /* NEXT | WRITE */
            .next = 2,
        };
        fake_guest_write(desc_pa + 1 * sizeof(d1), &d1, sizeof(d1));

        /* Set descriptor 2: Status Byte (1 byte, Device-write) */
        struct test_vring_desc d2 = {
            .addr = status_pa,
            .len = 1,
            .flags = 2, /* WRITE */
            .next = 0,
        };
        fake_guest_write(desc_pa + 2 * sizeof(d2), &d2, sizeof(d2));

        /* Put descriptor 0 into Available Ring */
        struct test_vring_avail avail;
        avail.flags = 0;
        avail.idx = 1;
        avail.ring[0] = 0; /* Head is desc 0 */
        fake_guest_write(avail_pa, &avail, sizeof(avail));

        /* Initialize Used Ring */
        struct test_vring_used used;
        used.flags = 0;
        used.idx = 0;
        fake_guest_write(used_pa, &used, sizeof(used));

        /* Notify queue 0 */
        uint64_t notify_val = 0;
        storage_bar0_access(0x1000, 2, 1, &notify_val);

        /* Verify request completed */
        fake_guest_read(&status, status_pa, 1);
        CHECK(status == 0, "VirtIO Block Read returned VIRTIO_BLK_S_OK (0)");

        /* Verify data matches synthetic disk at sector 2 (offset 1024) */
        uint8_t read_buf[512];
        fake_guest_read(read_buf, data_pa, 512);
        CHECK(memcmp(read_buf, synthetic_disk + 1024, 512) == 0,
              "Read data exactly matches backing disk contents at sector 2");

        /* Verify Used Ring was updated */
        fake_guest_read(&used, used_pa, sizeof(used));
        CHECK(used.idx == 1, "Used ring index incremented to 1");
        CHECK(used.ring[0].id == 0, "Used ring entry id matches head descriptor (0)");
        CHECK(used.ring[0].len == 512, "Used ring entry length matches 512 bytes");

        /* Verify ISR status */
        uint64_t isr_val = 0;
        storage_bar0_access(0x2000, 1, 0, &isr_val);
        CHECK((isr_val & 1) != 0, "ISR queue interrupt bit set on completion");

        /* Reading ISR clears it */
        storage_bar0_access(0x2000, 1, 0, &isr_val);
        CHECK(isr_val == 0, "ISR read-to-clear clears interrupt status");
    }

    /* 5. Apple Proprietary Barrier Command (VIRTIO_BLK_T_APPLE_BARRIER = 0x10000) */
    {
        uint64_t desc_pa = 0x10000;
        uint64_t avail_pa = 0x11000;
        uint64_t hdr_pa = 0x20000;
        uint64_t status_pa = 0x22000;

        struct test_virtio_blk_outhdr hdr = {
            .type = 0x10000, /* VIRTIO_BLK_T_APPLE_BARRIER */
            .ioprio = 0,
            .sector = 0,
        };
        fake_guest_write(hdr_pa, &hdr, sizeof(hdr));

        uint8_t status = 0xFF;
        fake_guest_write(status_pa, &status, 1);

        struct test_vring_desc d0 = {
            .addr = hdr_pa,
            .len = sizeof(hdr),
            .flags = 1, /* NEXT */
            .next = 1,
        };
        fake_guest_write(desc_pa + 0 * sizeof(d0), &d0, sizeof(d0));

        struct test_vring_desc d1 = {
            .addr = status_pa,
            .len = 1,
            .flags = 2, /* WRITE */
            .next = 0,
        };
        fake_guest_write(desc_pa + 1 * sizeof(d1), &d1, sizeof(d1));

        struct test_vring_avail avail;
        avail.flags = 0;
        avail.idx = 2; /* Second request */
        avail.ring[1] = 0;
        fake_guest_write(avail_pa, &avail, sizeof(avail));

        uint64_t notify_val = 0;
        storage_bar0_access(0x1000, 2, 1, &notify_val);

        fake_guest_read(&status, status_pa, 1);
        CHECK(status == 0, "Apple Barrier request handled as successful no-op");
        CHECK(last_virq == 65, "Virtqueue request asserted INTx INTID 65 (interrupt-base 64, dev 1 INTA)");

        /* The kernel's interrupt-line write in config 0x3c selects the
         * INTID delivered; it reads back as written. */
        uint64_t line = 0x47;
        storage_pcie_cfg_access((1u << 15) | 0x3c, 1, 1, &line);
        line = 0;
        storage_pcie_cfg_access((1u << 15) | 0x3c, 1, 0, &line);
        CHECK(line == 0x47, "Interrupt line reads back the kernel's write");
        avail.idx = 3;
        avail.ring[2] = 0;
        fake_guest_write(avail_pa, &avail, sizeof(avail));
        storage_bar0_access(0x1000, 2, 1, &notify_val);
        CHECK(last_virq == 0x47, "Completion follows the programmed interrupt line");
        line = 65;
        storage_pcie_cfg_access((1u << 15) | 0x3c, 1, 1, &line);
    }

    /* 6. Backdoor Interface Panic Coredump Sink */
    {
        uint64_t cmd_pa = 0x30000;
        uint64_t retval_pa = 0x30100;

        /* Write cmd struct with retval at offset 32 */
        fake_guest_write(cmd_pa + 32, &retval_pa, 8);

        uint8_t ret = 0xFF;
        fake_guest_write(retval_pa, &ret, 1);

        int handled = storage_bdif_cmd(0x10000, cmd_pa);
        CHECK(handled == 1, "BDIF AUX cmd handled cleanly");

        fake_guest_read(&ret, retval_pa, 1);
        CHECK(ret == 0, "BDIF AUX retval set to VBLK_RET_SUCCESS (0)");
    }

    /* 7. Parameter Page Initialization (layout.h offsets 0x58 and 0x60) */
    {
        uint8_t fake_params[0x70];
        memset(fake_params, 0, sizeof(fake_params));
        uint64_t param_disk_pa = (uintptr_t)synthetic_disk;
        uint64_t param_disk_size = disk_size;
        memcpy(&fake_params[0x58], &param_disk_pa, sizeof(param_disk_pa));
        memcpy(&fake_params[0x60], &param_disk_size, sizeof(param_disk_size));

        storage_init_params((uintptr_t)fake_params);
        CHECK(storage_is_active() == 1, "storage_init_params activated disk controller");
    }

    free(synthetic_disk);

    if (failures) {
        printf("FAIL test-vmapple-storage: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS test-vmapple-storage: %d checks\n", checks);
    return 0;
}
