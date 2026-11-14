#ifndef O62M_CLOCK_CONTROLS_H
#define O62M_CLOCK_CONTROLS_H

/* Retained control words only. No PLL status, clock edges or reset values. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define O62M_CLOCK_CONTROLS_MAX 1024u
#define O62M_CLOCK_CONTROL_FIELDS_MAX 32u

typedef struct O62MClockControlDef {
    uint64_t address;
    /* Each proved consumer changes one field; do not authorize mixed writes. */
    uint32_t field_mask[O62M_CLOCK_CONTROL_FIELDS_MAX];
} O62MClockControlDef;

typedef enum O62MClockControlsResult {
    O62M_CLOCK_CONTROLS_OK = 0,
    O62M_CLOCK_CONTROLS_BAD_ARGUMENT,
    O62M_CLOCK_CONTROLS_BAD_INPUTS,
    O62M_CLOCK_CONTROLS_BAD_DEFINITIONS,
    O62M_CLOCK_CONTROLS_DISABLED,
    O62M_CLOCK_CONTROLS_BAD_ACCESS,
    O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS,
    O62M_CLOCK_CONTROLS_VALUE_RANGE,
    O62M_CLOCK_CONTROLS_READ_ONLY,
    O62M_CLOCK_CONTROLS_OPAQUE_CHANGE
} O62MClockControlsResult;

typedef struct O62MClockControls {
    bool enabled;
    size_t count;
    const O62MClockControlDef *defs;
    uint32_t word[O62M_CLOCK_CONTROLS_MAX];
} O62MClockControls;

/* Definitions have static lifetime and are independently audited consumers.
 * Every initial full word is explicit. Invalid starts preserve prior state.
 * Successful starts are a host experiment lifecycle, not hardware reset. */
static inline O62MClockControlsResult o62m_clock_controls_init(
    O62MClockControls *state, bool enabled, const O62MClockControlDef *defs,
    size_t count, const uint32_t *seeds, size_t seed_count)
{
    if (state == NULL) {
        return O62M_CLOCK_CONTROLS_BAD_ARGUMENT;
    }
    if (enabled && (defs == NULL || seeds == NULL || count == 0 ||
                    count > O62M_CLOCK_CONTROLS_MAX || seed_count != count)) {
        return O62M_CLOCK_CONTROLS_BAD_INPUTS;
    }
    if (enabled) {
        for (size_t i = 0; i < count; ++i) {
            if ((defs[i].address & UINT64_C(3)) != 0) {
                return O62M_CLOCK_CONTROLS_BAD_DEFINITIONS;
            }
            for (unsigned f = 0; f < O62M_CLOCK_CONTROL_FIELDS_MAX; ++f) {
                for (unsigned g = 0; g < f; ++g) {
                    if ((defs[i].field_mask[f] & defs[i].field_mask[g]) != 0) {
                        return O62M_CLOCK_CONTROLS_BAD_DEFINITIONS;
                    }
                }
            }
            for (size_t j = 0; j < i; ++j) {
                if (defs[j].address == defs[i].address) {
                    return O62M_CLOCK_CONTROLS_BAD_DEFINITIONS;
                }
            }
        }
    }
    O62MClockControls next = {0};
    next.enabled = enabled;
    if (enabled) {
        next.defs = defs;
        next.count = count;
        for (size_t i = 0; i < count; ++i) {
            next.word[i] = seeds[i];
        }
    }
    *state = next;
    return O62M_CLOCK_CONTROLS_OK;
}

static inline O62MClockControlsResult o62m_clock_controls_index(
    const O62MClockControls *state, uint64_t address, unsigned size, size_t *index)
{
    if (state == NULL || index == NULL) {
        return O62M_CLOCK_CONTROLS_BAD_ARGUMENT;
    }
    if (!state->enabled) {
        return O62M_CLOCK_CONTROLS_DISABLED;
    }
    if (size != 4 || (address & UINT64_C(3)) != 0) {
        return O62M_CLOCK_CONTROLS_BAD_ACCESS;
    }
    for (size_t i = 0; i < state->count; ++i) {
        if (state->defs[i].address == address) {
            *index = i;
            return O62M_CLOCK_CONTROLS_OK;
        }
    }
    return O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS;
}

/* Repeated reads return the retained word. They do not advance a handshake. */
static inline O62MClockControlsResult o62m_clock_controls_read(
    const O62MClockControls *state, uint64_t address, unsigned size, uint32_t *out)
{
    if (out == NULL) {
        return O62M_CLOCK_CONTROLS_BAD_ARGUMENT;
    }
    size_t i;
    O62MClockControlsResult result =
        o62m_clock_controls_index(state, address, size, &i);
    if (result != O62M_CLOCK_CONTROLS_OK) {
        return result;
    }
    *out = state->word[i];
    return O62M_CLOCK_CONTROLS_OK;
}

/* The mask restricts changed bits, not opaque readback. No side effect beyond
 * retained control state is claimed. Unproved/read-only fields reject writes. */
static inline O62MClockControlsResult o62m_clock_controls_write(
    O62MClockControls *state, uint64_t address, unsigned size, uint64_t value)
{
    size_t i;
    O62MClockControlsResult result =
        o62m_clock_controls_index(state, address, size, &i);
    if (result != O62M_CLOCK_CONTROLS_OK) {
        return result;
    }
    if (value > UINT32_MAX) {
        return O62M_CLOCK_CONTROLS_VALUE_RANGE;
    }
    bool writable = false, allowed = false;
    uint32_t delta = state->word[i] ^ (uint32_t)value;
    for (unsigned field = 0; field < O62M_CLOCK_CONTROL_FIELDS_MAX; ++field) {
        uint32_t mask = state->defs[i].field_mask[field];
        if (mask != 0) {
            writable = true;
            if ((delta & ~mask) == 0) {
                allowed = true;
            }
        }
    }
    if (!writable) {
        return O62M_CLOCK_CONTROLS_READ_ONLY;
    }
    if (!allowed) {
        return O62M_CLOCK_CONTROLS_OPAQUE_CHANGE;
    }
    state->word[i] = (uint32_t)value;
    return O62M_CLOCK_CONTROLS_OK;
}

static inline const char *o62m_clock_controls_reason(O62MClockControlsResult result)
{
    static const char *const reason[] = {
        "ok", "bad-argument", "missing-or-wrong-count-inputs", "invalid-definitions",
        "disabled", "bad-width-or-alignment", "unknown-address", "out-of-u32",
        "read-only", "unproved-bits-changed"
    };
    return (unsigned)result < sizeof(reason) / sizeof(reason[0])
        ? reason[result] : "unknown-error";
}

#endif
