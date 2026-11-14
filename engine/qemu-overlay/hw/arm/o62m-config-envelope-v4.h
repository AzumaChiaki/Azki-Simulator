#ifndef O62M_CONFIG_ENVELOPE_V4_H
#define O62M_CONFIG_ENVELOPE_V4_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Original OTA605e823c/605e829c exact configuration-write observations.
 * Not a permission, pad, clock, security, or ready/status device model.
 * The original outer605e8308 has at most four bank cycles. Body transactions
 * are handled separately by the BSP; this helper authorizes none of them.
 */
#define O62M_CONFIG_ENVELOPE_MAX_CYCLES 4u

typedef enum O62MConfigEnvelopeResultV1 {
    O62M_CONFIG_ENVELOPE_OK = 0,
    O62M_CONFIG_ENVELOPE_BAD_ARGUMENT,
    O62M_CONFIG_ENVELOPE_DISABLED,
    O62M_CONFIG_ENVELOPE_INVALID_STATE,
    O62M_CONFIG_ENVELOPE_UNSUPPORTED_ACCESS,
    O62M_CONFIG_ENVELOPE_BAD_SEQUENCE,
    O62M_CONFIG_ENVELOPE_CYCLE_LIMIT
} O62MConfigEnvelopeResultV1;

typedef struct O62MConfigEnvelopeV1 {
    bool enabled;
    uint32_t next_step;
    uint32_t known_mask;
    uint32_t word[5];
    uint64_t accepted_writes;
    uint64_t completed_cycles;
} O62MConfigEnvelopeV1;

typedef struct O62MConfigEnvelopeOperationV1 {
    uint32_t pc;
    uint32_t address;
    uint32_t value;
    unsigned word_index;
} O62MConfigEnvelopeOperationV1;

static const O62MConfigEnvelopeOperationV1 o62m_config_envelope_ops_v1[10] = {
    {0x605e8240, 0x4c050024, 0x00000000, 0},
    {0x605e8242, 0x4c050028, 0x00000000, 1},
    {0x605e8248, 0x4a000040, 0x00000000, 2},
    {0x605e824a, 0x4a000044, 0x00000000, 3},
    {0x605e824c, 0x4a000048, 0x00000000, 4},
    {0x605e82aa, 0x4c050024, 0xffffffff, 0},
    {0x605e82ac, 0x4c050028, 0xbfffdfff, 1},
    {0x605e82b2, 0x4a000040, 0xffffffff, 2},
    {0x605e82b4, 0x4a000044, 0xffffffff, 3},
    {0x605e82ba, 0x4a000048, 0x00ffffff, 4}
};

static inline const char *o62m_config_envelope_reason_v1(O62MConfigEnvelopeResultV1 r)
{
    switch (r) {
    case O62M_CONFIG_ENVELOPE_OK: return "ok";
    case O62M_CONFIG_ENVELOPE_BAD_ARGUMENT: return "bad-argument";
    case O62M_CONFIG_ENVELOPE_DISABLED: return "explicit-observation-policy-required";
    case O62M_CONFIG_ENVELOPE_INVALID_STATE: return "invalid-observation-metadata";
    case O62M_CONFIG_ENVELOPE_UNSUPPORTED_ACCESS: return "only-proven32bit-writes-supported";
    case O62M_CONFIG_ENVELOPE_BAD_SEQUENCE: return "exact-original-write-step-required";
    case O62M_CONFIG_ENVELOPE_CYCLE_LIMIT: return "four-original-bank-cycles-exhausted";
    }
    return "invalid-result";
}

static inline bool o62m_config_envelope_address_v1(uint64_t address)
{
    return address == UINT64_C(0x4c050024) || address == UINT64_C(0x4c050028) ||
           address == UINT64_C(0x4a000040) || address == UINT64_C(0x4a000044) ||
           address == UINT64_C(0x4a000048);
}

/* Explicit host construction only, not a guest reset or rearm command. */
static inline bool o62m_config_envelope_init_v1(O62MConfigEnvelopeV1 *model,
                                                bool explicit_enable)
{
    if (!model) { return false; }
    memset(model, 0, sizeof(*model));
    model->enabled = explicit_enable;
    return true;
}

static inline bool o62m_config_envelope_valid_v1(const O62MConfigEnvelopeV1 *model)
{
    uint32_t expected_known;
    if (model->next_step >= 10 ||
        model->completed_cycles > O62M_CONFIG_ENVELOPE_MAX_CYCLES ||
        model->accepted_writes > 10u * O62M_CONFIG_ENVELOPE_MAX_CYCLES ||
        model->accepted_writes % 10u != model->next_step ||
        model->accepted_writes / 10u != model->completed_cycles) {
        return false;
    }
    expected_known = model->completed_cycles || model->next_step >= 5 ? 31u :
                     (UINT32_C(1) << model->next_step) - 1u;
    if (model->known_mask != expected_known) { return false; }
    /* The exact writes also determine every previously known word. Unknown
     * words have zero host storage from construction, never guest readback. */
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t expected = 0;
        if (model->next_step <= 5) {
            if (model->completed_cycles && i >= model->next_step) {
                expected = o62m_config_envelope_ops_v1[5 + i].value;
            }
        } else if (i < model->next_step - 5) {
            expected = o62m_config_envelope_ops_v1[5 + i].value;
        }
        if (model->word[i] != expected) { return false; }
    }
    return true;
}

/* There is deliberately no guest readback. value is input only and is never
 * changed, even for successful writes. Every rejection leaves all model and
 * value bytes unchanged. After step5, independent body accesses neither
 * advance this model nor become authorized by it; next accepted is endstep1.
 */
