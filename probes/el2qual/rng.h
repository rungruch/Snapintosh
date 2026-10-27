/* SPDX-License-Identifier: MIT */
/* el2qual's entropy check (load option "rng"; docs/GAOKUN3-PHASE3.md, P7).
 *
 * Phase 0C condition 3: XNU wants 256 non-zero bytes in /chosen/random-seed,
 * and the gaokun3 has no RNDR, so a Route B boot must take them from a real
 * source or refuse. The candidate is UEFI's RNG protocol, before
 * ExitBootServices. This reports whether the firmware offers it, which
 * algorithms it lists, and whether 256 bytes come back that are not all zero
 * and differ between two reads. It does not grade the source: the RAW
 * algorithm is entropy "directly from the source", the others are DRBGs whose
 * seed is not visible here.
 *
 * Read only. rng.c holds no firmware access of its own: el2qual.c passes
 * LocateProtocol in, tools/test-el2qual-rng.c a fake. The protocol and
 * algorithm GUIDs, the interface layout and the status codes are UEFI 2.10's
 * (section 37.5), checked against EDK2's MdePkg/Include/Protocol/Rng.h and
 * Guid/Rng.h, read as references. */
#ifndef EL2QUAL_RNG_H
#define EL2QUAL_RNG_H

#include <stdint.h>
#include "uefi.h"

/* EFI_RNG_PROTOCOL: GetInfo, then GetRNG. Lengths are in bytes; a null
 * algorithm asks for the driver's default. */
struct el2q_rng_protocol {
    efi_status (*get_info)(struct el2q_rng_protocol *self, uint64_t *list_size, struct efi_guid *list);
    efi_status (*get_rng)(struct el2q_rng_protocol *self, const struct efi_guid *algorithm,
                          uint64_t length, uint8_t *value);
};

extern const struct efi_guid el2q_rng_protocol_guid;

/* The algorithms UEFI names, as the report spells them: RAW, the SP800-90
 * CTR, HASH and HMAC DRBGs, the X9.31 AES and 3DES generators, the Arm RNDR
 * DRBG, and "?" for one this does not know. */
enum el2q_rng_alg {
    EL2Q_RNG_ALG_OTHER, EL2Q_RNG_ALG_RAW, EL2Q_RNG_ALG_CTR, EL2Q_RNG_ALG_HASH, EL2Q_RNG_ALG_HMAC,
    EL2Q_RNG_ALG_AES, EL2Q_RNG_ALG_3DES, EL2Q_RNG_ALG_RNDR, EL2Q_RNG_ALG_COUNT
};

#define EL2Q_RNG_MAX_ALGS 8
#define EL2Q_RNG_BYTES 256      /* what XNU takes from /chosen/random-seed */

/* Two GetRNG calls of EL2Q_RNG_BYTES each. Only a status of exactly
 * EFI_SUCCESS counts: a warning is not delivered entropy. */
struct el2q_rng_read {
    uint32_t asked;
    efi_status status[2];
    uint32_t nonzero;           /* both succeeded, and neither read was all zero */
    uint32_t differ;            /* both succeeded, and the two reads differ */
    uint32_t distinct;          /* distinct byte values in the first read; information only */
};

enum el2q_rng_state {
    EL2Q_RNG_NOT_ASKED,
    EL2Q_RNG_NOT_FOUND,         /* LocateProtocol failed, or returned no interface */
    EL2Q_RNG_UNUSABLE,          /* found, but no algorithm gave usable bytes */
    EL2Q_RNG_USABLE_DEFAULT,    /* the default algorithm did; RAW did not, or is not listed */
    EL2Q_RNG_USABLE_RAW,        /* RAW did */
};

struct el2q_rng {
    uint32_t state;             /* enum el2q_rng_state */
    efi_status locate_status;
    uint32_t null_interface;    /* LocateProtocol succeeded without an interface */
    efi_status info_status;
    uint32_t listed;            /* algorithms GetInfo reported, perhaps more than stored */
    uint8_t alg[EL2Q_RNG_MAX_ALGS];   /* enum el2q_rng_alg, the first EL2Q_RNG_MAX_ALGS */
    struct el2q_rng_read def;   /* the default algorithm */
    struct el2q_rng_read raw;   /* RAW, asked only when GetInfo lists it */
};

typedef efi_status (*el2q_rng_locate_fn)(void *ctx, const struct efi_guid *guid, void **protocol);

/* Before ExitBootServices: locates the protocol, lists its algorithms, and
 * reads the default algorithm, then RAW if it is listed. */
void el2q_rng_probe(el2q_rng_locate_fn locate, void *ctx, struct el2q_rng *out);

/* Fixed-format report lines, empty past the last; see rng.c. */
#define EL2Q_RNG_LINE_COLS 64
#define EL2Q_RNG_LINES 5
void el2q_rng_line(const struct el2q_rng *r, unsigned index, char out[EL2Q_RNG_LINE_COLS + 1]);

#endif
