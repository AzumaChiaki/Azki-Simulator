#ifndef O62M_TRNG_UNKEYED_V1_H
#define O62M_TRNG_UNKEYED_V1_H

#include <stdbool.h>
#include <stdint.h>

/* Bounded configuration-only Merak-compatible experiment, not a TRNG.
 * Only the five exact original O62M platform configuration values are accepted.
 * Each word remains unreadable until an actual accepted fullword write.
 *
 * An explicitly selected EMPTY_AO_KEY policy permits a negative key-valid
 * observation: the virtual store contains no key. Other status bits are an
 * independent full-u32 opaque input (bit0 must0), not guessed RES0/reset bits.
 * No key injection/generation, entropy, timing, health, ACK or IRQ exists here.
 * The caller owns architectural privilege/security and clock preconditions.
 */
#define O62M_TRNG_UNKEYED_CONFIG_COUNT 5u
#define O62M_TRNG_UNKEYED_MAGIC UINT32_C(0x54554331)
#define O62M_TRNG_UNKEYED_CTL UINT64_C(0x50333380)
#define O62M_TRNG_UNKEYED_STAT UINT64_C(0x50333398)

typedef enum O62MTrngUnkeyedResult {
    O62M_TRNG_UNKEYED_OK = 0,
    O62M_TRNG_UNKEYED_BAD_ARGUMENT,
    O62M_TRNG_UNKEYED_BAD_INITIAL,
    O62M_TRNG_UNKEYED_BAD_STATE,
    O62M_TRNG_UNKEYED_BAD_ACCESS,
    O62M_TRNG_UNKEYED_UNSUPPORTED_REGISTER,
    O62M_TRNG_UNKEYED_UNWRITTEN_CONFIG,
    O62M_TRNG_UNKEYED_UNSUPPORTED_CONFIG_VALUE,
    O62M_TRNG_UNKEYED_POLICY_REQUIRED,
    O62M_TRNG_UNKEYED_CONTROL_UNIMPLEMENTED,
    O62M_TRNG_UNKEYED_KEY_GENERATION_UNIMPLEMENTED,
    O62M_TRNG_UNKEYED_STATUS_WRITE_UNIMPLEMENTED
} O62MTrngUnkeyedResult;

typedef enum O62MTrngAOKeyState {
    O62M_TRNG_AO_KEY_EMPTY = 0
} O62MTrngAOKeyState;

typedef struct O62MTrngConfigDef {
    uint64_t address;
    uint32_t supported_mask;
    uint32_t original_value;
} O62MTrngConfigDef;

static const O62MTrngConfigDef o62m_trng_unkeyed_defs[O62M_TRNG_UNKEYED_CONFIG_COUNT] = {
    {UINT64_C(0x50333384), UINT32_C(0x0000001f), UINT32_C(0x0000000f)},
    {UINT64_C(0x50333388), UINT32_C(0x00ffffff), UINT32_C(0x0003000a)},
    {UINT64_C(0x5033338c), UINT32_C(0x00007777), UINT32_C(0x00007654)},
    {UINT64_C(0x50333390), UINT32_C(0x00007807), UINT32_C(0x00004001)},
    {UINT64_C(0x50333394), UINT32_C(0x007fffff), UINT32_C(0x00572e4d)}
};

typedef struct O62MTrngUnkeyed {
    uint32_t magic;
    uint32_t config[O62M_TRNG_UNKEYED_CONFIG_COUNT];
    uint32_t written_mask;
    uint32_t status_opaque;
    O62MTrngAOKeyState ao_key_state;
    bool explicit_unkeyed_policy;
} O62MTrngUnkeyed;

static inline const char *o62m_trng_unkeyed_reason(O62MTrngUnkeyedResult r)
{
    switch (r) {
    case O62M_TRNG_UNKEYED_OK: return "ok";
    case O62M_TRNG_UNKEYED_BAD_ARGUMENT: return "bad-argument";
    case O62M_TRNG_UNKEYED_BAD_INITIAL: return "bad-explicit-initial-state";
    case O62M_TRNG_UNKEYED_BAD_STATE: return "uninitialized-or-unsupported-key-state";
    case O62M_TRNG_UNKEYED_BAD_ACCESS: return "aligned-u32-access-required";
    case O62M_TRNG_UNKEYED_UNSUPPORTED_REGISTER: return "trng-register-unimplemented";
    case O62M_TRNG_UNKEYED_UNWRITTEN_CONFIG: return "configuration-initial-value-unknown";
    case O62M_TRNG_UNKEYED_UNSUPPORTED_CONFIG_VALUE: return "outside-exact-original-config-profile";
    case O62M_TRNG_UNKEYED_POLICY_REQUIRED: return "explicit-empty-AO-key-policy-required";
    case O62M_TRNG_UNKEYED_CONTROL_UNIMPLEMENTED: return "trng-control-reset-or-read-unimplemented";
    case O62M_TRNG_UNKEYED_KEY_GENERATION_UNIMPLEMENTED: return "hardware-key-generation-unimplemented";
    case O62M_TRNG_UNKEYED_STATUS_WRITE_UNIMPLEMENTED: return "key-status-write-unimplemented";
    }
    return "invalid-result";
}

