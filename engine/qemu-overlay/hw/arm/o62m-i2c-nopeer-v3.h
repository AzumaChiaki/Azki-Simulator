/* SPDX-License-Identifier: MIT
 * Virtual I2C master with bounded TX/RX FIFOs. ACK completion is supplied
 * only by the attached QEMU I2C peer after a timed byte transaction.
 * The canonical configuration remains in O62MI2CEmptyConfigV3. A deadline
 * requests a real QEMU bus START; only its actual nonzero result may commit
 * address-NACK. No successful transfer, RX byte, IRQ or peer is fabricated.
 */
#ifndef O62M_I2C_NOPEER_V3_H
#define O62M_I2C_NOPEER_V3_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "o62m-i2c-empty-config-v3.h"

#define O62M_I2C_NOPEER_CAPACITY_V3 16u
enum {
    O62M_I2C_NOPEER_OK_V3 = 0,      /* due(): wait; no bus request issued */
    O62M_I2C_NOPEER_REQUEST_V3 = 1,
    O62M_I2C_NOPEER_REJECT_V3 = -1,
    O62M_I2C_NOPEER_TIME_V3 = -2,
    O62M_I2C_NOPEER_OVERFLOW_V3 = -3,
    O62M_I2C_NOPEER_UNEXPECTED_ACK_V3 = -4,
    O62M_I2C_NOPEER_STALE_V3 = -5
};
typedef struct O62MI2CNopeerRequestV3 {
    uint64_t generation;
    uint32_t command;
    uint8_t target;
    bool read;
} O62MI2CNopeerRequestV3;
typedef struct O62MI2CNopeerV3 {
    uint32_t tx[O62M_I2C_NOPEER_CAPACITY_V3];
    uint8_t rx[16], rx_head, rx_count;
    bool bus_open, bus_read;
    uint64_t acked, received, abort_discarded;
    uint32_t hz;
    uint32_t abort_source;
    uint8_t head, count, target;
    bool enabled, active, pending, issued, abort_latched;
    uint64_t generation, deadline_ns, now_ns;
    uint64_t reads, writes, enqueued, requests_issued, nacks, cancels;
} O62MI2CNopeerV3;

static inline void o62m_i2c_nopeer_wipe_v3(O62MI2CNopeerV3 *s)
{
    if(s) {
        volatile unsigned char *p=(volatile unsigned char *)s;
        for(size_t i=0;i<sizeof(*s);i++)p[i]=0;
    }
}
static inline void o62m_i2c_nopeer_flush_v3(O62MI2CNopeerV3 *s)
{
    volatile uint32_t *p=s->tx;
    for(unsigned i=0;i<O62M_I2C_NOPEER_CAPACITY_V3;i++)p[i]=0;
    s->head=0;s->count=0;s->target=0;
    memset(s->rx,0,sizeof(s->rx));s->rx_head=s->rx_count=0;
    s->bus_open=false;s->bus_read=false;
    s->active=false;s->pending=false;s->issued=false;s->deadline_ns=0;
}
static inline int o62m_i2c_nopeer_init_v3(O62MI2CNopeerV3 *s,
                                        bool enabled,uint32_t hz,uint64_t now)
{
    if(!s||!hz)return O62M_I2C_NOPEER_REJECT_V3;
    O62MI2CNopeerV3 fresh={0};
    fresh.enabled=enabled;fresh.hz=hz;fresh.now_ns=now;*s=fresh;
    return O62M_I2C_NOPEER_OK_V3;
}
static inline bool o62m_i2c_nopeer_valid_v3(const O62MI2CNopeerV3 *s,
                                          const O62MI2CEmptyConfigV3 *cfg)
{
    if(!s||!s->hz||!o62m_i2c_empty_config_valid_v3(cfg)||
       s->head>=O62M_I2C_NOPEER_CAPACITY_V3||s->count>O62M_I2C_NOPEER_CAPACITY_V3||
       s->rx_head>=16||s->rx_count>16||
       s->active!=(s->count!=0)||s->pending!=s->active||
       (s->issued&&!s->pending)||
       s->abort_source!=(s->abort_latched?UINT32_C(1):UINT32_C(0))||
       (!s->enabled&&(s->pending||s->abort_latched)))return false;
    if(s->pending) {
        if(!s->generation||!s->deadline_ns||s->abort_latched||
           cfg->config[11]!=1||!(cfg->config[0]&1)||
           (cfg->config[0]&UINT32_C(0x10))||
           cfg->config[1]<1||cfg->config[1]>127||s->target!=cfg->config[1])return false;
        for(unsigned i=0;i<s->count;i++)
            if(s->tx[(s->head+i)%O62M_I2C_NOPEER_CAPACITY_V3]&~UINT32_C(0x7ff))return false;
    } else if(s->head||s->deadline_ns||(!s->bus_open&&s->target))return false;
    return true;
}
/* Virtual address-phase latency: nine SCL cycles, with each SCL period the
 * selected HCNT+LCNT input clocks. This is a declared functional policy,
 * not a physical timing measurement or hidden clock-gate assertion. */