static inline O62MConfigEnvelopeResultV1 o62m_config_envelope_access_v1(
        O62MConfigEnvelopeV1 *model, uint64_t pc, uint64_t address,
        unsigned size, bool write, uint64_t *value)
{
    const O62MConfigEnvelopeOperationV1 *op;
    O62MConfigEnvelopeV1 next;
    if (!model || !value) { return O62M_CONFIG_ENVELOPE_BAD_ARGUMENT; }
    if (!o62m_config_envelope_valid_v1(model)) {
        return O62M_CONFIG_ENVELOPE_INVALID_STATE;
    }
    if (!model->enabled) { return O62M_CONFIG_ENVELOPE_DISABLED; }
    if (!write || size != 4 || !o62m_config_envelope_address_v1(address)) {
        return O62M_CONFIG_ENVELOPE_UNSUPPORTED_ACCESS;
    }
    if (model->completed_cycles == O62M_CONFIG_ENVELOPE_MAX_CYCLES) {
        return O62M_CONFIG_ENVELOPE_CYCLE_LIMIT;
    }
    op = &o62m_config_envelope_ops_v1[model->next_step];
    if (pc != op->pc || address != op->address || *value != op->value) {
        return O62M_CONFIG_ENVELOPE_BAD_SEQUENCE;
    }
    next = *model;
    next.word[op->word_index] = op->value;
    next.known_mask |= UINT32_C(1) << op->word_index;
    next.accepted_writes++;
    next.next_step++;
    if (next.next_step == 10) {
        next.next_step = 0;
        next.completed_cycles++;
    }
    *model = next;
    return O62M_CONFIG_ENVELOPE_OK;
}

/* Original AP pinmux configuration envelope. Same five retained words as
 * OTA, with independent firmware sequence and no OTA four-bank cycle bound. */
static inline O62MConfigEnvelopeResultV1 o62m_ap_config_envelope_access_v2(
        O62MConfigEnvelopeV1 *model, uint64_t pc, uint64_t address,
        unsigned size, bool write, uint64_t *value)
{
    static const uint32_t ap_pc[10] = {
        0x60c3af8a,0x60c3af8c,0x60c3af92,0x60c3af94,0x60c3af96,
        0x60da6f62,0x60da6f64,0x60da6f6a,0x60da6f6c,0x60da6f72
    };
    if (!model || !value) { return O62M_CONFIG_ENVELOPE_BAD_ARGUMENT; }
    if (!model->enabled) { return O62M_CONFIG_ENVELOPE_DISABLED; }
    if (!write || size != 4) { return O62M_CONFIG_ENVELOPE_UNSUPPORTED_ACCESS; }
    if (model->next_step >= 10 || model->accepted_writes == UINT64_MAX) {
        return O62M_CONFIG_ENVELOPE_INVALID_STATE;
    }
    const O62MConfigEnvelopeOperationV1 *op = &o62m_config_envelope_ops_v1[model->next_step];
    if (pc != ap_pc[model->next_step] || address != op->address || *value != op->value) {
        return O62M_CONFIG_ENVELOPE_BAD_SEQUENCE;
    }
    model->word[op->word_index] = (uint32_t)*value;
    model->known_mask |= UINT32_C(1) << op->word_index;
    model->accepted_writes++;
    if (++model->next_step == 10) {
        model->next_step = 0;
        model->completed_cycles++;
    }
    return O62M_CONFIG_ENVELOPE_OK;
}

/* Original CP sequence shares the same five configuration words. */
static inline O62MConfigEnvelopeResultV1 o62m_cp_config_envelope_access_v4(
        O62MConfigEnvelopeV1 *model, uint64_t pc, uint64_t address,
        unsigned size, bool write, uint64_t *value)
{
    static const uint32_t cp_pc[10] = {
        0x640c0d94,0x640c0d98,0x640c0d9a,0x640c0d9c,0x640c0d9e,
        0x640c0dc6,0x640c0dc8,0x640c0dce,0x640c0dd0,0x640c0dd2
    };
    static const uint32_t cp_second_pc[10] = {
        0x640c0e48,0x640c0e4c,0x640c0e4e,0x640c0e50,0x640c0e52,
        0x640c0e7a,0x640c0e7c,0x640c0e82,0x640c0e84,0x640c0e86
    };
    if (!model || !value) { return O62M_CONFIG_ENVELOPE_BAD_ARGUMENT; }
    if (!model->enabled) { return O62M_CONFIG_ENVELOPE_DISABLED; }
    if (!write || size != 4) { return O62M_CONFIG_ENVELOPE_UNSUPPORTED_ACCESS; }
    if (model->next_step >= 10 || model->accepted_writes == UINT64_MAX) {
        return O62M_CONFIG_ENVELOPE_INVALID_STATE;
    }
    const O62MConfigEnvelopeOperationV1 *op = &o62m_config_envelope_ops_v1[model->next_step];
    if ((pc != cp_pc[model->next_step] && pc != cp_second_pc[model->next_step]) || address != op->address || *value != op->value) {
        return O62M_CONFIG_ENVELOPE_BAD_SEQUENCE;
    }
    model->word[op->word_index] = (uint32_t)*value;
    model->known_mask |= UINT32_C(1) << op->word_index;
    model->accepted_writes++;
    if (++model->next_step == 10) {
        model->next_step = 0;
        model->completed_cycles++;
    }
    return O62M_CONFIG_ENVELOPE_OK;
}

#endif
