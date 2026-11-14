#ifndef O62M_OTP_READ_V1_H
#define O62M_OTP_READ_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define O62M_OTP_INPUT_WORDS 128u
/* Explicit external data, not recovered silicon reset or an OTP dump. Only the
 * original read path is supported. Programming, status and calibration omitted.
 * No value is synthesized: read window returns the explicit corresponding word.
 * Every rejected transaction preserves state and the caller output. */
typedef struct O62MOtpRead { uint32_t key,word[128]; bool unlocked,initialized; } O62MOtpRead;
static inline bool o62m_otp_read_init(O62MOtpRead *m,uint32_t key,const uint32_t *words,unsigned count) {
 if(!m||!words||count!=128) return false;
 O62MOtpRead trial;memset(&trial,0,sizeof trial);trial.key=key;
 memcpy(trial.word,words,sizeof trial.word);trial.initialized=true;*m=trial;return true;
}
static inline bool o62m_otp_read_access(O62MOtpRead *m,uint64_t address,unsigned size,
 bool write,uint64_t pc,uint64_t *value) {
 if(!m||!value||!m->initialized||size!=4||(address&3)||(write&&*value>UINT32_MAX)) return false;
 if(address==0x5a300064) {
  if(write) {
   if(pc!=0x73e61b9c||(*value!=0x4d474352&&*value!=0)) return false;
   m->key=(uint32_t)*value;m->unlocked=m->key==0x4d474352;return true;
  }
  if(pc!=0x73e61b9a) return false;
  *value=m->key;return true;
 }
 if(address>=0x5a301000&&address<=0x5a3011fc&&!write&&pc==0x73e6188a&&m->unlocked) {
  *value=m->word[(address-0x5a301000)/4];return true;
 }
 return false;
}
#endif