static inline int o62m_i2c_nopeer_delay_v3(const O62MI2CNopeerV3 *s,
                                         const O62MI2CEmptyConfigV3 *cfg,
                                         uint64_t *delay)
{
    if(!delay||!o62m_i2c_nopeer_valid_v3(s,cfg))return O62M_I2C_NOPEER_REJECT_V3;
    unsigned speed=(cfg->config[0]>>1)&3u;
    if(!speed)return O62M_I2C_NOPEER_REJECT_V3;
    unsigned high=2u+(speed-1u)*2u;
    uint64_t h=cfg->config[high],l=cfg->config[high+1];
    if(!h||!l)return O62M_I2C_NOPEER_REJECT_V3;
    uint64_t cycles=UINT64_C(9)*(h+l);
    uint64_t whole=cycles/s->hz,rem=cycles%s->hz;
    if(whole>UINT64_MAX/UINT64_C(1000000000))return O62M_I2C_NOPEER_OVERFLOW_V3;
    uint64_t ns=whole*UINT64_C(1000000000);
    /* rem<2^32 and Hz<2^32, so the numerator fits uint64_t. */
    uint64_t fraction=(rem*UINT64_C(1000000000)+s->hz-1u)/s->hz;
    if(ns>UINT64_MAX-fraction)return O62M_I2C_NOPEER_OVERFLOW_V3;
    ns+=fraction;
    if(!ns)return O62M_I2C_NOPEER_REJECT_V3;
    *delay=ns;return O62M_I2C_NOPEER_OK_V3;
}
static inline int o62m_i2c_nopeer_access_v3(O62MI2CNopeerV3 *s,
    O62MI2CEmptyConfigV3 *cfg,uint64_t now,uint32_t offset,unsigned size,
    bool write,uint64_t *value)
{
    if(!value||!o62m_i2c_nopeer_valid_v3(s,cfg)||!s->enabled||
       size!=4||(offset&3)||(write&&*value>UINT32_MAX))return O62M_I2C_NOPEER_REJECT_V3;
    if(now<s->now_ns)return O62M_I2C_NOPEER_TIME_V3;
    if((write?s->writes:s->reads)==UINT64_MAX)return O62M_I2C_NOPEER_OVERFLOW_V3;
    if(write&&offset==0x10) {
        uint32_t command=(uint32_t)*value;
        if((command&~UINT32_C(0x7ff))||cfg->config[11]!=1||
           !(cfg->config[0]&1)||(cfg->config[0]&UINT32_C(0x10))||
           cfg->config[1]<1||cfg->config[1]>127)return O62M_I2C_NOPEER_REJECT_V3;
        /* TX_ABRT holds the FIFO in reset until the clear register is read. */
        if(s->abort_latched) {
            if(s->abort_discarded==UINT64_MAX)return O62M_I2C_NOPEER_OVERFLOW_V3;
            s->abort_discarded++;s->writes++;s->now_ns=now;
            return O62M_I2C_NOPEER_OK_V3;
        }
        if(s->count==O62M_I2C_NOPEER_CAPACITY_V3)return O62M_I2C_NOPEER_REJECT_V3;
        if(s->enqueued==UINT64_MAX||(!s->pending&&s->generation==UINT64_MAX))
            return O62M_I2C_NOPEER_OVERFLOW_V3;
        uint64_t deadline=s->deadline_ns;
        if(!s->pending) {
            uint64_t delay=0;
            int result=o62m_i2c_nopeer_delay_v3(s,cfg,&delay);
            if(result)return result;
            if(now>UINT64_MAX-delay)return O62M_I2C_NOPEER_OVERFLOW_V3;
            deadline=now+delay;
        }
        /* All potentially failing work precedes mutation. Queued words do
         * not receive ACKs, complete, or reschedule the first address phase. */
        s->tx[(s->head+s->count)%O62M_I2C_NOPEER_CAPACITY_V3]=command;
        if(!s->pending) {
            s->generation++;s->target=(uint8_t)cfg->config[1];
            s->deadline_ns=deadline;s->active=true;s->pending=true;s->issued=false;
        }
        s->count++;s->enqueued++;s->writes++;s->now_ns=now;
        return O62M_I2C_NOPEER_OK_V3;
    }
    if(!write) {
        uint32_t result=0;bool dynamic=true,clear=false;
        if(offset==0x70) {
            if(s->active||s->bus_open)result|=UINT32_C(0x21); /* ACTIVITY and MST_ACTIVITY */
            if(s->count<O62M_I2C_NOPEER_CAPACITY_V3)result|=2u;
            if(!s->count)result|=4u;
            if(s->rx_count)result|=8u;
            if(s->rx_count==16)result|=16u;
        } else if(offset==0x74)result=s->count;
        else if(offset==0x78)result=s->rx_count; /* No receive data has been produced. */
        else if(offset==0x10) {
            if(!s->rx_count)return O62M_I2C_NOPEER_REJECT_V3;
            result=s->rx[s->rx_head];s->rx[s->rx_head]=0;
            s->rx_head=(s->rx_head+1)%16;s->rx_count--;
        } else if(offset==0x34) {
            if(s->rx_count>cfg->config[9])result|=4u;
            if(s->count<=cfg->config[10])result|=UINT32_C(0x10);
            if(s->abort_latched)result|=UINT32_C(0x40);
        } else if(offset==0x80)result=s->abort_source;
        else if(offset==0x54||offset==0x40)clear=true;
        else dynamic=false;
        if(dynamic) {
            if(clear) {s->abort_latched=false;s->abort_source=0;}
            *value=result;s->reads++;s->now_ns=now;
            return O62M_I2C_NOPEER_OK_V3;
        }
    }
    if(write&&offset==0x6c&&*value==0) {
        bool cancel=s->pending||s->count||s->bus_open||s->rx_count;
        if(cancel&&(s->generation==UINT64_MAX||s->cancels==UINT64_MAX))
            return O62M_I2C_NOPEER_OVERFLOW_V3;
        O62MI2CEmptyConfigV3 trial=*cfg;uint64_t data=0;
        if(!o62m_i2c_empty_config_access_v3(&trial,offset,size,true,&data))
            return O62M_I2C_NOPEER_REJECT_V3;
        *cfg=trial;
        if(cancel) {s->generation++;s->cancels++;}
        o62m_i2c_nopeer_flush_v3(s);s->writes++;s->now_ns=now;
        return O62M_I2C_NOPEER_OK_V3;
    }
    if(s->pending||s->count||s->bus_open||s->rx_count)return O62M_I2C_NOPEER_REJECT_V3;
    O62MI2CEmptyConfigV3 trial=*cfg;
    uint64_t data=write?*value:0;
    if(!o62m_i2c_empty_config_access_v3(&trial,offset,size,write,&data))
        return O62M_I2C_NOPEER_REJECT_V3;
    *cfg=trial;s->now_ns=now;
    if(write)s->writes++;else {*value=data;s->reads++;}
    return O62M_I2C_NOPEER_OK_V3;
}
/* REQUEST is delivered once for each pending generation. The caller must
 * immediately perform the actual QEMU START and complete_nack(), or stop
 * strictly on an unexpected ACK. No bus callback occurs inside this header. */
