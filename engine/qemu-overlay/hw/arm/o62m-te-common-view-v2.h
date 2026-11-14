#ifndef O62M_TE_COMMON_VIEW_V2_H
#define O62M_TE_COMMON_VIEW_V2_H
#include "o62m-te-context-v1.h"
#include "o62m-virtual-ao-v1.h"
/* A per-bit view, not a second TOP/STAT seed. The frozen capacity engine owns
 * lock/ready. This view owns only the actual host0 software-init bit. AO-valid
 * is derived from the same real private store, never cached in another word.
 * Caller serializes all owners and keeps their addresses/instance fixed. */
#define O62M_TE_COMMON_VIEW_MAGIC_V1 UINT32_C(0x54435631)
#define O62M_TE_COMMON_SW_TOP_V1 UINT32_C(2)
#define O62M_TE_COMMON_SW_STAT_V1 UINT32_C(0x40)
#define O62M_TE_COMMON_AO_STAT_V1 UINT32_C(0x800000)
typedef struct O62MTeCommonViewV1 {
 uint32_t magic,sw_init_done;
 uint64_t ao_instance_id;
 const O62MTeContext *context_owner;
} O62MTeCommonViewV1;
static inline O62MTeContextResult o62m_te_common_view_init_v1(
 O62MTeCommonViewV1 *v,const O62MTeContext *c,const O62MVirtualAoV1 *a)
{
 if(!v||!c||!a) {return O62M_TE_CONTEXT_BAD_ARGUMENT;}
 if(o62m_virtual_ao_overlap_v1(v,sizeof(*v),c,sizeof(*c))||
    o62m_virtual_ao_overlap_v1(v,sizeof(*v),a,sizeof(*a))) {return O62M_TE_CONTEXT_BAD_ARGUMENT;}
 O62MVirtualAoMetadataV1 m;
 if((c->top&O62M_TE_COMMON_SW_TOP_V1)||(c->initial_top&O62M_TE_COMMON_SW_TOP_V1)||
    !o62m_te_context_valid(c)||o62m_virtual_ao_query_v1(a,&m)||!a->enabled||
    m.phase!=O62M_VAO_EMPTY_V1||m.material_present||
    (c->initial_stat&(O62M_TE_COMMON_SW_STAT_V1|O62M_TE_COMMON_AO_STAT_V1))) {
  return O62M_TE_CONTEXT_BAD_INITIAL;
 }
 O62MTeCommonViewV1 next={O62M_TE_COMMON_VIEW_MAGIC_V1,0,m.instance_id,c};
 *v=next;return O62M_TE_CONTEXT_OK;
}
static inline O62MTeContextResult o62m_te_common_view_access_v1(
 O62MTeCommonViewV1 *v,O62MTeContext *c,const O62MVirtualAoV1 *a,
 uint64_t address,unsigned size,bool write,uint64_t *value,int64_t now_ns)
{
 if(!v||!c||!a||!value) {return O62M_TE_CONTEXT_BAD_ARGUMENT;}
 if(o62m_virtual_ao_overlap_v1(value,sizeof(*value),v,sizeof(*v))||
    o62m_virtual_ao_overlap_v1(value,sizeof(*value),c,sizeof(*c))||
    o62m_virtual_ao_overlap_v1(value,sizeof(*value),a,sizeof(*a))||
    o62m_virtual_ao_overlap_v1(v,sizeof(*v),c,sizeof(*c))||
    o62m_virtual_ao_overlap_v1(v,sizeof(*v),a,sizeof(*a))||
    o62m_virtual_ao_overlap_v1(c,sizeof(*c),a,sizeof(*a))) {return O62M_TE_CONTEXT_BAD_ARGUMENT;}
 O62MVirtualAoMetadataV1 m;
 if(v->magic!=O62M_TE_COMMON_VIEW_MAGIC_V1||v->sw_init_done>1||v->context_owner!=c||
    (c->top&O62M_TE_COMMON_SW_TOP_V1)||(c->initial_top&O62M_TE_COMMON_SW_TOP_V1)||
    !o62m_te_context_valid(c)||o62m_virtual_ao_query_v1(a,&m)||!a->enabled||
    m.instance_id!=v->ao_instance_id||
    (c->initial_stat&(O62M_TE_COMMON_SW_STAT_V1|O62M_TE_COMMON_AO_STAT_V1))) {
  return O62M_TE_CONTEXT_UNINITIALIZED;
 }
 if(now_ns<a->last_mutation_ns) {return O62M_TE_CONTEXT_TIME_REVERSED;}
 if(size!=4||(address&3)||(write&&*value>UINT32_MAX)) {return O62M_TE_CONTEXT_BAD_ACCESS;}
 bool key_present=m.phase==O62M_VAO_VALID_V1&&m.material_present;
 if(v->sw_init_done&&!key_present) {return O62M_TE_CONTEXT_UNINITIALIZED;}
 O62MTeContext trial=*c;uint64_t data=write?*value:0;bool mark=false;
 if(write&&address==O62M_TE_CONTEXT_TOP) {
  if(v->sw_init_done&&!(data&O62M_TE_COMMON_SW_TOP_V1)) {return O62M_TE_CONTEXT_UNSUPPORTED_TRANSITION;}
  if(data&O62M_TE_COMMON_SW_TOP_V1) {
   if(!key_present||c->phase!=O62M_TE_CONTEXT_READY_PHASE||
      data!=((uint64_t)c->top|O62M_TE_COMMON_SW_TOP_V1)) {
    return O62M_TE_CONTEXT_UNSUPPORTED_TRANSITION;
   }
   mark=true;data=c->top;
  }
 }
 O62MTeContextResult result=o62m_te_context_access(&trial,address,size,write,&data,now_ns);
 if(result!=O62M_TE_CONTEXT_OK) {return result;}
 uint32_t done=v->sw_init_done||mark;
 if(!write) {
  if(address==O62M_TE_CONTEXT_TOP) {data|=done?O62M_TE_COMMON_SW_TOP_V1:0;}
  if(address==O62M_TE_CONTEXT_STAT) {
   data|=(done?O62M_TE_COMMON_SW_STAT_V1:0)|(key_present?O62M_TE_COMMON_AO_STAT_V1:0);
  }
 }
 *c=trial;v->sw_init_done=done;if(!write)*value=data;
 return O62M_TE_CONTEXT_OK;
}
#endif
