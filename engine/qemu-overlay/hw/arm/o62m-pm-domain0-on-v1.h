/* Explicit virtual service for an ON request to the already-on AP domain.
 * The service latency is not a measured electrical power-up time.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef O62M_PM_DOMAIN0_ON_V1_H
#define O62M_PM_DOMAIN0_ON_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
typedef struct O62MPMDomain0OnV1 {
    uint32_t initial, status;
    bool pending;
    uint64_t generation, requests, completions, cancelled;
    int64_t deadline_ns;
} O62MPMDomain0OnV1;
static inline void o62m_pm_domain0_cancel_v1(O62MPMDomain0OnV1 *s)
{
    if(s->pending)++s->cancelled;
    s->pending=false;s->deadline_ns=0;
    if(s->generation!=UINT64_MAX)++s->generation;
}
static inline void o62m_pm_domain0_reset_v1(O62MPMDomain0OnV1 *s)
{ o62m_pm_domain0_cancel_v1(s);s->status=s->initial; }
static inline bool o62m_pm_domain0_request_v1(O62MPMDomain0OnV1 *s,uint32_t value,int64_t now)
{
    if(value!=1||s->initial!=3||s->status!=3||s->pending||now<0||
       now>INT64_MAX-1000||s->generation==UINT64_MAX||s->requests==UINT64_MAX)return false;
    ++s->generation;++s->requests;s->pending=true;s->status&=~2u;s->deadline_ns=now+1000;return true;
}
static inline bool o62m_pm_domain0_complete_v1(O62MPMDomain0OnV1 *s,uint64_t generation,int64_t now)
{
    if(!s->pending||generation!=s->generation||now<s->deadline_ns||
       s->initial!=3||s->status!=1||s->completions==UINT64_MAX)return false;
    s->status|=2u;s->pending=false;s->deadline_ns=0;++s->completions;return true;
}
#endif