static inline int o62m_i2c_nopeer_due_v3(O62MI2CNopeerV3 *s,
    const O62MI2CEmptyConfigV3 *cfg,uint64_t now,O62MI2CNopeerRequestV3 *request)
{
    if(!request||!o62m_i2c_nopeer_valid_v3(s,cfg)||!s->enabled)
        return O62M_I2C_NOPEER_REJECT_V3;
    if(now<s->now_ns)return O62M_I2C_NOPEER_TIME_V3;
    if(!s->pending||s->issued||now<s->deadline_ns) {
        s->now_ns=now;return O62M_I2C_NOPEER_OK_V3;
    }
    if(s->requests_issued==UINT64_MAX)return O62M_I2C_NOPEER_OVERFLOW_V3;
    /* No failures follow this point; avoid a second request/payload copy. */
    *request=(O62MI2CNopeerRequestV3){0};
    request->generation=s->generation;request->target=s->target;
    request->command=s->tx[s->head];
    request->read=(request->command&UINT32_C(0x100))!=0;
    s->issued=true;s->requests_issued++;s->now_ns=now;
    return O62M_I2C_NOPEER_REQUEST_V3;
}
static inline int o62m_i2c_nopeer_complete_nack_v3(O62MI2CNopeerV3 *s,
    const O62MI2CEmptyConfigV3 *cfg,uint64_t now,uint64_t generation,int bus_result)
{
    if(!o62m_i2c_nopeer_valid_v3(s,cfg)||!s->enabled)return O62M_I2C_NOPEER_REJECT_V3;
    if(generation!=s->generation)return O62M_I2C_NOPEER_STALE_V3;
    if(now<s->now_ns)return O62M_I2C_NOPEER_TIME_V3;
    if(!s->pending||!s->issued||now<s->deadline_ns)return O62M_I2C_NOPEER_REJECT_V3;
    if(!bus_result)return O62M_I2C_NOPEER_UNEXPECTED_ACK_V3;
    if(s->nacks==UINT64_MAX)return O62M_I2C_NOPEER_OVERFLOW_V3;
    o62m_i2c_nopeer_flush_v3(s);
    s->abort_latched=true;s->abort_source=1;s->nacks++;s->now_ns=now;
    return O62M_I2C_NOPEER_OK_V3;
}
static inline int o62m_i2c_nopeer_complete_ack_v3(O62MI2CNopeerV3 *s,
    const O62MI2CEmptyConfigV3 *cfg,uint64_t now,uint64_t generation,uint8_t received)
{
    if(!o62m_i2c_nopeer_valid_v3(s,cfg)||!s->issued||generation!=s->generation||
       now<s->deadline_ns)return O62M_I2C_NOPEER_REJECT_V3;
    uint32_t command=s->tx[s->head];
    if((command&0x100)&&s->rx_count==16)return O62M_I2C_NOPEER_OVERFLOW_V3;
    uint64_t delay=0;
    if(s->count>1&&o62m_i2c_nopeer_delay_v3(s,cfg,&delay))return O62M_I2C_NOPEER_REJECT_V3;
    if(now>UINT64_MAX-delay)return O62M_I2C_NOPEER_OVERFLOW_V3;
    if(command&0x100){s->rx[(s->rx_head+s->rx_count)%16]=received;s->rx_count++;s->received++;}
    s->tx[s->head]=0;s->head=(s->head+1)%16;s->count--;s->acked++;
    s->bus_open=!(command&0x200);s->bus_read=(command&0x100)!=0;
    s->issued=false;s->now_ns=now;
    if(s->count)s->deadline_ns=now+delay;
    else {s->head=0;s->active=s->pending=false;s->deadline_ns=0;
          if(!s->bus_open)s->target=0;}
    return O62M_I2C_NOPEER_OK_V3;
}
static inline int o62m_i2c_nopeer_next_deadline_v3(const O62MI2CNopeerV3 *s,
    const O62MI2CEmptyConfigV3 *cfg,bool *present,uint64_t *deadline)
{
    if(!present||!deadline||!o62m_i2c_nopeer_valid_v3(s,cfg))
        return O62M_I2C_NOPEER_REJECT_V3;
    bool available=s->enabled&&s->pending&&!s->issued;
    if(available)*deadline=s->deadline_ns;
    *present=available;return O62M_I2C_NOPEER_OK_V3;
}
#endif