static inline O62MTrngUnkeyedResult o62m_trng_unkeyed_init(
    O62MTrngUnkeyed *model, bool explicit_unkeyed_policy, uint64_t status_opaque)
{
    O62MTrngUnkeyed next = {0};
    if (!model) {
        return O62M_TRNG_UNKEYED_BAD_ARGUMENT;
    }
    if (status_opaque > UINT32_MAX || (status_opaque & 1u)) {
        return O62M_TRNG_UNKEYED_BAD_INITIAL;
    }
    next.magic = O62M_TRNG_UNKEYED_MAGIC;
    next.status_opaque = (uint32_t)status_opaque;
    next.ao_key_state = O62M_TRNG_AO_KEY_EMPTY;
    next.explicit_unkeyed_policy = explicit_unkeyed_policy;
    *model = next;
    return O62M_TRNG_UNKEYED_OK;
}

static inline bool o62m_trng_unkeyed_valid(const O62MTrngUnkeyed *model)
{
    unsigned i;
    if (!model || model->magic != O62M_TRNG_UNKEYED_MAGIC ||
        model->ao_key_state != O62M_TRNG_AO_KEY_EMPTY ||
        (model->status_opaque & 1u) ||
        (model->written_mask & ~UINT32_C(0x1f))) {
        return false;
    }
    for (i = 0; i < O62M_TRNG_UNKEYED_CONFIG_COUNT; ++i) {
        if (model->written_mask & (1u << i)) {
            if (model->config[i] != o62m_trng_unkeyed_defs[i].original_value ||
                (model->config[i] & ~o62m_trng_unkeyed_defs[i].supported_mask)) {
                return false;
            }
        } else if (model->config[i] != 0) {
            /* Unwritten zero is bookkeeping only and never exposed. */
            return false;
        }
    }
    return true;
}

/* No failed transaction changes any model byte or caller value. There is no
 * time/read-count progression and no code path can create a nonempty key.
 */
static inline O62MTrngUnkeyedResult o62m_trng_unkeyed_access(
    O62MTrngUnkeyed *model, uint64_t address, unsigned size,
    bool write, uint64_t *value)
{
    unsigned i;
    if (!model || !value) {
        return O62M_TRNG_UNKEYED_BAD_ARGUMENT;
    }
    if (!o62m_trng_unkeyed_valid(model)) {
        return O62M_TRNG_UNKEYED_BAD_STATE;
    }
    if (size != 4u || (address & 3u) || (write && *value > UINT32_MAX)) {
        return O62M_TRNG_UNKEYED_BAD_ACCESS;
    }
    if (address == O62M_TRNG_UNKEYED_CTL) {
        if (write && (*value & 1u)) {
            return O62M_TRNG_UNKEYED_KEY_GENERATION_UNIMPLEMENTED;
        }
        return O62M_TRNG_UNKEYED_CONTROL_UNIMPLEMENTED;
    }
    if (address == O62M_TRNG_UNKEYED_STAT) {
        if (write) {
            return O62M_TRNG_UNKEYED_STATUS_WRITE_UNIMPLEMENTED;
        }
        if (!model->explicit_unkeyed_policy) {
            return O62M_TRNG_UNKEYED_POLICY_REQUIRED;
        }
        /* Validity requires EMPTY AO storage and opaque.bit0==0. This is
         * a negative key-state observation, never a generated/healthy key. */
        *value = model->status_opaque;
        return O62M_TRNG_UNKEYED_OK;
    }
    for (i = 0; i < O62M_TRNG_UNKEYED_CONFIG_COUNT; ++i) {
        const O62MTrngConfigDef *def = &o62m_trng_unkeyed_defs[i];
        if (address != def->address) {
            continue;
        }
        if (!write) {
            if (!(model->written_mask & (1u << i))) {
                return O62M_TRNG_UNKEYED_UNWRITTEN_CONFIG;
            }
            *value = model->config[i];
            return O62M_TRNG_UNKEYED_OK;
        }
        if ((*value & ~def->supported_mask) || *value != def->original_value) {
            return O62M_TRNG_UNKEYED_UNSUPPORTED_CONFIG_VALUE;
        }
        model->config[i] = (uint32_t)*value;
        model->written_mask |= 1u << i;
        return O62M_TRNG_UNKEYED_OK;
    }
    return O62M_TRNG_UNKEYED_UNSUPPORTED_REGISTER;
}

#endif
