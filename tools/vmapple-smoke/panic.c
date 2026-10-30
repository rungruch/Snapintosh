/* SPDX-License-Identifier: MIT */
/* Panic-message extraction for the Route B monitor (Phase 3 step H4).
 *
 * When the stock 26A428 VirtualMac kernel panics before its serial console
 * is up, it does not print; it fills a debugger_state structure with the
 * panic format string and a pointer to its varargs, then halts in
 * PEHaltRestart (OBSERVED, run 39/40). The monitor reads that structure at
 * EL2 and formats the message so a person can read it on the panel.
 *
 * Two pieces:
 *   panic_format  -- a bounded mini-printf over an abstract argument source.
 *                    Pure logic, host-tested (tools/test-vmapple-monitor.c).
 *   panic_render  -- EL2 glue: read the guest's format string and varargs
 *                    through mon_translate_va and format them. EL2 only; it
 *                    dereferences guest physical memory.
 *
 * Nothing here invents a value: a string or argument it cannot read is
 * shown as "<?>", and an unreadable format string renders nothing. The
 * va_list indirection (db_panic_args is a va_list*, and Apple's arm64
 * va_list is a plain stack pointer) is INFERRED from published XNU and is
 * what a hardware run confirms.
 */
#include "monitor.h"

static void put_c(char *out, unsigned cap, unsigned *k, char c)
{
    if (*k + 1 < cap) out[(*k)++] = c;
}

static void put_s(char *out, unsigned cap, unsigned *k, const char *s)
{
    while (*s) put_c(out, cap, k, *s++);
}

/* An unsigned value in `base` with optional minimum field width. `neg`
 * prepends '-'. Zero-pad fills with '0' after the sign, space-pad before
 * it. Left-justification is not supported (panic strings do not use it).
 * %d narrower than 64 bits is not modelled: every vararg slot is read as
 * 64 bits, which is correct for the %s/%llx/%p forms XNU's panic strings
 * use here. */
static void put_num(char *out, unsigned cap, unsigned *k, uint64_t v,
                    unsigned base, int upper, unsigned width, int zero, int neg)
{
    char tmp[24];
    unsigned n = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digs[v % base]; v /= base; }
    unsigned total = n + (neg ? 1u : 0u);
    if (!zero)
        for (unsigned i = total; i < width; i++) put_c(out, cap, k, ' ');
    if (neg) put_c(out, cap, k, '-');
    if (zero)
        for (unsigned i = total; i < width; i++) put_c(out, cap, k, '0');
    while (n) put_c(out, cap, k, tmp[--n]);
}

unsigned panic_format(char *out, unsigned cap, const char *fmt,
                      const struct panic_args *a)
{
    unsigned k = 0, argi = 0;
    if (cap == 0) return 0;
    for (unsigned i = 0; fmt[i]; i++) {
        char c = fmt[i];
        if (c != '%') { put_c(out, cap, &k, c); continue; }
        i++;
        if (!fmt[i]) { put_c(out, cap, &k, '%'); break; }
        int zero = 0;
        while (fmt[i] == '0' || fmt[i] == '-' || fmt[i] == ' ' ||
               fmt[i] == '+' || fmt[i] == '#') {
            if (fmt[i] == '0') zero = 1;
            i++;
        }
        unsigned width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9') { width = width * 10 + (unsigned)(fmt[i] - '0'); i++; }
        while (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z' ||
               fmt[i] == 'j' || fmt[i] == 't') i++;
        char conv = fmt[i];
        uint64_t v;
        switch (conv) {
        case '%':
            put_c(out, cap, &k, '%');
            break;
        case 'c':
            if (a->get(a->ctx, argi++, &v)) goto done;
            put_c(out, cap, &k, (char)v);
            break;
        case 's': {
            if (a->get(a->ctx, argi++, &v)) goto done;
            char sbuf[80];
            if (a->str && a->str(a->ctx, v, sbuf, sizeof sbuf) == 0)
                put_s(out, cap, &k, sbuf);
            else
                put_s(out, cap, &k, "<?>");
            break;
        }
        case 'p':
            if (a->get(a->ctx, argi++, &v)) goto done;
            put_s(out, cap, &k, "0x");
            put_num(out, cap, &k, v, 16, 0, width, zero, 0);
            break;
        case 'x':
        case 'X':
            if (a->get(a->ctx, argi++, &v)) goto done;
            put_num(out, cap, &k, v, 16, conv == 'X', width, zero, 0);
            break;
        case 'u':
            if (a->get(a->ctx, argi++, &v)) goto done;
            put_num(out, cap, &k, v, 10, 0, width, zero, 0);
            break;
        case 'd':
        case 'i': {
            if (a->get(a->ctx, argi++, &v)) goto done;
            int64_t s = (int64_t)v;
            int neg = s < 0;
            uint64_t m = neg ? (uint64_t)(-s) : (uint64_t)s;
            put_num(out, cap, &k, m, 10, 0, width, zero, neg);
            break;
        }
        default:
            /* An unknown conversion is emitted literally and consumes no
             * argument, so the rest of the message stays aligned. */
            put_c(out, cap, &k, '%');
            if (conv) put_c(out, cap, &k, conv);
            break;
        }
        if (!conv) break;
    }
done:
    out[k] = 0;
    return k;
}

