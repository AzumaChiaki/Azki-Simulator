#ifndef O62M_SENSYS_CONFIG_V1_H
#define O62M_SENSYS_CONFIG_V1_H
#include <stdbool.h>
#include <stdint.h>
/* Partial virtual configuration service with an absent, unpublished peer. */
typedef struct O62MSensysConfigV1 {
    uint32_t desired, effective;
    uint64_t generation, requests, completions, replaced;
    int64_t deadline_ns;
    bool pending, valid;
} O62MSensysConfigV1;
static inline bool o62m_sensys_config_request_v1(O62MSensysConfigV1 *s,
        uint64_t value,bool set,int64_t now)
{
    uint32_t next=(uint32_t)value,changed=next^s->desired;
    if(value>UINT32_MAX||now<0||now>INT64_MAX-1000||s->generation==UINT64_MAX||
       (changed&~UINT32_C(0xffffe))||(changed&&(changed&(changed-1)))||
       (set?(s->desired&~next):(next&~s->desired)))return false;
    if(s->pending)++s->replaced;
    s->desired=next;++s->generation;++s->requests;s->deadline_ns=now+1000;
    s->pending=true;s->valid=false;return true;
}
static inline void o62m_sensys_config_cancel_v1(O62MSensysConfigV1 *s)
{
    s->pending=false;s->valid=false;
    if(s->generation!=UINT64_MAX)++s->generation;
}
static inline bool o62m_sensys_config_complete_v1(O62MSensysConfigV1 *s,
        uint64_t generation,int64_t now)
{
    if(!s->pending||generation!=s->generation||now<s->deadline_ns)return false;
    s->effective=s->desired;s->pending=false;s->valid=true;++s->completions;return true;
}
static inline bool o62m_sensys_config_status_v1(const O62MSensysConfigV1 *s,uint32_t *word)
{
    if(!s->pending&&!s->valid)return false;
    *word=s->pending?UINT32_C(0xc0):0;return true;
}
#endif
