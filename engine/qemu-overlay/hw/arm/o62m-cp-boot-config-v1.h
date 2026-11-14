#ifndef O62M_CP_BOOT_CONFIG_V1_H
#define O62M_CP_BOOT_CONFIG_V1_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
typedef struct O62MCpBootConfigV1 {
 uint32_t requested,active;
 bool pending,valid;
 uint64_t generation,requests,commits;
 int64_t deadline;
} O62MCpBootConfigV1;
static inline void o62m_cp_boot_config_cancel_v1(O62MCpBootConfigV1*s)
{s->generation++;s->pending=false;s->valid=false;s->requested=0;s->deadline=0;}
static inline void o62m_cp_boot_config_reset_v1(O62MCpBootConfigV1*s)
{uint64_t gen=s->generation+1;memset(s,0,sizeof *s);s->generation=gen;}
static inline bool o62m_cp_boot_config_request_v1(O62MCpBootConfigV1*s,uint32_t value,int64_t now)
{
 if(s->pending||now<0||now>INT64_MAX-500)return false;
 s->requested=value;s->pending=true;s->valid=false;s->generation++;s->requests++;s->deadline=now+500;return true;
}
static inline bool o62m_cp_boot_config_complete_v1(O62MCpBootConfigV1*s,uint64_t generation,int64_t now)
{
 if(!s->pending||generation!=s->generation||now<s->deadline)return false;
 s->active=s->requested;s->requested=0;s->pending=false;s->valid=true;s->deadline=0;s->commits++;return true;
}
static inline bool o62m_cp_boot_config_status_v1(const O62MCpBootConfigV1*s,uint32_t *busy)
{if(!s->pending&&!s->valid)return false;*busy=s->pending?1:0;return true;}
#endif
