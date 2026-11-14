#ifndef O62M_DISPLAY_TE_INPUT_V1_H
#define O62M_DISPLAY_TE_INPUT_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define O62M_TE_PERIOD_NS INT64_C(16666667)
#define O62M_TE_HIGH_NS INT64_C(1000000)
typedef struct {
    bool running, high, pending;
    int64_t deadline;
    uint64_t rises, falls, acknowledgements;
} O62MDisplayTEInputV1;
static inline void o62m_display_te_reset_v1(O62MDisplayTEInputV1 *s)
{ memset(s,0,sizeof(*s)); }
static inline void o62m_display_te_config_v1(O62MDisplayTEInputV1 *s,bool enabled,int64_t now)
{
    if(!enabled){s->running=false;s->high=false;s->deadline=0;return;}
    if(!s->running){s->running=true;s->high=false;s->deadline=now+O62M_TE_PERIOD_NS;}
}
static inline bool o62m_display_te_advance_v1(O62MDisplayTEInputV1 *s,int64_t now)
{
    if(!s->running||now<s->deadline)return false;
    do {
        s->high=!s->high;
        if(s->high){++s->rises;s->pending=true;s->deadline+=O62M_TE_HIGH_NS;}
        else {++s->falls;s->deadline+=O62M_TE_PERIOD_NS-O62M_TE_HIGH_NS;}
    } while(now>=s->deadline);
    return true;
}
static inline void o62m_display_te_ack_v1(O62MDisplayTEInputV1 *s)
{ if(s->pending){s->pending=false;++s->acknowledgements;} }
static inline bool o62m_display_te_irq_v1(const O62MDisplayTEInputV1 *s,bool enabled,bool masked,bool routed)
{ return s->pending&&enabled&&!masked&&routed; }
#endif
