#ifndef O62M_AP_MBOX_OUTBOUND_V2_H
#define O62M_AP_MBOX_OUTBOUND_V2_H
#include "mbox-outbound-v2.h"
/* Separate AP outgoing history; no physical alias, delivery or consumption. */
static inline bool o62m_ap_mbox_outbound_notify_v1(O62MMboxOutbound *m,uint64_t address,
 unsigned size,uint64_t value,uint64_t pc,uint64_t now_ns) {
 if(!m||size!=4||!value||value>UINT32_MAX||(value&(value-1))||pc!=0x60c2ff9a||
    m->count>=O62M_MBOX_OUTBOUND_CAPACITY||(m->count&&now_ns<m->request[m->count-1].now_ns))return false;
 for(unsigned d=0;d<5;d++)for(unsigned half=0;half<2;half++)
  if(address==UINT64_C(0x4c120010)+d*0x28u+half*4u) {
   unsigned bit=0;while((value>>bit)!=1u)bit++;
   O62MMboxRequest request={now_ns,d,bit+half*32};m->request[m->count++]=request;return true;
  }
 return false;
}
#endif
