/* SPDX-License-Identifier: MIT */
/* el2qual's readers of what the firmware hands over: the ACPI tables, the
 * device tree, the GOP modes, the memory map and SMBIOS. See report.h;
 * tools/test-el2qual.c exercises all of it on the host. */
#include "report.h"

/* ---- Firmware tables ----------------------------------------------------
 * ACPI tables are byte-packed and may sit at any alignment, and UEFI can
 * leave SCTLR.A set, so every multi-byte field is assembled from bytes. */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int same(const void *a, const char *b, size_t n)
{
    const uint8_t *x = a;
    for (size_t i = 0; i < n; i++)
        if (x[i] != (uint8_t)b[i]) return 0;
    return 1;
}

/* No firmware table is anywhere near this long; a larger length field is
 * corruption, and trusting it would walk arbitrary memory. */
#define ACPI_TABLE_LIMIT 0x100000u

static void parse_madt(const uint8_t *t, uint32_t len, struct el2q_acpi *o)
{
    o->have_madt = 1;
    for (uint32_t off = 44; off + 2 <= len;) {
        uint8_t type = t[off], size = t[off + 1];
        if (size < 2 || off + size > len) break;
        /* GICC: Flags at 12 (bit 0 Enabled), MPIDR at 68. Online-capable but
         * disabled CPUs are left alone: the firmware did not bring them up
         * for its own OS either. */
        if (type == 0x0b && size >= 76 && (le32(t + off + 12) & 1) &&
            o->cpu_count < EL2Q_MAX_CPUS)
            o->mpidr[o->cpu_count++] = le64(t + off + 68) & 0xff00ffffffull;
        if (type == 0x0c && size >= 21)               /* GICD: version at 20 */
            o->gic_version = t[off + 20];
        off += size;
    }
}

int el2q_acpi_parse(const void *rsdp_address, struct el2q_acpi *o)
{
    const uint8_t *rsdp = rsdp_address;
    *o = (struct el2q_acpi){0};
    if (!rsdp || !same(rsdp, "RSD PTR ", 8) || rsdp[15] < 2) return -1;
    const uint8_t *xsdt = (const uint8_t *)(uintptr_t)le64(rsdp + 24);
    if (!xsdt || !same(xsdt, "XSDT", 4)) return -1;
    uint32_t len = le32(xsdt + 4);
    if (len < 36 || len > ACPI_TABLE_LIMIT) return -1;
    for (uint32_t off = 36; off + 8 <= len; off += 8) {
        const uint8_t *t = (const uint8_t *)(uintptr_t)le64(xsdt + off);
        if (!t) continue;
        uint32_t tlen = le32(t + 4);
        if (tlen < 36 || tlen > ACPI_TABLE_LIMIT) continue;
        if (same(t, "APIC", 4)) parse_madt(t, tlen, o);
        if (same(t, "SPCR", 4) && tlen >= 52) {
            o->have_spcr = 1;
            o->uart_type = t[36];                  /* interface type */
            o->uart_space = t[40];                 /* GAS address space */
            o->uart_base = le64(t + 44);           /* GAS address */
        }
    }
    return 0;
}

/* slbounce switches to EL2 only when the device tree handed to the OS
 * disables the Adreno zap shader: EL2 Linux loads the GPU firmware itself,
 * so the gate is how slbounce knows the DT is one meant for EL2. It takes the
 * first node compatible with "qcom,adreno" and its "zap-shader" child, and
 * wants status "disabled" (slbounce src/bounce_main.c). Same rule here, so
 * the screen says before ExitBootServices whether the bounce can happen. */
enum { FDT_BEGIN_NODE = 1, FDT_END_NODE = 2, FDT_PROP = 3, FDT_NOP = 4, FDT_END = 9 };

/* The structure and strings blocks of a flattened device tree, each bounded
 * by the header's totalsize, which is itself bounded by limit. */
