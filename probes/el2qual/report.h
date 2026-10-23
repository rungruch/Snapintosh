/* SPDX-License-Identifier: MIT */
/* el2qual's measurement record and everything that turns it into a verdict.
 *
 * Nothing here touches hardware: the firmware tables arrive as pointers, the
 * register values as numbers, and the output is text and pixels. That is what
 * lets tools/test-el2qual.c run all of it on the Mac against fixtures,
 * including the A16's measured registers, before anything runs on a device. */
#ifndef EL2QUAL_REPORT_H
#define EL2QUAL_REPORT_H

#include <stddef.h>
#include <stdint.h>

#define EL2Q_VERSION      1
#define EL2Q_MAX_CPUS     16
#define EL2Q_ID_COUNT     45    /* the allocated encodings probes/idregs.h reads */
#define EL2Q_PAC_TRIALS   4
#define EL2Q_TEXT_LINES   112
#define EL2Q_TEXT_COLS    64

/* One ID register: its encoding in the op0=3, op1=0, CRn=0 space (the same
 * (CRm, op2) pair probes/idregs.h's id_register() takes) and a short name. */
struct el2q_idreg {
    uint8_t crm, op2, aarch64;
    char name[11];
};
extern const struct el2q_idreg el2q_idregs[EL2Q_ID_COUNT];
int el2q_id_index(unsigned crm, unsigned op2);   /* -1 if not in the table */

/* Instructions executed behind a fixup: each either runs or raises an
 * exception at EL2, whose ESR is recorded. Executing is the evidence. */
enum el2q_probe {
    EL2Q_PROBE_RNDR,       /* MRS RNDR: FEAT_RNG */
    EL2Q_PROBE_VNCR,       /* MRS VNCR_EL2: FEAT_NV2 */
    EL2Q_PROBE_CNTPOFF,    /* MRS CNTPOFF_EL2: FEAT_ECV */
    EL2Q_PROBE_HFGRTR,     /* MRS HFGRTR_EL2: FEAT_FGT */
    EL2Q_PROBE_APIAKEY,    /* MRS APIAKeyLo_EL1: FEAT_PAuth key storage */
    EL2Q_PROBE_PACGA,      /* PACGA: FEAT_PACQARMA5/3 or IMPDEF generic auth */
    EL2Q_PROBES
};

enum el2q_cpu_state {
    EL2Q_CPU_UNTESTED,     /* not attempted (no EL2, no PSCI, or slots full) */
    EL2Q_CPU_PSCI_FAILED,  /* CPU_ON returned an error */
    EL2Q_CPU_NO_RESPONSE,  /* CPU_ON succeeded; the core never reported */
    EL2Q_CPU_WRONG_EL,     /* the core entered our code below EL2 */
    EL2Q_CPU_FAULT,        /* an unexpected exception at EL2 */
    EL2Q_CPU_DONE
};

struct el2q_cpu {
    uint64_t target_mpidr;           /* the affinity CPU_ON was given (MADT) */
    uint32_t state, done;            /* done: release-stored by the core */
    int64_t psci_on, psci_affinity;  /* CPU_ON result; AFFINITY_INFO afterwards */
    uint64_t early;                  /* entry marker the core stored, MMU off */
    uint64_t current_el, mpidr, hcr;
    uint64_t id[EL2Q_ID_COUNT];
    uint64_t probe_esr[EL2Q_PROBES], probe_value[EL2Q_PROBES];
    uint64_t pac_ran;                /* 0: skipped (nopac), every PAC field unset */
    uint64_t pac_in[EL2Q_PAC_TRIALS], pac_out[EL2Q_PAC_TRIALS];
    uint64_t pac_rt_esr, pac_rt_ran;
    uint64_t pac_rt[3];              /* PACIA, AUTIA of it, XPACI of it */
    uint64_t tgran[7];               /* probes/tgran16.c's out[] */
    uint64_t fault_esr, fault_elr, fault_far, fault_vector;
};

enum el2q_dt_state {
    EL2Q_DT_NONE,          /* no device tree configuration table */
    EL2Q_DT_BAD,           /* a table whose header does not parse */
    EL2Q_DT_NO_ZAP,        /* no zap-shader under a qcom,adreno node */
    EL2Q_DT_ZAP_OKAY,      /* zap-shader enabled: slbounce will not switch */
    EL2Q_DT_ZAP_DISABLED   /* the gate slbounce looks for before switching */
};

struct el2q_acpi {
    uint32_t have_madt, have_spcr;
    uint32_t cpu_count, gic_version;
    uint64_t mpidr[EL2Q_MAX_CPUS];
    uint32_t uart_type, uart_space;  /* SPCR interface type, GAS space ID */
    uint64_t uart_base;
};

struct el2q_run {
    uint64_t start_el, end_el;       /* CurrentEL>>2 at entry and after EBS */
    uint64_t ebs_status, ebs_tries;
    uint64_t hcr_entry, sctlr_entry; /* EL2 state as the bounce left it */
    uint32_t emulated_vhe;           /* the QEMU-only slbounce emulation ran */
    uint32_t dt_state;
    struct el2q_acpi acpi;
    int64_t psci_version;            /* -1 when PSCI was not reachable */
    uint32_t fb_width, fb_height, fb_stride, fb_format;
    uint32_t fb_mode, fb_first_mode; /* GOP mode drawn on; the one firmware left */
    uint64_t fb_base;                /* 0: nothing to draw on */
    uint32_t fb_source;              /* enum el2q_fb_source */
    uint32_t fb_dt;                  /* enum el2q_sfb_state of the DT fallback */
    uint64_t start_id[EL2Q_ID_COUNT];/* ID registers at the starting EL */
    uint32_t cpu_count;              /* slots used; slot 0 is the boot CPU */
    struct el2q_cpu cpu[EL2Q_MAX_CPUS];
};

