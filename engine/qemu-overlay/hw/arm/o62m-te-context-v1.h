#ifndef O62M_TE_CONTEXT_V1_H
#define O62M_TE_CONTEXT_V1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Conditional Merak-compatible context-capacity evaluator, not a full TE.
 * Pinned public ARM2020 te_drv.c and the original O62M ABI audit establish
 * sizes, the lock trigger, successbit24 and host0-only overflowbit5.
 * Initial geometry/status/host contents and moduleHz/workcycles are explicit.
 * Offsets below describe a host-major LOGICAL accounting layout only. No
 * physical SRAM address, slot order, contents or cryptographic access exists.
 * No clock pause/reset/unlock/reconfigure/error-clear/OTP/TRNG is invented.
 */
#define O62M_TE_CONTEXT_HOST_MAX 16u
#define O62M_TE_CONTEXT_KINDS 4u
#define O62M_TE_CONTEXT_INITIAL_WORDS 24u
#define O62M_TE_CONTEXT_TOP UINT64_C(0x50333708)
#define O62M_TE_CONTEXT_STAT UINT64_C(0x50330014)
#define O62M_TE_CONTEXT_HOST0_IRQ UINT64_C(0x50330020)
#define O62M_TE_CONTEXT_READY UINT32_C(0x01000000)
#define O62M_TE_CONTEXT_OVERFLOW UINT32_C(0x00000020)

typedef enum O62MTeContextResult {
    O62M_TE_CONTEXT_OK = 0,
    O62M_TE_CONTEXT_BAD_ARGUMENT,
    O62M_TE_CONTEXT_BAD_INITIAL,
    O62M_TE_CONTEXT_UNINITIALIZED,
    O62M_TE_CONTEXT_BAD_ACCESS,
    O62M_TE_CONTEXT_UNSUPPORTED_REGISTER,
    O62M_TE_CONTEXT_UNSUPPORTED_TRANSITION,
    O62M_TE_CONTEXT_TIME_REVERSED,
    O62M_TE_CONTEXT_DEADLINE_OVERFLOW
} O62MTeContextResult;

typedef enum O62MTeContextPhase {
    O62M_TE_CONTEXT_UNLOCKED = 0,
    O62M_TE_CONTEXT_PENDING,
    O62M_TE_CONTEXT_READY_PHASE,
    O62M_TE_CONTEXT_OVERFLOW_PHASE
} O62MTeContextPhase;

typedef enum O62MTeContextKind {
    O62M_TE_CONTEXT_HASH_SHORT = 0,
    O62M_TE_CONTEXT_HASH_LONG,
    O62M_TE_CONTEXT_SCA_SHORT,
    O62M_TE_CONTEXT_SCA_LONG
} O62MTeContextKind;

typedef struct O62MTeContextSegment {
    uint32_t logical_offset;
    uint32_t length_bytes;
    unsigned configured_slots;
    unsigned allocated_slots;
    unsigned slot_bytes;
    bool participating;
} O62MTeContextSegment;

typedef struct O62MTeContext {
    unsigned hash_nhost, sca_nhost;
    uint32_t ctx_sram_word;
    uint32_t initial_top, initial_stat, initial_host0_irq;
    uint32_t top, stat, host0_irq;
    uint32_t module_hz, work_cycles;
    uint32_t host_config[O62M_TE_CONTEXT_HOST_MAX];
    O62MTeContextSegment segment[O62M_TE_CONTEXT_HOST_MAX][O62M_TE_CONTEXT_KINDS];
    uint32_t required_bytes, capacity_bytes;
    uint64_t duration_ns;
    int64_t started_ns, deadline_ns, last_virtual_ns;
    O62MTeContextPhase phase;
    unsigned evaluations_started;
    bool initialized;
} O62MTeContext;

typedef struct O62MTeContextSnapshot {
    unsigned hash_nhost, sca_nhost;
    uint32_t host_config[O62M_TE_CONTEXT_HOST_MAX];
    O62MTeContextSegment segment[O62M_TE_CONTEXT_HOST_MAX][O62M_TE_CONTEXT_KINDS];
    uint32_t required_bytes, capacity_bytes;
    bool capacity_fits;
    uint32_t top, stat, host0_irq;
    uint64_t duration_ns;
    int64_t started_ns, deadline_ns, last_virtual_ns;
    O62MTeContextPhase phase;
    unsigned evaluations_started;
} O62MTeContextSnapshot;

static inline unsigned o62m_te_context_slot_bytes(unsigned kind)
{
    static const unsigned bytes[O62M_TE_CONTEXT_KINDS] = {44u, 84u, 52u, 84u};
    return kind < O62M_TE_CONTEXT_KINDS ? bytes[kind] : 0u;
}

