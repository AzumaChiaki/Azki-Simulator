#ifndef O62M_PMIC_REGULATORS_V1_H
#define O62M_PMIC_REGULATORS_V1_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include "o62m-pmic-regulator-defs-v2.h"

/* Explicit virtual configuration state only. No voltage feedback/ready model.
 * Transport timing, selected slave, ADC68 mirror and cancellation belong to BSP.
 * Alias reads are deliberately unsupported, including the original RMW prefix.
 */
typedef enum {
    O62M_PMIC_REGULATOR_V1_OK = 0,
    O62M_PMIC_REGULATOR_V1_BAD_ARGUMENT,
    O62M_PMIC_REGULATOR_V1_NOT_INITIALIZED,
    O62M_PMIC_REGULATOR_V1_UNKNOWN_REGISTER,
    O62M_PMIC_REGULATOR_V1_VALUE_RANGE,
    O62M_PMIC_REGULATOR_V1_READ_ONLY,
    O62M_PMIC_REGULATOR_V1_FIELD_DELTA,
    O62M_PMIC_REGULATOR_V1_ALIAS_COMMAND,
    O62M_PMIC_REGULATOR_V1_COUNTER_OVERFLOW
} O62MPmicRegulatorResultV1;

typedef struct {
    uint16_t word[O62M_PMIC_REGULATOR_WORD_COUNT_V1];
    bool initialized;
    uint64_t writes, normal_writes, alias_writes;
    uint64_t word_writes[O62M_PMIC_REGULATOR_WORD_COUNT_V1];
} O62MPmicRegulatorsV1;

static inline int o62m_pmic_regulator_index_v1(uint32_t reg)
{
    for (unsigned i = 0; i < O62M_PMIC_REGULATOR_WORD_COUNT_V1; ++i) {
        if (reg == o62m_pmic_regulator_defs_v1[i].reg) { return (int)i; }
    }
    return -1;
}

static inline int o62m_pmic_regulator_alias_index_v1(uint32_t reg)
{
    for (unsigned i = 0; i < O62M_PMIC_REGULATOR_ALIAS_COUNT_V1; ++i) {
        if (reg == o62m_pmic_regulator_aliases_v1[i].reg) { return (int)i; }
    }
    return -1;
}

static inline bool o62m_pmic_regulator_supports_v1(uint32_t reg)
{
    return o62m_pmic_regulator_index_v1(reg) >= 0 ||
           o62m_pmic_regulator_alias_index_v1(reg) >= 0;
}

static inline uint32_t o62m_pmic_regulator_address_v1(size_t index)
{
    return index < O62M_PMIC_REGULATOR_WORD_COUNT_V1 ?
           o62m_pmic_regulator_defs_v1[index].reg : UINT32_MAX;
}

static inline O62MPmicRegulatorResultV1 o62m_pmic_regulator_init_v1(
    O62MPmicRegulatorsV1 *m, const uint32_t *inputs, size_t count)
{
    if (!m || !inputs || count != O62M_PMIC_REGULATOR_WORD_COUNT_V1) {
        return O62M_PMIC_REGULATOR_V1_BAD_ARGUMENT;
    }
    O62MPmicRegulatorsV1 trial;
    memset(&trial, 0, sizeof(trial));
    for (unsigned i = 0; i < O62M_PMIC_REGULATOR_WORD_COUNT_V1; ++i) {
        if (inputs[i] > UINT16_MAX) { return O62M_PMIC_REGULATOR_V1_VALUE_RANGE; }
        trial.word[i] = (uint16_t)inputs[i];
    }
    trial.initialized = true;
    *m = trial;
    return O62M_PMIC_REGULATOR_V1_OK;
}

static inline bool o62m_pmic_regulator_output_overlaps_v1(
    const O62MPmicRegulatorsV1 *m, const uint16_t *out)
{
    uintptr_t a = (uintptr_t)m, b = (uintptr_t)out;
    return b >= a ? b-a < sizeof(*m) : a-b < sizeof(*out);
}

static inline O62MPmicRegulatorResultV1 o62m_pmic_regulator_read_v1(
    const O62MPmicRegulatorsV1 *m, uint32_t reg, uint16_t *out)
{
    if (!m || !out || o62m_pmic_regulator_output_overlaps_v1(m, out)) {
        return O62M_PMIC_REGULATOR_V1_BAD_ARGUMENT;
    }
    if (!m->initialized) { return O62M_PMIC_REGULATOR_V1_NOT_INITIALIZED; }
    int index = o62m_pmic_regulator_index_v1(reg);
    if (index < 0) { return O62M_PMIC_REGULATOR_V1_UNKNOWN_REGISTER; }
    *out = m->word[index];
    return O62M_PMIC_REGULATOR_V1_OK;
}

