#ifndef O62M_HASH_STREAM_V1_H
#define O62M_HASH_STREAM_V1_H
#include "o62m-hash-backend-v1/o62m-hmac-ladder-v2.h"
#define O62M_HASH_STREAM_NODES_V1 128u
#define O62M_HASH_STREAM_BYTES_V1 UINT64_C(0x1000000)
typedef bool (*O62MHashStreamReadV1)(void *,uint64_t,uint8_t *,size_t,bool);
typedef struct O62MHashStreamPlanV1 {
 uint64_t address[O62M_HASH_STREAM_NODES_V1],length[O62M_HASH_STREAM_NODES_V1],total;
 unsigned count;
} O62MHashStreamPlanV1;
enum { O62M_HASH_STREAM_OK_V1=0,O62M_HASH_STREAM_LIST_V1,
 O62M_HASH_STREAM_ATTR_V1,O62M_HASH_STREAM_BUDGET_V1,
 O62M_HASH_STREAM_SOURCE_V1,O62M_HASH_STREAM_BLOCK_V1,O62M_HASH_STREAM_CRYPTO_V1 };
static inline uint64_t o62m_hash_stream_le64_v1(const uint8_t *p)
{ uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v; }
/* NULL output asks the same RAM reader to validate the complete span. */
static inline int o62m_hash_stream_plan_v1(uint64_t list,O62MHashStreamReadV1 read,
 void *opaque,O62MHashStreamPlanV1 *plan)
{
 if(!read||!plan)return O62M_HASH_STREAM_LIST_V1;
 memset(plan,0,sizeof(*plan));uint8_t raw[16]={0};int result=O62M_HASH_STREAM_LIST_V1;
 if(list&31)goto done;
 for(unsigned i=0;i<=O62M_HASH_STREAM_NODES_V1;i++) {
  if(list>UINT64_MAX-16u*i||!read(opaque,list+16u*i,raw,sizeof raw,true))goto done;
  uint64_t attr=o62m_hash_stream_le64_v1(raw),address=o62m_hash_stream_le64_v1(raw+8);
  o62m_sha256_zeroize_v1(raw,sizeof raw);
  if(attr==UINT64_MAX&&address==0) {
   result=plan->count&&!(plan->total&63)?O62M_HASH_STREAM_OK_V1:O62M_HASH_STREAM_BLOCK_V1;
   goto done;
  }
  if(attr>>32){result=O62M_HASH_STREAM_ATTR_V1;goto done;}
  uint64_t length=(uint64_t)(uint32_t)attr+1;
  if(i==O62M_HASH_STREAM_NODES_V1||length>O62M_HASH_STREAM_BYTES_V1-plan->total) {
   result=O62M_HASH_STREAM_BUDGET_V1;goto done;
  }
  if(address>UINT64_MAX-length||!read(opaque,address,NULL,(size_t)length,false)) {
   result=O62M_HASH_STREAM_SOURCE_V1;goto done;
  }
  plan->address[i]=address;plan->length[i]=length;plan->total+=length;plan->count++;
 }
done:
 o62m_sha256_zeroize_v1(raw,sizeof raw);
 if(result)o62m_sha256_zeroize_v1(plan,sizeof(*plan));return result;
}
static inline int o62m_hash_stream_update_v1(O62MHmacSha256V1 *context,
 const O62MHashStreamPlanV1 *plan,O62MHashStreamReadV1 read,void *opaque)
{
 if(!context||!context->active||!plan||!plan->count||plan->count>O62M_HASH_STREAM_NODES_V1||
    !plan->total||plan->total>O62M_HASH_STREAM_BYTES_V1||(plan->total&63)||!read)
  return O62M_HASH_STREAM_CRYPTO_V1;
 O62MHmacSha256V1 trial=*context;uint8_t scratch[4096]={0};int result=O62M_HASH_STREAM_OK_V1;
 for(unsigned i=0;i<plan->count&&!result;i++) {
  for(uint64_t done=0;done<plan->length[i];) {
   size_t take=plan->length[i]-done<sizeof scratch?(size_t)(plan->length[i]-done):sizeof scratch;
   if(!read(opaque,plan->address[i]+done,scratch,take,false))result=O62M_HASH_STREAM_SOURCE_V1;
   else if(!o62m_hmac_sha256_update_v1(&trial,scratch,take))result=O62M_HASH_STREAM_CRYPTO_V1;
   o62m_sha256_zeroize_v1(scratch,sizeof scratch);if(result)break;done+=take;
  }
 }
 if(!result)*context=trial;
 o62m_hmac_sha256_destroy_v1(&trial);o62m_sha256_zeroize_v1(scratch,sizeof scratch);return result;
}
#endif