static inline const char *o62m_te_context_reason(O62MTeContextResult result)
{
    switch (result) {
    case O62M_TE_CONTEXT_OK: return "ok";
    case O62M_TE_CONTEXT_BAD_ARGUMENT: return "bad-argument";
    case O62M_TE_CONTEXT_BAD_INITIAL: return "bad-explicit-initial-state";
    case O62M_TE_CONTEXT_UNINITIALIZED: return "uninitialized-or-invalid-state";
    case O62M_TE_CONTEXT_BAD_ACCESS: return "u32-word-access-required";
    case O62M_TE_CONTEXT_UNSUPPORTED_REGISTER: return "context-register-unimplemented";
    case O62M_TE_CONTEXT_UNSUPPORTED_TRANSITION: return "unlock-reconfigure-or-unrelated-top-change-unimplemented";
    case O62M_TE_CONTEXT_TIME_REVERSED: return "virtual-time-reversed";
    case O62M_TE_CONTEXT_DEADLINE_OVERFLOW: return "virtual-deadline-overflow";
    }
    return "invalid-result";
}

/* Initial words: HASHnhost, SCAnhost, fullCTX_SRAMword(low16 count), TOP,
 * STAT, HOST0IRQ, moduleHz, workcycles, sixteen independently explicit cfgs.
 * This supports a clean unlocked attempt only, not a pre-initialized device.
 * Unknown TOP/STAT/IRQ bits remain exact input. Hostcfg upper4 bits are out of
 * this supported profile, not classified as a silicon validation failure.
 */
static inline O62MTeContextResult o62m_te_context_init(O62MTeContext *model,
                                                      const uint64_t *words,
                                                      size_t count)
{
    O62MTeContext next = {0};
    uint32_t cursor = 0;
    unsigned i, k, used;

    if (!model || !words || count != O62M_TE_CONTEXT_INITIAL_WORDS) {
        return O62M_TE_CONTEXT_BAD_ARGUMENT;
    }
    for (i = 0; i < O62M_TE_CONTEXT_INITIAL_WORDS; ++i) {
        if (words[i] > UINT32_MAX) {
            return O62M_TE_CONTEXT_BAD_INITIAL;
        }
    }
    if (words[0] < 1u || words[0] > O62M_TE_CONTEXT_HOST_MAX ||
        words[1] < 1u || words[1] > O62M_TE_CONTEXT_HOST_MAX ||
        !(words[2] & UINT32_C(0xffff)) || (words[3] & 3u) ||
        (words[4] & O62M_TE_CONTEXT_READY) ||
        (words[5] & O62M_TE_CONTEXT_OVERFLOW) ||
        words[6] < 1u || words[6] > UINT32_C(1000000000) ||
        words[7] < 1u || words[7] > UINT32_C(1000000)) {
        return O62M_TE_CONTEXT_BAD_INITIAL;
    }
    next.hash_nhost = (unsigned)words[0];
    next.sca_nhost = (unsigned)words[1];
    next.ctx_sram_word = (uint32_t)words[2];
    next.initial_top = next.top = (uint32_t)words[3];
    next.initial_stat = next.stat = (uint32_t)words[4];
    next.initial_host0_irq = next.host0_irq = (uint32_t)words[5];
    next.module_hz = (uint32_t)words[6];
    next.work_cycles = (uint32_t)words[7];
    next.capacity_bytes = (next.ctx_sram_word & UINT32_C(0xffff)) * 4u;
    next.duration_ns = ((uint64_t)next.work_cycles * UINT64_C(1000000000) +
                        next.module_hz - 1u) / next.module_hz;
    used = next.hash_nhost > next.sca_nhost ? next.hash_nhost : next.sca_nhost;
    for (i = 0; i < O62M_TE_CONTEXT_HOST_MAX; ++i) {
        uint32_t cfg = (uint32_t)words[8u + i];
        if ((cfg & UINT32_C(0xf0000000)) || (i >= used && cfg != 0)) {
            return O62M_TE_CONTEXT_BAD_INITIAL;
        }
        /* This is immutable initial data, including host0 RO security bits. */
        next.host_config[i] = cfg;
        for (k = 0; k < O62M_TE_CONTEXT_KINDS; ++k) {
            O62MTeContextSegment *segment = &next.segment[i][k];
            segment->participating = i < (k < 2u ? next.hash_nhost : next.sca_nhost);
            segment->configured_slots = (cfg >> (6u * k)) & 63u;
            segment->allocated_slots = segment->participating ? segment->configured_slots : 0u;
            segment->slot_bytes = o62m_te_context_slot_bytes(k);
            segment->logical_offset = cursor;
            segment->length_bytes = segment->allocated_slots * segment->slot_bytes;
            cursor += segment->length_bytes;
        }
    }
    next.required_bytes = cursor;
    next.phase = O62M_TE_CONTEXT_UNLOCKED;
    next.initialized = true;
    *model = next;
    return O62M_TE_CONTEXT_OK;
}

