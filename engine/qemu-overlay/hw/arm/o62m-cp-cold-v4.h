#ifndef O62M_CP_COLD_V4_H
#define O62M_CP_COLD_V4_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
typedef struct O62MCpColdV1 {
 uint32_t entry,clock_control,request_control;
 uint32_t pending_config[5],active_config[5],pending_timing[3],active_timing[3];
 unsigned stage,phase;
 bool on,available,release_accepted;
 uint64_t generation,requests,config_commits,transitions;
 int64_t deadline;
} O62MCpColdV1;
static inline void o62m_cp_cold_cancel_v1(O62MCpColdV1*s)
{
 s->generation++;s->phase=0;s->deadline=0;s->on=s->available=false;s->stage=0;
 memset(s->pending_config,0,sizeof s->pending_config);memset(s->pending_timing,0,sizeof s->pending_timing);
}
static inline void o62m_cp_cold_reset_v1(O62MCpColdV1*s)
{uint64_t gen=s->generation+1;memset(s,0,sizeof *s);s->generation=gen;}
static inline bool o62m_cp_cold_entry_v1(O62MCpColdV1*s,uint32_t value)
{
 if(s->stage||s->phase||s->release_accepted||(value&3)||value<UINT32_C(0x64000000)||value>=UINT32_C(0x64d00000))return false;
 s->entry=value;s->stage=1;return true;
}
static inline bool o62m_cp_cold_reason_v1(O62MCpColdV1*s,uint32_t value)
{if(s->stage!=1||value!=UINT32_C(0x636f6c64))return false;s->stage=2;return true;}
static inline bool o62m_cp_cold_on_v1(O62MCpColdV1*s,uint32_t value,const uint32_t timing[3],bool configured)
{
 if(s->stage!=2||value!=1||!configured)return false;
 memcpy(s->pending_timing,timing,sizeof s->pending_timing);s->stage=3;s->on=s->available=false;return true;
}
static inline bool o62m_cp_cold_clock_note_v1(O62MCpColdV1*s,unsigned step,uint32_t value)
{if(value!=1||step<4||step>5||s->stage!=step-1)return false;s->stage=step;return true;}
static inline bool o62m_cp_cold_submit_v1(O62MCpColdV1*s,uint32_t value,const uint32_t config[5],int64_t now)
{
 if(s->stage!=5||s->phase||value!=1||now<0||now>INT64_MAX-1000)return false;
 memcpy(s->pending_config,config,sizeof s->pending_config);s->stage=6;s->phase=1;
 s->request_control=value;s->deadline=now+1000;s->generation++;s->requests++;return true;
}
static inline bool o62m_cp_cold_source_v3(const O62MCpColdV1*s,const uint32_t config[5],const uint32_t timing[3],bool configured)
{
 const uint32_t *c=s->phase?s->pending_config:s->active_config,*t=s->phase?s->pending_timing:s->active_timing;
 if(s->release_accepted&&!s->phase) {
  if(!s->on||!s->available)return false;
  static const uint32_t runtime_set_mask[5]={0x1d2,0,0x8000,0,3};
  for(unsigned i=0;i<5;i++)
   if(((config[i]^c[i])&~runtime_set_mask[i])||(c[i]&~config[i]))return false;
  return configured&&!memcmp(t,timing,3*sizeof(uint32_t));
 }
 return configured&&!memcmp(c,config,5*sizeof(uint32_t))&&!memcmp(t,timing,3*sizeof(uint32_t));
}
static inline bool o62m_cp_cold_complete_v1(O62MCpColdV1*s,uint64_t generation,int64_t now)
{
 if(!s->phase||s->generation!=generation||now<s->deadline)return false;
 if(s->phase==1) {
  if(now>INT64_MAX-500)return false;
  memcpy(s->active_config,s->pending_config,sizeof s->active_config);
  memcpy(s->active_timing,s->pending_timing,sizeof s->active_timing);
  s->config_commits++;s->phase=2;s->deadline=now+500;return true;
 }
 s->phase=0;s->deadline=0;s->on=s->available=true;s->transitions++;
 memset(s->pending_config,0,sizeof s->pending_config);memset(s->pending_timing,0,sizeof s->pending_timing);return true;
}
static inline bool o62m_cp_cold_clock_v1(O62MCpColdV1*s,uint32_t value)
{if(s->stage!=6||s->phase||!s->on||!s->available||value!=(s->clock_control|7u))return false;s->clock_control=value;s->stage=7;return true;}
static inline bool o62m_cp_cold_release_v1(O62MCpColdV1*s,uint32_t old,uint32_t value)
{if(s->stage!=7||s->phase||!s->on||!s->available||s->release_accepted||value!=(old|0x80u))return false;s->release_accepted=true;s->stage=8;return true;}
#endif
