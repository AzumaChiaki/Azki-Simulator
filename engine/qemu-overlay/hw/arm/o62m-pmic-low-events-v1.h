#ifndef O62M_PMIC_LOW_EVENTS_V1_H
#define O62M_PMIC_LOW_EVENTS_V1_H
#include "o62m-auxadc-channel-adapter-v1.h"
/* Low six event sources are quiescent in this virtual profile. ADC owns
 * bit8, conversion data and the canonical B0 mask word. */
static inline bool o62m_pmic_low_events_write_v1(O62MPmicAdc *core,
    const O62MPmicAdcAPAdapterV1 *adapter,uint16_t *pending,
    uint16_t reg,uint16_t value,int64_t now)
{
    O62MPmicAdc next=*core;uint16_t events=*pending;
    if(reg==0xb8) {
        uint16_t opaque=o62m_pmic_adc_opaque(core,0x13f);
        if((value&(uint16_t)~0x13f)!=opaque)return false;
        uint16_t adc_value=(value&0x100)|o62m_pmic_adc_opaque(core,0x100);
        if(!o62m_pmic_adc_ap_write_v1(&next,adapter,reg,adc_value,now))return false;
        events&=~(value&0x3f);
    } else if(reg==0x5b0) {
        if(value&~0x3f)return false;
        if(!o62m_pmic_adc_ap_advance_v1(&next,now))return false;
        next.config_b0|=value;
    } else return false;
    *core=next;*pending=events;return true;
}
static inline uint16_t o62m_pmic_low_events_project_v1(uint16_t reg,uint16_t base,uint16_t pending)
{
    if(reg==0xb4)return (base&~0x3f)|(pending&0x3f);
    if(reg==0xb8)return base&~0x3f;
    return base;
}
#endif
