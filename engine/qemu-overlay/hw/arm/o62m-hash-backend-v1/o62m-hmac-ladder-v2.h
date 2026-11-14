#ifndef O62M_HMAC_LADDER_V2_H
#define O62M_HMAC_LADDER_V2_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "crypto/aes.h"
#define MBEDTLS_CONFIG_FILE "o62m-mbed-config.h"
#include "vendor/mbedtls/sha256.h"
#include "vendor/mbedtls/platform_util.h"
/* SHA source remains an exact upstream copy; private symbol names come from
 * the minimal configuration. No platform/device/network/entropy calls. */
void o62m_sha256_zeroize_v1(void *p,size_t n)
{ volatile uint8_t *b=p;while(n--)*b++=0; }
#include "vendor/sha256-upstream.c"
typedef struct O62MHmacSha256V1 {
 O62MPrivateSha256V1 inner;
 uint8_t outer_pad[64];
 bool active;
} O62MHmacSha256V1;
static inline bool o62m_crypto_overlap_v1(const void *a,size_t na,const void *b,size_t nb)
{ uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<na:x-y<nb; }
/* Explicit virtual model assumption, not recovered O62M silicon truth:
 * k1=AES128-ECB-DEC(root16,ek1),k2=DEC(k1,ek2),k3=DEC(k2,ek3).
 * The OTP16 profile is mandatory: never truncate a physical/virtual32B root. */
static inline bool o62m_ladder_aes128_dec3_v1(const uint8_t *root,size_t root_bytes,
 const uint8_t *eks,size_t eks_bytes,uint8_t out[16])
{
 if(!root||!eks||!out||root_bytes!=16||eks_bytes!=48||
    o62m_crypto_overlap_v1(root,16,out,16)||o62m_crypto_overlap_v1(eks,48,out,16))return false;
 uint8_t current[16],next[16]={0};AES_KEY schedule;
 memcpy(current,root,sizeof current);memset(&schedule,0,sizeof schedule);bool ok=true;
 for(unsigned i=0;i<3;i++) {
  if(AES_set_decrypt_key(current,128,&schedule)) {ok=false;break;}
  AES_decrypt(eks+16*i,next,&schedule);
  o62m_sha256_zeroize_v1(&schedule,sizeof schedule);
  memcpy(current,next,sizeof current);o62m_sha256_zeroize_v1(next,sizeof next);
 }
 if(ok)memcpy(out,current,16);
 o62m_sha256_zeroize_v1(current,sizeof current);o62m_sha256_zeroize_v1(next,sizeof next);
 o62m_sha256_zeroize_v1(&schedule,sizeof schedule);return ok;
}
static inline void o62m_hmac_sha256_destroy_v1(O62MHmacSha256V1 *m)
{ if(m)o62m_sha256_zeroize_v1(m,sizeof(*m)); }
static inline bool o62m_hmac_sha256_init_v1(O62MHmacSha256V1 *m,const uint8_t *key,size_t bytes)
{
 if(!m||m->active||(!key&&bytes)||bytes>64||(key&&o62m_crypto_overlap_v1(m,sizeof(*m),key,bytes)))return false;
 O62MHmacSha256V1 next={0};uint8_t pad[64]={0};
 if(bytes)memcpy(pad,key,bytes);
 for(unsigned i=0;i<64;i++){next.outer_pad[i]=pad[i]^0x5c;pad[i]^=0x36;}
 o62m_sha256_init_v1(&next.inner);
 bool ok=!o62m_sha256_starts_v1(&next.inner,0)&&!o62m_sha256_update_v1(&next.inner,pad,sizeof pad);
 o62m_sha256_zeroize_v1(pad,sizeof pad);
 if(ok){next.active=true;*m=next;}
 o62m_sha256_zeroize_v1(&next,sizeof next);return ok;
}
static inline bool o62m_hmac_sha256_update_v1(O62MHmacSha256V1 *m,const uint8_t *data,size_t bytes)
{
 if(!m||!m->active||(!data&&bytes)||(data&&o62m_crypto_overlap_v1(m,sizeof(*m),data,bytes)))return false;
 return !o62m_sha256_update_v1(&m->inner,data,bytes);
}
static inline bool o62m_hmac_sha256_final_v1(const O62MHmacSha256V1 *m,uint8_t out[32])
{
 if(!m||!m->active||!out||o62m_crypto_overlap_v1(m,sizeof(*m),out,32))return false;
 O62MPrivateSha256V1 inner,outer;uint8_t digest[32]={0},result[32]={0};
 o62m_sha256_init_v1(&inner);o62m_sha256_init_v1(&outer);o62m_sha256_clone_v1(&inner,&m->inner);
 bool ok=!o62m_sha256_finish_v1(&inner,digest)&&!o62m_sha256_starts_v1(&outer,0)&&
         !o62m_sha256_update_v1(&outer,m->outer_pad,64)&&!o62m_sha256_update_v1(&outer,digest,32)&&
         !o62m_sha256_finish_v1(&outer,result);
 if(ok)memcpy(out,result,32);
 o62m_sha256_free_v1(&inner);o62m_sha256_free_v1(&outer);
 o62m_sha256_zeroize_v1(digest,sizeof digest);o62m_sha256_zeroize_v1(result,sizeof result);return ok;
}
static inline bool o62m_hmac_sha256_inner_final_v2(const O62MHmacSha256V1 *m,uint8_t out[32])
{
 if(!m||!m->active||!out||o62m_crypto_overlap_v1(m,sizeof(*m),out,32))return false;
 O62MPrivateSha256V1 copy;uint8_t result[32]={0};o62m_sha256_init_v1(&copy);
 o62m_sha256_clone_v1(&copy,&m->inner);bool ok=!o62m_sha256_finish_v1(&copy,result);
 if(ok)memcpy(out,result,32);o62m_sha256_free_v1(&copy);o62m_sha256_zeroize_v1(result,sizeof result);return ok;
}
static inline bool o62m_hmac_sha256_outer_final_v2(const O62MHmacSha256V1 *m,
 const uint8_t inner_digest[32],uint8_t out[32])
{
 if(!m||!m->active||!inner_digest||!out||o62m_crypto_overlap_v1(m,sizeof(*m),out,32))return false;
 O62MPrivateSha256V1 outer;uint8_t result[32]={0};o62m_sha256_init_v1(&outer);
 bool ok=!o62m_sha256_starts_v1(&outer,0)&&!o62m_sha256_update_v1(&outer,m->outer_pad,64)&&
         !o62m_sha256_update_v1(&outer,inner_digest,32)&&!o62m_sha256_finish_v1(&outer,result);
 if(ok)memcpy(out,result,32);o62m_sha256_free_v1(&outer);o62m_sha256_zeroize_v1(result,sizeof result);return ok;
}
#endif
