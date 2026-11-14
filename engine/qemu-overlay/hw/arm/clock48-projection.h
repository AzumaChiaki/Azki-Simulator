#ifndef O62M_CLOCK48_PROJECTION_H
#define O62M_CLOCK48_PROJECTION_H

/* Conditional configuration projection only. No clock, timer or ready effects. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum O62MClock48Result {
    O62M_CLOCK48_OK = 0,
    O62M_CLOCK48_BAD_ARGUMENT,
    O62M_CLOCK48_BAD_SEEDS,
    O62M_CLOCK48_DISABLED,
    O62M_CLOCK48_UNCONFIGURED,
    O62M_CLOCK48_BAD_ACCESS,
    O62M_CLOCK48_UNKNOWN_ADDRESS,
    O62M_CLOCK48_SEQUENCE_COMPLETE,
    O62M_CLOCK48_WRONG_PHASE,
    O62M_CLOCK48_READ_ALREADY_PENDING,
    O62M_CLOCK48_READ_REQUIRED,
    O62M_CLOCK48_RESERVED_CHANGE,
    O62M_CLOCK48_WRONG_OBSERVED_WRITE
} O62MClock48Result;

typedef struct O62MClock48Projection {
    bool enabled;
    bool configured;
    bool pending_read;
    uint8_t phase;
    uint32_t seed[5];
    uint32_t word[5];
} O62MClock48Projection;

static inline uint64_t o62m_clock48_address(unsigned index)
{
    static const uint64_t addresses[5] = {
        UINT64_C(0x50060048), UINT64_C(0x5006004c),
        UINT64_C(0x500600ec), UINT64_C(0x50060058),
        UINT64_C(0x500600f4)
    };
    return index < 5 ? addresses[index] : UINT64_MAX;
}

static inline uint32_t o62m_clock48_mask(unsigned index)
{
    return index == 2 || index == 4 ? UINT32_C(0x00020000)
                                   : UINT32_C(0x00010000);
}

static inline O62MClock48Result o62m_clock48_projection_init(
    O62MClock48Projection *state, bool enabled,
    const uint32_t *seeds, size_t seed_count)
{
    if (state == NULL) {
        return O62M_CLOCK48_BAD_ARGUMENT;
    }
    if (enabled && (seeds == NULL || seed_count != 5)) {
        return O62M_CLOCK48_BAD_SEEDS;
    }
    O62MClock48Projection next = {0};
    next.enabled = enabled;
    next.configured = enabled;
    if (enabled) {
        for (unsigned i = 0; i < 5; ++i) {
            next.seed[i] = seeds[i];
            next.word[i] = seeds[i];
        }
    }
    *state = next;
    return O62M_CLOCK48_OK;
}

static inline O62MClock48Result o62m_clock48_validate(
    const O62MClock48Projection *state, uint64_t address,
    unsigned size, unsigned *index)
{
    if (state == NULL || index == NULL) {
        return O62M_CLOCK48_BAD_ARGUMENT;
    }
    if (!state->enabled) {
        return O62M_CLOCK48_DISABLED;
    }
    if (!state->configured) {
        return O62M_CLOCK48_UNCONFIGURED;
    }
    if (size != 4 || (address & UINT64_C(3)) != 0) {
        return O62M_CLOCK48_BAD_ACCESS;
    }
    unsigned found = 5;
    for (unsigned i = 0; i < 5; ++i) {
        if (address == o62m_clock48_address(i)) {
            found = i;
            break;
        }
    }
    if (found == 5) {
        return O62M_CLOCK48_UNKNOWN_ADDRESS;
    }
    if (state->phase >= 5) {
        return O62M_CLOCK48_SEQUENCE_COMPLETE;
    }
    if (found != state->phase) {
        return O62M_CLOCK48_WRONG_PHASE;
    }
    *index = found;
    return O62M_CLOCK48_OK;
}

static inline O62MClock48Result o62m_clock48_projection_read(
    O62MClock48Projection *state, uint64_t address,
    unsigned size, uint32_t *readout)
{
    if (readout == NULL) {
        return O62M_CLOCK48_BAD_ARGUMENT;
    }
    unsigned index;
    O62MClock48Result result =
        o62m_clock48_validate(state, address, size, &index);
    if (result != O62M_CLOCK48_OK) {
        return result;
    }
    if (state->pending_read) {
        return O62M_CLOCK48_READ_ALREADY_PENDING;
    }
    const uint32_t value = state->word[index];
    state->pending_read = true;
    *readout = value;
    return O62M_CLOCK48_OK;
}

static inline O62MClock48Result o62m_clock48_projection_write(
    O62MClock48Projection *state, uint64_t address,
    unsigned size, uint32_t value)
{
    unsigned index;
    O62MClock48Result result =
        o62m_clock48_validate(state, address, size, &index);
    if (result != O62M_CLOCK48_OK) {
        return result;
    }
    if (!state->pending_read) {
        return O62M_CLOCK48_READ_REQUIRED;
    }
    const uint32_t mask = o62m_clock48_mask(index);
    const uint32_t old = state->word[index];
    if (((old ^ value) & ~mask) != 0) {
        return O62M_CLOCK48_RESERVED_CHANGE;
    }
    const uint32_t expected = index < 3 ? old & ~mask : old | mask;
    if (value != expected) {
        return O62M_CLOCK48_WRONG_OBSERVED_WRITE;
    }
    state->word[index] = value;
    state->pending_read = false;
    ++state->phase;
    return O62M_CLOCK48_OK;
}

static inline const char *o62m_clock48_reason(O62MClock48Result result)
{
    static const char *const reasons[] = {
        "ok", "bad-argument", "bad-seeds", "disabled", "unconfigured",
        "bad-width-or-alignment", "unknown-address", "sequence-complete",
        "wrong-phase", "read-already-pending", "read-required",
        "reserved-change", "wrong-observed-write"
    };
    return (unsigned)result < sizeof(reasons) / sizeof(reasons[0])
        ? reasons[result] : "unknown-error";
}

#endif
