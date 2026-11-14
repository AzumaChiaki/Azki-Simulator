#ifndef O62M_VIRTUAL_OTP_V1_H
#define O62M_VIRTUAL_OTP_V1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Explicit virtual power-on image construction and typed AO shadow loading.
 * This is not measured silicon fuse/reset/timing behavior. Key material is
 * private; the adapter must supply actual OS randomness and wipe its staging.
 * No programming, PUF, generic control changes, secret MMIO or rekey protocol.
 * The caller serializes work and MMIO and keeps the instance identifier fixed. */
#define O62M_VOTP_MAGIC_V1 UINT32_C(0x4f545031)
#define O62M_VOTP_MAX_V1 2048u
#define O62M_VOTP_STAT_MASK_V1 UINT32_C(0x007f0080)
typedef enum O62MVirtualOtpResultV1 {
 O62M_VOTP_OK_V1=0,O62M_VOTP_ARGUMENT_V1,O62M_VOTP_INITIAL_V1,
 O62M_VOTP_STATE_V1,O62M_VOTP_ACCESS_V1,O62M_VOTP_REGISTER_V1,
 O62M_VOTP_TRANSITION_V1,O62M_VOTP_TIME_V1,O62M_VOTP_TOKEN_V1,
 O62M_VOTP_SOURCE_FAILED_V1,O62M_VOTP_SECRET_ABI_V1
} O62MVirtualOtpResultV1;
typedef enum O62MVirtualOtpPhaseV1 {
 O62M_VOTP_LOADING_V1=1,O62M_VOTP_VALID_V1,O62M_VOTP_FAILED_V1
} O62MVirtualOtpPhaseV1;
typedef struct O62MVirtualOtpSpecV1 {
 uint64_t instance_id;
 int64_t start_ns,work_ns;
 unsigned key_bytes,image_bytes;
 uint32_t setting_initial,update_opaque;
 const uint8_t *public_image;
 size_t public_bytes;
} O62MVirtualOtpSpecV1;
typedef struct O62MVirtualOtpJobV1 {
 uint64_t instance_id,serial;
 int64_t due_ns;
 size_t source_bytes;
} O62MVirtualOtpJobV1;
typedef struct O62MVirtualOtpV1 {
 uint32_t magic,setting_initial,control,update_opaque,load_error;
 uint64_t instance_id;
 int64_t start_ns,due_ns,last_mutation_ns;
 unsigned key_bytes,image_bytes,shadow_valid;
 O62MVirtualOtpPhaseV1 phase;
 bool backing_present;
 uint8_t public_image[O62M_VOTP_MAX_V1];
 uint8_t private_backing[O62M_VOTP_MAX_V1];
 uint8_t private_shadow[112];
} O62MVirtualOtpV1;
typedef struct O62MVirtualOtpMetadataV1 {
 uint64_t instance_id;
 int64_t due_ns,last_mutation_ns;
 unsigned key_bytes,image_bytes,shadow_valid;
 O62MVirtualOtpPhaseV1 phase;
 bool backing_present;
} O62MVirtualOtpMetadataV1;
static inline void o62m_virtual_otp_wipe_v1(void *p,size_t n)
{ volatile uint8_t *b=p;while(n--) {*b++=0;} }
static inline bool o62m_virtual_otp_zero_v1(const void *p,size_t n)
{ const uint8_t *b=p;uint8_t any=0;while(n--) {any|=*b++;}return !any; }
static inline bool o62m_virtual_otp_overlap_v1(const void *a,size_t na,const void *b,size_t nb)
{ uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<na:x-y<nb; }
static inline uint32_t o62m_virtual_otp_u32_v1(const uint8_t *b)
{ return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
static inline unsigned o62m_virtual_otp_prefix_v1(unsigned key_bytes)
{ return 48u+2u*key_bytes; }
static inline void o62m_virtual_otp_fields_v1(unsigned k,unsigned offset[6],unsigned size[6])
{
 offset[0]=0;size[0]=4;offset[1]=4;size[1]=k;
 offset[2]=4+k;size[2]=4;offset[3]=8+k;size[3]=k;
 offset[4]=40+2*k;size[4]=4;offset[5]=44+2*k;size[5]=4;
}
static inline bool o62m_virtual_otp_valid_v1(const O62MVirtualOtpV1 *m)
{
 return m&&m->magic==O62M_VOTP_MAGIC_V1&&m->instance_id&&
  (m->key_bytes==16||m->key_bytes==32)&&m->image_bytes>=o62m_virtual_otp_prefix_v1(m->key_bytes)&&
  m->image_bytes<=O62M_VOTP_MAX_V1&&m->start_ns>=0&&m->due_ns>m->start_ns&&
  m->last_mutation_ns>=m->start_ns&&!(m->setting_initial&UINT32_C(0xff))&&
  !(m->update_opaque&UINT32_C(0x1ff))&&!(m->control&~UINT32_C(0x1ff00))&&
  !(m->load_error&~UINT32_C(0x10))&&
  ((m->phase==O62M_VOTP_LOADING_V1&&!m->backing_present&&!m->shadow_valid&&!m->load_error)||
   (m->phase==O62M_VOTP_VALID_V1&&m->backing_present&&m->shadow_valid==63&&!m->load_error&&m->last_mutation_ns>=m->due_ns)||
   (m->phase==O62M_VOTP_FAILED_V1&&!m->backing_present&&!m->shadow_valid&&m->load_error==16&&m->last_mutation_ns>=m->due_ns));
}
static inline O62MVirtualOtpResultV1 o62m_virtual_otp_init_v1(O62MVirtualOtpV1 *m,const O62MVirtualOtpSpecV1 *s)
{
 if(!m||!s||!s->public_image) {return O62M_VOTP_ARGUMENT_V1;}
 if(o62m_virtual_otp_overlap_v1(m,sizeof(*m),s,sizeof(*s))||
    o62m_virtual_otp_overlap_v1(m,sizeof(*m),s->public_image,s->public_bytes)) {return O62M_VOTP_ARGUMENT_V1;}
 if(m->magic) {return O62M_VOTP_STATE_V1;}
 if(!s->instance_id||(s->key_bytes!=16&&s->key_bytes!=32)||s->start_ns<0||s->work_ns<=0||
    s->work_ns>INT64_MAX-s->start_ns||s->image_bytes<o62m_virtual_otp_prefix_v1(s->key_bytes)||
    s->image_bytes>O62M_VOTP_MAX_V1||s->public_bytes!=s->image_bytes||
    (s->setting_initial&UINT32_C(0xff))||(s->update_opaque&UINT32_C(0x1ff))) {return O62M_VOTP_INITIAL_V1;}
 unsigned off[6],len[6];o62m_virtual_otp_fields_v1(s->key_bytes,off,len);
 if(!o62m_virtual_otp_zero_v1(s->public_image+off[1],len[1])||
    !o62m_virtual_otp_zero_v1(s->public_image+off[3],len[3])) {return O62M_VOTP_INITIAL_V1;}
 uint32_t lcs=o62m_virtual_otp_u32_v1(s->public_image+off[4])&7u;
 if(lcs!=0&&lcs!=1&&lcs!=3&&lcs!=7) {return O62M_VOTP_INITIAL_V1;}
 O62MVirtualOtpV1 next={0};next.magic=O62M_VOTP_MAGIC_V1;next.instance_id=s->instance_id;
 next.setting_initial=s->setting_initial;next.control=s->setting_initial&UINT32_C(0x1ff00);
 next.update_opaque=s->update_opaque;next.key_bytes=s->key_bytes;next.image_bytes=s->image_bytes;
 next.start_ns=next.last_mutation_ns=s->start_ns;next.due_ns=s->start_ns+s->work_ns;next.phase=O62M_VOTP_LOADING_V1;
 memcpy(next.public_image,s->public_image,s->image_bytes);*m=next;return O62M_VOTP_OK_V1;
}
static inline O62MVirtualOtpResultV1 o62m_virtual_otp_job_v1(
 const O62MVirtualOtpV1 *m,int64_t now,O62MVirtualOtpJobV1 *job,bool *due)
{
 if(!m||!job||!due) {return O62M_VOTP_ARGUMENT_V1;}
 if(o62m_virtual_otp_overlap_v1(job,sizeof(*job),m,sizeof(*m))||
    o62m_virtual_otp_overlap_v1(due,sizeof(*due),m,sizeof(*m))||
    o62m_virtual_otp_overlap_v1(job,sizeof(*job),due,sizeof(*due))) {return O62M_VOTP_ARGUMENT_V1;}
 if(!o62m_virtual_otp_valid_v1(m)) {return O62M_VOTP_STATE_V1;}
 if(now<m->last_mutation_ns) {return O62M_VOTP_TIME_V1;}
 O62MVirtualOtpJobV1 next={m->instance_id,1,m->due_ns,2u*m->key_bytes};
 *job=next;*due=m->phase==O62M_VOTP_LOADING_V1&&now>=m->due_ns;return O62M_VOTP_OK_V1;
}
static inline O62MVirtualOtpResultV1 o62m_virtual_otp_complete_v1(
 O62MVirtualOtpV1 *m,int64_t now,const O62MVirtualOtpJobV1 *job,
 const uint8_t *keys,size_t bytes,bool source_ok)
{
 if(!m||!job||(source_ok&&!keys)) {return O62M_VOTP_ARGUMENT_V1;}
 if(o62m_virtual_otp_overlap_v1(m,sizeof(*m),job,sizeof(*job))||
    (keys&&o62m_virtual_otp_overlap_v1(m,sizeof(*m),keys,bytes))) {return O62M_VOTP_ARGUMENT_V1;}
 if(!o62m_virtual_otp_valid_v1(m)||m->phase!=O62M_VOTP_LOADING_V1) {return O62M_VOTP_STATE_V1;}
 if(now<m->last_mutation_ns||now<m->due_ns) {return O62M_VOTP_TIME_V1;}
 if(job->instance_id!=m->instance_id||job->serial!=1||job->due_ns!=m->due_ns||job->source_bytes!=2u*m->key_bytes) {return O62M_VOTP_TOKEN_V1;}
 if(source_ok&&bytes!=2u*m->key_bytes) {return O62M_VOTP_ARGUMENT_V1;}
 if(!source_ok) {
  o62m_virtual_otp_wipe_v1(m->private_backing,sizeof(m->private_backing));
  o62m_virtual_otp_wipe_v1(m->private_shadow,sizeof(m->private_shadow));
  m->phase=O62M_VOTP_FAILED_V1;m->load_error=16;m->last_mutation_ns=now;return O62M_VOTP_SOURCE_FAILED_V1;
 }
 unsigned off[6],len[6];o62m_virtual_otp_fields_v1(m->key_bytes,off,len);
 memcpy(m->private_backing,m->public_image,m->image_bytes);
 memcpy(m->private_backing+off[1],keys,m->key_bytes);
 memcpy(m->private_backing+off[3],keys+m->key_bytes,m->key_bytes);
 unsigned copied=0;
 for(unsigned i=0;i<6;i++) {
  memcpy(m->private_shadow+off[i],m->private_backing+off[i],len[i]);
  copied|=1u<<i; /* Each bit follows its actual independently addressed copy. */
 }
 m->shadow_valid=copied;m->backing_present=true;m->phase=O62M_VOTP_VALID_V1;m->last_mutation_ns=now;
 return O62M_VOTP_OK_V1;
}
static inline O62MVirtualOtpResultV1 o62m_virtual_otp_access_v1(
 O62MVirtualOtpV1 *m,int64_t now,uint64_t address,unsigned size,bool write,uint64_t *value)
{
 if(!m||!value) {return O62M_VOTP_ARGUMENT_V1;}
 if(o62m_virtual_otp_overlap_v1(m,sizeof(*m),value,sizeof(*value))) {return O62M_VOTP_ARGUMENT_V1;}
 if(!o62m_virtual_otp_valid_v1(m)) {return O62M_VOTP_STATE_V1;}
 if(now<m->last_mutation_ns) {return O62M_VOTP_TIME_V1;}
 if(size!=4||(address&3)||(write&&*value>UINT32_MAX)) {return O62M_VOTP_ACCESS_V1;}
 uint32_t setting=(m->setting_initial&~UINT32_C(0x1ff00))|m->control|m->load_error|
                  (m->phase==O62M_VOTP_VALID_V1?1u:0u);
 uint32_t update=m->update_opaque|(m->shadow_valid<<3)|(m->backing_present?4u:0u);
 if(address==UINT64_C(0x50333600)) {
  if(!write) {*value=setting;return O62M_VOTP_OK_V1;}
  /* Original direct-read RMW only; unknown generic wire effects stay strict. */
  if(((uint32_t)*value^setting)&~UINT32_C(0x100)) {return O62M_VOTP_TRANSITION_V1;}
  m->control=(uint32_t)*value&UINT32_C(0x1ff00);m->last_mutation_ns=now;return O62M_VOTP_OK_V1;
 }
 if(address==UINT64_C(0x50333610)) {
  if(write) {return O62M_VOTP_TRANSITION_V1;}*value=update;return O62M_VOTP_OK_V1;
 }
 if(address<UINT64_C(0x50331000)||address-UINT64_C(0x50331000)>m->image_bytes-4u) {return O62M_VOTP_REGISTER_V1;}
 if(write) {return O62M_VOTP_TRANSITION_V1;}
 unsigned offset=(unsigned)(address-UINT64_C(0x50331000));
 unsigned off[6],len[6];o62m_virtual_otp_fields_v1(m->key_bytes,off,len);
 if((offset>=off[1]&&offset<off[1]+len[1])||(offset>=off[3]&&offset<off[3]+len[3])) {return O62M_VOTP_SECRET_ABI_V1;}
 if(m->phase!=O62M_VOTP_VALID_V1) {return O62M_VOTP_STATE_V1;}
 if(m->control&UINT32_C(0x100)) {*value=o62m_virtual_otp_u32_v1(m->private_backing+offset);return O62M_VOTP_OK_V1;}
 for(unsigned i=0;i<6;i++) {
  if(offset>=off[i]&&offset+4u<=off[i]+len[i]&&(m->shadow_valid&(1u<<i))) {
   *value=o62m_virtual_otp_u32_v1(m->private_shadow+offset);return O62M_VOTP_OK_V1;
  }
 }
 return O62M_VOTP_TRANSITION_V1;
}
static inline O62MVirtualOtpResultV1 o62m_virtual_otp_query_v1(
 const O62MVirtualOtpV1 *m,O62MVirtualOtpMetadataV1 *out)
{
 if(!m||!out||o62m_virtual_otp_overlap_v1(m,sizeof(*m),out,sizeof(*out))) {return O62M_VOTP_ARGUMENT_V1;}
 if(!o62m_virtual_otp_valid_v1(m)) {return O62M_VOTP_STATE_V1;}
 O62MVirtualOtpMetadataV1 next={m->instance_id,m->due_ns,m->last_mutation_ns,m->key_bytes,m->image_bytes,m->shadow_valid,m->phase,m->backing_present};
 *out=next;return O62M_VOTP_OK_V1;
}
static inline O62MVirtualOtpResultV1 o62m_virtual_otp_global_v1(
 const O62MVirtualOtpV1 *m,int64_t now,uint32_t *out)
{
 if(!m||!out||o62m_virtual_otp_overlap_v1(m,sizeof(*m),out,sizeof(*out))) {return O62M_VOTP_ARGUMENT_V1;}
 if(!o62m_virtual_otp_valid_v1(m)) {return O62M_VOTP_STATE_V1;}
 if(now<m->last_mutation_ns) {return O62M_VOTP_TIME_V1;}
 *out=(m->shadow_valid<<16)|(m->backing_present?UINT32_C(0x80):0)|
      (m->phase==O62M_VOTP_VALID_V1?UINT32_C(0x400000):0);return O62M_VOTP_OK_V1;
}
static inline void o62m_virtual_otp_destroy_v1(O62MVirtualOtpV1 *m)
{ if(m) {o62m_virtual_otp_wipe_v1(m,sizeof(*m));} }
#endif
