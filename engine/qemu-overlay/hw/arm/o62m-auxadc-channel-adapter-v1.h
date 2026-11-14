/* Opt-in original-AP AUXADC channel adapter. Experimental inputs/profile only;
 * not a silicon reset, timing, channel-availability or IRQ specification.
 * One existing O62MPmicAdc owns all configuration, work and completed data.
 * This header neither drives an IRQ nor touches original guest bookkeeping.
 */
#ifndef O62M_AUXADC_CHANNEL_ADAPTER_V1_H
#define O62M_AUXADC_CHANNEL_ADAPTER_V1_H

#include "pmic-adc-model.h"
#include <stddef.h>

#define O62M_AUXADC_CHANNEL_MASK_V1 UINT16_C(0x80ff)
#define O62M_AUXADC_EXTRA_INPUT_MASK_V1 UINT16_C(0x80e7)
#define O62M_AUXADC_EXTRA_INPUT_COUNT_V1 7u

typedef struct O62MPmicAdcAPAdapterV1 {
    uint16_t present_mask;
    uint16_t extra_inputs[O62M_AUXADC_EXTRA_INPUT_COUNT_V1];
} O62MPmicAdcAPAdapterV1;

/* Exactly seven optional input slots; channels 3/4 stay in core.inputs[2]. */
static inline int o62m_pmic_adc_ap_extra_index_v1(unsigned channel)
{
    switch (channel) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 5: return 3;
    case 6: return 4;
    case 7: return 5;
    case 15: return 6;
    default: return -1;
    }
}

static inline bool o62m_pmic_adc_ap_channel_valid_v1(unsigned channel)
{
    return channel <= 7 || channel == 15;
}

static inline bool o62m_pmic_adc_ap_overlap_v1(
    const void *first, size_t first_size, const void *second, size_t second_size)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;
    return a <= b ? b - a < first_size : a - b < second_size;
}

static inline bool o62m_pmic_adc_ap_adapter_valid_v1(
    const O62MPmicAdcAPAdapterV1 *adapter)
{
    static const unsigned channels[O62M_AUXADC_EXTRA_INPUT_COUNT_V1] = {
        0, 1, 2, 5, 6, 7, 15
    };
    if (!adapter ||
        (adapter->present_mask & (uint16_t)~O62M_AUXADC_EXTRA_INPUT_MASK_V1)) {
        return false;
    }
    for (unsigned i = 0; i < O62M_AUXADC_EXTRA_INPUT_COUNT_V1; ++i) {
        uint16_t bit = (uint16_t)(UINT32_C(1) << channels[i]);
        if ((adapter->present_mask & bit) && adapter->extra_inputs[i] > 0x0fff) {
            return false;
        }
    }
    return true;
}

static inline bool o62m_pmic_adc_ap_init_v1(O62MPmicAdcAPAdapterV1 *adapter)
{
    if (!adapter) {
        return false;
    }
    memset(adapter, 0, sizeof(*adapter));
    return true;
}

static inline bool o62m_pmic_adc_ap_set_input_v1(
    O62MPmicAdc *core, O62MPmicAdcAPAdapterV1 *adapter,
    unsigned channel, uint16_t raw12)
{
    int index;
    if (!core || !o62m_pmic_adc_ap_adapter_valid_v1(adapter) || raw12 > 0x0fff) {
        return false;
    }
    if (channel == 3 || channel == 4) {
        O62MPmicAdc next = *core;
        if (!o62m_pmic_adc_set_input(&next, channel, raw12)) {
            return false;
        }
        *core = next;
        return true;
    }
    index = o62m_pmic_adc_ap_extra_index_v1(channel);
    if (index < 0) {
        return false;
    }
    adapter->extra_inputs[index] = raw12;
    adapter->present_mask |= (uint16_t)(UINT32_C(1) << channel);
    return true;
}

/* The BSP schedules core.deadline_ns. Only the old update performs completion. */
static inline bool o62m_pmic_adc_ap_advance_v1(O62MPmicAdc *core, int64_t now_ns)
{
    if (!core || now_ns < 0) {
        return false;
    }
    if (core->pending && now_ns >= core->deadline_ns &&
        core->completed == UINT64_MAX) {
        return false;
    }
    o62m_pmic_adc_update(core, now_ns);
    return true;
}

/* Complete replacement for the old ADC read call. A false return is final:
 * never retry a claimed result read with o62m_pmic_adc_read, whose sample
 * counter indexes only legacy channels 3/4. Failed calls leave core/out intact.
 */