/* ---- EL2 glue: read the guest's panic strings and arguments ------------- */

static uint64_t rd64_va(uint64_t va, int *ok)
{
    uint64_t par = mon_translate_va(va);
    if (par & 1) { *ok = 0; return 0; }
    uint64_t pa = (par & 0x0000fffffffff000ull) | (va & 0xfff);
    mon_clean_range(pa, 8);
    *ok = 1;
    return *(const volatile uint64_t *)(uintptr_t)pa;
}

/* Copy the C string at guest VA into buf (at most cap-1 characters).
 * Each 4 KiB page is translated on its own, because the guest's stage-1
 * mapping need not be physically contiguous across pages (XNU's
 * kernel-state panic format is 651 bytes and can cross one). A page that
 * does not translate ends the string where it is. Returns 0 when the
 * first page translated. */
static int copy_str_va(uint64_t va, char *buf, unsigned cap)
{
    unsigned i = 0;
    while (i + 1 < cap) {
        uint64_t par = mon_translate_va(va);
        if (par & 1) {
            buf[i] = 0;
            return i ? 0 : -1;
        }
        uint64_t pa = (par & 0x0000fffffffff000ull) | (va & 0xfff);
        unsigned page_left = 0x1000u - (unsigned)(va & 0xfff);
        unsigned limit = cap - 1 - i < page_left ? cap - 1 - i : page_left;
        mon_clean_range(pa, limit);
        const volatile char *s = (const volatile char *)(uintptr_t)pa;
        for (unsigned j = 0; j < limit; j++) {
            if (!s[j]) { buf[i] = 0; return 0; }
            buf[i++] = s[j];
        }
        va += limit;
    }
    buf[i] = 0;
    return 0;
}

/* The band's font has no glyph for a control character and draws it as
 * '?', which turned XNU's "\n\t  x0:" into "??  x0:" (run 41). Draw each
 * run of spaces, tabs and line breaks as one space instead, which also
 * fits more of the register dump on each page. */
static unsigned squeeze_space(char *s, unsigned n)
{
    unsigned k = 0;
    int space = 0;
    for (unsigned i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!space && k) s[k++] = ' ';
            space = 1;
        } else {
            s[k++] = c;
            space = 0;
        }
    }
    if (k && s[k - 1] == ' ') k--;
    s[k] = 0;
    return k;
}

struct el2_args { uint64_t base; };

static int el2_get(void *ctx, unsigned index, uint64_t *out)
{
    struct el2_args *e = (struct el2_args *)ctx;
    int ok;
    *out = rd64_va(e->base + (uint64_t)index * 8, &ok);
    return ok ? 0 : -1;
}

static int el2_str(void *ctx, uint64_t va, char *buf, unsigned cap)
{
    (void)ctx;
    return copy_str_va(va, buf, cap);
}