static inline O62MPmicRegulatorResultV1 o62m_pmic_regulator_validate_write_v1(
    const O62MPmicRegulatorsV1 *m, uint32_t reg, uint32_t value)
{
    if (!m) { return O62M_PMIC_REGULATOR_V1_BAD_ARGUMENT; }
    if (!m->initialized) { return O62M_PMIC_REGULATOR_V1_NOT_INITIALIZED; }
    if (value > UINT16_MAX) { return O62M_PMIC_REGULATOR_V1_VALUE_RANGE; }
    int index = o62m_pmic_regulator_index_v1(reg);
    int alias = -1;
    if (index >= 0) {
        uint16_t mask = o62m_pmic_regulator_defs_v1[index].selector_mask;
        if (!mask) { return O62M_PMIC_REGULATOR_V1_READ_ONLY; }
        if (reg == 0x024c) {
            if (value & (uint32_t)(uint16_t)~mask) { return O62M_PMIC_REGULATOR_V1_FIELD_DELTA; }
            value = (m->word[index] & (uint16_t)~mask) | value;
        }
        if (((uint32_t)m->word[index] ^ value) & (uint32_t)(uint16_t)~mask) {
            return O62M_PMIC_REGULATOR_V1_FIELD_DELTA;
        }
    } else {
        alias = o62m_pmic_regulator_alias_index_v1(reg);
        if (alias < 0) { return O62M_PMIC_REGULATOR_V1_UNKNOWN_REGISTER; }
        index = o62m_pmic_regulator_aliases_v1[alias].target_index;
        uint16_t mask = o62m_pmic_regulator_defs_v1[index].enable_mask;
        if (!value || (index != 1 && (value & (value-1))) ||
            (value & (uint32_t)(uint16_t)~mask)) {
            return O62M_PMIC_REGULATOR_V1_ALIAS_COMMAND;
        }
    }
    if (m->writes == UINT64_MAX || m->word_writes[index] == UINT64_MAX ||
        (alias >= 0 ? m->alias_writes : m->normal_writes) == UINT64_MAX) {
        return O62M_PMIC_REGULATOR_V1_COUNTER_OVERFLOW;
    }
    return O62M_PMIC_REGULATOR_V1_OK;
}

static inline O62MPmicRegulatorResultV1 o62m_pmic_regulator_write_v1(
    O62MPmicRegulatorsV1 *m, uint32_t reg, uint32_t value)
{
    O62MPmicRegulatorResultV1 result =
        o62m_pmic_regulator_validate_write_v1(m, reg, value);
    if (result != O62M_PMIC_REGULATOR_V1_OK) { return result; }
    int index = o62m_pmic_regulator_index_v1(reg);
    if (index >= 0) {
        m->word[index] = reg == 0x024c ?
            (m->word[index] & (uint16_t)~UINT16_C(0x003f)) | (uint16_t)value : (uint16_t)value;
        ++m->normal_writes;
    } else {
        int alias = o62m_pmic_regulator_alias_index_v1(reg);
        index = o62m_pmic_regulator_aliases_v1[alias].target_index;
        if (o62m_pmic_regulator_aliases_v1[alias].set) {
            m->word[index] |= (uint16_t)value;
        } else {
            m->word[index] &= (uint16_t)~value;
        }
        ++m->alias_writes;
    }
    ++m->writes;
    ++m->word_writes[index];
    return O62M_PMIC_REGULATOR_V1_OK;
}

static inline const char *o62m_pmic_regulator_reason_v1(O62MPmicRegulatorResultV1 r)
{
    switch (r) {
    case O62M_PMIC_REGULATOR_V1_OK: return "ok";
    case O62M_PMIC_REGULATOR_V1_BAD_ARGUMENT: return "bad-argument-or-output-overlap";
    case O62M_PMIC_REGULATOR_V1_NOT_INITIALIZED: return "not-initialized";
    case O62M_PMIC_REGULATOR_V1_UNKNOWN_REGISTER: return "unknown-register-or-alias-read";
    case O62M_PMIC_REGULATOR_V1_VALUE_RANGE: return "value-exceeds-u16";
    case O62M_PMIC_REGULATOR_V1_READ_ONLY: return "read-only-configuration-word";
    case O62M_PMIC_REGULATOR_V1_FIELD_DELTA: return "changed-bits-outside-selector-field";
    case O62M_PMIC_REGULATOR_V1_ALIAS_COMMAND: return "alias-requires-supported-nonzero-enable-mask";
    case O62M_PMIC_REGULATOR_V1_COUNTER_OVERFLOW: return "counter-overflow";
    default: return "unknown-result";
    }
}
#endif