enum el2q_color { EL2Q_WHITE, EL2Q_GREEN, EL2Q_RED, EL2Q_YELLOW, EL2Q_CYAN };

struct el2q_text {
    unsigned count;
    uint8_t color[EL2Q_TEXT_LINES];
    char line[EL2Q_TEXT_LINES][EL2Q_TEXT_COLS + 1];
};

/* Firmware tables. Both take the table's address as found in the UEFI
 * configuration table and read nothing beyond the lengths the tables
 * declare. */
int el2q_acpi_parse(const void *rsdp, struct el2q_acpi *out);
enum el2q_dt_state el2q_fdt_zap_state(const void *fdt, size_t limit);

/* A GOP mode as QueryMode describes it. valid is 0 when QueryMode failed. */
#define EL2Q_GOP_MODES 32
struct el2q_gop_mode { uint32_t width, height, format, valid; };

/* The modes to try, in order, when the current one has no framebuffer
 * address: valid 32 bpp RGB or BGR modes other than `current`, the largest
 * area first and, among equal areas, the lower mode number. Writes at most
 * EL2Q_GOP_MODES entries to order[] and returns how many. */
unsigned el2q_gop_order(const struct el2q_gop_mode *modes, unsigned count, unsigned current,
                        unsigned order[EL2Q_GOP_MODES]);

/* Where the screen came from: a GOP mode, or, when no GOP mode has a
 * framebuffer address, the device tree's /chosen simple-framebuffer. */
enum el2q_fb_source { EL2Q_FB_NONE, EL2Q_FB_GOP, EL2Q_FB_DT };

/* A /chosen simple-framebuffer node (Linux
 * Documentation/devicetree/bindings/display/simple-framebuffer.yaml). */
struct el2q_simplefb {
    uint64_t base, size;
    uint32_t width, height, stride;  /* stride in bytes */
    uint32_t bgr;                    /* 1: [ax]8r8g8b8, BGRX in memory; 0: [ax]8b8g8r8 */
};

enum el2q_sfb_state {
    EL2Q_SFB_NOT_TRIED,    /* a GOP mode had an address */
    EL2Q_SFB_NO_DT,        /* no device tree configuration table */
    EL2Q_SFB_BAD_DT,       /* the tree, or the node's reg cells, do not parse */
    EL2Q_SFB_NO_NODE,      /* no enabled simple-framebuffer child of /chosen */
    EL2Q_SFB_FORMAT,       /* not a 32 bpp format el2qual draws */
    EL2Q_SFB_GEOMETRY,     /* address, width, height, stride and size disagree */
    EL2Q_SFB_NO_MAP,       /* GetMemoryMap failed (set by el2qual.c) */
    EL2Q_SFB_NOT_RESERVED, /* not inside one Reserved region (set by el2qual.c) */
    EL2Q_SFB_OK
};

/* The first enabled simple-framebuffer child of /chosen, checked for a
 * drawable format and self-consistent geometry. *out is filled only on OK. */
enum el2q_sfb_state el2q_fdt_simplefb(const void *fdt, size_t limit, struct el2q_simplefb *out);

/* Whether [base, base + size) lies inside a single EfiReservedMemoryType
 * descriptor of a GetMemoryMap buffer (descriptors desc_size bytes apart). */
int el2q_memmap_reserved(const void *map, uint64_t map_size, uint64_t desc_size,
                         uint64_t base, uint64_t size);

/* Console wording for a fallback outcome. */
const char *el2q_sfb_words(uint32_t state);

/* Verdicts, shared by the report and the tests. */
enum el2q_verdict { EL2Q_NOT_RUN, EL2Q_YES, EL2Q_NO, EL2Q_INCONCLUSIVE };
enum el2q_verdict el2q_pauth_signs(const struct el2q_cpu *cpu);
enum el2q_verdict el2q_walks_16k(const struct el2q_cpu *cpu);

/* The screen report, and one line per raw register for a serial log. */
void el2q_report(const struct el2q_run *run, struct el2q_text *text);
void el2q_raw_line(const struct el2q_run *run, unsigned slot, unsigned index,
                   char out[EL2Q_TEXT_COLS + 1]);

/* One fixed-format line per slot for tools/qemu-el2qual.py:
 *   SUM <slot> EL<n> <state> PAC=<v> 16K=<v> NV2=<f>/<p> ECV=... FGT=... RNDR=...
 * <v> is Y, N, I (inconclusive) or - (not run); <f> the ID field and <p> OK,
 * UD (undefined) or EX (another exception) for the matching probe
 * instruction. A slot that did not finish stops after <state>. */
void el2q_summary_line(const struct el2q_run *run, unsigned slot,
                       char out[EL2Q_TEXT_COLS + 1]);

/* Draws the report into a 32 bpp framebuffer (stride in pixels), at the
 * largest integer scale of a 6x9 cell that fits every line. bgr selects
 * PixelBlueGreenRedReserved8BitPerColor over the RGB order. */
void el2q_render(const struct el2q_text *text, uint32_t *fb, uint32_t width,
                 uint32_t height, uint32_t stride, int bgr);

#endif
