#ifndef O62M_CP_CPLF_V3_H
#define O62M_CP_CPLF_V3_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct {
 uint32_t route[6];
 uint64_t claims,eois;
 uint8_t active;
 bool active_valid;
 bool input[3];
} O62MCPCplfV3;
static inline bool o62m_cp_cplf_route_v3(O62MCPCplfV3 *m,unsigned index,uint32_t word)
{
 if(index>=6||(index==5&&(word&~UINT32_C(0xff))))return false;
 for(unsigned bit=0;bit<32;bit+=2)if(((word>>bit)&3)>1)return false;
 m->route[index]=word;return true;
}
static inline unsigned o62m_cp_cplf_pending_v3(const O62MCPCplfV3 *m,bool enabled,
                                               const uint32_t priority[11])
{
 if(!enabled||m->active_valid)return UINT32_MAX;
 static const unsigned irqs[3]={44,45,0};
 unsigned selected=UINT32_MAX,best=8;
 for(unsigned i=0;i<3;i++) {
  unsigned irq=irqs[i];
  if(!m->input[i]||((m->route[irq/16]>>(2*(irq%16)))&3)!=1)continue;
  unsigned rank=(priority[irq/8]>>(3*(irq%8)))&7;
  if(rank<best||(rank==best&&irq<selected)){best=rank;selected=irq;}
 }
 return selected;
}
static inline uint32_t o62m_cp_cplf_claim_v3(O62MCPCplfV3 *m,bool enabled,
                                           const uint32_t priority[11])
{
 unsigned irq=o62m_cp_cplf_pending_v3(m,enabled,priority);
 if(irq==UINT32_MAX)return irq;
 m->active=irq;m->active_valid=true;++m->claims;return irq;
}
static inline bool o62m_cp_cplf_eoi_v3(O62MCPCplfV3 *m,unsigned bank,uint32_t word)
{
 if(!m->active_valid||bank!=m->active/32||word!=(UINT32_C(1)<<(m->active%32)))return false;
 m->active_valid=false;m->active=0;++m->eois;return true;
}
static inline void o62m_cp_cplf_reset_v3(O62MCPCplfV3 *m)
{memset(m,0,sizeof *m);}
#endif
