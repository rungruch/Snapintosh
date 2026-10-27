/* SPDX-License-Identifier: MIT */
/* See s2bwrite.h. A host model: not linked into el2qual. */
#include "s2bwrite.h"

#define SMMU_sGFSR       0x48u
#define SMMU_TLBIALLNSNH 0x68u
#define SMMU_TLBIALLH    0x6cu
#define SMMU_sTLBGSYNC   0x70u
#define SMMU_sTLBGSTATUS 0x74u
#define GSACTIVE         (1u << 0)
#define SMMU_S2CR(n)     (0xc00ull + 4ull * (n))
#define S2CR_TYPE        (3u << 16)
#define S2CR_BYPASS      (1u << 16)
#define XHCI_USBCMD      0x0u
#define XHCI_USBSTS      0x4u
#define USBCMD_RUN       (1u << 0)
#define USBSTS_HALT      (1u << 0)
/* What Linux writes to the invalidate and sync registers (QCOM_DUMMY_VAL). */
#define DUMMY            0xffffffffu
/* A second bound on the sync poll, in case the counter does not advance. */
#define POLL_LIMIT       (1u << 20)

/* The stage-1 result must be PLANNED and describe exactly the one-field
 * change s2bypass.c plans, at the address it names; a result that was
 * altered, or made for another SMMU, is not written from. */
static int plan_consistent(const struct el2q_s2b_result *plan, uint64_t smmu)
{
    return plan && plan->state == EL2Q_S2B_PLANNED && plan->group < 255 &&
           plan->at == smmu + SMMU_S2CR(plan->group) && (plan->old & S2CR_TYPE) == 0 &&
           plan->planned == ((plan->old & ~S2CR_TYPE) | S2CR_BYPASS) &&
           plan->restore == plan->old;
}

/* Every store is followed by a DSB, so it has completed before the read
 * that checks it, and before the next store. */
static uint64_t store(const struct el2q_s2bw_hw *hw, struct el2q_s2bw_result *out,
                      uint64_t address, uint32_t value)
{
    out->writes++;
    uint64_t esr = hw->write32(hw->ctx, address, value);
    hw->barrier(hw->ctx);
    return esr;
}

/* TLBIALLH, TLBIALLNSNH, then a sync polled until sTLBGSTATUS.GSACTIVE
 * clears: Linux's reset sequence. Sets out->synced. */
static void invalidate(const struct el2q_s2bw_hw *hw, struct el2q_s2bw_result *out, uint64_t smmu)
{
    uint64_t esr = store(hw, out, smmu + SMMU_TLBIALLH, DUMMY);
    if (!esr) esr = store(hw, out, smmu + SMMU_TLBIALLNSNH, DUMMY);
    if (!esr) esr = store(hw, out, smmu + SMMU_sTLBGSYNC, DUMMY);
    if (esr) {
        out->detail = esr;
        out->synced = 3;
        return;
    }
    uint64_t start = hw->now(hw->ctx);
    for (;;) {
        uint32_t status = 0;
        esr = hw->read32(hw->ctx, smmu + SMMU_sTLBGSTATUS, &status);
        out->polls++;
        if (esr) {
            out->detail = esr;
            out->synced = 3;
            return;
        }
        if (!(status & GSACTIVE)) {
            out->synced = 1;
            return;
        }
        if (out->polls >= POLL_LIMIT || hw->now(hw->ctx) - start > hw->sync_ticks) {
            out->synced = 2;
            return;
        }
    }
}

