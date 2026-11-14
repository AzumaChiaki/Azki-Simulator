#ifndef O62M_TE_PLATFORM_FATAL_V1_H
#define O62M_TE_PLATFORM_FATAL_V1_H
#include <stdbool.h>
#include <stdint.h>
/* One original attempt, with an explicit external fatal initial condition.
 * This diagnostic never supplies a pass/ready/entropy value. Any retry fails.
 * It is not a normal TrustEngine implementation and establishes no boot pass. */
typedef struct O62MTePlatformFatal {uint32_t initial_control,control,host_state;unsigned phase;bool initialized;} O62MTePlatformFatal;
static inline bool o62m_te_platform_fatal_init(O62MTePlatformFatal *m,uint32_t control,uint32_t host) {
 if(!m||(control&1u)||!(host&0x20u)) return false;
 O62MTePlatformFatal n={control,control,host,0,true};*m=n;return true;
}
static inline bool o62m_te_platform_fatal_access(O62MTePlatformFatal *m,uint64_t address,
 unsigned size,bool write,uint64_t pc,uint64_t *value) {
 if(!m||!value||!m->initialized||size!=4||(write&&*value>UINT32_MAX))return false;
 if(m->phase==0&&!write&&address==0x50333708&&pc==0x73e5ed12) {
  *value=m->control;m->phase=1;return true;
 }
 if(m->phase==1&&write&&address==0x50333708&&pc==0x73e5ed1e&&*value==(m->initial_control|1u)) {
  m->control=(uint32_t)*value;m->phase=2;return true;
 }
 if(m->phase==2&&!write&&address==0x50330020&&pc==0x73e5ed24) {
  *value=m->host_state;m->phase=3;return true;
 }
 return false;
}
#endif
