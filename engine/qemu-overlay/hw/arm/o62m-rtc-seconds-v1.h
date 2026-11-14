#ifndef O62M_RTC_SECONDS_V1_H
#define O62M_RTC_SECONDS_V1_H

#include <stdbool.h>
#include <stdint.h>

/* Explicit experimental policy: an always-running, virtual-time seconds RTC.
 * Original AP reads register111c low16 then1120 high16 and converts seconds.
 * Each read samples its own supplied virtual_ns; no pair latch is invented.
 * Physical oscillator, reset value, calibration, latch and gate are unknown.
 * SPMI bus gating belongs outside this helper and never pauses this counter.
 * No setter commit, ready, busy, ACK, alarm, or interrupt behavior is supplied.
 */
#define O62M_RTC_NANOSECONDS_PER_SECOND UINT64_C(1000000000)

typedef enum O62MRTCResult {
    O62M_RTC_OK = 0,
    O62M_RTC_BAD_ARGUMENT,
    O62M_RTC_BAD_INITIAL,
    O62M_RTC_UNINITIALIZED,
    O62M_RTC_UNSUPPORTED_REGISTER,
    O62M_RTC_UNSUPPORTED_WRITE,
    O62M_RTC_TIME_REVERSED,
    O62M_RTC_COUNTER_OVERFLOW
} O62MRTCResult;

typedef struct O62MRTCSeconds {
    uint32_t seconds;
    uint32_t subsecond_ns;
    int64_t last_virtual_ns;
    bool initialized;
} O62MRTCSeconds;

static inline const char *o62m_rtc_reason(O62MRTCResult result)
{
    switch (result) {
    case O62M_RTC_OK: return "ok";
    case O62M_RTC_BAD_ARGUMENT: return "bad-argument";
    case O62M_RTC_BAD_INITIAL: return "bad-explicit-initial-state";
    case O62M_RTC_UNINITIALIZED: return "uninitialized-or-invalid-state";
    case O62M_RTC_UNSUPPORTED_REGISTER: return "rtc-register-unimplemented";
    case O62M_RTC_UNSUPPORTED_WRITE: return "rtc-write-or-commit-unimplemented";
    case O62M_RTC_TIME_REVERSED: return "virtual-time-reversed";
    case O62M_RTC_COUNTER_OVERFLOW: return "u32-counter-wrap-semantics-unknown";
    }
    return "invalid-result";
}

static inline O62MRTCResult o62m_rtc_init(O62MRTCSeconds *model,
                                         uint64_t seconds,
                                         uint64_t subsecond_ns,
                                         int64_t reference_virtual_ns)
{
    O62MRTCSeconds next = {0};

    if (!model) {
        return O62M_RTC_BAD_ARGUMENT;
    }
    if (seconds > UINT32_MAX ||
        subsecond_ns >= O62M_RTC_NANOSECONDS_PER_SECOND ||
        reference_virtual_ns < 0) {
        return O62M_RTC_BAD_INITIAL;
    }
    next.seconds = (uint32_t)seconds;
    next.subsecond_ns = (uint32_t)subsecond_ns;
    next.last_virtual_ns = reference_virtual_ns;
    next.initialized = true;
    *model = next;
    return O62M_RTC_OK;
}

static inline bool o62m_rtc_valid(const O62MRTCSeconds *model)
{
    return model && model->initialized && model->last_virtual_ns >= 0 &&
           model->subsecond_ns < O62M_RTC_NANOSECONDS_PER_SECOND;
}

/* No non-OK transaction commits any state or caller output. This lazy
 * advancement accounts for every elapsed ns, including intervals with no bus
 * access. Overflow stops before making any unknown hardware-wrap choice. */
static inline O62MRTCResult o62m_rtc_advance(O62MRTCSeconds *model,
                                            int64_t virtual_ns)
{
    O62MRTCSeconds next;
    uint64_t elapsed, subsecond, whole;

    if (!model) {
        return O62M_RTC_BAD_ARGUMENT;
    }
    if (!o62m_rtc_valid(model)) {
        return O62M_RTC_UNINITIALIZED;
    }
    if (virtual_ns < model->last_virtual_ns) {
        return O62M_RTC_TIME_REVERSED;
    }
    elapsed = (uint64_t)(virtual_ns - model->last_virtual_ns);
    subsecond = model->subsecond_ns +
                elapsed % O62M_RTC_NANOSECONDS_PER_SECOND;
    whole = elapsed / O62M_RTC_NANOSECONDS_PER_SECOND +
            subsecond / O62M_RTC_NANOSECONDS_PER_SECOND;
    if (whole > (uint64_t)UINT32_MAX - model->seconds) {
        return O62M_RTC_COUNTER_OVERFLOW;
    }
    next = *model;
    next.seconds += (uint32_t)whole;
    next.subsecond_ns = (uint32_t)(subsecond % O62M_RTC_NANOSECONDS_PER_SECOND);
    next.last_virtual_ns = virtual_ns;
    *model = next;
    return O62M_RTC_OK;
}

/* PMIC register values are 16-bit. SPMI selection, command encoding, timed
 * transactions, bus gating, and valid/busy bits are separate bus behavior. */
static inline O62MRTCResult o62m_rtc_read(O62MRTCSeconds *model,
                                         uint32_t register_number,
                                         uint16_t *value, int64_t virtual_ns)
{
    O62MRTCSeconds next;
    O62MRTCResult result;
    uint16_t sample;

    if (!model || !value) {
        return O62M_RTC_BAD_ARGUMENT;
    }
    if (!o62m_rtc_valid(model)) {
        return O62M_RTC_UNINITIALIZED;
    }
    if (register_number != 0x111cu && register_number != 0x1120u) {
        return O62M_RTC_UNSUPPORTED_REGISTER;
    }
    next = *model;
    result = o62m_rtc_advance(&next, virtual_ns);
    if (result != O62M_RTC_OK) {
        return result;
    }
    sample = register_number == 0x111cu ? (uint16_t)next.seconds :
                                        (uint16_t)(next.seconds >> 16);
    *model = next;
    *value = sample;
    return O62M_RTC_OK;
}

/* Original software writes110c,1108,1104=1; the physical commit and ACK
 * semantics are not recovered. All writes stay unsupported without shadow
 * readback, implicit counter replacement, or a fake completion result. */
static inline O62MRTCResult o62m_rtc_write(O62MRTCSeconds *model,
                                          uint32_t register_number,
                                          uint16_t value, int64_t virtual_ns)
{
    (void)register_number;
    (void)value;
    (void)virtual_ns;
    if (!model) {
        return O62M_RTC_BAD_ARGUMENT;
    }
    if (!o62m_rtc_valid(model)) {
        return O62M_RTC_UNINITIALIZED;
    }
    return O62M_RTC_UNSUPPORTED_WRITE;
}

#endif
