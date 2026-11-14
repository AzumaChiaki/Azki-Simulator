/* O62M experimental PMIC ADC contract, inferred from original SBL 3.1.115.
 * This is NOT a silicon reset/timing specification. See adc-contract.md.
 * No transport, IRQ wiring or QEMU dependencies. Unknown accesses return false.
 */
#ifndef AZKI_O62M_PMIC_ADC_MODEL_H
#define AZKI_O62M_PMIC_ADC_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

typedef struct O62MPmicAdc {
    uint16_t opaque_seed;
    uint16_t config_b0, config_430, config_298, config_1c, config_434;
    uint16_t control_280, control_68, channel;
    uint16_t inputs[2], pending_sample, result;
    unsigned pending_channel, result_channel;
    bool pending, ready, result_valid;
    int64_t latency_ns, deadline_ns;
    uint64_t started, completed, cancelled, acknowledged, sample_reads[2];
} O62MPmicAdc;

static inline uint16_t o62m_pmic_adc_opaque(const O62MPmicAdc *s, uint16_t mask)
{
    return s->opaque_seed & (uint16_t)~mask;
}

/* The seed supplies ONLY opaque fields. Modeled controls start quiescent;
 * that is an explicit experimental initial condition, not a recovered reset. */
static inline bool o62m_pmic_adc_init(O62MPmicAdc *s, uint16_t opaque_seed,
                                    uint16_t ch3, uint16_t ch4,
                                    int64_t latency_ns)
{
    if (ch3 > 4095 || ch4 > 4095 || latency_ns <= 0) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->opaque_seed = opaque_seed;
    s->inputs[0] = ch3;
    s->inputs[1] = ch4;
    s->latency_ns = latency_ns;
    s->config_b0 = o62m_pmic_adc_opaque(s, 0x300);
    s->config_430 = o62m_pmic_adc_opaque(s, 0x3fc3);
    s->config_298 = o62m_pmic_adc_opaque(s, 0xff);
    s->config_1c = o62m_pmic_adc_opaque(s, 8);
    s->config_434 = o62m_pmic_adc_opaque(s, 0xff);
    s->control_280 = o62m_pmic_adc_opaque(s, 0xd);
    s->control_68 = o62m_pmic_adc_opaque(s, 0x40);
    return true;
}

static inline bool o62m_pmic_adc_set_input(O62MPmicAdc *s, unsigned channel,
                                         uint16_t raw)
{
    if (channel < 3 || channel > 4 || raw > 4095) {
        return false;
    }
    s->inputs[channel - 3] = raw;
    return true;
}

static inline void o62m_pmic_adc_update(O62MPmicAdc *s, int64_t now_ns)
{
    if (s->pending && now_ns >= s->deadline_ns) {
        s->pending = false;
        s->result = s->pending_sample;
        s->result_channel = s->pending_channel;
        s->result_valid = true;
        s->ready = true;
        s->completed++;
    }
}

static inline void o62m_pmic_adc_cancel(O62MPmicAdc *s)
{
    if (s->pending) {
        s->pending = false;
        s->cancelled++;
    }
}

static inline bool o62m_pmic_adc_read(O62MPmicAdc *s, uint16_t reg,
                                    uint16_t *value, int64_t now_ns)
{
    o62m_pmic_adc_update(s, now_ns);
    switch (reg) {
    case 0xb0: *value = s->config_b0; return true;
    case 0x430: *value = s->config_430; return true;
    case 0x298: *value = s->config_298; return true;
    case 0x1c: *value = s->config_1c; return true;
    case 0x434: *value = s->config_434; return true;
    case 0xb4:
        *value = o62m_pmic_adc_opaque(s, 0x300) | (s->ready ? 0x100 : 0);
        return true;
    case 0xb8: *value = o62m_pmic_adc_opaque(s, 0x100); return true;
    case 0x290:
        if (!s->result_valid) {
            return false;
        }
        *value = o62m_pmic_adc_opaque(s, 0xfff) | s->result;
        s->sample_reads[s->result_channel - 3]++;
        return true;
    default: return false;
    }
}

static inline bool o62m_pmic_adc_rmw(uint16_t *reg, uint16_t mask,
                                   uint16_t value)
{
    if ((value ^ *reg) & (uint16_t)~mask) {
        return false;
    }
    *reg = value;
    return true;
}

static inline bool o62m_pmic_adc_write(O62MPmicAdc *s, uint16_t reg,
                                     uint16_t value, int64_t now_ns)
{
    o62m_pmic_adc_update(s, now_ns);
    switch (reg) {
    case 0xb0: return o62m_pmic_adc_rmw(&s->config_b0, 0x300, value);
    case 0x430: return o62m_pmic_adc_rmw(&s->config_430, 0x3fc3, value);
    case 0x298: return o62m_pmic_adc_rmw(&s->config_298, 0xff, value);
    case 0x1c: return o62m_pmic_adc_rmw(&s->config_1c, 8, value);
    case 0x434: return o62m_pmic_adc_rmw(&s->config_434, 0xff, value);
    case 0x2a8:
        if (value != 3 && value != 4) {
            return false;
        }
        s->channel = value;
        return true;
    case 0x568: /* +0x500 set alias of 0x68, helper 60065994. */
        if (value & ~0x40) { return false; }
        s->control_68 |= value;
        return true;
    case 0xa68: /* +0xa00 clear alias of 0x68. */
        if (value & ~0x40) { return false; }
        s->control_68 &= ~value;
        if (!(s->control_68 & 0x40)) { o62m_pmic_adc_cancel(s); }
        return true;
    case 0x780: { /* +0x500 set alias of 0x280. */
        bool edge;
        if (value & ~0xd) { return false; }
        edge = (value & 4) && !(s->control_280 & 4);
        if (edge && (!(s->control_68 & 0x40) || s->pending || s->ready ||
                     s->channel < 3 || s->channel > 4 || now_ns < 0 ||
                     now_ns > INT64_MAX - s->latency_ns)) {
            return false;
        }
        s->control_280 |= value;
        if (edge) {
            s->pending = true;
            s->pending_channel = s->channel;
            s->pending_sample = s->inputs[s->channel - 3];
            s->deadline_ns = now_ns + s->latency_ns;
            s->started++;
        }
        return true;
    }
    case 0xc80: /* +0xa00 clear alias. Clear trigger does not ack status. */
        if (value & ~0xd) { return false; }
        s->control_280 &= ~value;
        if (value & 4) { o62m_pmic_adc_cancel(s); }
        return true;
    case 0xb8: /* 60065a3e: clear trigger, read b8, OR0x100, write b8. */
        if ((value & (uint16_t)~0x100) != o62m_pmic_adc_opaque(s, 0x100)) {
            return false;
        }
        if (value & 0x100) {
            s->ready = false;
            s->acknowledged++;
        }
        return true;
    default: return false;
    }
}
#endif
