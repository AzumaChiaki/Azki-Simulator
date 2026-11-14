#ifndef O62M_VIRTUAL_AUXADC_RESET_V1_H
#define O62M_VIRTUAL_AUXADC_RESET_V1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "pmic-adc-model.h"

/* Explicit measurement-reset VIRTUAL policy supported by xr_auxadc_reset's
 * original request/wait/request sequence. Not a silicon reset/TRM or full register
 * reset model. Caller owns shared SPMI serialization, real commit deadlines,
 * and rejecting ordinary ADC work while asserted. Other PMIC work is separate.
 */
typedef struct {
    bool initialized, asserted;
    uint16_t initial294;
    uint64_t reads, asserts, releases;
} O62MVirtualAuxadcResetV1;

static inline bool o62m_virtual_auxadc_reset_overlap_v1(
    const void *a_ptr, size_t a_size, const void *b_ptr, size_t b_size)
{
    uintptr_t a = (uintptr_t)a_ptr, b = (uintptr_t)b_ptr;
    return b >= a ? b-a < a_size : a-b < b_size;
}

/* Construction only; supply a zero-initialized object. No ADC state is reset. */
static inline bool o62m_virtual_auxadc_reset_init_v1(
    O62MVirtualAuxadcResetV1 *m, uint32_t initial294)
{
    if (!m || m->initialized || initial294 > UINT16_MAX) { return false; }
    O62MVirtualAuxadcResetV1 trial;
    memset(&trial, 0, sizeof(trial));
    trial.initialized = true;
    trial.initial294 = (uint16_t)initial294;
    *m = trial;
    return true;
}

/* Independent explicit RO data. No sample/result/ready mirror or side effect. */
static inline bool o62m_virtual_auxadc_reset_read_v1(
    O62MVirtualAuxadcResetV1 *m, uint16_t *out)
{
    if (!m || !out || !m->initialized || m->reads == UINT64_MAX ||
        o62m_virtual_auxadc_reset_overlap_v1(m, sizeof(*m), out, sizeof(*out))) {
        return false;
    }
    *out = m->initial294;
    ++m->reads;
    return true;
}

static inline bool o62m_virtual_auxadc_reset_supports_write_v1(uint32_t reg)
{
    return reg == UINT32_C(0x0a14) || reg == UINT32_C(0x0514);
}

/* Invoke only at the actual shared SPMI bus commit (preflight on copies is OK).
 * Caller first advances ADC to the bus commit time (or copied preflight time),
 * so an earlier conversion deadline completes before this reset request.
 * All failures leave BOTH objects untouched. Repeated assert/release requests
 * are accepted and counted; only an actually pending conversion is cancelled.
 */
static inline bool o62m_virtual_auxadc_reset_apply_v1(
    O62MVirtualAuxadcResetV1 *m, O62MPmicAdc *adc,
    uint32_t reg, uint32_t value)
{
    if (!m || !adc || !m->initialized ||
        o62m_virtual_auxadc_reset_overlap_v1(m, sizeof(*m), adc, sizeof(*adc)) ||
        !o62m_virtual_auxadc_reset_supports_write_v1(reg) || value != 2u) {
        return false;
    }
    if (reg == UINT32_C(0x0a14)) {
        if (m->asserts == UINT64_MAX || (adc->pending && adc->cancelled == UINT64_MAX)) {
            return false;
        }
        if (adc->pending) {
            adc->pending = false;
            ++adc->cancelled;
        }
        adc->ready = false;
        adc->result_valid = false;
        adc->result = 0;
        adc->pending_sample = 0;
        m->asserted = true;
        ++m->asserts;
    } else {
        if (m->releases == UINT64_MAX) { return false; }
        m->asserted = false;
        ++m->releases;
    }
    return true;
}
#endif