struct fdt_blocks {
    const uint8_t *strings;
    uint32_t off_struct, end, size_strings;
};

static int fdt_open(const uint8_t *b, size_t limit, struct fdt_blocks *f)
{
    if (limit < 40 || be32(b) != 0xd00dfeedu) return -1;
    uint32_t total = be32(b + 4), off_struct = be32(b + 8), off_strings = be32(b + 12);
    uint32_t size_strings = be32(b + 32), size_struct = be32(b + 36);
    if (total > limit || total < 40 || off_struct > total || off_strings > total ||
        size_struct > total - off_struct || size_strings > total - off_strings)
        return -1;
    *f = (struct fdt_blocks){b + off_strings, off_struct, off_struct + size_struct, size_strings};
    return 0;
}

enum el2q_dt_state el2q_fdt_zap_state(const void *fdt, size_t limit)
{
    const uint8_t *b = fdt;
    if (!b) return EL2Q_DT_NONE;
    struct fdt_blocks f;
    if (fdt_open(b, limit, &f)) return EL2Q_DT_BAD;
    const uint8_t *strings = f.strings;
    uint32_t size_strings = f.size_strings;
    int depth = 0, adreno_depth = -1, zap_depth = -1;
    enum el2q_dt_state zap = EL2Q_DT_ZAP_OKAY;       /* no status property: okay */
    for (uint32_t p = f.off_struct, end = f.end; p + 4 <= end;) {
        uint32_t token = be32(b + p);
        p += 4;
        if (token == FDT_BEGIN_NODE) {
            const uint8_t *name = b + p;
            uint32_t n = 0;
            while (p + n < end && name[n]) n++;
            if (p + n >= end) return EL2Q_DT_BAD;
            p += (n + 4) & ~3u;
            depth++;
            if (zap_depth < 0 && adreno_depth == depth - 1 && n >= 10 &&
                same(name, "zap-shader", 10) && (n == 10 || name[10] == '@'))
                zap_depth = depth;
        } else if (token == FDT_END_NODE) {
            if (depth == zap_depth) return zap;
            if (depth == adreno_depth) return EL2Q_DT_NO_ZAP;
            if (--depth < 0) return EL2Q_DT_BAD;
        } else if (token == FDT_PROP) {
            if (p + 8 > end) return EL2Q_DT_BAD;
            uint32_t len = be32(b + p), nameoff = be32(b + p + 4);
            const uint8_t *value = b + p + 8;
            p += 8;
            if (len > end - p || nameoff >= size_strings) return EL2Q_DT_BAD;
            p += (len + 3) & ~3u;
            const uint8_t *pname = strings + nameoff;
            size_t room = size_strings - nameoff;
            if (adreno_depth < 0 && room >= 11 && same(pname, "compatible", 11)) {
                /* An exact entry of the string list, as libfdt matches it. */
                for (uint32_t s = 0; s < len;) {
                    uint32_t e = s;
                    while (e < len && value[e]) e++;
                    if (e - s == 11 && same(value + s, "qcom,adreno", 11)) adreno_depth = depth;
                    s = e + 1;
                }
            }
            if (depth == zap_depth && room >= 7 && same(pname, "status", 7))
                zap = (len >= 9 && same(value, "disabled", 9)) ? EL2Q_DT_ZAP_DISABLED
                                                                : EL2Q_DT_ZAP_OKAY;
        } else if (token == FDT_END) {
            return EL2Q_DT_NO_ZAP;
        } else if (token != FDT_NOP) {
            return EL2Q_DT_BAD;
        }
    }
    return EL2Q_DT_BAD;
}

