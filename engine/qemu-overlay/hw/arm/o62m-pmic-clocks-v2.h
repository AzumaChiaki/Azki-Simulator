#ifndef O62M_PMIC_CLOCKS_V2_H
#define O62M_PMIC_CLOCKS_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "o62m-pmic-clock-defs-v2.h"

/* Six independent explicit16bit data words. This is configuration retention,
 * not a physical clock, oscillator, status, divider-rate or SPMI model.
 * Delta0 or one proven field per transaction; no union-mask mixed updates.
 */
typedef enum O62MPmicClockResultV2 {
    O62M_PMIC_CLOCK_OK_V2 = 0,
    O62M_PMIC_CLOCK_BAD_ARGUMENT_V2,
    O62M_PMIC_CLOCK_UNINITIALIZED_V2,
    O62M_PMIC_CLOCK_BAD_COUNT_V2,
    O62M_PMIC_CLOCK_BAD_REGISTER_V2,
    O62M_PMIC_CLOCK_BAD_VALUE_V2,
    O62M_PMIC_CLOCK_UNPROVED_FIELD_V2,
    O62M_PMIC_CLOCK_MIXED_FIELDS_V2,
    O62M_PMIC_CLOCK_COUNT_OVERFLOW_V2
} O62MPmicClockResultV2;

typedef struct O62MPmicClockV2 {
    uint16_t word[O62M_PMIC_CLOCK_WORDS_V2];
    bool initialized;
    uint64_t writes,word_writes[O62M_PMIC_CLOCK_WORDS_V2];
} O62MPmicClockV2;

static inline const char *o62m_pmic_clock_reason_v2(O62MPmicClockResultV2 r)
{
    switch(r) {
    case O62M_PMIC_CLOCK_OK_V2:return "ok";
    case O62M_PMIC_CLOCK_BAD_ARGUMENT_V2:return "bad-argument";
    case O62M_PMIC_CLOCK_UNINITIALIZED_V2:return "six-explicit-independent16bit-inputs-required";
    case O62M_PMIC_CLOCK_BAD_COUNT_V2:return "exact-six-word-count-required";
    case O62M_PMIC_CLOCK_BAD_REGISTER_V2:return "unknown-pmic-clock-register";
    case O62M_PMIC_CLOCK_BAD_VALUE_V2:return "full16bit-data-required";
    case O62M_PMIC_CLOCK_UNPROVED_FIELD_V2:return "unknown-bit-delta";
    case O62M_PMIC_CLOCK_MIXED_FIELDS_V2:return "changed-bits-must-fit-one-proven-field";
    case O62M_PMIC_CLOCK_COUNT_OVERFLOW_V2:return "committed-write-count-overflow";
    }
    return "invalid-result";
}

static inline int o62m_pmic_clock_index_v2(uint32_t reg)
{
    for(unsigned i=0;i<O62M_PMIC_CLOCK_WORDS_V2;++i) {
        if(reg==o62m_pmic_clock_defs_v2[i].reg) {return (int)i;}
    }
    return -1;
}

static inline bool o62m_pmic_clock_address_v2(uint32_t reg)
{
    return o62m_pmic_clock_index_v2(reg)>=0;
}

static inline O62MPmicClockResultV2 o62m_pmic_clock_init_v2(O62MPmicClockV2 *model,
        const uint32_t *initial,size_t count)
{
    O62MPmicClockV2 next={0};
    if(!model || !initial) {return O62M_PMIC_CLOCK_BAD_ARGUMENT_V2;}
    if(count!=O62M_PMIC_CLOCK_WORDS_V2) {return O62M_PMIC_CLOCK_BAD_COUNT_V2;}
    for(unsigned i=0;i<O62M_PMIC_CLOCK_WORDS_V2;++i) {
        if(initial[i]>UINT16_MAX) {return O62M_PMIC_CLOCK_BAD_VALUE_V2;}
        next.word[i]=(uint16_t)initial[i];
    }
    next.initialized=true;
    *model=next;
    return O62M_PMIC_CLOCK_OK_V2;
}

/* Output must be independent storage, not overlapping model fields. */
static inline O62MPmicClockResultV2 o62m_pmic_clock_read_v2(
        const O62MPmicClockV2 *model,uint32_t reg,uint16_t *output)
{
    if(!model || !output) {return O62M_PMIC_CLOCK_BAD_ARGUMENT_V2;}
    if(!model->initialized) {return O62M_PMIC_CLOCK_UNINITIALIZED_V2;}
    int index=o62m_pmic_clock_index_v2(reg);
    if(index<0) {return O62M_PMIC_CLOCK_BAD_REGISTER_V2;}
    *output=model->word[index];
    return O62M_PMIC_CLOCK_OK_V2;
}

/* Pure preflight only. The BSP must apply a successful write through the real
 * delayed SPMI commit path; validation never submits or completes a command.
 */
static inline O62MPmicClockResultV2 o62m_pmic_clock_validate_write_v2(
        const O62MPmicClockV2 *model,uint32_t reg,uint32_t value)
{
    if(!model) {return O62M_PMIC_CLOCK_BAD_ARGUMENT_V2;}
    if(!model->initialized) {return O62M_PMIC_CLOCK_UNINITIALIZED_V2;}
    int index=o62m_pmic_clock_index_v2(reg);
    if(index<0) {return O62M_PMIC_CLOCK_BAD_REGISTER_V2;}
    if(value>UINT16_MAX) {return O62M_PMIC_CLOCK_BAD_VALUE_V2;}
    const O62MPmicClockDefV2 *def=&o62m_pmic_clock_defs_v2[index];
    uint32_t delta=value^model->word[index];
    if(delta&~(uint32_t)def->union_mask) {return O62M_PMIC_CLOCK_UNPROVED_FIELD_V2;}
    if(delta) {
        bool one_field=false;
        for(unsigned i=0;i<def->field_count;++i) {
            if(!(delta&~(uint32_t)def->field_mask[i])) {one_field=true;break;}
        }
        if(!one_field) {return O62M_PMIC_CLOCK_MIXED_FIELDS_V2;}
    }
    if(model->writes==UINT64_MAX || model->word_writes[index]==UINT64_MAX) {
        return O62M_PMIC_CLOCK_COUNT_OVERFLOW_V2;
    }
    return O62M_PMIC_CLOCK_OK_V2;
}

/* Single backing per register, including all aliases. Register001c must also
 * be the unique source for the BSP's existing ADC config_1c mirror. This
 * helper does not emulate ADC clock changes or accept a second initial seed.
 * Every error is atomic for all words and counters.
 */
static inline O62MPmicClockResultV2 o62m_pmic_clock_write_v2(O62MPmicClockV2 *model,
        uint32_t reg,uint32_t value)
{
    O62MPmicClockResultV2 result=o62m_pmic_clock_validate_write_v2(model,reg,value);
    if(result!=O62M_PMIC_CLOCK_OK_V2) {return result;}
    unsigned index=(unsigned)o62m_pmic_clock_index_v2(reg);
    O62MPmicClockV2 next=*model;
    next.word[index]=(uint16_t)value;
    next.writes++;
    next.word_writes[index]++;
    *model=next;
    return O62M_PMIC_CLOCK_OK_V2;
}

#endif
