#ifndef O62M_RTC32K_REFERENCE_V1_H
#define O62M_RTC32K_REFERENCE_V1_H
#include <stdbool.h>
#include <stdint.h>
#define O62M_RTC32K_QUALIFY_NS INT64_C(976563)
typedef struct O62MRTC32kReferenceV1 {
    uint64_t generation, starts, completions;
    int64_t deadline_ns;
    bool source_available, pending, ready;
} O62MRTC32kReferenceV1;
static inline bool o62m_rtc32k_source_start_v1(O62MRTC32kReferenceV1 *s,bool available,int64_t now)
{
    if(now<0||s->generation==UINT64_MAX||(available&&now>INT64_MAX-O62M_RTC32K_QUALIFY_NS))return false;
    s->generation++;s->source_available=available;s->ready=false;s->pending=available;
    s->deadline_ns=available?now+O62M_RTC32K_QUALIFY_NS:0;
    if(available)s->starts++;
    return true;
}
static inline bool o62m_rtc32k_complete_v1(O62MRTC32kReferenceV1 *s,uint64_t generation,int64_t now)
{
    if(!s->pending||!s->source_available||generation!=s->generation||now<s->deadline_ns)return false;
    s->pending=false;s->ready=true;s->completions++;return true;
}
static inline uint16_t o62m_rtc32k_status_v1(const O62MRTC32kReferenceV1 *s)
{ return s->source_available&&s->ready?UINT16_C(0x400):0; }
static inline void o62m_rtc32k_stop_v1(O62MRTC32kReferenceV1 *s)
{ s->source_available=false;s->pending=false;s->ready=false; }
#endif