/* ---- Screen mode -----------------------------------------------------------
 * After ExitBootServices the only way to show anything is to write pixels to
 * a GOP framebuffer, and a mode may not have one. The gaokun3's firmware
 * leaves the screen in mode 7, a 2560x1600 landscape twin of the native
 * 1600x2560 panel that its RotateScreen driver serves through Blt only, and
 * reports framebuffer address 0 for it (gaokun-android
 * docs/hw/gk3probe-e3-20261005.txt, docs/boot-entry-design.md §2.1). Its
 * native mode 0 has the same area and a lower number, so it is tried first.
 * Whether a candidate has an address is known only after SetMode, so this
 * orders the candidates and el2qual.c tries them. */

unsigned el2q_gop_order(const struct el2q_gop_mode *modes, unsigned count, unsigned current,
                        unsigned order[EL2Q_GOP_MODES])
{
    unsigned n = 0;
    if (count > EL2Q_GOP_MODES) count = EL2Q_GOP_MODES;
    for (unsigned i = 0; i < count; i++) {
        const struct el2q_gop_mode *m = &modes[i];
        if (i == current || !m->valid || (m->format != 0 && m->format != 1) ||
            !m->width || !m->height)
            continue;
        uint64_t area = (uint64_t)m->width * m->height;
        /* Insertion by area, larger first; a later index never passes an
         * equal area, so ties keep ascending mode numbers. */
        unsigned at = n;
        while (at && (uint64_t)modes[order[at - 1]].width * modes[order[at - 1]].height < area) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
        n++;
    }
    return n;
}

/* When no GOP mode has an address, the device tree is the other record of
 * the scanout buffer: the gaokun3's has /chosen/framebuffer@c6200000, a
 * 1600x2560 a8r8g8b8 simple-framebuffer, and the firmware's memory map
 * marks the 36 MiB around it Reserved (gaokun-android gk3probe log). A wrong
 * address would be a write into memory el2qual does not own, so the node
 * must be self-consistent here and lie inside one Reserved descriptor
 * (el2q_memmap_reserved) before el2qual.c draws on it. */

static int string_in_list(const uint8_t *v, uint32_t len, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < len;) {
        uint32_t e = i;
        while (e < len && v[e]) e++;
        if (e - i == n && same(v + i, s, n)) return 1;
        i = e + 1;
    }
    return 0;
}

static uint64_t cells_at(const uint8_t *v, unsigned count)
{
    uint64_t x = 0;
    for (unsigned i = 0; i < count; i++) x = x << 32 | be32(v + 4 * i);
    return x;
}

struct sfb_node {
    int compatible, disabled;
    const uint8_t *reg, *format;
    uint32_t reg_len, format_len, width, height, stride, have;  /* have: width|height|stride */
};

static enum el2q_sfb_state sfb_check(const struct sfb_node *n, unsigned ac, unsigned sc,
                                     struct el2q_simplefb *out)
{
    if (ac < 1 || ac > 2 || sc < 1 || sc > 2 || !n->reg || n->reg_len < 4 * (ac + sc))
        return EL2Q_SFB_BAD_DT;
    struct el2q_simplefb fb = {cells_at(n->reg, ac), cells_at(n->reg + 4 * ac, sc),
                               n->width, n->height, n->stride, 0};
    /* The 32 bpp formats of the binding; the first two are BGRX in memory,
     * the GOP layout el2q_render calls bgr. */
    static const struct { char name[9]; uint32_t bgr; } formats[] = {
        {"a8r8g8b8", 1}, {"x8r8g8b8", 1}, {"a8b8g8r8", 0}, {"x8b8g8r8", 0},
    };
    unsigned f = 0;
    while (f < 4 && !(n->format && n->format_len == 9 && same(n->format, formats[f].name, 9))) f++;
    if (f == 4) return EL2Q_SFB_FORMAT;
    fb.bgr = formats[f].bgr;
    if (n->have != 7 || !fb.base || (fb.base & 3) || fb.base + fb.size < fb.base ||
        !fb.width || !fb.height || fb.width > 16384 || fb.height > 16384 ||
        (fb.stride & 3) || fb.stride < fb.width * 4 || (uint64_t)fb.stride * fb.height > fb.size)
        return EL2Q_SFB_GEOMETRY;
    *out = fb;
    return EL2Q_SFB_OK;
}

