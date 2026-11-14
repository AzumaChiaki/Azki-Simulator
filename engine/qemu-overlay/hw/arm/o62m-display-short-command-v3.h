#ifndef O62M_DISPLAY_SHORT_COMMAND_V3_H
#define O62M_DISPLAY_SHORT_COMMAND_V3_H
#include <stdbool.h>
#include <stdint.h>
#define O62M_DISPLAY_SHORT_HISTORY_V3 64
#define O62M_DISPLAY_SHORT_DELAY_NS_V3 INT64_C(1231)
#define O62M_DISPLAY_NOPEER_DELAY_NS_V3 INT64_C(1000000)
enum { O62M_DISPLAY_RESPONSE_NONE_V3, O62M_DISPLAY_RESPONSE_WAIT_V3, O62M_DISPLAY_RESPONSE_TIMEOUT_V3, O62M_DISPLAY_RESPONSE_CANCELLED_V3 };
enum { O62M_DISPLAY_PENDING_V3=1, O62M_DISPLAY_CONSUMED_V3, O62M_DISPLAY_CANCELLED_V3 };
typedef struct O62MDisplayShortRecordV3 {
    uint32_t header;
    unsigned phase, response_outcome;
    bool low_power;
    uint64_t generation, pll_generation, ulps_generation;
    int64_t request_ns, consumed_ns, response_ns;
} O62MDisplayShortRecordV3;
typedef struct O62MDisplayShortV3 {
    O62MDisplayShortRecordV3 history[O62M_DISPLAY_SHORT_HISTORY_V3];
    unsigned history_count;
    bool pending, read_pending, faulted;
    uint64_t generation, consumed, cancelled, peer_timeouts;
    int64_t deadline_ns, peer_deadline_ns;
} O62MDisplayShortV3;
static inline void o62m_display_short_cancel_v3(O62MDisplayShortV3 *s)
{
    if(s->pending||s->read_pending) {
        O62MDisplayShortRecordV3 *r=&s->history[s->history_count-1];
        if(s->pending)r->phase=O62M_DISPLAY_CANCELLED_V3;
        if(s->read_pending)r->response_outcome=O62M_DISPLAY_RESPONSE_CANCELLED_V3;
        s->pending=false;s->read_pending=false;s->faulted=true;++s->cancelled;
    }
    if(s->generation!=UINT64_MAX)++s->generation;
}
static inline void o62m_display_short_reset_v3(O62MDisplayShortV3 *s)
{
    o62m_display_short_cancel_v3(s);s->faulted=false;
}
static inline bool o62m_display_short_status_v3(const O62MDisplayShortV3 *s,uint32_t *word)
{
    if(s->faulted)return false;
    /* Accepted reads target the explicitly absent peer; no RX bytes are produced. */
    *word=(1u<<2)|(1u<<4)|(s->pending?(1u<<1):(1u<<0))|(s->read_pending?(1u<<6):0);return true;
}
static inline bool o62m_display_short_write_type_v3(unsigned type)
{
    return type==0x03||type==0x13||type==0x23||type==0x05||type==0x15;
}
static inline unsigned o62m_display_short_lp_bit_v3(unsigned type)
{
    switch(type) {
    case 0x03:return 8; case 0x13:return 9; case 0x23:return 10;
    case 0x05:return 16; case 0x15:return 17; case 0x14:return 12;
    default:return 24;
    }
}
static inline bool o62m_display_short_submit_v3(O62MDisplayShortV3 *s,uint64_t header,
    int64_t now,uint64_t pll_generation,uint64_t ulps_generation,
    bool eligible,bool low_power,bool ack_requested)
{
    unsigned type=header&0x3f;
    if(!eligible||ack_requested||s->faulted||s->pending||s->read_pending||
       s->history_count>=O62M_DISPLAY_SHORT_HISTORY_V3||header>0xffffff||
       (type!=0x37&&type!=0x14&&!o62m_display_short_write_type_v3(type))||s->generation==UINT64_MAX||
       now<0||now>INT64_MAX-O62M_DISPLAY_SHORT_DELAY_NS_V3)return false;
    ++s->generation;
    s->history[s->history_count++]=(O62MDisplayShortRecordV3){
        .header=(uint32_t)header,.phase=O62M_DISPLAY_PENDING_V3,.low_power=low_power,
        .generation=s->generation,.pll_generation=pll_generation,.ulps_generation=ulps_generation,
        .request_ns=now,.consumed_ns=-1,.response_ns=-1,
        .response_outcome=type==0x14?O62M_DISPLAY_RESPONSE_WAIT_V3:O62M_DISPLAY_RESPONSE_NONE_V3};
    s->pending=true;s->read_pending=type==0x14;s->peer_deadline_ns=-1;
    s->deadline_ns=now+O62M_DISPLAY_SHORT_DELAY_NS_V3;return true;
}
static inline bool o62m_display_short_consume_v3(O62MDisplayShortV3 *s,uint64_t generation,
    int64_t now,uint64_t pll_generation,uint64_t ulps_generation,bool eligible)
{
    if(!s->pending||s->faulted||!eligible||generation!=s->generation||now<s->deadline_ns)return false;
    O62MDisplayShortRecordV3 *r=&s->history[s->history_count-1];
    if(r->pll_generation!=pll_generation||r->ulps_generation!=ulps_generation)return false;
    if(s->read_pending&&now>INT64_MAX-O62M_DISPLAY_NOPEER_DELAY_NS_V3)return false;
    r->phase=O62M_DISPLAY_CONSUMED_V3;r->consumed_ns=now;
    if(s->read_pending)s->peer_deadline_ns=now+O62M_DISPLAY_NOPEER_DELAY_NS_V3;
    s->pending=false;++s->consumed;return true;
}
static inline bool o62m_display_short_nopeer_v3(O62MDisplayShortV3 *s,uint64_t generation,
    int64_t now,uint64_t pll_generation,uint64_t ulps_generation,bool eligible)
{
    if(s->pending||!s->read_pending||s->faulted||!eligible||generation!=s->generation||
       now<s->peer_deadline_ns)return false;
    O62MDisplayShortRecordV3 *r=&s->history[s->history_count-1];
    if(r->pll_generation!=pll_generation||r->ulps_generation!=ulps_generation)return false;
    r->response_outcome=O62M_DISPLAY_RESPONSE_TIMEOUT_V3;r->response_ns=now;
    s->read_pending=false;++s->peer_timeouts;return true;
}
#endif