uint32_t el2q_s2bw_run(const struct el2q_s2b_result *plan, uint64_t smmu, uint64_t xhci_op,
                       const struct el2q_s2bw_hw *hw, struct el2q_s2bw_result *out)
{
    *out = (struct el2q_s2bw_result){.state = EL2Q_S2BW_REFUSED_PLAN};
    if (!hw || !hw->read32 || !hw->write32 || !hw->barrier || !hw->serror || !hw->now)
        return out->state;
    if (!plan_consistent(plan, smmu)) return out->state;
    out->group = plan->group;
    out->old = plan->old;
    out->planned = plan->planned;
    if (hw->serror(hw->ctx)) return out->state = EL2Q_S2BW_REFUSED_PENDING;

    /* "No DMA" is the premise of this stage: USB0's controller must be
     * halted, so no transaction of its stream can meet the bypass. */
    uint64_t esr = hw->read32(hw->ctx, xhci_op + XHCI_USBCMD, &out->usbcmd);
    if (!esr) esr = hw->read32(hw->ctx, xhci_op + XHCI_USBSTS, &out->usbsts);
    if (esr) {
        out->detail = esr;
        return out->state = EL2Q_S2BW_REFUSED_BUSY;
    }
    if ((out->usbcmd & USBCMD_RUN) || !(out->usbsts & USBSTS_HALT))
        return out->state = EL2Q_S2BW_REFUSED_BUSY;

    uint32_t now = 0;
    esr = hw->read32(hw->ctx, plan->at, &now);
    if (esr) out->detail = esr;
    if (esr || now != plan->old) return out->state = EL2Q_S2BW_REFUSED_MOVED;

    /* ---- From here on, the SMMU is written. ---- */
    uint32_t first = 0;
    out->wrote = 1;
    esr = store(hw, out, plan->at, plan->planned);
    out->fault_step = esr ? 1 : 0;
    if (!esr) {
        esr = hw->read32(hw->ctx, plan->at, &out->readback);
        out->fault_step = esr ? 2 : 0;
    }
    if (esr) {
        out->detail = esr;
        first = EL2Q_S2BW_WRITE_FAULT;
    } else {
        out->readback_ok = 1;
        /* Not taken: the register is as it was, so nothing else is
         * written. This is a full answer to the stage's question. */
        if (out->readback == plan->old) return out->state = EL2Q_S2BW_DROPPED;
        if (out->readback != plan->planned) first = EL2Q_S2BW_UNEXPECTED;
    }

    /* The restore, whatever happened above: the bypass may have taken even
     * where its store or read-back faulted. */
    out->restored = 1;
    esr = store(hw, out, plan->at, plan->old);
    if (esr) out->fault_step = 3;
    else {
        esr = hw->read32(hw->ctx, plan->at, &out->restore_readback);
        if (esr) out->fault_step = 4;
    }
    if (esr) {
        out->detail = esr;
        return out->state = EL2Q_S2BW_RESTORE_FAILED;
    }
    out->restore_ok = 1;
    /* An unknown register state is not improved by more writes; a power
     * cycle reloads the firmware's SMMU setup. */
    if (out->restore_readback != plan->old) return out->state = EL2Q_S2BW_RESTORE_FAILED;

    invalidate(hw, out, smmu);
    if (hw->read32(hw->ctx, smmu + SMMU_sGFSR, &out->gfsr)) out->gfsr = 0xffffffffu;
    if (first) return out->state = first;
    if (out->synced != 1) return out->state = EL2Q_S2BW_SYNC_TIMEOUT;
    if (out->gfsr) return out->state = EL2Q_S2BW_GFAULT;
    if (hw->serror(hw->ctx)) return out->state = EL2Q_S2BW_SERROR;
    return out->state = EL2Q_S2BW_DONE;
}

/* ---- Report lines --------------------------------------------------------------
 * Nothing when not asked. Refused before any write:
 *   SMMU WRITE REFUSED: <PLAN|MOVED|SERROR ALREADY PENDING|
 *                        USB0 NOT HALTED CMD <8 hex> STS <8 hex>|USB0 READ ESR <8 hex>>
 *   SMMU WRITE NOTHING WRITTEN
 * Once the bypass was written, with no holes:
 *   SMMU WRITE S2CR<nn> <old> -> <planned> READ BACK <8 hex|FAULT|NOT DONE>
 *   SMMU WRITE RESTORE <old> READ BACK <8 hex|FAULT|NOT DONE>  (unless DROPPED)
 *   SMMU WRITE TLBI SYNC <OK|TIMEOUT|FAULT> POLLS <n> GFSR <8 hex>  (if tried)
 *   SMMU WRITE <outcome>, WRITES <n>
 * READ BACK NOT DONE means the store before it faulted, so it was not read;
 * READ BACK FAULT means the read itself faulted. */

struct lbuf { char *s; unsigned n; };

static void put_s(struct lbuf *b, const char *s)
{
    while (*s && b->n < EL2Q_S2BW_LINE_COLS) b->s[b->n++] = *s++;
    b->s[b->n] = 0;
}

static void put_hex(struct lbuf *b, uint64_t v, unsigned digits)
{
    char t[17];
    for (unsigned i = 0; i < digits; i++) t[i] = "0123456789ABCDEF"[(v >> (4 * (digits - 1 - i))) & 15];
    t[digits] = 0;
    put_s(b, t);
}