enum el2q_sfb_state el2q_fdt_simplefb(const void *fdt, size_t limit, struct el2q_simplefb *out)
{
    const uint8_t *b = fdt;
    if (!b) return EL2Q_SFB_NO_DT;
    struct fdt_blocks f;
    if (fdt_open(b, limit, &f)) return EL2Q_SFB_BAD_DT;
    /* The root node is the first node, at depth 1: /chosen is at 2 and the
     * framebuffer nodes, its children, at 3. */
    enum { CHOSEN = 2, CHILD = 3 };
    int depth = 0, chosen = 0;
    unsigned ac = 2, sc = 1;                    /* the devicetree spec's defaults */
    enum el2q_sfb_state seen = EL2Q_SFB_NO_NODE;
    struct sfb_node n = {0};
    for (uint32_t p = f.off_struct; p + 4 <= f.end;) {
        uint32_t token = be32(b + p);
        p += 4;
        if (token == FDT_BEGIN_NODE) {
            const uint8_t *name = b + p;
            uint32_t len = 0;
            while (p + len < f.end && name[len]) len++;
            if (p + len >= f.end) return EL2Q_SFB_BAD_DT;
            p += (len + 4) & ~3u;
            depth++;
            if (depth == CHOSEN) chosen = len == 6 && same(name, "chosen", 6);
            if (chosen && depth == CHILD) n = (struct sfb_node){0};
        } else if (token == FDT_END_NODE) {
            if (chosen && depth == CHILD && n.compatible && !n.disabled) {
                /* Keep a failing node's reason; a later one may still do. */
                seen = sfb_check(&n, ac, sc, out);
                if (seen == EL2Q_SFB_OK) return seen;
            }
            if (chosen && depth == CHOSEN) return seen;
            if (--depth < 0) return EL2Q_SFB_BAD_DT;
        } else if (token == FDT_PROP) {
            if (p + 8 > f.end) return EL2Q_SFB_BAD_DT;
            uint32_t len = be32(b + p), nameoff = be32(b + p + 4);
            const uint8_t *v = b + p + 8;
            p += 8;
            if (len > f.end - p || nameoff >= f.size_strings) return EL2Q_SFB_BAD_DT;
            p += (len + 3) & ~3u;
            if (!chosen) continue;
            const uint8_t *name = f.strings + nameoff;
            size_t room = f.size_strings - nameoff;
#define PROP_IS(s) (room >= sizeof(s) && same(name, s, sizeof(s)))
            if (depth == CHOSEN && len == 4 && PROP_IS("#address-cells")) ac = be32(v);
            if (depth == CHOSEN && len == 4 && PROP_IS("#size-cells")) sc = be32(v);
            if (depth != CHILD) continue;
            if (PROP_IS("compatible")) n.compatible = string_in_list(v, len, "simple-framebuffer", 18);
            if (PROP_IS("status"))
                n.disabled = !((len == 5 && same(v, "okay", 5)) || (len == 3 && same(v, "ok", 3)));
            if (PROP_IS("reg")) { n.reg = v; n.reg_len = len; }
            if (PROP_IS("format")) { n.format = v; n.format_len = len; }
            if (len == 4 && PROP_IS("width")) { n.width = be32(v); n.have |= 1; }
            if (len == 4 && PROP_IS("height")) { n.height = be32(v); n.have |= 2; }
            if (len == 4 && PROP_IS("stride")) { n.stride = be32(v); n.have |= 4; }
#undef PROP_IS
        } else if (token == FDT_END) {
            return seen;
        } else if (token != FDT_NOP) {
            return EL2Q_SFB_BAD_DT;
        }
    }
    return EL2Q_SFB_BAD_DT;
}

