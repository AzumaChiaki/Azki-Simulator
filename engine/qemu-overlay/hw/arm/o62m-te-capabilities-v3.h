#ifndef O62M_TE_CAPABILITIES_V3_H
#define O62M_TE_CAPABILITIES_V3_H
#include "o62m-te-context-v1.h"
/* Immutable explicit virtual RTL configuration, not physical reset data.
 * The canonical COMMON owner supplies hash/sca host counts and CTX_SRAM.
 * All words are raw: the unchanged guest parses its own bitfield ABI.
 * These fields never complete crypto, DMA, OTP shadowing, reset or IRQ. */
#define O62M_TE_CAPS_MAGIC_V3 UINT32_C(0x54435033)
#define O62M_TE_CAPS_WORDS_V3 8u
typedef struct O62MTeCapabilitiesV3 {
 uint32_t magic;
 uint32_t raw[O62M_TE_CAPS_WORDS_V3];
 const O62MTeContext *owner;
} O62MTeCapabilitiesV3;
static inline bool o62m_te_caps_overlap_v3(const void *a,size_t na,const void *b,size_t nb)
{
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 return x<=y?y-x<na:x-y<nb;
}
static inline O62MTeContextResult o62m_te_caps_init_v3(
 O62MTeCapabilitiesV3 *v,const O62MTeContext *c,const uint64_t *raw,size_t count)
{
 if(!v||!c||!raw||count!=O62M_TE_CAPS_WORDS_V3) {return O62M_TE_CONTEXT_BAD_ARGUMENT;}
 if(o62m_te_caps_overlap_v3(v,sizeof(*v),c,sizeof(*c))||
    o62m_te_caps_overlap_v3(v,sizeof(*v),raw,count*sizeof(*raw))) {
  return O62M_TE_CONTEXT_BAD_ARGUMENT;
 }
 if(!o62m_te_context_valid(c)) {return O62M_TE_CONTEXT_BAD_INITIAL;}
 for(size_t i=0;i<count;i++) {if(raw[i]>UINT32_MAX) {return O62M_TE_CONTEXT_BAD_INITIAL;}}
 if((raw[0]&UINT64_C(0x3ff))!=((uint64_t)c->hash_nhost|((uint64_t)c->sca_nhost<<5))) {
  return O62M_TE_CONTEXT_BAD_INITIAL;
 }
 O62MTeCapabilitiesV3 next={0};next.magic=O62M_TE_CAPS_MAGIC_V3;next.owner=c;
 for(size_t i=0;i<count;i++) {next.raw[i]=(uint32_t)raw[i];}
 /* No second geometry owner; low10 are always projected from COMMON. */
 next.raw[0]&=~UINT32_C(0x3ff);*v=next;return O62M_TE_CONTEXT_OK;
}
static inline O62MTeContextResult o62m_te_caps_access_v3(
 const O62MTeCapabilitiesV3 *v,const O62MTeContext *c,
 uint64_t address,unsigned size,bool write,uint64_t *value)
{
 static const uint64_t addresses[O62M_TE_CAPS_WORDS_V3]={
  UINT64_C(0x50330008),UINT64_C(0x5033000c),UINT64_C(0x50330010),
  UINT64_C(0x503300d0),UINT64_C(0x503300d4),UINT64_C(0x503300d8),UINT64_C(0x503300dc),UINT64_C(0x50330004)};
 if(!v||!c||!value) {return O62M_TE_CONTEXT_BAD_ARGUMENT;}
 if(o62m_te_caps_overlap_v3(value,sizeof(*value),v,sizeof(*v))||
    o62m_te_caps_overlap_v3(value,sizeof(*value),c,sizeof(*c))) {return O62M_TE_CONTEXT_BAD_ARGUMENT;}
 if(v->magic!=O62M_TE_CAPS_MAGIC_V3||v->owner!=c||!o62m_te_context_valid(c)||
    (v->raw[0]&UINT32_C(0x3ff))) {return O62M_TE_CONTEXT_UNINITIALIZED;}
 if(size!=4||(address&3)) {return O62M_TE_CONTEXT_BAD_ACCESS;}
 if(write) {return O62M_TE_CONTEXT_UNSUPPORTED_TRANSITION;}
 uint32_t result;
 if(address>=UINT64_C(0x50330024)&&address<=UINT64_C(0x5033009c)&&
    !((address-UINT64_C(0x50330024))&7u)) {
  result=c->host_config[(address-UINT64_C(0x50330024))/8u];
 }
 else if(address==UINT64_C(0x50330018)) {result=c->ctx_sram_word;}
 else {
  size_t i;
  for(i=0;i<O62M_TE_CAPS_WORDS_V3&&addresses[i]!=address;i++) {}
  if(i==O62M_TE_CAPS_WORDS_V3) {return O62M_TE_CONTEXT_UNSUPPORTED_REGISTER;}
  result=v->raw[i];
  if(!i) {result|=c->hash_nhost|(c->sca_nhost<<5);}
 }
 *value=result;return O62M_TE_CONTEXT_OK;
}
#endif