static inline bool o62m_te_context_valid(const O62MTeContext *model)
{
    uint32_t cursor = 0;
    unsigned i, k, used;
    uint64_t duration;
    bool success;

    if (!model || !model->initialized || model->hash_nhost < 1u ||
        model->hash_nhost > O62M_TE_CONTEXT_HOST_MAX || model->sca_nhost < 1u ||
        model->sca_nhost > O62M_TE_CONTEXT_HOST_MAX ||
        !(model->ctx_sram_word & UINT32_C(0xffff)) ||
        (model->initial_top & 3u) || (model->initial_stat & O62M_TE_CONTEXT_READY) ||
        (model->initial_host0_irq & O62M_TE_CONTEXT_OVERFLOW) ||
        model->module_hz < 1u || model->module_hz > UINT32_C(1000000000) ||
        model->work_cycles < 1u || model->work_cycles > UINT32_C(1000000) ||
        model->last_virtual_ns < 0) {
        return false;
    }
    duration = ((uint64_t)model->work_cycles * UINT64_C(1000000000) +
                 model->module_hz - 1u) / model->module_hz;
    if (model->duration_ns != duration ||
        model->capacity_bytes != (model->ctx_sram_word & UINT32_C(0xffff)) * 4u) {
        return false;
    }
    used = model->hash_nhost > model->sca_nhost ? model->hash_nhost : model->sca_nhost;
    for (i = 0; i < O62M_TE_CONTEXT_HOST_MAX; ++i) {
        uint32_t cfg = model->host_config[i];
        if ((cfg & UINT32_C(0xf0000000)) || (i >= used && cfg != 0)) {
            return false;
        }
        for (k = 0; k < O62M_TE_CONTEXT_KINDS; ++k) {
            const O62MTeContextSegment *segment = &model->segment[i][k];
            bool active = i < (k < 2u ? model->hash_nhost : model->sca_nhost);
            unsigned configured = (cfg >> (6u * k)) & 63u;
            unsigned allocated = active ? configured : 0u;
            unsigned size = o62m_te_context_slot_bytes(k);
            if (segment->participating != active ||
                segment->configured_slots != configured ||
                segment->allocated_slots != allocated || segment->slot_bytes != size ||
                segment->logical_offset != cursor || segment->length_bytes != allocated * size) {
                return false;
            }
            cursor += segment->length_bytes;
        }
    }
    if (model->required_bytes != cursor) {
        return false;
    }
    if (model->phase == O62M_TE_CONTEXT_UNLOCKED) {
        return model->top == model->initial_top && model->stat == model->initial_stat &&
               model->host0_irq == model->initial_host0_irq &&
               model->evaluations_started == 0u && model->started_ns == 0 &&
               model->deadline_ns == 0;
    }
    if (model->phase < O62M_TE_CONTEXT_PENDING || model->phase > O62M_TE_CONTEXT_OVERFLOW_PHASE ||
        model->top != (model->initial_top | 1u) || model->evaluations_started != 1u ||
        model->started_ns < 0 || model->started_ns > INT64_MAX - (int64_t)duration ||
        model->deadline_ns != model->started_ns + (int64_t)duration ||
        model->last_virtual_ns < model->started_ns) {
        return false;
    }
    if (model->phase == O62M_TE_CONTEXT_PENDING) {
        return model->last_virtual_ns < model->deadline_ns &&
               model->stat == model->initial_stat && model->host0_irq == model->initial_host0_irq;
    }
    success = model->required_bytes <= model->capacity_bytes;
    return model->last_virtual_ns >= model->deadline_ns &&
           model->phase == (success ? O62M_TE_CONTEXT_READY_PHASE : O62M_TE_CONTEXT_OVERFLOW_PHASE) &&
           model->stat == (model->initial_stat | (success ? O62M_TE_CONTEXT_READY : 0u)) &&
           model->host0_irq == (model->initial_host0_irq | (success ? 0u : O62M_TE_CONTEXT_OVERFLOW));
}