/* ---- the guest's console scrollback (run 43) ----------------------------
 * Once the kernel's console prints, XNU writes its whole panic log there:
 * the panic string, the saved registers, the backtrace and the kext list.
 * The band shows only the console's first line and its last three, so in
 * run 42 the panic string scrolled away before it could be read. These are
 * the last CONSOLE_SCROLL_LINES committed lines, kept so that a halted
 * guest's console can be paged back through the band. */
static char scroll[CONSOLE_SCROLL_LINES][CONSOLE_SCROLL_COLS];
static unsigned scroll_next, scroll_count;

void console_scroll_reset(void)
{
    scroll_next = 0;
    scroll_count = 0;
}

void console_scroll_add(const char *line)
{
    unsigned i = 0;
    for (; i + 1 < CONSOLE_SCROLL_COLS && line[i]; i++) scroll[scroll_next][i] = line[i];
    scroll[scroll_next][i] = 0;
    scroll_next = (scroll_next + 1) % CONSOLE_SCROLL_LINES;
    if (scroll_count < CONSOLE_SCROLL_LINES) scroll_count++;
}

static const char *scroll_line(unsigned oldest_index)
{
    return scroll[(scroll_next + CONSOLE_SCROLL_LINES - scroll_count + oldest_index)
                  % CONSOLE_SCROLL_LINES];
}

unsigned console_scroll_page(const char *out[], unsigned rows, unsigned page)
{
    for (unsigned r = 0; r < rows; r++) out[r] = "";
    if (!scroll_count || !rows) return 0;
    /* Start a few lines before XNU's first "panic(cpu N caller ...)" line,
     * so the reason comes first and what the kernel printed just before
     * it stays in view. Without one, start at the oldest line kept. */
    unsigned start = 0;
    for (unsigned i = 0; i < scroll_count; i++) {
        const char *s = scroll_line(i);
        if (s[0] == 'p' && s[1] == 'a' && s[2] == 'n' && s[3] == 'i' && s[4] == 'c' && s[5] == '(') {
            start = i > CONSOLE_SCROLL_CONTEXT ? i - CONSOLE_SCROLL_CONTEXT : 0;
            break;
        }
    }
    unsigned pages = (scroll_count - start + rows - 1) / rows;
    unsigned first = start + (page % pages) * rows;
    for (unsigned r = 0; r < rows; r++)
        if (first + r < scroll_count) out[r] = scroll_line(first + r);
    return pages;
}

unsigned panic_render(char out[][PANIC_LINE_CAP], unsigned maxlines, unsigned width,
                      unsigned page, uint64_t fmt_va, uint64_t args_va)
{
    for (unsigned i = 0; i < maxlines; i++) out[i][0] = 0;
    if (width == 0 || width > PANIC_LINE_CAP - 1) width = PANIC_LINE_CAP - 1;

    /* db_panic_args is a va_list* (XNU); dereference it once for the
     * char* the compiler stored (Apple arm64 va_list is a stack pointer).
     * If that read faults, fall back to treating args_va as the list. */
    int ok;
    uint64_t list = rd64_va(args_va, &ok);
    struct el2_args ea = { ok ? list : args_va };
    struct panic_args pa = { el2_get, el2_str, &ea };

    /* Static, not on the EL2 stack: XNU's kernel-state panic is a 651-byte
     * format with 41 conversions and formats to about 1,100 characters,
     * including the saved ESR and FAR on its last line. This runs from
     * the CNTHP interrupt handler on the one CPU, never reentrantly. */
    static char fmt[1024];
    static char msg[1536];
    if (copy_str_va(fmt_va, fmt, sizeof fmt) != 0) return 0;
    unsigned total = panic_format(msg, sizeof msg, fmt, &pa);
    total = squeeze_space(msg, total);

    unsigned start = page * maxlines * width;
    for (unsigned l = 0; l < maxlines; l++) {
        unsigned off = start + l * width;
        if (off >= total) { out[l][0] = 0; continue; }
        unsigned n = total - off;
        if (n > width) n = width;
        for (unsigned j = 0; j < n; j++) out[l][j] = msg[off + j];
        out[l][n] = 0;
    }
    return total;
}
