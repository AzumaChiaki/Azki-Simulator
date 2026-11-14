#ifndef O62M_PMIC_CLOCK_SET_V3_H
#define O62M_PMIC_CLOCK_SET_V3_H
#include "o62m-pmic-clocks-v2.h"

static inline O62MPmicClockResultV2 o62m_pmic_clock_set_validate_v3(
    const O62MPmicClockV2 *m,uint32_t reg,uint32_t value)
{
    if(!m)return O62M_PMIC_CLOCK_BAD_ARGUMENT_V2;
    if(reg!=0x0504&&reg!=0x0a04)return O62M_PMIC_CLOCK_BAD_REGISTER_V2;
    if(value!=0x0400&&(reg!=0x0a04||value!=0x0008))return O62M_PMIC_CLOCK_UNPROVED_FIELD_V2;
    return o62m_pmic_clock_validate_write_v2(m,0x0004,reg==0x0504?m->word[0]|value:m->word[0]&~value);
}

static inline O62MPmicClockResultV2 o62m_pmic_clock_set_write_v3(
    O62MPmicClockV2 *m,uint32_t reg,uint32_t value)
{
    O62MPmicClockResultV2 r=o62m_pmic_clock_set_validate_v3(m,reg,value);
    if(r)return r;
    return o62m_pmic_clock_write_v2(m,0x0004,reg==0x0504?m->word[0]|value:m->word[0]&~value);
}
#endif