/* Internal staged time advancement. Call only on a valid trial copy. */
static inline void o62m_te_context_step(O62MTeContext *trial, int64_t virtual_ns)
{
    if (trial->phase == O62M_TE_CONTEXT_PENDING && virtual_ns >= trial->deadline_ns) {
        if (trial->required_bytes <= trial->capacity_bytes) {
            trial->stat |= O62M_TE_CONTEXT_READY;
            trial->phase = O62M_TE_CONTEXT_READY_PHASE;
        } else {
            trial->host0_irq |= O62M_TE_CONTEXT_OVERFLOW;
            trial->phase = O62M_TE_CONTEXT_OVERFLOW_PHASE;
        }
    }
    trial->last_virtual_ns = virtual_ns;
}

/* Only TOP read/write, STAT read, and HOST0IRQ read are exposed.
 * A valid lock0->1 write starts exactly one capacity evaluation. Identical
 * locked writes are idempotent: no restart, unlock, flagclear, or retry.
 * Every failure preserves all state bytes and the caller's value, including
 * a pending deadline reached during a subsequently rejected transaction. */
static inline O62MTeContextResult o62m_te_context_access(O62MTeContext *model,
                                                        uint64_t address,
                                                        unsigned size, bool write,
                                                        uint64_t *value,
                                                        int64_t virtual_ns)
{
    O62MTeContext trial;
    bool trigger = false;
    uint32_t sample;

    if (!model || !value) {
        return O62M_TE_CONTEXT_BAD_ARGUMENT;
    }
    if (!o62m_te_context_valid(model)) {
        return O62M_TE_CONTEXT_UNINITIALIZED;
    }
    if (size != 4u || (address & 3u) || (write && *value > UINT32_MAX)) {
        return O62M_TE_CONTEXT_BAD_ACCESS;
    }
    if (address != O62M_TE_CONTEXT_TOP && address != O62M_TE_CONTEXT_STAT &&
        address != O62M_TE_CONTEXT_HOST0_IRQ) {
        return O62M_TE_CONTEXT_UNSUPPORTED_REGISTER;
    }
    if (write) {
        if (address != O62M_TE_CONTEXT_TOP) {
            return O62M_TE_CONTEXT_UNSUPPORTED_REGISTER;
        }
        if (*value != model->top) {
            if (model->phase == O62M_TE_CONTEXT_UNLOCKED && *value == (model->top | 1u)) {
                trigger = true;
            } else {
                return O62M_TE_CONTEXT_UNSUPPORTED_TRANSITION;
            }
        }
    }
    if (virtual_ns < model->last_virtual_ns) {
        return O62M_TE_CONTEXT_TIME_REVERSED;
    }
    if (trigger && virtual_ns > INT64_MAX - (int64_t)model->duration_ns) {
        return O62M_TE_CONTEXT_DEADLINE_OVERFLOW;
    }
    trial = *model;
    o62m_te_context_step(&trial, virtual_ns);
    if (trigger) {
        trial.top |= 1u;
        trial.started_ns = virtual_ns;
        trial.deadline_ns = virtual_ns + (int64_t)trial.duration_ns;
        trial.phase = O62M_TE_CONTEXT_PENDING;
        trial.evaluations_started = 1u;
    }
    if (!write) {
        sample = address == O62M_TE_CONTEXT_TOP ? trial.top :
                 address == O62M_TE_CONTEXT_STAT ? trial.stat : trial.host0_irq;
        *value = sample;
    }
    *model = trial;
    return O62M_TE_CONTEXT_OK;
}

static inline O62MTeContextResult o62m_te_context_snapshot(const O62MTeContext *model,
                                                          O62MTeContextSnapshot *out)
{
    O62MTeContextSnapshot next = {0};
    unsigned i, k;

    if (!model || !out) {
        return O62M_TE_CONTEXT_BAD_ARGUMENT;
    }
    if (!o62m_te_context_valid(model)) {
        return O62M_TE_CONTEXT_UNINITIALIZED;
    }
    next.hash_nhost = model->hash_nhost;
    next.sca_nhost = model->sca_nhost;
    for (i = 0; i < O62M_TE_CONTEXT_HOST_MAX; ++i) {
        next.host_config[i] = model->host_config[i];
        for (k = 0; k < O62M_TE_CONTEXT_KINDS; ++k) {
            next.segment[i][k] = model->segment[i][k];
        }
    }
    next.required_bytes = model->required_bytes;
    next.capacity_bytes = model->capacity_bytes;
    next.capacity_fits = model->required_bytes <= model->capacity_bytes;
    next.top = model->top;
    next.stat = model->stat;
    next.host0_irq = model->host0_irq;
    next.duration_ns = model->duration_ns;
    next.started_ns = model->started_ns;
    next.deadline_ns = model->deadline_ns;
    next.last_virtual_ns = model->last_virtual_ns;
    next.phase = model->phase;
    next.evaluations_started = model->evaluations_started;
    *out = next;
    return O62M_TE_CONTEXT_OK;
}

#endif
