#ifndef O62M_TIMER0_STOP_PROJECTION_H
#define O62M_TIMER0_STOP_PROJECTION_H

/* Constructor stop RMW observation only; no counter, busy, IRQ, or clock model. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum O62MTimer0StopResult {
    O62M_TIMER0_STOP_OK = 0,
    O62M_TIMER0_STOP_BAD_ARGUMENT,
    O62M_TIMER0_STOP_BAD_INITIAL,
    O62M_TIMER0_STOP_DISABLED,
    O62M_TIMER0_STOP_BAD_ACCESS,
    O62M_TIMER0_STOP_UNKNOWN_ADDRESS,
    O62M_TIMER0_STOP_SEQUENCE_COMPLETE,
    O62M_TIMER0_STOP_READ_ALREADY_PENDING,
    O62M_TIMER0_STOP_READ_REQUIRED,
    O62M_TIMER0_STOP_RESERVED_CHANGE,
    O62M_TIMER0_STOP_WRONG_OBSERVED_WRITE,
    O62M_TIMER0_STOP_VALUE_RANGE
} O62MTimer0StopResult;

typedef struct O62MTimer0StopProjection {
    bool enabled;
    /* 0: one read required; 1: one write required; 2: sequence observed. */
    uint8_t phase;
    uint32_t initial;
    uint32_t word;
} O62MTimer0StopProjection;

/*
 * Host experiment lifecycle only: no hardware reset/reset-value claim.
 * Default-off via zero initialization or init(..., false, ...).
 * Explicit full-u32 input required when enabled; present=true,value=0 is legal.
 * Invalid init leaves state unchanged. Successful init starts a new experiment.
 */
static inline O62MTimer0StopResult o62m_timer0_stop_init(
    O62MTimer0StopProjection *state, bool enabled,
    bool initial_present, uint64_t initial)
{
    if (state == NULL) {
        return O62M_TIMER0_STOP_BAD_ARGUMENT;
    }
    if (enabled && (!initial_present || initial > UINT32_MAX)) {
        return O62M_TIMER0_STOP_BAD_INITIAL;
    }
    O62MTimer0StopProjection next = {0};
    next.enabled = enabled;
    if (enabled) {
        next.initial = (uint32_t)initial;
        next.word = (uint32_t)initial;
    }
    *state = next;
    return O62M_TIMER0_STOP_OK;
}

static inline O62MTimer0StopResult o62m_timer0_stop_validate(
    const O62MTimer0StopProjection *state, uint64_t address, unsigned size)
{
    if (state == NULL) {
        return O62M_TIMER0_STOP_BAD_ARGUMENT;
    }
    if (!state->enabled) {
        return O62M_TIMER0_STOP_DISABLED;
    }
    if (size != 4 || (address & UINT64_C(3)) != 0) {
        return O62M_TIMER0_STOP_BAD_ACCESS;
    }
    if (address != UINT64_C(0x50030010)) {
        return O62M_TIMER0_STOP_UNKNOWN_ADDRESS;
    }
    if (state->phase >= 2) {
        return O62M_TIMER0_STOP_SEQUENCE_COMPLETE;
    }
    return O62M_TIMER0_STOP_OK;
}

/*
 * Valid initialized state and independent output storage are required.
 * A successful read marks only the experiment trace, not a hardware read effect.
 * Every rejection preserves state and readout.
 */
static inline O62MTimer0StopResult o62m_timer0_stop_read(
    O62MTimer0StopProjection *state, uint64_t address,
    unsigned size, uint32_t *readout)
{
    if (readout == NULL) {
        return O62M_TIMER0_STOP_BAD_ARGUMENT;
    }
    O62MTimer0StopResult result =
        o62m_timer0_stop_validate(state, address, size);
    if (result != O62M_TIMER0_STOP_OK) {
        return result;
    }
    if (state->phase != 0) {
        return O62M_TIMER0_STOP_READ_ALREADY_PENDING;
    }
    const uint32_t value = state->word;
    state->phase = 1;
    *readout = value;
    return O62M_TIMER0_STOP_OK;
}

static inline O62MTimer0StopResult o62m_timer0_stop_write(
    O62MTimer0StopProjection *state, uint64_t address,
    unsigned size, uint64_t value)
{
    O62MTimer0StopResult result =
        o62m_timer0_stop_validate(state, address, size);
    if (result != O62M_TIMER0_STOP_OK) {
        return result;
    }
    if (state->phase != 1) {
        return O62M_TIMER0_STOP_READ_REQUIRED;
    }
    if (value > UINT32_MAX) {
        return O62M_TIMER0_STOP_VALUE_RANGE;
    }
    /* Preserve every other bit, including opaque/mode bits; no decode implied. */
    const uint32_t preserve = UINT32_C(0xfffffffe);
    if (((state->initial ^ (uint32_t)value) & preserve) != 0) {
        return O62M_TIMER0_STOP_RESERVED_CHANGE;
    }
    const uint32_t expected = state->initial & preserve;
    if (value != expected) {
        return O62M_TIMER0_STOP_WRONG_OBSERVED_WRITE;
    }
    state->word = (uint32_t)value;
    state->phase = 2;
    return O62M_TIMER0_STOP_OK;
}

static inline const char *o62m_timer0_stop_reason(O62MTimer0StopResult result)
{
    static const char *const reasons[] = {
        "ok", "bad-argument", "missing-or-out-of-u32-initial", "disabled",
        "bad-width-or-alignment", "unknown-address", "sequence-complete",
        "read-already-pending", "read-required", "preserved-bits-changed",
        "wrong-observed-write", "value-out-of-u32"
    };
    return (unsigned)result < sizeof(reasons) / sizeof(reasons[0])
        ? reasons[result] : "unknown-error";
}

#endif