/* EFI_MEMORY_DESCRIPTOR (UEFI 7.2): Type at 0, PhysicalStart at 8,
 * NumberOfPages at 24; descriptors are desc_size apart, which may exceed the
 * structure (48 on the gaokun3). EfiReservedMemoryType is 0. */
int el2q_memmap_reserved(const void *map, uint64_t map_size, uint64_t desc_size,
                         uint64_t base, uint64_t size)
{
    const uint8_t *m = map;
    if (!m || desc_size < 40 || !size || base + size < base) return 0;
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        uint64_t start = le64(m + off + 8), pages = le64(m + off + 24);
        if (le32(m + off) != 0 || pages > UINT64_MAX >> 12) continue;
        uint64_t end = start + (pages << 12);
        if (end >= start && base >= start && base + size <= end) return 1;
    }
    return 0;
}

/* The same descriptors, for el2q_s2w_region (s2walk.h): every one with
 * pages that overlaps [base, end) is counted, and the lowest-starting
 * EL2Q_S2W_MAP_ENTRIES kept in address order, since UEFI does not promise
 * a sorted map. Attribute is at 32. */
void el2q_memmap_region(const void *map, uint64_t map_size, uint64_t desc_size,
                        uint64_t base, uint64_t end, struct el2q_s2w_region *out)
{
    *out = (struct el2q_s2w_region){.state = EL2Q_S2W_REGION_NO_MAP, .base = base, .end = end};
    const uint8_t *m = map;
    if (!m || desc_size < 40 || !map_size) return;
    out->state = EL2Q_S2W_REGION_READ;
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        uint64_t start = le64(m + off + 8), pages = le64(m + off + 24);
        if (!pages || pages > UINT64_MAX >> 12 || start + (pages << 12) < start) continue;
        if (start >= end || start + (pages << 12) <= base) continue;
        struct el2q_s2w_map_entry e = {le32(m + off), start, pages, le64(m + off + 32)};
        unsigned n = out->count < EL2Q_S2W_MAP_ENTRIES ? out->count : EL2Q_S2W_MAP_ENTRIES;
        out->count++;
        if (n == EL2Q_S2W_MAP_ENTRIES && start >= out->e[n - 1].start) continue;
        if (n == EL2Q_S2W_MAP_ENTRIES) n--;
        while (n && out->e[n - 1].start > start) {
            out->e[n] = out->e[n - 1];
            n--;
        }
        out->e[n] = e;
    }
}

const char *el2q_sfb_words(uint32_t state)
{
    static const char *const words[] = {
        [EL2Q_SFB_NOT_TRIED] = "NOT TRIED",
        [EL2Q_SFB_NO_DT] = "NO DEVICE TREE",
        [EL2Q_SFB_BAD_DT] = "DEVICE TREE UNREADABLE",
        [EL2Q_SFB_NO_NODE] = "NO /CHOSEN SIMPLE-FRAMEBUFFER",
        [EL2Q_SFB_FORMAT] = "UNSUPPORTED PIXEL FORMAT",
        [EL2Q_SFB_GEOMETRY] = "ADDRESS, SIZE OR STRIDE INCONSISTENT",
        [EL2Q_SFB_NO_MAP] = "NO MEMORY MAP",
        [EL2Q_SFB_NOT_RESERVED] = "NOT INSIDE ONE RESERVED REGION",
        [EL2Q_SFB_OK] = "OK",
    };
    return state < sizeof words / sizeof words[0] ? words[state] : "?";
}

/* ---- SMBIOS (DMTF DSP0134) ------------------------------------------------
 * The SMBIOS 3 entry point ("_SM3_") gives its length at 6, the version at 7
 * and 8, the maximum structure table size at 0x0c and the table's address at
 * 0x10. The 2.1 one ("_SM_") gives its length at 5 and the version at 6 and
 * 7, then embeds a "_DMI_" entry at 0x10 with its own checksum over 15 bytes,
 * the table length at 0x16 and its address at 0x18. Each structure is a
 * header (type, formatted length, handle), the formatted area, then a set of
 * NUL-terminated strings ended by a second NUL; string 1 is the first, and 0
 * means none. Like the ACPI tables, everything is read byte by byte. */

