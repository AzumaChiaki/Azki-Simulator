#ifndef O62M_PM32K_COUNTER_V1_H
#define O62M_PM32K_COUNTER_V1_H

#include <stdbool.h>
#include <stdint.h>

/* Independent, opt-in always-running virtual32768Hz counter.
 * Original AP4c0302c4 ticks are converted to seconds by a rightshift15.
 * Physical oscillator/gate/reset/phase/wrap are not established. There is no
 * connection to the different4a000090 counter or an unprovedTEE5c03alias.
 * Initialticks and fractional phase are explicit, never recovered defaults.
 * No write, latch, ready, alarm or IRQ behavior is supplied.
 */
#define O62M_PM32K_ADDRESS UINT64_C(0x4c0302c4)
#define O62M_PM32K_HZ UINT64_C(32768)
#define O62M_PM32K_DENOMINATOR UINT64_C(1000000000)

typedef enum O62MPM32KResult {
    O62M_PM32K_OK = 0,
    O62M_PM32K_BAD_ARGUMENT,
    O62M_PM32K_BAD_INITIAL,
    O62M_PM32K_UNINITIALIZED,
    O62M_PM32K_TIME_REVERSED,
    O62M_PM32K_COUNTER_OVERFLOW
} O62MPM32KResult;

typedef struct O62MPM32KCounter {
    uint32_t ticks;
    /* Fraction ofone tick, numerator over1e9; this field is NOT nanoseconds. */
    uint32_t fraction_numerator;
    int64_t last_virtual_ns;
    bool initialized;
} O62MPM32KCounter;

static inline const char *o62m_pm32k_reason(O62MPM32KResult result)
{
    switch (result) {
    case O62M_PM32K_OK: return "ok";
    case O62M_PM32K_BAD_ARGUMENT: return "bad-argument";
    case O62M_PM32K_BAD_INITIAL: return "bad-explicit-initial-state";
    case O62M_PM32K_UNINITIALIZED: return "uninitialized-or-invalid-state";
    case O62M_PM32K_TIME_REVERSED: return "virtual-time-reversed";
    case O62M_PM32K_COUNTER_OVERFLOW: return "u32-counter-wrap-semantics-unknown";
    }
    return "invalid-result";
}

static inline O62MPM32KResult o62m_pm32k_init(O62MPM32KCounter *model,
                                             uint64_t initial_ticks,
                                             uint64_t fraction_numerator,
                                             int64_t reference_virtual_ns)
{
    O62MPM32KCounter next = {0};
    if (!model) {
        return O62M_PM32K_BAD_ARGUMENT;
    }
    if (initial_ticks > UINT32_MAX || fraction_numerator >= O62M_PM32K_DENOMINATOR ||
        reference_virtual_ns < 0) {
        return O62M_PM32K_BAD_INITIAL;
    }
    next.ticks = (uint32_t)initial_ticks;
    next.fraction_numerator = (uint32_t)fraction_numerator;
    next.last_virtual_ns = reference_virtual_ns;
    next.initialized = true;
    *model = next;
    return O62M_PM32K_OK;
}

static inline bool o62m_pm32k_valid(const O62MPM32KCounter *model)
{
    return model && model->initialized && model->last_virtual_ns >= 0 &&
           model->fraction_numerator < O62M_PM32K_DENOMINATOR;
}

/* Split elapsedseconds/nanoseconds before multiplying: elapsed_ns*32768 can
 * exceedu64 at valid INT64 timestamps. Each splitterm and their sum fitu64.
 * All failures leave every state byte unchanged. No wrap choice is guessed.
 */
static inline O62MPM32KResult o62m_pm32k_advance(O62MPM32KCounter *model,
                                                int64_t virtual_ns)
{
    O62MPM32KCounter next;
    uint64_t elapsed, fraction, increment;

    if (!model) {
        return O62M_PM32K_BAD_ARGUMENT;
    }
    if (!o62m_pm32k_valid(model)) {
        return O62M_PM32K_UNINITIALIZED;
    }
    if (virtual_ns < model->last_virtual_ns) {
        return O62M_PM32K_TIME_REVERSED;
    }
    elapsed = (uint64_t)(virtual_ns - model->last_virtual_ns);
    fraction = model->fraction_numerator +
               (elapsed % O62M_PM32K_DENOMINATOR) * O62M_PM32K_HZ;
    increment = (elapsed / O62M_PM32K_DENOMINATOR) * O62M_PM32K_HZ +
                fraction / O62M_PM32K_DENOMINATOR;
    if (increment > (uint64_t)UINT32_MAX - model->ticks) {
        return O62M_PM32K_COUNTER_OVERFLOW;
    }
    next = *model;
    next.ticks += (uint32_t)increment;
    next.fraction_numerator = (uint32_t)(fraction % O62M_PM32K_DENOMINATOR);
    next.last_virtual_ns = virtual_ns;
    *model = next;
    return O62M_PM32K_OK;
}

/* The caller must enforce the single proved aligned32 read MMIO address.
 * This pure helper owns only this counter's independent state and clock.
 * It advances through intervals with no bus/CPU activity or reads; there is
 * no read-count dependence. Failed reads preserve state and caller output.
 */
static inline O62MPM32KResult o62m_pm32k_read(O62MPM32KCounter *model,
                                            uint32_t *value, int64_t virtual_ns)
{
    O62MPM32KCounter next;
    O62MPM32KResult result;
    if (!model || !value) {
        return O62M_PM32K_BAD_ARGUMENT;
    }
    next = *model;
    result = o62m_pm32k_advance(&next, virtual_ns);
    if (result != O62M_PM32K_OK) {
        return result;
    }
    *model = next;
    *value = next.ticks;
    return O62M_PM32K_OK;
}

#endif
