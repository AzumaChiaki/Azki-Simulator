#ifndef O62M_PMIC1000_V1_H
#define O62M_PMIC1000_V1_H

#include <stdbool.h>
#include <stdint.h>

/* Single explicit16bit data word, not an SPMI controller or clock-ready
 * model. The only permitted changed field is original PMIC mux width1,
 * shift1. All gate/secondary/unknown bits are opaque and must be preserved.
 * Bus selection, busy/valid and delayed commit belong to the existing BSP.
 */
typedef enum O62MPmic1000ResultV1 {
    O62M_PMIC1000_OK_V1 = 0,
    O62M_PMIC1000_BAD_ARGUMENT_V1,
    O62M_PMIC1000_UNINITIALIZED_V1,
    O62M_PMIC1000_BAD_VALUE_V1,
    O62M_PMIC1000_UNPROVED_FIELD_V1,
    O62M_PMIC1000_COUNT_OVERFLOW_V1
} O62MPmic1000ResultV1;

typedef struct O62MPmic1000V1 {
    uint16_t word;
    bool initialized;
    uint64_t writes;
} O62MPmic1000V1;

static inline const char *o62m_pmic1000_reason_v1(O62MPmic1000ResultV1 r)
{
    switch(r) {
    case O62M_PMIC1000_OK_V1:return "ok";
    case O62M_PMIC1000_BAD_ARGUMENT_V1:return "bad-argument";
    case O62M_PMIC1000_UNINITIALIZED_V1:return "explicit-shared16bit-input-required";
    case O62M_PMIC1000_BAD_VALUE_V1:return "full16bit-value-required";
    case O62M_PMIC1000_UNPROVED_FIELD_V1:return "only-proven-mux-bit1-may-change";
    case O62M_PMIC1000_COUNT_OVERFLOW_V1:return "committed-write-count-overflow";
    }
    return "invalid-result";
}

static inline O62MPmic1000ResultV1 o62m_pmic1000_init_v1(O62MPmic1000V1 *model,
                                                       uint32_t initial)
{
    if(!model) {return O62M_PMIC1000_BAD_ARGUMENT_V1;}
    if(initial>UINT16_MAX) {return O62M_PMIC1000_BAD_VALUE_V1;}
    O62MPmic1000V1 next={0};
    next.word=(uint16_t)initial;
    next.initialized=true;
    *model=next;
    return O62M_PMIC1000_OK_V1;
}

/* Output requires independent storage, not overlapping model fields. */
static inline O62MPmic1000ResultV1 o62m_pmic1000_read_v1(
        const O62MPmic1000V1 *model,uint16_t *output)
{
    if(!model || !output) {return O62M_PMIC1000_BAD_ARGUMENT_V1;}
    if(!model->initialized) {return O62M_PMIC1000_UNINITIALIZED_V1;}
    *output=model->word;
    return O62M_PMIC1000_OK_V1;
}

/* Pure submission preflight; does not commit before the real bus deadline. */
static inline O62MPmic1000ResultV1 o62m_pmic1000_validate_write_v1(
        const O62MPmic1000V1 *model,uint32_t value)
{
    if(!model) {return O62M_PMIC1000_BAD_ARGUMENT_V1;}
    if(!model->initialized) {return O62M_PMIC1000_UNINITIALIZED_V1;}
    if(value>UINT16_MAX) {return O62M_PMIC1000_BAD_VALUE_V1;}
    if((value^model->word)&~UINT32_C(2)) {return O62M_PMIC1000_UNPROVED_FIELD_V1;}
    if(model->writes==UINT64_MAX) {return O62M_PMIC1000_COUNT_OVERFLOW_V1;}
    return O62M_PMIC1000_OK_V1;
}

/* Called only by the BSP's actual SPMI pending-operation commit path.
 * Every failure leaves all state unchanged. Successful unchanged writes
 * still count a real committed transaction, without inventing clock effects.
 */
static inline O62MPmic1000ResultV1 o62m_pmic1000_write_v1(O62MPmic1000V1 *model,
                                                        uint32_t value)
{
    O62MPmic1000ResultV1 result=o62m_pmic1000_validate_write_v1(model,value);
    if(result!=O62M_PMIC1000_OK_V1) {return result;}
    O62MPmic1000V1 next=*model;
    next.word=(uint16_t)value;
    next.writes++;
    *model=next;
    return O62M_PMIC1000_OK_V1;
}

#endif
