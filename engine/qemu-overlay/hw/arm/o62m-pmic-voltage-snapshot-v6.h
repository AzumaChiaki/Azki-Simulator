#ifndef O62M_PMIC_VOLTAGE_SNAPSHOT_V6_H
#define O62M_PMIC_VOLTAGE_SNAPSHOT_V6_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
typedef struct {
    uint32_t source_raw24,latched_raw24,source_current_raw24,latched_current_raw24;
    uint32_t current_bias;
    uint64_t charge_positive,charge_negative,latched_positive,latched_negative;
    uint64_t charge_fraction_ns;
    int64_t charge_last_ns;
    uint32_t voltage_history[8],current_history[8],latched_voltage_history[8],latched_current_history[8];
    unsigned history_count,latched_history_count;
    uint64_t sample_ticks,latched_sample_ticks;
    bool source_available,clock_available,enabled,pending,valid,ready;
    int64_t latency_ns,deadline_ns;
    uint64_t generation,requests,completions,cancellations;
} O62MPmicVoltageSnapshotV6;
static inline bool o62m_pmic_voltage_init_v6(O62MPmicVoltageSnapshotV6 *m,
    uint32_t raw,int64_t latency,bool source,bool clock,uint32_t current,uint32_t bias,int64_t now)
{
    if(raw>0xffffff||current>0x7fffff||bias>0x7fffff||latency<=0||now<0)return false;
    memset(m,0,sizeof(*m));m->source_raw24=raw;m->latency_ns=latency;
    m->current_bias=bias;m->charge_last_ns=now;m->source_current_raw24=current;m->source_available=source;m->clock_available=clock;return true;
}
static inline bool o62m_pmic_charge_advance_v6(O62MPmicVoltageSnapshotV6 *m,int64_t now)
{
    const uint64_t period=UINT64_C(15625000),limit=UINT64_C(0xffffffffffff);
    if(now<m->charge_last_ns||m->source_raw24>0xffffff||m->source_current_raw24>0x7fffff||m->current_bias>0x7fffff)return false;
    uint64_t fraction=m->charge_fraction_ns,positive=m->charge_positive,negative=m->charge_negative;
    if(m->enabled&&m->source_available&&m->clock_available) {
        uint64_t elapsed=(uint64_t)(now-m->charge_last_ns)+fraction;
        uint64_t ticks=elapsed/period;fraction=elapsed%period;
        int64_t net=(int64_t)m->source_current_raw24-(int64_t)m->current_bias;
        uint64_t magnitude=net<0?(uint64_t)-net:(uint64_t)net;
        uint64_t *total=net<0?&negative:&positive;
        if(*total>limit||(magnitude&&ticks>(limit-*total)/magnitude))return false;
        if(ticks>UINT64_MAX-m->sample_ticks)return false;
        *total+=ticks*magnitude;
        if(ticks>=8) {
            for(unsigned i=0;i<8;i++){m->voltage_history[i]=m->source_raw24;m->current_history[i]=m->source_current_raw24;}
            m->history_count=8;
        } else for(uint64_t tick=0;tick<ticks;tick++) {
            if(m->history_count==8) {
                memmove(m->voltage_history,m->voltage_history+1,7*sizeof(uint32_t));
                memmove(m->current_history,m->current_history+1,7*sizeof(uint32_t));
            } else m->history_count++;
            m->voltage_history[m->history_count-1]=m->source_raw24;
            m->current_history[m->history_count-1]=m->source_current_raw24;
        }
        m->sample_ticks+=ticks;
    }
    m->charge_last_ns=now;m->charge_fraction_ns=fraction;
    m->charge_positive=positive;m->charge_negative=negative;return true;
}
static inline bool o62m_pmic_voltage_gate_v6(O62MPmicVoltageSnapshotV6 *m,uint16_t f0,int64_t now)
{
    bool open=(f0&0x803)==0x803;
    if(!open&&(m->pending||m->ready)&&m->generation==UINT64_MAX)return false;
    if(!o62m_pmic_charge_advance_v6(m,now))return false;
    if(!open&&(m->pending||m->ready)) {
        m->generation++;if(m->pending)m->cancellations++;
        m->pending=false;m->ready=false;
    }
    m->enabled=open;return true;
}
static inline bool o62m_pmic_voltage_reset_v6(O62MPmicVoltageSnapshotV6 *m,int64_t now)
{
    if(now<0||m->generation==UINT64_MAX)return false;
    m->generation++;if(m->pending)m->cancellations++;m->pending=false;m->enabled=false;
    m->charge_last_ns=now;m->charge_fraction_ns=0;m->charge_positive=0;m->charge_negative=0;
    m->history_count=0;m->sample_ticks=0;
    memset(m->voltage_history,0,sizeof(m->voltage_history));memset(m->current_history,0,sizeof(m->current_history));
    m->valid=false;m->ready=false;m->latched_raw24=0;m->latched_current_raw24=0;m->latched_positive=0;m->latched_negative=0;m->latched_history_count=0;m->latched_sample_ticks=0;
    memset(m->latched_voltage_history,0,sizeof(m->latched_voltage_history));
    memset(m->latched_current_history,0,sizeof(m->latched_current_history));return true;
}
static inline bool o62m_pmic_voltage_ack_v6(O62MPmicVoltageSnapshotV6 *m,int64_t now)
{
    bool sample=m->enabled&&m->source_available&&m->clock_available;
    if(now<0||m->generation==UINT64_MAX||m->requests==UINT64_MAX||
       (sample&&now>INT64_MAX-m->latency_ns))return false;
    if(!o62m_pmic_charge_advance_v6(m,now))return false;
    m->generation++;m->requests++;if(m->pending)m->cancellations++;
    m->pending=sample;m->valid=false;m->ready=false;m->latched_raw24=0;m->latched_current_raw24=0;m->latched_positive=0;m->latched_negative=0;m->latched_history_count=0;m->latched_sample_ticks=0;
    memset(m->latched_voltage_history,0,sizeof(m->latched_voltage_history));
    memset(m->latched_current_history,0,sizeof(m->latched_current_history));
    if(sample)m->deadline_ns=now+m->latency_ns;
    return true;
}
static inline bool o62m_pmic_voltage_complete_v6(O62MPmicVoltageSnapshotV6 *m,
    uint64_t generation,int64_t now)
{
    if(!m->pending||generation!=m->generation||now<m->deadline_ns||
       !m->enabled||!m->source_available||!m->clock_available||
       m->source_raw24>0xffffff||m->source_current_raw24>0x7fffff||m->completions==UINT64_MAX)return false;
    if(!o62m_pmic_charge_advance_v6(m,now))return false;
    m->latched_history_count=m->history_count;m->latched_sample_ticks=m->sample_ticks;
    memcpy(m->latched_voltage_history,m->voltage_history,sizeof(m->voltage_history));
    memcpy(m->latched_current_history,m->current_history,sizeof(m->current_history));
    m->latched_positive=m->charge_positive;m->latched_negative=m->charge_negative;
    m->latched_raw24=m->source_raw24;m->latched_current_raw24=m->source_current_raw24;m->pending=false;m->valid=true;m->ready=true;m->completions++;
    return true;
}
static inline bool o62m_pmic_voltage_read_v6(const O62MPmicVoltageSnapshotV6 *m,
    uint16_t reg,uint16_t *value)
{
    if(!m->valid)return false;
    if(reg>=0x174&&reg<=0x1f0&&!(reg&3)) {
        if(m->latched_history_count!=8)return false;
        unsigned offset=reg<0x1b4?reg-0x174:reg-0x1b4;
        uint32_t raw=reg<0x1b4?m->latched_voltage_history[offset/8]:m->latched_current_history[offset/8];
        *value=(offset&4)?raw&0xffff:raw>>16;return true;
    }
    /* The virtual differential view uses this same completed sample as
     * its positive input and an explicit zero reference input. */
    if(reg==0x164||reg==0x13c)*value=m->latched_raw24>>16;
    else if(reg==0x168||reg==0x140)*value=m->latched_raw24&0xffff;
    else if(reg==0x144||reg==0x148)*value=0;
    else if(reg==0x16c)*value=m->latched_current_raw24>>16;
    else if(reg==0x170)*value=m->latched_current_raw24&0xffff;
    else if(reg==0x1fc)*value=m->latched_positive&0xffff;
    else if(reg==0x1f8)*value=(m->latched_positive>>16)&0xffff;
    else if(reg==0x1f4)*value=m->latched_positive>>32;
    else if(reg==0x208)*value=m->latched_negative&0xffff;
    else if(reg==0x204)*value=(m->latched_negative>>16)&0xffff;
    else if(reg==0x200)*value=m->latched_negative>>32;
    else return false;
    return true;
}
static inline uint16_t o62m_pmic_voltage_status_v6(const O62MPmicVoltageSnapshotV6 *m,uint16_t base)
{ return (base&~0x800)|(m->ready?0x800:0); }
#endif
