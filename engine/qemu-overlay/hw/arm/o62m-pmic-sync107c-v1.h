/* SPDX-License-Identifier: MIT
 * Explicit BusOnly barrier profile: request() is called only at successful
 * actual107c peercommit after prior serialized bus writes drained. Positive
 * elapsed virtual processing time produces ACKbit1. No ADC/DVS-settle claim.
 * The full16 canonical backing is external and is never stored here.
 */
#ifndef O62M_PMIC_SYNC107C_V1_H
#define O62M_PMIC_SYNC107C_V1_H
#include <stdint.h>
#include <stdbool.h>
enum {
    O62M_PMIC_SYNC107C_OK_V1=0,
    O62M_PMIC_SYNC107C_REJECT_V1=-1,
    O62M_PMIC_SYNC107C_STALE_V1=-2,
    O62M_PMIC_SYNC107C_TIME_V1=-3,
    O62M_PMIC_SYNC107C_OVERFLOW_V1=-4,
    O62M_PMIC_SYNC107C_NOT_DUE_V1=-5
};
typedef struct O62MPmicSync107cV1 {
    bool enabled,pending,ack;
    int64_t latency_ns,now_ns,started_ns,deadline_ns;
    uint64_t generation,requests,completions;
} O62MPmicSync107cV1;
static inline bool o62m_pmic_sync107c_valid_v1(const O62MPmicSync107cV1 *s)
{
    if(!s||s->latency_ns<=0||s->now_ns<0||(s->pending&&s->ack)||
       s->completions>s->requests)return false;
    if(!s->enabled&&(s->pending||s->ack||s->generation||s->requests||s->completions))return false;
    if(s->pending)return s->generation&&s->requests>s->completions&&
        s->started_ns>=0&&s->started_ns<=s->now_ns&&s->deadline_ns>s->started_ns;
    return !s->started_ns&&!s->deadline_ns&&(!s->ack||(s->generation&&s->completions));
}
/* Fresh constructor only. Re-init requires caller's hard old-worker barrier. */
static inline int o62m_pmic_sync107c_init_v1(O62MPmicSync107cV1 *s,bool enabled,
                                           int64_t latency_ns,int64_t now)
{
    if(!s||latency_ns<=0)return O62M_PMIC_SYNC107C_REJECT_V1;
    if(now<0)return O62M_PMIC_SYNC107C_TIME_V1;
    O62MPmicSync107cV1 fresh={0};
    fresh.enabled=enabled;fresh.latency_ns=latency_ns;fresh.now_ns=now;*s=fresh;
    return O62M_PMIC_SYNC107C_OK_V1;
}
static inline int o62m_pmic_sync107c_request_v1(O62MPmicSync107cV1 *s,int64_t now)
{
    if(!o62m_pmic_sync107c_valid_v1(s)||!s->enabled)return O62M_PMIC_SYNC107C_REJECT_V1;
    if(now<s->now_ns)return O62M_PMIC_SYNC107C_TIME_V1;
    if(s->generation==UINT64_MAX||s->requests==UINT64_MAX||
       s->latency_ns>INT64_MAX-now)return O62M_PMIC_SYNC107C_OVERFLOW_V1;
    s->generation++;s->requests++;s->pending=true;s->ack=false;
    s->started_ns=now;s->deadline_ns=now+s->latency_ns;s->now_ns=now;
    return O62M_PMIC_SYNC107C_OK_V1;
}
static inline int o62m_pmic_sync107c_complete_v1(O62MPmicSync107cV1 *s,
                                               int64_t now,uint64_t generation)
{
    if(!o62m_pmic_sync107c_valid_v1(s)||!s->enabled)return O62M_PMIC_SYNC107C_REJECT_V1;
    if(generation!=s->generation)return O62M_PMIC_SYNC107C_STALE_V1;
    if(now<s->now_ns)return O62M_PMIC_SYNC107C_TIME_V1;
    if(!s->pending)return O62M_PMIC_SYNC107C_REJECT_V1;
    if(now<s->deadline_ns)return O62M_PMIC_SYNC107C_NOT_DUE_V1;
    if(s->completions==UINT64_MAX)return O62M_PMIC_SYNC107C_OVERFLOW_V1;
    s->completions++;s->pending=false;s->ack=true;s->now_ns=now;
    s->started_ns=0;s->deadline_ns=0;
    return O62M_PMIC_SYNC107C_OK_V1;
}
static inline int o62m_pmic_sync107c_cancel_v1(O62MPmicSync107cV1 *s,int64_t now)
{
    if(!o62m_pmic_sync107c_valid_v1(s)||!s->enabled)return O62M_PMIC_SYNC107C_REJECT_V1;
    if(now<s->now_ns)return O62M_PMIC_SYNC107C_TIME_V1;
    if(s->generation==UINT64_MAX)return O62M_PMIC_SYNC107C_OVERFLOW_V1;
    s->generation++;s->pending=false;s->ack=false;s->now_ns=now;
    s->started_ns=0;s->deadline_ns=0;
    return O62M_PMIC_SYNC107C_OK_V1;
}
static inline int o62m_pmic_sync107c_next_deadline_v1(const O62MPmicSync107cV1 *s,
                                                    bool *present,int64_t *deadline)
{
    if(!present||!deadline||!o62m_pmic_sync107c_valid_v1(s))return O62M_PMIC_SYNC107C_REJECT_V1;
    bool due=s->enabled&&s->pending;
    if(due)*deadline=s->deadline_ns;
    *present=due;return O62M_PMIC_SYNC107C_OK_V1;
}
/* Caller rejects initial backingbit1 conflict. Normalize RO echoes before
 * existing full16 opaque-preserving writable-bit validation, not instead of it. */
static inline uint16_t o62m_pmic_sync107c_normalize_v1(uint16_t incoming)
{ return (uint16_t)(incoming&~UINT16_C(2)); }
static inline int o62m_pmic_sync107c_project_v1(const O62MPmicSync107cV1 *s,
                                               uint16_t backing,uint16_t *value)
{
    if(!value||!o62m_pmic_sync107c_valid_v1(s)||!s->enabled)return O62M_PMIC_SYNC107C_REJECT_V1;
    *value=(uint16_t)((backing&~UINT16_C(2))|(s->ack?UINT16_C(2):UINT16_C(0)));
    return O62M_PMIC_SYNC107C_OK_V1;
}
#endif
