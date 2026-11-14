/* Functional virtual USB/domain9 PMC adapter. Original up waits bit0=0;
 * down has no post-poll. Initial state and transition latency are explicit
 * virtual inputs. Pending bit0=1 means not-settled-on in this model, not a
 * recovered physical busy encoding. No IRQ or USB packet completion.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef O62M_PM_BIT_DOMAIN9_V1_H
#define O62M_PM_BIT_DOMAIN9_V1_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
typedef struct O62MPMBitDomain9V1 {
    bool on,pending,target_on;
    uint32_t last_command;
    uint64_t completed;
    int64_t last_ns,latency_ns,due_ns;
} O62MPMBitDomain9V1;
static inline bool o62m_pm_bit_domain9_init_v1(O62MPMBitDomain9V1 *s,uint32_t initial_word,int64_t latency,int64_t now)
{
    if(!s||initial_word>1||latency<=0||now<0)return false;
    memset(s,0,sizeof(*s));s->on=initial_word==0;s->latency_ns=latency;s->last_ns=now;return true;
}
static inline bool o62m_pm_bit_domain9_advance_v1(O62MPMBitDomain9V1 *s,int64_t now)
{
    if(!s||now<s->last_ns)return false;
    if(s->pending&&now>=s->due_ns){s->on=s->target_on;s->pending=false;s->completed++;}
    s->last_ns=now;return true;
}
static inline bool o62m_pm_bit_domain9_request_v1(O62MPMBitDomain9V1 *s,uint64_t command,int64_t now)
{
    if(!s||s->pending||now<s->last_ns||s->completed==UINT64_MAX||
       (command!=0&&command!=UINT64_C(0x4c504d45))||s->latency_ns>INT64_MAX-now)return false;
    s->last_command=(uint32_t)command;s->target_on=command==0;
    s->due_ns=now+s->latency_ns;s->pending=true;s->last_ns=now;return true;
}
static inline uint32_t o62m_pm_bit_domain9_status_v1(const O62MPMBitDomain9V1 *s)
{return s->pending||!s->on?1u:0u;}
static inline bool o62m_pm_bit_domain9_clock_available_v1(const O62MPMBitDomain9V1 *s)
{return s->on&&!s->pending;}
#endif
