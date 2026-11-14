#ifndef O62M_PUBAP_QUIESCE_V1_H
#define O62M_PUBAP_QUIESCE_V1_H
#include <stdbool.h>
#include <stdint.h>
/* Partial virtual control service; frequency-transition commands are unsupported. */
typedef struct O62MPubapQuiesceV1 {
    uint64_t generation, requests, completions;
    int64_t deadline_ns;
    bool pending, valid;
} O62MPubapQuiesceV1;
static inline bool o62m_pubap_quiesce_start_v1(O62MPubapQuiesceV1 *s,int64_t now)
{
    if(now<0||now>INT64_MAX-1000||s->generation==UINT64_MAX)return false;
    ++s->generation;++s->requests;s->deadline_ns=now+1000;
    s->pending=true;s->valid=false;return true;
}
static inline void o62m_pubap_quiesce_cancel_v1(O62MPubapQuiesceV1 *s)
{
    s->pending=false;s->valid=false;
    if(s->generation!=UINT64_MAX)++s->generation;
}
static inline bool o62m_pubap_quiesce_complete_v1(O62MPubapQuiesceV1 *s,
        uint64_t generation,int64_t now,bool domain_available)
{
    if(!s->pending||generation!=s->generation||now<s->deadline_ns||
       !domain_available)return false;
    s->pending=false;s->valid=true;++s->completions;return true;
}
static inline bool o62m_pubap_quiesce_status_v1(const O62MPubapQuiesceV1 *s,uint32_t *word)
{
    if(!s->pending&&!s->valid)return false;
    *word=s->pending?(UINT32_C(1)<<4)|(UINT32_C(7)<<17):(UINT32_C(1)<<17);
    return true;
}
#endif