#define SMBIOS_TABLE_MAX (1u << 20)

static int sums_to_zero(const uint8_t *p, unsigned n)
{
    uint8_t s = 0;
    while (n--) s = (uint8_t)(s + *p++);
    return s == 0;
}

/* String `index` of a structure's set [set, end), where end is just past the
 * set's last NUL. Only printable ASCII is copied, at most EL2Q_FW_TEXT. */
static void fw_string(const uint8_t *set, const uint8_t *end, unsigned index,
                      char out[EL2Q_FW_TEXT + 1])
{
    unsigned n = 0;
    for (unsigned i = 1; index && set < end && *set; i++) {
        const uint8_t *s = set;
        while (set < end && *set) set++;
        if (i == index) {
            for (; s < set && n < EL2Q_FW_TEXT; s++)
                out[n++] = *s >= 0x20 && *s < 0x7f ? (char)*s : '?';
            break;
        }
        set++;
    }
    out[n] = 0;
}

enum el2q_smbios_state el2q_smbios_parse(const void *entry, struct el2q_firmware *o)
{
    const uint8_t *e = entry;
    uint64_t address;
    uint32_t size;
    o->state = EL2Q_SMBIOS_NONE;
    o->major = o->minor = 0;
    o->vendor[0] = o->version[0] = o->date[0] = o->maker[0] = o->product[0] = 0;
    if (!e) return EL2Q_SMBIOS_NONE;
    o->state = EL2Q_SMBIOS_BAD;
    if (same(e, "_SM3_", 5)) {
        if (e[6] < 0x18 || !sums_to_zero(e, e[6])) return EL2Q_SMBIOS_BAD;
        o->major = e[7];
        o->minor = e[8];
        size = le32(e + 0x0c);
        address = le64(e + 0x10);
    } else if (same(e, "_SM_", 4)) {
        if (e[5] < 0x1f || !sums_to_zero(e, e[5]) || !same(e + 0x10, "_DMI_", 5) ||
            !sums_to_zero(e + 0x10, 0x0f))
            return EL2Q_SMBIOS_BAD;
        o->major = e[6];
        o->minor = e[7];
        size = (uint32_t)e[0x16] | (uint32_t)e[0x17] << 8;
        address = le32(e + 0x18);
    } else {
        return EL2Q_SMBIOS_BAD;
    }
    if (!address || size < 4 || size > SMBIOS_TABLE_MAX) return EL2Q_SMBIOS_BAD;

    const uint8_t *p = (const uint8_t *)(uintptr_t)address, *end = p + size;
    int bios = 0, system = 0;
    while (end - p >= 4) {
        unsigned type = p[0], length = p[1];
        if (length < 4 || length > (size_t)(end - p)) return EL2Q_SMBIOS_BAD;
        const uint8_t *set = p + length, *q = set;
        while (end - q >= 2 && (q[0] || q[1])) q++;
        if (end - q < 2) return EL2Q_SMBIOS_BAD;
        if (type == 0 && length >= 0x09 && !bios) {
            fw_string(set, q + 1, p[4], o->vendor);
            fw_string(set, q + 1, p[5], o->version);
            fw_string(set, q + 1, p[8], o->date);
            bios = 1;
        } else if (type == 1 && length >= 0x08 && !system) {
            /* Manufacturer and Product Name only; 07h is the serial number. */
            fw_string(set, q + 1, p[4], o->maker);
            fw_string(set, q + 1, p[5], o->product);
            system = 1;
        }
        if (type == 127) break;
        p = q + 2;
    }
    o->state = bios ? EL2Q_SMBIOS_OK : EL2Q_SMBIOS_NO_BIOS;
    return o->state;
}