static void put_dec(struct lbuf *b, uint64_t v)
{
    char d[20];
    unsigned n = 0;
    do d[n++] = (char)('0' + v % 10); while ((v /= 10) && n < sizeof d);
    while (n) {
        char c[2] = {d[--n], 0};
        put_s(b, c);
    }
}

enum { L_WRITE, L_RESTORE, L_SYNC, L_OUTCOME };

void el2q_s2bw_line(const struct el2q_s2bw_result *r, unsigned index,
                    char out[EL2Q_S2BW_LINE_COLS + 1])
{
    struct lbuf b = {out, 0};
    out[0] = 0;
    if (r->state == EL2Q_S2BW_NOT_ASKED || r->state >= EL2Q_S2BW_COUNT) return;
    if (!r->wrote) {
        if (index == 0) {
            put_s(&b, "SMMU WRITE REFUSED: ");
            if (r->state == EL2Q_S2BW_REFUSED_PLAN) put_s(&b, "PLAN");
            else if (r->state == EL2Q_S2BW_REFUSED_MOVED) put_s(&b, "MOVED");
            else if (r->state == EL2Q_S2BW_REFUSED_PENDING) put_s(&b, "SERROR ALREADY PENDING");
            else if (r->detail) { put_s(&b, "USB0 READ ESR "); put_hex(&b, r->detail, 8); }
            else {
                put_s(&b, "USB0 NOT HALTED CMD "); put_hex(&b, r->usbcmd, 8);
                put_s(&b, " STS "); put_hex(&b, r->usbsts, 8);
            }
        } else if (index == 1) {
            put_s(&b, "SMMU WRITE NOTHING WRITTEN");
        }
        return;
    }
    /* The lines that exist for this outcome, in order, so there is no hole:
     * the report stops at the first empty line. */
    unsigned kinds[4], n = 0;
    kinds[n++] = L_WRITE;
    if (r->restored) kinds[n++] = L_RESTORE;
    if (r->synced) kinds[n++] = L_SYNC;
    kinds[n++] = L_OUTCOME;
    if (index >= n) return;
    put_s(&b, "SMMU WRITE ");
    switch (kinds[index]) {
    case L_WRITE:
        put_s(&b, "S2CR"); put_hex(&b, r->group, 2);
        put_s(&b, " "); put_hex(&b, r->old, 8);
        put_s(&b, " -> "); put_hex(&b, r->planned, 8);
        put_s(&b, " READ BACK ");
        if (r->readback_ok) put_hex(&b, r->readback, 8);
        else put_s(&b, r->fault_step == 1 ? "NOT DONE" : "FAULT");
        break;
    case L_RESTORE:
        put_s(&b, "RESTORE "); put_hex(&b, r->old, 8);
        put_s(&b, " READ BACK ");
        if (r->restore_ok) put_hex(&b, r->restore_readback, 8);
        else put_s(&b, r->fault_step == 3 ? "NOT DONE" : "FAULT");
        break;
    case L_SYNC:
        put_s(&b, "TLBI SYNC ");
        put_s(&b, r->synced == 1 ? "OK" : r->synced == 2 ? "TIMEOUT" : "FAULT");
        put_s(&b, " POLLS "); put_dec(&b, r->polls);
        put_s(&b, " GFSR "); put_hex(&b, r->gfsr, 8);
        break;
    default:
        switch (r->state) {
        case EL2Q_S2BW_DONE: put_s(&b, "DONE: TOOK AND UNDONE"); break;
        case EL2Q_S2BW_DROPPED: put_s(&b, "DROPPED: NOT TAKEN"); break;
        case EL2Q_S2BW_WRITE_FAULT:
            put_s(&b, "FAULT ESR "); put_hex(&b, r->detail, 8); put_s(&b, ", RESTORED");
            break;
        case EL2Q_S2BW_UNEXPECTED: put_s(&b, "UNEXPECTED READ BACK, RESTORED"); break;
        case EL2Q_S2BW_RESTORE_FAILED: put_s(&b, "RESTORE FAILED: POWER CYCLE"); break;
        case EL2Q_S2BW_SYNC_TIMEOUT: put_s(&b, "RESTORED, SYNC INCOMPLETE"); break;
        case EL2Q_S2BW_GFAULT: put_s(&b, "RESTORED, GLOBAL FAULT"); break;
        case EL2Q_S2BW_SERROR: put_s(&b, "RESTORED, SERROR PENDING"); break;
        default: put_s(&b, "?"); break;
        }
        put_s(&b, ", WRITES ");
        put_dec(&b, r->writes);
        break;
    }
}
