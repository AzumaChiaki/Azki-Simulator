#ifndef O62M_BBPLL_V1_H
#define O62M_BBPLL_V1_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define O62M_BBPLL_WORDS_V1 46u
#define O62M_BBPLL_CONFIG_MASK_V1 (((UINT64_C(1)<<43)-1)&~(UINT64_C(1)<<5))
typedef struct {
 uint32_t word[46],captured[43],active[43];
 uint64_t written,generation,requests,completions,cancelled,rate_hz;
 int64_t deadline;
 bool pending,locked;
} O62MBBPllV1;
static inline void o62m_bbpll_cancel_v1(O62MBBPllV1 *m)
{++m->generation;m->cancelled+=m->pending;m->pending=false;m->locked=false;m->rate_hz=0;}
static inline void o62m_bbpll_reset_v1(O62MBBPllV1 *m)
{uint64_t g=m->generation+1;memset(m,0,sizeof *m);m->generation=g;}
static inline bool o62m_bbpll_tuple_v1(const uint32_t *w)
{return (w[0]&63)==1&&((w[0]>>9)&7)==2&&((w[0]>>6)&7)==1&&
 (w[3]&4095)==37&&(w[4]&0xffffff)==0xcf29bf;}
static inline bool o62m_bbpll_write_v1(O62MBBPllV1 *m,unsigned i,uint32_t value,int64_t now,bool source)
{
 if(i>=46)return false;
 bool start=i==0&&(value&0x1000)&&!(m->word[0]&0x1000);
 if(start) {
  uint32_t check[43];memcpy(check,m->word,sizeof check);check[0]=value;
  if(!source||now<0||now>INT64_MAX-1000||
     (m->written&O62M_BBPLL_CONFIG_MASK_V1)!=O62M_BBPLL_CONFIG_MASK_V1||
     !o62m_bbpll_tuple_v1(check)||(check[8]&0x20000)||(check[9]&0x20000))return false;
 }
 bool config=i<43&&i!=5;
 if(config&&value!=m->word[i])o62m_bbpll_cancel_v1(m);
 m->word[i]=value;m->written|=UINT64_C(1)<<i;
 if(start){memcpy(m->captured,m->word,sizeof m->captured);m->pending=true;m->deadline=now+1000;++m->requests;}
 return true;
}
static inline bool o62m_bbpll_complete_v1(O62MBBPllV1 *m,uint64_t generation,int64_t now,bool source)
{
 if(!m->pending||m->generation!=generation||now<m->deadline||!source)return false;
 for(unsigned i=0;i<43;i++)if(i!=5&&m->word[i]!=m->captured[i])return false;
 if(!o62m_bbpll_tuple_v1(m->captured)||!(m->captured[0]&0x1000))return false;
 memcpy(m->active,m->captured,sizeof m->active);m->rate_hz=245760000;
 m->pending=false;m->locked=true;++m->completions;return true;
}
#endif
