/* Bounded raw PMU ADC inputs. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1503_ADC_H
#define BEST1503_ADC_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BES_ADC_CHANNELS 8
#define BES_ADC_TEXT_MAX 47

/* All accesses must hold the caller's ADC lock. Values are uncalibrated raw
 * words. Reads count actual guest FIFO dequeues, not QMP acceptance. */
typedef struct {
    uint16_t values[BES_ADC_CHANNELS];
    uint64_t version;
    uint64_t reads[BES_ADC_CHANNELS];
    uint64_t last_read_version[BES_ADC_CHANNELS];
} BesAdcInput;

static inline void bes_adc_init(BesAdcInput *adc)
{
    memset(adc, 0, sizeof(*adc));
    for (unsigned i = 0; i < BES_ADC_CHANNELS; i++) {
        adc->values[i] = 21000; /* Preserve the original fixture at boot. */
    }
}

/* One fixed vector, decimal CSV, at most 47 bytes; no allocation or partial
 * update on invalid input. Used only by the dedicated QOM property. */
static inline bool bes_adc_parse(const char *text, uint16_t out[BES_ADC_CHANNELS])
{
    if (!text || strnlen(text, BES_ADC_TEXT_MAX + 1) > BES_ADC_TEXT_MAX) {
        return false;
    }
    uint16_t next[BES_ADC_CHANNELS];
    const char *p = text;
    for (unsigned i = 0; i < BES_ADC_CHANNELS; i++) {
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 5) {
                return false;
            }
            value = value * 10 + (*p++ - '0');
        }
        if (!digits || value > UINT16_MAX ||
            *p != (i + 1 == BES_ADC_CHANNELS ? '\0' : ',')) {
            return false;
        }
        next[i] = value;
        if (i + 1 < BES_ADC_CHANNELS) {
            p++;
        }
    }
    memcpy(out, next, sizeof(next));
    return true;
}

static inline bool bes_adc_replace(BesAdcInput *adc,
                                   const uint16_t values[BES_ADC_CHANNELS])
{
    if (adc->version == UINT64_MAX) {
        return false;
    }
    memcpy(adc->values, values, sizeof(adc->values));
    adc->version++;
    return true;
}

static inline uint16_t bes_adc_sample(const BesAdcInput *adc, unsigned channel,
                                     uint64_t *version)
{
    *version = adc->version;
    return adc->values[channel]; /* caller has checked channel < 8 */
}

static inline void bes_adc_observe(BesAdcInput *adc, unsigned channel,
                                   uint64_t sampled_version)
{
    if (adc->reads[channel] != UINT64_MAX) {
        adc->reads[channel]++;
    }
    adc->last_read_version[channel] = sampled_version;
}

#endif
