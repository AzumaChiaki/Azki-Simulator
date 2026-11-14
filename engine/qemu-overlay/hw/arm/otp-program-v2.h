#ifndef O62M_OTP_PROGRAM_V2_H
#define O62M_OTP_PROGRAM_V2_H
#include "otp-read-v1.h"
/* Virtual board OTP: programming sets requested bits in the existing data
 * owner. Each accepted data store commits synchronously. This models the
 * original write-then-status ABI, not a measured silicon programming latency. */
typedef struct O62MOtpProgramV2 {
 uint32_t status,control;
 uint64_t commits,clears,control_reads,control_writes;
 bool result_valid;
} O62MOtpProgramV2;
static inline bool o62m_otp_program_access_v2(O62MOtpProgramV2 *p,O62MOtpRead *m,
 uint64_t address,unsigned size,bool write,uint64_t pc,uint64_t *value)
{
 if(!p||!m||!value||!m->initialized||size!=4||(address&3)||(write&&*value>UINT32_MAX))return false;
 if(address==UINT64_C(0x5a300060)) {
  if(write) {
   if(pc!=UINT64_C(0x73e61b96)||(*value&~UINT64_C(1)))return false;
   p->control=(uint32_t)*value;p->control_writes++;
  } else {
   if(pc!=UINT64_C(0x73e61b8a))return false;
   *value=p->control;p->control_reads++;
  }
  return true;
 }
 if(address==UINT64_C(0x5a30007c)) {
  if(!write) {if(pc!=UINT64_C(0x73e61ba8))return false;*value=0;return true;}
  if(pc!=UINT64_C(0x73e61bb4)||(*value&~UINT64_C(0x3f)))return false;
  p->status&=~(uint32_t)*value;p->clears++;return true;
 }
 if(address==UINT64_C(0x5a300068)) {
  if(write||pc!=UINT64_C(0x73e61ba0)||!p->result_valid)return false;
  *value=p->status;return true;
 }
 if(address==UINT64_C(0x5a300064)&&write&&*value==UINT64_C(0x4d474357)) {
  if(pc!=UINT64_C(0x73e61b9c))return false;
  m->key=(uint32_t)*value;m->unlocked=false;p->result_valid=false;return true;
 }
 if(address>=UINT64_C(0x5a301000)&&address<=UINT64_C(0x5a3011fc)&&write) {
  if(pc!=UINT64_C(0x73e619f0)||m->key!=UINT32_C(0x4d474357))return false;
  m->word[(address-UINT64_C(0x5a301000))/4]|=(uint32_t)*value;
  p->status=0;p->result_valid=true;p->commits++;return true;
 }
 return o62m_otp_read_access(m,address,size,write,pc,value);
}
#endif
