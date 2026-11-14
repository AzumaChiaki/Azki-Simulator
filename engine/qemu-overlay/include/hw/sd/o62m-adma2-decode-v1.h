#ifndef O62M_ADMA2_DECODE_V1_H
#define O62M_ADMA2_DECODE_V1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Byte decoder only: no DMA, command completion, RAM alias or ready state.
 * Effective addressing flags are chosen by the controller from actual CSR.
 * Physical address permissions/ranges, table alignment, active descriptor
 * progress, chain/work limits and hardware error IRQ belong to the engine.
 */
typedef struct {
    uint8_t address_bits; /* exactly32 or64 */
    bool v4;
    bool length26;
} O62MAdma2ModeV1;

typedef enum {
    O62M_ADMA2_V1_OK = 0,
    O62M_ADMA2_V1_BAD_ARGUMENT,
    O62M_ADMA2_V1_UNSUPPORTED_FORMAT,
    O62M_ADMA2_V1_SHORT_BUFFER,
    O62M_ADMA2_V1_INVALID_DESCRIPTOR,
    O62M_ADMA2_V1_RESERVED_BITS,
    O62M_ADMA2_V1_UNSUPPORTED_ACTION,
    O62M_ADMA2_V1_UNKNOWN_ZERO_LENGTH26
} O62MAdma2ResultV1;

typedef struct {
    uint64_t address; /* exact encoded address, never silently masked */
    uint32_t encoded_length;
    uint32_t transfer_bytes; /* only TRANSFER action; NOP/LINK are zero */
    uint8_t action; /* 0NOP, 2TRANSFER, 3LINK */
    uint8_t descriptor_bytes;
    bool end;
    bool interrupt;
} O62MAdma2DecodedV1;

static inline uint8_t o62m_adma2_stride_v1(const O62MAdma2ModeV1 *mode)
{
    if (!mode) { return 0; }
    if (mode->address_bits == 32) { return 8; }
    if (mode->address_bits == 64) { return mode->v4 ? 16 : 12; }
    return 0;
}

static inline uint32_t o62m_adma2_u32le_v1(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline bool o62m_adma2_ranges_overlap_v1(
    const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t pa = (uintptr_t)a, pb = (uintptr_t)b;
    if (!na || !nb) { return false; }
    return pa >= pb ? pa-pb < nb : pb-pa < na;
}

/* available is the actual readable host buffer span. Inputs and mode must
 * remain immutable; overlapping output is rejected. All failures preserve
 * output byte-for-byte. No key/device data is logged by this helper.
 */
static inline O62MAdma2ResultV1 o62m_adma2_decode_v1(
    const uint8_t *bytes, size_t available, const O62MAdma2ModeV1 *mode,
    O62MAdma2DecodedV1 *out)
{
    if (!bytes || !mode || !out) { return O62M_ADMA2_V1_BAD_ARGUMENT; }
    uint8_t stride = o62m_adma2_stride_v1(mode);
    if (!stride) { return O62M_ADMA2_V1_UNSUPPORTED_FORMAT; }
    if (available < stride) { return O62M_ADMA2_V1_SHORT_BUFFER; }
    if (o62m_adma2_ranges_overlap_v1(bytes, available, out, sizeof(*out)) ||
        o62m_adma2_ranges_overlap_v1(mode, sizeof(*mode), out, sizeof(*out))) {
        return O62M_ADMA2_V1_BAD_ARGUMENT;
    }
    uint32_t command = o62m_adma2_u32le_v1(bytes);
    if (!(command & 1u)) { return O62M_ADMA2_V1_INVALID_DESCRIPTOR; }
    if ((command & 8u) || (!mode->length26 && (command & UINT32_C(0xffc0)))) {
        return O62M_ADMA2_V1_RESERVED_BITS;
    }
    uint8_t action = (uint8_t)((command >> 4) & 3u);
    if (action == 1) { return O62M_ADMA2_V1_UNSUPPORTED_ACTION; }
    if (stride == 16 && o62m_adma2_u32le_v1(bytes+12)) {
        return O62M_ADMA2_V1_RESERVED_BITS;
    }
    O62MAdma2DecodedV1 trial;
    memset(&trial, 0, sizeof(trial));
    trial.address = o62m_adma2_u32le_v1(bytes+4);
    if (mode->address_bits == 64) {
        trial.address |= (uint64_t)o62m_adma2_u32le_v1(bytes+8) << 32;
    }
    trial.encoded_length = command >> 16;
    if (mode->length26) {
        trial.encoded_length |= ((command >> 6) & UINT32_C(0x3ff)) << 16;
    }
    trial.action = action;
    trial.descriptor_bytes = stride;
    trial.end = (command & 2u) != 0;
    trial.interrupt = (command & 4u) != 0;
    if (action == 2) {
        if (!trial.encoded_length && mode->length26) {
            return O62M_ADMA2_V1_UNKNOWN_ZERO_LENGTH26;
        }
        trial.transfer_bytes = trial.encoded_length ? trial.encoded_length : UINT32_C(65536);
    }
    *out = trial;
    return O62M_ADMA2_V1_OK;
}

static inline const char *o62m_adma2_reason_v1(O62MAdma2ResultV1 r)
{
    switch (r) {
    case O62M_ADMA2_V1_OK: return "ok";
    case O62M_ADMA2_V1_BAD_ARGUMENT: return "bad-argument-or-output-overlap";
    case O62M_ADMA2_V1_UNSUPPORTED_FORMAT: return "unsupported-address-format";
    case O62M_ADMA2_V1_SHORT_BUFFER: return "descriptor-buffer-too-short";
    case O62M_ADMA2_V1_INVALID_DESCRIPTOR: return "descriptor-valid-bit-clear";
    case O62M_ADMA2_V1_RESERVED_BITS: return "unimplemented-reserved-bits";
    case O62M_ADMA2_V1_UNSUPPORTED_ACTION: return "adma1-set-length-is-not-adma2";
    case O62M_ADMA2_V1_UNKNOWN_ZERO_LENGTH26: return "zero-length26-transfer-unproved";
    default: return "unknown-result";
    }
}
#endif
