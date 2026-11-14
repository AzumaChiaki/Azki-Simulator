#ifndef O62M_XIPC_CONFIG_V1_H
#define O62M_XIPC_CONFIG_V1_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
typedef struct O62MXipcConfigV1 {
 uint8_t active,pending_selector;
 bool pending;
 uint64_t generation,commits;
 int64_t deadline;
} O62MXipcConfigV1;
static inline void o62m_xipc_config_cancel_v1(O62MXipcConfigV1*s)
{s->generation++;s->pending=false;s->pending_selector=0;s->deadline=0;}
static inline void o62m_xipc_config_reset_v1(O62MXipcConfigV1*s)
{uint64_t generation=s->generation+1;memset(s,0,sizeof *s);s->generation=generation;}
static inline bool o62m_xipc_config_submit_v1(O62MXipcConfigV1*s,uint32_t selector,int64_t now)
{
 if(s->pending||selector>31||now<0||now>INT64_MAX-500)return false;
 s->pending_selector=selector;s->pending=true;s->generation++;s->deadline=now+500;return true;
}
static inline bool o62m_xipc_config_complete_v1(O62MXipcConfigV1*s,uint64_t generation,int64_t now)
{
 if(!s->pending||s->generation!=generation||now<s->deadline)return false;
 s->active=s->pending_selector;s->pending_selector=0;s->pending=false;s->deadline=0;s->commits++;return true;
}
#endif
