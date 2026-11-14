#ifndef O62M_PMIC_BOOT_EVENTS_V6_H
#define O62M_PMIC_BOOT_EVENTS_V6_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* Partial virtual PMIC boot-event bank. Event sources start quiescent.
 * Historical boot metadata starts empty.
 * Version zero is a virtual label; it is not a physical revision measurement. */
typedef struct {
    uint16_t config1030, recorded[7], pending[5], last_capacity;
    uint8_t recorded_valid;
    uint16_t nfc_config0260;
    bool nfc_config_written;
    uint64_t writes;
} O62MPmicBootEventsV6;
static inline unsigned o62m_pmic_boot_event_kind_v6(uint16_t reg,unsigned *index)
{
    static const uint16_t recorded[]={0x12b0,0x12b4,0x12b8,0x12bc,0x12c0,0x12c4,0x12e4};
    static const uint16_t pending[]={0x1084,0x109c,0x10a0,0x10a4,0x10a8};
    *index=0;
    if(reg==0x1030)return 1;
    if(reg==0)return 4;
    if(reg==0x12c8){*index=4;return 5;}
    if(reg==0x12cc)return 6;
    if(reg==0x0260)return 8;
    if(reg==0x014c||reg==0x0150){*index=reg==0x014c;return 7;}
    if(reg>=0x1088&&reg<=0x1094&&(reg&3)==0){*index=(reg-0x1088)/4;return 5;}
    for(unsigned i=0;i<7;i++)if(reg==recorded[i]){*index=i;return 2;}
    for(unsigned i=0;i<5;i++)if(reg==pending[i]){*index=i;return 3;}
    return 0;
}
static inline void o62m_pmic_boot_events_init_v6(O62MPmicBootEventsV6 *m)
{ memset(m,0,sizeof(*m));m->recorded_valid=0x7f; }
static inline bool o62m_pmic_boot_events_validate_v6(const O62MPmicBootEventsV6 *m,uint16_t reg,uint32_t value)
{
    unsigned i,kind=o62m_pmic_boot_event_kind_v6(reg,&i);
    if(value>UINT16_MAX||m->writes==UINT64_MAX)return false;
    if(kind==1)return value==(m->config1030&~8u);
    if(kind==2)return value==0;
    if(kind==8)return value==0xb;
    return kind==3||kind==6;
}
static inline bool o62m_pmic_boot_events_write_v6(O62MPmicBootEventsV6 *m,uint16_t reg,uint32_t value)
{
    if(!o62m_pmic_boot_events_validate_v6(m,reg,value))return false;
    unsigned i,kind=o62m_pmic_boot_event_kind_v6(reg,&i);
    if(kind==1)m->config1030=value;
    else if(kind==2){m->recorded[i]=value;m->recorded_valid|=1u<<i;}
    else if(kind==6)m->last_capacity=value;
    else if(kind==8){m->nfc_config0260=value;m->nfc_config_written=true;}
    else m->pending[i]&=~(uint16_t)value;
    ++m->writes;return true;
}
static inline bool o62m_pmic_boot_events_read_v6(const O62MPmicBootEventsV6 *m,uint16_t reg,uint16_t *value)
{
    unsigned i,kind=o62m_pmic_boot_event_kind_v6(reg,&i);
    if(kind==1){*value=m->config1030;return true;}
    if(kind==4||kind==5){*value=0;return true;}
    if(kind==6){*value=m->last_capacity;return true;}
    if(kind==8&&m->nfc_config_written){*value=m->nfc_config0260;return true;}
    /* Explicit virtual signed24 current-calibration input, present at reset. */
    if(kind==7){const uint32_t raw24=0;*value=(raw24>>(i?16:0))&UINT16_MAX;return true;}
    if(kind==2&&(m->recorded_valid&(1u<<i))){*value=m->recorded[i];return true;}
    return false;
}
#endif
