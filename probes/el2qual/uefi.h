/* SPDX-License-Identifier: MIT */
/* The UEFI interfaces el2qual.c calls (UEFI 2.10 specification, only what
 * is used): the types and table layouts, each layout's offsets checked
 * against the specification's section. el2qual is the only user; the
 * GUIDs it looks up stay in el2qual.c. */
#ifndef EL2QUAL_UEFI_H
#define EL2QUAL_UEFI_H

#include <stddef.h>
#include <stdint.h>


typedef uint64_t efi_status;
typedef uint16_t char16;
#define EFI_ERROR(s) ((int64_t)(s) < 0)

struct efi_guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };

struct efi_table_header { uint64_t signature; uint32_t revision, size, crc32, reserved; };

struct efi_text_output {
    void *reset;
    efi_status (*output_string)(struct efi_text_output *self, const char16 *text);
};

struct efi_config_table { struct efi_guid guid; void *table; };

struct efi_boot_services {
    struct efi_table_header header;
    void *raise_tpl, *restore_tpl;
    efi_status (*allocate_pages)(uint32_t type, uint32_t memory_type, uint64_t pages,
                                 uint64_t *address);
    void *free_pages;
    efi_status (*get_memory_map)(uint64_t *size, void *map, uint64_t *key,
                                 uint64_t *descriptor_size, uint32_t *descriptor_version);
    void *allocate_pool, *free_pool, *create_event, *set_timer, *wait_for_event;
    void *signal_event, *close_event, *check_event, *install_protocol;
    void *reinstall_protocol, *uninstall_protocol;
    efi_status (*handle_protocol)(void *handle, const struct efi_guid *guid, void **out);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table, *load_image, *start_image, *exit, *unload_image;
    efi_status (*exit_boot_services)(void *image, uint64_t map_key);
    void *get_next_monotonic_count, *stall;
    efi_status (*set_watchdog_timer)(uint64_t timeout, uint64_t code, uint64_t size,
                                     const char16 *data);
    void *connect_controller, *disconnect_controller, *open_protocol, *close_protocol;
    void *open_protocol_information, *protocols_per_handle, *locate_handle_buffer;
    efi_status (*locate_protocol)(const struct efi_guid *guid, void *registration, void **out);
};
_Static_assert(offsetof(struct efi_boot_services, allocate_pages) == 40, "UEFI 7.2");
_Static_assert(offsetof(struct efi_boot_services, get_memory_map) == 56, "UEFI 7.2");
_Static_assert(offsetof(struct efi_boot_services, handle_protocol) == 152, "UEFI 7.3");
_Static_assert(offsetof(struct efi_boot_services, exit_boot_services) == 232, "UEFI 7.4");
_Static_assert(offsetof(struct efi_boot_services, set_watchdog_timer) == 256, "UEFI 7.5");
_Static_assert(offsetof(struct efi_boot_services, locate_protocol) == 320, "UEFI 7.3");

struct efi_system_table {
    struct efi_table_header header;
    char16 *vendor;
    uint32_t revision;
    void *console_in_handle, *con_in, *console_out_handle;
    struct efi_text_output *con_out;
    void *standard_error_handle, *std_err, *runtime_services;
    struct efi_boot_services *boot_services;
    uint64_t table_count;
    struct efi_config_table *tables;
};
_Static_assert(offsetof(struct efi_system_table, con_out) == 64, "UEFI 4.3");
_Static_assert(offsetof(struct efi_system_table, boot_services) == 96, "UEFI 4.3");
_Static_assert(offsetof(struct efi_system_table, tables) == 112, "UEFI 4.3");

struct efi_loaded_image {
    uint32_t revision;
    void *parent, *system_table, *device, *file_path, *reserved;
    uint32_t options_size;
    void *options;
    void *image_base;
    uint64_t image_size;
};
_Static_assert(offsetof(struct efi_loaded_image, options_size) == 48, "UEFI 9.1");
_Static_assert(offsetof(struct efi_loaded_image, image_base) == 64, "UEFI 9.1");

struct efi_gop_mode_info {
    uint32_t version, width, height, format, mask[4], stride;
};
struct efi_gop_mode {
    uint32_t max_mode, mode;
    struct efi_gop_mode_info *info;
    uint64_t info_size, framebuffer, framebuffer_size;
};
struct efi_gop {
    efi_status (*query_mode)(struct efi_gop *self, uint32_t mode, uint64_t *info_size,
                             struct efi_gop_mode_info **info);
    efi_status (*set_mode)(struct efi_gop *self, uint32_t mode);
    void *blt;
    struct efi_gop_mode *mode;
};
_Static_assert(offsetof(struct efi_gop, mode) == 24, "UEFI 12.9");
_Static_assert(offsetof(struct efi_gop_mode, framebuffer) == 24, "UEFI 12.9");

enum { GOP_RGBX = 0, GOP_BGRX = 1 };

#endif
