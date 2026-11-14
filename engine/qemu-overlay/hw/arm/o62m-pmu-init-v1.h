#ifndef O62M_PMU_INIT_V1_H
#define O62M_PMU_INIT_V1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A disabled-counting subset of the architectural, unbanked v8.1-M PMU.
 * No elapsed-time, instruction, event, IRQ, DWT, or debug model is supplied.
 * The first transaction that would leave CTRL.E && CNTEN nonzero is rejected
 * without committing any state. Every initial value is an explicit input.
 * References: Arm DDI0553B.y D1.2.199/200/201/204/208, pp1785-1801. */
#define O62M_PMU_INITIAL_WORDS 13u
#define O62M_PMU_EVENT_MAX 8u
#define O62M_PMU_CTRL_RETAINED UINT32_C(0x00000a21)
#define O62M_PMU_CTRL_COMMANDS UINT32_C(0x00000006)

typedef enum O62MPMUResult {
    O62M_PMU_OK = 0,
    O62M_PMU_BAD_ARGUMENT,
    O62M_PMU_BAD_INITIAL,
    O62M_PMU_UNINITIALIZED,
    O62M_PMU_BAD_ACCESS,
    O62M_PMU_UNSUPPORTED_REGISTER,
    O62M_PMU_UNSUPPORTED_COUNTING
} O62MPMUResult;

typedef struct O62MPMU {
    uint32_t control;
    uint32_t enabled;
    uint32_t overflow;
    uint32_t cycle_counter;
    uint16_t event_counter[O62M_PMU_EVENT_MAX];
    unsigned event_count;
    bool initialized;
} O62MPMU;

static inline uint32_t o62m_pmu_counter_mask(unsigned count)
{
    if (count < 2u || count > O62M_PMU_EVENT_MAX) {
        return 0;
    }
    return UINT32_C(0x80000000) | ((UINT32_C(1) << count) - 1u);
}

static inline const char *o62m_pmu_reason(O62MPMUResult result)
{
    switch (result) {
    case O62M_PMU_OK: return "ok";
    case O62M_PMU_BAD_ARGUMENT: return "bad-argument";
    case O62M_PMU_BAD_INITIAL: return "bad-explicit-initial-state";
    case O62M_PMU_UNINITIALIZED: return "uninitialized-or-invalid-state";
    case O62M_PMU_BAD_ACCESS: return "privileged-word-access-required";
    case O62M_PMU_UNSUPPORTED_REGISTER: return "register-or-write-unimplemented";
    case O62M_PMU_UNSUPPORTED_COUNTING: return "live-counting-unimplemented";
    }
    return "invalid-result";
}

/* words: eventCount, CTRL, CNTEN, OVS, CCNTR, EVCNTR0..7.
 * Initial CNTEN must be zero: this helper has no prior live-counting history.
 * Absent counters require zero input rather than discarding unknown values. */
static inline O62MPMUResult o62m_pmu_init(O62MPMU *model,
                                         const uint64_t *words, size_t count)
{
    O62MPMU next = {0};
    uint32_t mask;
    unsigned i;

    if (!model || !words || count != O62M_PMU_INITIAL_WORDS) {
        return O62M_PMU_BAD_ARGUMENT;
    }
    for (i = 0; i < O62M_PMU_INITIAL_WORDS; ++i) {
        if (words[i] > UINT32_MAX) {
            return O62M_PMU_BAD_INITIAL;
        }
    }
    mask = o62m_pmu_counter_mask((unsigned)words[0]);
    if (!mask || (words[1] & ~O62M_PMU_CTRL_RETAINED) || words[2] != 0 ||
        (words[3] & ~mask)) {
        return O62M_PMU_BAD_INITIAL;
    }
    next.event_count = (unsigned)words[0];
    next.control = (uint32_t)words[1];
    next.overflow = (uint32_t)words[3];
    next.cycle_counter = (uint32_t)words[4];
    for (i = 0; i < O62M_PMU_EVENT_MAX; ++i) {
        if (words[5u + i] > UINT16_MAX ||
            (i >= next.event_count && words[5u + i] != 0)) {
            return O62M_PMU_BAD_INITIAL;
        }
        next.event_counter[i] = (uint16_t)words[5u + i];
    }
    next.initialized = true;
    *model = next;
    return O62M_PMU_OK;
}

static inline bool o62m_pmu_valid(const O62MPMU *model)
{
    uint32_t mask;
    unsigned i;

    if (!model || !model->initialized) {
        return false;
    }
    mask = o62m_pmu_counter_mask(model->event_count);
    if (!mask || (model->control & ~O62M_PMU_CTRL_RETAINED) ||
        (model->enabled & ~mask) || (model->overflow & ~mask) ||
        ((model->control & 1u) && model->enabled)) {
        return false;
    }
    for (i = model->event_count; i < O62M_PMU_EVENT_MAX; ++i) {
        if (model->event_counter[i] != 0) {
            return false;
        }
    }
    return true;
}

/* offset is relative to 0xe0003000. Processor accesses only, not DAP.
 * Security state intentionally selects no bank. The architectural SAU/IDAU
 * exemption is a CPU translation responsibility, outside this state helper.
 * On every non-OK result, state and *value are unchanged. */
static inline O62MPMUResult o62m_pmu_access(O62MPMU *model, uint32_t offset,
                                           unsigned size, bool write,
                                           bool secure, bool privileged,
                                           uint64_t *value)
{
    O62MPMU next;
    uint32_t result, mask, word;
    unsigned i;

    (void)secure;
    if (!model || !value) {
        return O62M_PMU_BAD_ARGUMENT;
    }
    if (!o62m_pmu_valid(model)) {
        return O62M_PMU_UNINITIALIZED;
    }
    if (!privileged || size != 4u || (offset & 3u) ||
        (write && *value > UINT32_MAX)) {
        return O62M_PMU_BAD_ACCESS;
    }
    if (!write) {
        if (offset == 0xe04u) {
            result = model->control;
        } else if (offset == 0xc00u || offset == 0xc20u) {
            result = model->enabled;
        } else if (offset == 0xc80u) {
            result = model->overflow;
        } else if (offset == 0x07cu) {
            result = model->cycle_counter;
        } else if (offset < model->event_count * 4u) {
            result = model->event_counter[offset / 4u];
        } else {
            return O62M_PMU_UNSUPPORTED_REGISTER;
        }
        *value = result;
        return O62M_PMU_OK;
    }

    next = *model;
    word = (uint32_t)*value;
    mask = o62m_pmu_counter_mask(model->event_count);
    if (offset == 0xe04u) {
        /* Unsupported RES0 writes stay explicit rather than guessed. */
        if (word & ~(O62M_PMU_CTRL_RETAINED | O62M_PMU_CTRL_COMMANDS)) {
            return O62M_PMU_UNSUPPORTED_REGISTER;
        }
        next.control = word & O62M_PMU_CTRL_RETAINED;
        if (word & 4u) {
            next.cycle_counter = 0;
        }
        if (word & 2u) {
            for (i = 0; i < next.event_count; ++i) {
                next.event_counter[i] = 0;
            }
        }
    } else if (offset == 0xc00u) {
        next.enabled |= word & mask;
    } else if (offset == 0xc20u) {
        next.enabled &= ~(word & mask);
    } else if (offset == 0xc80u) {
        next.overflow &= ~(word & mask);
    } else {
        return O62M_PMU_UNSUPPORTED_REGISTER;
    }
    if ((next.control & 1u) && next.enabled) {
        return O62M_PMU_UNSUPPORTED_COUNTING;
    }
    *model = next;
    return O62M_PMU_OK;
}

#endif
