/* Functional virtual domain4 protocol, not recovered electrical O62M power.
 * Handover on-mask is an input, not generated completion. Status/guard/config
 * are independent. Config commits never power on a domain.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef O62M_PM_DOMAIN4_V2_H
#define O62M_PM_DOMAIN4_V2_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
typedef struct O62MPMDomain4V1 {
    uint32_t active_timing[3], pending_timing[3];
    uint32_t guard_word;
    uint8_t written;
    bool on,available,config_valid,config_pending,transition_pending,target_on;
    bool transition_waits_config;
    uint64_t queued_config_generation;
    uint64_t staging_generation,committed_generation,config_jobs,transitions;
    int64_t last_ns,config_latency_ns,transition_latency_ns,config_due_ns,transition_due_ns;
} O62MPMDomain4V1;
static inline bool o62m_pm_domain4_init_v1(O62MPMDomain4V1 *s,uint32_t on_mask,
    int64_t config_ns,int64_t transition_ns,int64_t now)
{
    if(!s||(on_mask&~UINT32_C(0x10))||config_ns<=0||transition_ns<=0||now<0)return false;
    memset(s,0,sizeof(*s));s->on=on_mask!=0;s->available=s->on;
    s->last_ns=now;s->config_latency_ns=config_ns;s->transition_latency_ns=transition_ns;
    return true;
}
static inline bool o62m_pm_domain4_advance_v1(O62MPMDomain4V1 *s,int64_t now)
{
    if(!s||now<s->last_ns)return false;
    if(s->config_pending&&now>=s->config_due_ns) {
        if(s->transition_waits_config&&(!s->transition_pending||!s->target_on||
           s->queued_config_generation!=s->committed_generation||
           s->transition_latency_ns>INT64_MAX-now))return false;
        /* Real copy into the active virtual configuration, not a poll stub. */
        memcpy(s->active_timing,s->pending_timing,sizeof(s->active_timing));
        memset(s->pending_timing,0,sizeof(s->pending_timing));
        s->config_pending=false;s->config_valid=true;s->available=!s->transition_pending;s->config_jobs++;
        if(s->transition_waits_config) {
            s->transition_waits_config=false;
            s->transition_due_ns=now+s->transition_latency_ns;
        }
    }
    if(s->transition_pending&&!s->transition_waits_config&&now>=s->transition_due_ns) {
        s->on=s->target_on;s->available=true;s->transition_pending=false;s->transitions++;
        /* In particular, guard_word is NOT a mirror of power state. */
    }
    s->last_ns=now;return true;
}
static inline bool o62m_pm_domain4_note_timing_write_v1(O62MPMDomain4V1 *s,unsigned index,int64_t now)
{
    if(!s||index>=3||now<s->last_ns||s->config_pending||s->transition_pending||s->staging_generation==UINT64_MAX)return false;
    s->written|=(uint8_t)(1u<<index);s->staging_generation++;s->last_ns=now;return true;
}
static inline bool o62m_pm_domain4_commit_v1(O62MPMDomain4V1 *s,const uint32_t actual_words[3],int64_t now)
{
    if(!s||!actual_words||now<s->last_ns)return false;
    if(s->written!=7||s->committed_generation==s->staging_generation) {s->last_ns=now;return true;}
    if(s->config_pending||s->transition_pending||s->config_jobs==UINT64_MAX||
       s->config_latency_ns>INT64_MAX-now)return false;
    memcpy(s->pending_timing,actual_words,sizeof(s->pending_timing));
    s->committed_generation=s->staging_generation;s->config_due_ns=now+s->config_latency_ns;
    s->config_pending=true;s->last_ns=now;return true;
}
static inline bool o62m_pm_domain4_request_v1(O62MPMDomain4V1 *s,bool target_on,uint32_t variant,int64_t now)
{
    if(!s||!variant||(variant&~15u)||(variant&(variant-1))||now<s->last_ns||
       !s->config_valid||!s->available||s->config_pending||s->transition_pending||
       s->transitions==UINT64_MAX||s->transition_latency_ns>INT64_MAX-now)return false;
    s->target_on=target_on;s->transition_due_ns=now+s->transition_latency_ns;
    s->transition_pending=true;s->available=false;s->last_ns=now;return true;
}
static inline bool o62m_pm_domain5_on_after_config_v2(O62MPMDomain4V1 *s,int64_t now)
{
    if(!s)return false;
    if(!s->config_pending)return o62m_pm_domain4_request_v1(s,true,8,now);
    if(now<s->last_ns||!s->config_valid||!s->available||s->transition_pending||
       s->transition_waits_config||s->written!=7||
       s->committed_generation!=s->staging_generation||
       s->transitions==UINT64_MAX)return false;
    s->target_on=true;s->transition_pending=true;s->transition_waits_config=true;
    s->queued_config_generation=s->committed_generation;s->available=false;s->last_ns=now;
    return true;
}
static inline void o62m_pm_domain_cancel_v2(O62MPMDomain4V1 *s)
{
    if(s->config_pending||s->transition_pending)s->available=false;
    s->config_pending=false;s->transition_pending=false;s->transition_waits_config=false;
    memset(s->pending_timing,0,sizeof(s->pending_timing));
}
static inline uint32_t o62m_pm_domain4_status_v1(const O62MPMDomain4V1 *s)
{ return (s->available?2u:0u)|(s->on?1u:0u); }
static inline int64_t o62m_pm_domain4_deadline_v1(const O62MPMDomain4V1 *s)
{
    if(s->config_pending)return s->config_due_ns;
    return s->transition_pending?s->transition_due_ns:-1;
}
#endif
