#ifndef O62M_AON144_READ_PROJECTION_H
#define O62M_AON144_READ_PROJECTION_H

/* One original is_enabled query only; no PLL, gate timing, or clock-ready model. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum O62MAon144ReadResult {
    O62M_AON144_READ_OK = 0,
    O62M_AON144_READ_BAD_ARGUMENT,
    O62M_AON144_READ_BAD_INITIAL,
    O62M_AON144_READ_DISABLED,
    O62M_AON144_READ_BAD_ACCESS,
    O62M_AON144_READ_UNKNOWN_ADDRESS,
    O62M_AON144_READ_SEQUENCE_COMPLETE
} O62MAon144ReadResult;

typedef struct O62MAon144ReadProjection {
    bool enabled;
    bool consumed;
    uint32_t word;
} O62MAon144ReadProjection;

/*
 * Host experiment lifecycle, not a hardware reset/reset-value claim.
 * Default-off via zero initialization or disabled init.
 * Enabled init requires independent explicit full-u32 input; explicit 0 is legal.
 * Failed init preserves state. Successful init starts a new single-read trace.
 */
static inline O62MAon144ReadResult o62m_aon144_read_init(
    O62MAon144ReadProjection *state, bool enabled, bool present, uint64_t word)
{
    if (state == NULL) {
        return O62M_AON144_READ_BAD_ARGUMENT;
    }
    if (enabled && (!present || word > UINT32_MAX)) {
        return O62M_AON144_READ_BAD_INITIAL;
    }
    O62MAon144ReadProjection next = {0};
    next.enabled = enabled;
    if (enabled) {
        next.word = (uint32_t)word;
    }
    *state = next;
    return O62M_AON144_READ_OK;
}

/*
 * Only absolute 5a098144, aligned32, exactly once. No alias and no write API.
 * Valid initialized state and independently stored readout are required.
 * Rejection changes neither state nor readout. Success consumes only an
 * experiment trace permission, not a hardware read effect.
 */
static inline O62MAon144ReadResult o62m_aon144_read(
    O62MAon144ReadProjection *state, uint64_t address,
    unsigned size, uint32_t *readout)
{
    if (state == NULL || readout == NULL) {
        return O62M_AON144_READ_BAD_ARGUMENT;
    }
    if (!state->enabled) {
        return O62M_AON144_READ_DISABLED;
    }
    if (size != 4 || (address & UINT64_C(3)) != 0) {
        return O62M_AON144_READ_BAD_ACCESS;
    }
    if (address != UINT64_C(0x5a098144)) {
        return O62M_AON144_READ_UNKNOWN_ADDRESS;
    }
    if (state->consumed) {
        return O62M_AON144_READ_SEQUENCE_COMPLETE;
    }
    const uint32_t word = state->word;
    state->consumed = true;
    *readout = word;
    return O62M_AON144_READ_OK;
}

static inline const char *o62m_aon144_read_reason(O62MAon144ReadResult result)
{
    static const char *const reasons[] = {
        "ok", "bad-argument", "missing-or-out-of-u32-initial", "disabled",
        "bad-width-or-alignment", "unknown-address", "sequence-complete"
    };
    return (unsigned)result < sizeof(reasons) / sizeof(reasons[0])
        ? reasons[result] : "unknown-error";
}

#endif