static inline bool o62m_pmic_adc_ap_read_v1(
    O62MPmicAdc *core, const O62MPmicAdcAPAdapterV1 *adapter,
    uint16_t reg, uint16_t *value, int64_t now_ns)
{
    O62MPmicAdc next;
    uint16_t result;
    bool matches;
    if (!core || !value || now_ns < 0 ||
        !o62m_pmic_adc_ap_adapter_valid_v1(adapter) ||
        o62m_pmic_adc_ap_overlap_v1(value, sizeof(*value), core, sizeof(*core)) ||
        o62m_pmic_adc_ap_overlap_v1(value, sizeof(*value), adapter,
                                  sizeof(*adapter))) {
        return false;
    }
    next = *core;
    if (!o62m_pmic_adc_ap_advance_v1(&next, now_ns)) {
        return false;
    }
    switch (reg) {
    case 0x2a8:
        if (!o62m_pmic_adc_ap_channel_valid_v1(next.channel)) {
            return false;
        }
        result = next.channel;
        break;
    case 0x290:
    case 0x2a0:
    case 0x2a4:
    case 0x28c:
        matches = (reg == 0x290 && next.result_channel <= 5) ||
                  (reg == 0x2a0 && next.result_channel == 6) ||
                  (reg == 0x2a4 && next.result_channel == 7) ||
                  (reg == 0x28c && next.result_channel == 15);
        if (!next.result_valid || next.result > 0x0fff || !matches) {
            return false;
        }
        result = o62m_pmic_adc_opaque(&next, 0x0fff) | next.result;
        if (next.result_channel == 3 || next.result_channel == 4) {
            if (next.sample_reads[next.result_channel - 3] == UINT64_MAX) {
                return false;
            }
            next.sample_reads[next.result_channel - 3]++;
        }
        break;
    default:
        if (!o62m_pmic_adc_read(&next, reg, &result, now_ns)) {
            return false;
        }
        break;
    }
    *core = next;
    *value = result;
    return true;
}

/* All checks and legacy updates run on a local copy. Even a failure observed
 * after a deadline cannot partially complete a job or change the live state.
 */
static inline bool o62m_pmic_adc_ap_write_v1(
    O62MPmicAdc *core, const O62MPmicAdcAPAdapterV1 *adapter,
    uint16_t reg, uint16_t value, int64_t now_ns)
{
    O62MPmicAdc next;
    if (!core || now_ns < 0 || !o62m_pmic_adc_ap_adapter_valid_v1(adapter)) {
        return false;
    }
    next = *core;
    if (!o62m_pmic_adc_ap_advance_v1(&next, now_ns)) {
        return false;
    }
    if (reg == 0x780 && (value & 4) && !(next.control_280 & 4) &&
        next.started == UINT64_MAX) {
        return false;
    }
    if (reg == 0x2a8) {
        if (!o62m_pmic_adc_ap_channel_valid_v1(value) ||
            ((next.pending || next.ready) && value != next.channel)) {
            return false;
        }
        if (value == 3 || value == 4) {
            if (!o62m_pmic_adc_write(&next, reg, value, now_ns)) {
                return false;
            }
        } else {
            next.channel = value;
        }
    } else if (reg == 0x780 && (value & 4) && !(next.control_280 & 4) &&
               next.channel != 3 && next.channel != 4) {
        int index = o62m_pmic_adc_ap_extra_index_v1(next.channel);
        if ((value & (uint16_t)~UINT16_C(0x000d)) || index < 0 ||
            !(adapter->present_mask &
              (uint16_t)(UINT32_C(1) << next.channel)) ||
            !(next.control_68 & 0x40) || next.pending || next.ready ||
            next.latency_ns <= 0 || now_ns > INT64_MAX - next.latency_ns) {
            return false;
        }
        next.control_280 |= value;
        next.pending = true;
        next.pending_channel = next.channel;
        next.pending_sample = adapter->extra_inputs[index];
        next.deadline_ns = now_ns + next.latency_ns;
        next.started++;
    } else if (!o62m_pmic_adc_write(&next, reg, value, now_ns)) {
        return false;
    }
    *core = next;
    return true;
}

/* Explicit success-only functional profile. BC is supplied by its existing
 * canonical owner. The BSP alone aggregates CC bit1 and routes external82.
 * Selector writes, starts, reads or enabling masks cannot invent readiness.
 */
static inline bool o62m_pmic_adc_ap_irq_ready_v1(
    const O62MPmicAdc *core, uint16_t canonical_bc)
{
    return core && core->ready && core->result_valid &&
           o62m_pmic_adc_ap_channel_valid_v1(core->result_channel) &&
           core->result <= 0x0fff &&
           (core->config_b0 & 0x100) && (canonical_bc & 2);
}

#endif
