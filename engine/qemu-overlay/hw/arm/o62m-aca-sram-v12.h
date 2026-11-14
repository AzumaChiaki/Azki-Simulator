#ifndef O62M_ACA_SRAM_V12_H
#define O62M_ACA_SRAM_V12_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define O62M_ACA_SRAM_BYTES_V12 8192u
#define O62M_ACA_SRAM_WORDS_V12 (O62M_ACA_SRAM_BYTES_V12/4u)
typedef struct O62MAcaSramV12 {
 uint32_t word[O62M_ACA_SRAM_WORDS_V12];
 uint8_t written[O62M_ACA_SRAM_WORDS_V12/8u];
 uint32_t gr[32],length[16],selector,gr_configured;
 uint64_t config_reads,config_writes,status_reads;
 uint8_t fifo_occupancy,opcode;
 uint16_t operand_a,operand_b,operation_bits,operand_a_words,operand_b_words,result_first,modulus_first,modulus_words;
 uint64_t generation,deadline,requests,completions,writeback_words;
 bool running,completed,xor_zero,carry,add_zero,and_zero,save_result;
 uint32_t write_cursor,read_cursor;
 uint64_t address_writes,data_writes,data_reads;
 bool initialized,write_selected,read_selected;
} O62MAcaSramV12;
static inline void o62m_aca_sram_wipe_v12(O62MAcaSramV12 *s)
{ volatile uint8_t *p=(volatile uint8_t *)s;for(size_t i=0;i<sizeof(*s);i++)p[i]=0; }
static inline bool o62m_aca_sram_init_v12(O62MAcaSramV12 *s,uint64_t bytes)
{ if(!s||bytes!=O62M_ACA_SRAM_BYTES_V12)return false;o62m_aca_sram_wipe_v12(s);s->initialized=true;return true; }
/* SRAM words retain the actual u32 bus value. Each data transaction advances
 * its selected word cursor; it never wraps or reports arithmetic completion. */
static inline bool o62m_aca_sram_access_v12(O62MAcaSramV12 *s,unsigned offset,bool write,uint32_t *value)
{
 if(!s||!value||!s->initialized||(offset&3))return false;
 if(offset==0x14c) {
  if(write||s->fifo_occupancy>1)return false;
  *value=(1u-s->fifo_occupancy)|(s->fifo_occupancy?32u:16u)|(1u<<18)|
   (s->running?2u<<16:0)|(s->xor_zero?1u<<8:0)|(s->carry?1u<<9:0)|(s->add_zero?1u<<6:0)|(s->and_zero?1u<<7:0);
  s->status_reads++;return true;
 }
 if(offset==0x160) {
  if(write)return (*value&~UINT32_C(0x1df))==0;
  *value=s->completed&&!s->running&&!s->fifo_occupancy?32u:0u;return true;
 }
 if(write&&s->fifo_occupancy)return false;
 uint32_t *config=NULL,mask=0;
 if(offset<0x80){config=&s->gr[offset/4];mask=UINT32_C(0x007f0fff);}
 else if(offset>=0x100&&offset<0x140){config=&s->length[(offset-0x100)/4];mask=UINT32_C(0x3fff);}
 else if(offset==0x140){config=&s->selector;mask=UINT32_C(0xfffff);}
 if(config) {
  if(write) {
   if(*value&~mask)return false;
   if(offset<0x80) {
    unsigned first=(*value&0xfff)*4u,bytes=((*value>>16)&127u)*16u;
    if(first>=O62M_ACA_SRAM_BYTES_V12||bytes>O62M_ACA_SRAM_BYTES_V12-first)return false;
   }
   *config=*value;if(offset<0x80)s->gr_configured|=UINT32_C(1)<<(offset/4);s->config_writes++;
  } else {*value=*config;s->config_reads++;}
  return true;
 }
 if(offset==0x150||offset==0x158) {
  if(!write||*value>=O62M_ACA_SRAM_WORDS_V12)return false;
  if(offset==0x150){s->write_cursor=*value;s->write_selected=true;}
  else {s->read_cursor=*value;s->read_selected=true;}
  s->address_writes++;return true;
 }
 if(offset==0x154) {
  if(!write||!s->write_selected||s->write_cursor>=O62M_ACA_SRAM_WORDS_V12)return false;
  unsigned n=s->write_cursor;s->word[n]=*value;s->written[n/8]|=(uint8_t)(1u<<(n%8));
  s->write_cursor++;s->data_writes++;return true;
 }
 if(offset==0x15c) {
  if(write||!s->read_selected||s->read_cursor>=O62M_ACA_SRAM_WORDS_V12)return false;
  unsigned n=s->read_cursor;if(!(s->written[n/8]&(1u<<(n%8))))return false;
  *value=s->word[n];s->read_cursor++;s->data_reads++;return true;
 }
 return false;
}
/* The single-entry virtual engine accepts the observed no-save XOR/subtraction and immediate ADD/SUB/AND and logical SHR with writeback, plus GR division with quotient/remainder writeback.
 * SRAM/configuration writes remain blocked until this entry finishes or cancels. */
static inline bool o62m_aca_op_span_v12(const O62MAcaSramV12 *s,unsigned gr,unsigned bits,unsigned *first)
{
 uint32_t value=s->gr[gr];unsigned offset=value&0xfffu,bytes=((value>>16)&127u)*16u;
 unsigned words=(bits+31u)/32u;
 if(!bits||bits>2048u||offset>=O62M_ACA_SRAM_WORDS_V12||words>O62M_ACA_SRAM_WORDS_V12-offset||bytes<words*4u)return false;
 for(unsigned i=0;i<words;i++)if(!(s->written[(offset+i)/8]&(1u<<((offset+i)%8))))return false;
 *first=offset;return true;
}
#define O62M_ACA_MODEXP_WORDS_V12 65u
static inline void o62m_aca_words_wipe_v12(uint32_t *p,unsigned n)
{volatile uint32_t *v=p;for(unsigned i=0;i<n;i++)v[i]=0;}
static inline bool o62m_aca_ge_v12(const uint32_t *a,const uint32_t *b,unsigned n)
{for(unsigned i=n;i--;)if(a[i]!=b[i])return a[i]>b[i];return true;}
/* a,b are reduced residues. The carry above the limb range participates in
 * the comparison; one subtraction suffices because a+b < 2N. */
static inline void o62m_aca_addmod_v12(uint32_t *out,const uint32_t *a,const uint32_t *b,const uint32_t *mod,unsigned n)
{
 uint32_t carry=0;
 for(unsigned i=0;i<n;i++){uint64_t sum=(uint64_t)a[i]+b[i]+carry;out[i]=(uint32_t)sum;carry=(uint32_t)(sum>>32);}
 if(carry||o62m_aca_ge_v12(out,mod,n)) {
  uint32_t borrow=0;for(unsigned i=0;i<n;i++){uint64_t d=(uint64_t)out[i]-mod[i]-borrow;out[i]=(uint32_t)d;borrow=(uint32_t)(d>>63);}
 }
}
static inline unsigned o62m_aca_bitlen_v12(const uint32_t *a,unsigned n)
{while(n&&!a[n-1])n--;if(!n)return 0;uint32_t v=a[n-1];unsigned bits=0;while(v){bits++;v>>=1;}return (n-1)*32u+bits;}
static inline void o62m_aca_mulmod_v12(uint32_t *out,const uint32_t *a,const uint32_t *b,const uint32_t *mod,unsigned n)
{
 uint32_t product[65]={0},multiple[65]={0};memcpy(multiple,a,n*4u);unsigned bits=o62m_aca_bitlen_v12(b,n);
 for(unsigned bit=0;bit<bits;bit++) {
  if((b[bit/32]>>(bit%32))&1u)o62m_aca_addmod_v12(product,product,multiple,mod,n);
  if(bit+1<bits)o62m_aca_addmod_v12(multiple,multiple,multiple,mod,n);
 }
 memcpy(out,product,n*4u);o62m_aca_words_wipe_v12(product,65);o62m_aca_words_wipe_v12(multiple,65);
}
/* The driver's NP contract uses at most a 408-bit numerator. */
static inline void o62m_aca_np_v12(const uint32_t *mod,unsigned bits,uint32_t *expected)
{
 uint32_t divisor[13]={0},remainder[14]={0},quotient[13]={0};
 unsigned shift=bits>272u?bits-272u:0u,den_bits=bits-shift,power=den_bits+135u;
 for(unsigned bit=0;bit<den_bits;bit++)if((mod[(bit+shift)/32]>>((bit+shift)%32))&1u)divisor[bit/32]|=UINT32_C(1)<<(bit%32);
 bool lower=false;for(unsigned bit=0;bit<shift;bit++)lower|=((mod[bit/32]>>(bit%32))&1u)!=0;
 if(lower){uint32_t carry=1;for(unsigned i=0;i<13&&carry;i++){uint64_t v=(uint64_t)divisor[i]+carry;divisor[i]=(uint32_t)v;carry=(uint32_t)(v>>32);}}
 for(unsigned bit=power+1u;bit--;) {
  uint32_t carry=bit==power;
  for(unsigned i=0;i<14;i++){uint32_t next=remainder[i]>>31;remainder[i]=(remainder[i]<<1)|carry;carry=next;}
  if(remainder[13]||o62m_aca_ge_v12(remainder,divisor,13)) {
   uint32_t borrow=0;for(unsigned i=0;i<13;i++){uint64_t v=(uint64_t)remainder[i]-divisor[i]-borrow;remainder[i]=(uint32_t)v;borrow=(uint32_t)(v>>63);}remainder[13]-=borrow;
   quotient[bit/32]|=UINT32_C(1)<<(bit%32);
  }
 }
 memcpy(expected,quotient,8*4u);o62m_aca_words_wipe_v12(divisor,13);o62m_aca_words_wipe_v12(remainder,14);o62m_aca_words_wipe_v12(quotient,13);
}
static inline bool o62m_aca_modexp_input_v12(const O62MAcaSramV12 *s,unsigned id,unsigned words,unsigned *first,unsigned *read_words)
{
 if(!(s->gr_configured&(UINT32_C(1)<<id)))return false;
 unsigned start=s->gr[id]&0xfffu,n=((s->gr[id]>>16)&127u)*4u;
 if(!n||n>words)n=words;
 if(start>=O62M_ACA_SRAM_WORDS_V12||words>O62M_ACA_SRAM_WORDS_V12-start)return false;
 for(unsigned i=0;i<n;i++)if(!(s->written[(start+i)/8]&(1u<<((start+i)%8))))return false;
 *first=start;*read_words=n;return true;
}
static inline bool o62m_aca_modexp_entry_v12(O62MAcaSramV12 *s,uint32_t entry,unsigned bits)
{
 if(!bits||bits>2048u||(entry&((1u<<17)|(1u<<11))))return false;
 unsigned ids[6]={(entry>>18)&31u,(entry>>12)&31u,s->selector&31u,(entry>>6)&31u,(s->selector>>5)&31u,(s->selector>>10)&31u};
 unsigned starts[6],allocated=((bits+2u+127u)/128u)*4u,words=(bits+31u)/32u,aw,bw,nw;
 for(unsigned i=0;i<6;i++) {
  if(!(s->gr_configured&(UINT32_C(1)<<ids[i])))return false;
  starts[i]=s->gr[ids[i]]&0xfffu;unsigned size=i==4?8u:i==1?((bits+127u)/128u)*4u:allocated;
  if(starts[i]>=O62M_ACA_SRAM_WORDS_V12||size>O62M_ACA_SRAM_WORDS_V12-starts[i])return false;
 }
 if(!o62m_aca_modexp_input_v12(s,ids[0],(bits+2u+31u)/32u,&starts[0],&aw)||
    !o62m_aca_modexp_input_v12(s,ids[1],words,&starts[1],&bw)||
    !o62m_aca_modexp_input_v12(s,ids[2],(bits+2u+31u)/32u,&starts[2],&nw))return false;
 for(unsigned i=0;i<8;i++)if(!(s->written[(starts[4]+i)/8]&(1u<<((starts[4]+i)%8))))return false;
 uint32_t mod[65]={0},np[8]={0};for(unsigned i=0;i<nw;i++)mod[i]=s->word[starts[2]+i];
 bool valid=o62m_aca_bitlen_v12(mod,nw)==bits;
 if(valid){o62m_aca_np_v12(mod,bits,np);for(unsigned i=0;i<8;i++)valid&=np[i]==s->word[starts[4]+i];}
 if((bits&31u)&&bw==words)valid&=(s->word[starts[1]+words-1]>>(bits&31u))==0;
 unsigned a_limit=(bits+2u+31u)/32u;
 if(((bits+2u)&31u)&&aw==a_limit)valid&=(s->word[starts[0]+a_limit-1]>>((bits+2u)&31u))==0;
 o62m_aca_words_wipe_v12(mod,65);o62m_aca_words_wipe_v12(np,8);
 if(!valid)return false;
 s->operand_a=starts[0];s->operand_b=starts[1];s->result_first=starts[3];s->modulus_first=starts[2];
 s->operand_a_words=aw;s->operand_b_words=bw;s->modulus_words=nw;
 return true;
}
static inline void o62m_aca_modexp_v12(O62MAcaSramV12 *s)
{
 uint32_t mod[65]={0},base[65]={0},exponent[65]={0},result[65]={0},one[65]={0},a[65]={0};
 unsigned words=(s->operation_bits+31u)/32u,out_words=((s->operation_bits+2u+127u)/128u)*4u;
 for(unsigned i=0;i<s->modulus_words;i++)mod[i]=s->word[s->modulus_first+i];
 for(unsigned i=0;i<s->operand_a_words;i++)a[i]=s->word[s->operand_a+i];
 for(unsigned i=0;i<s->operand_b_words;i++)exponent[i]=s->word[s->operand_b+i];
 one[0]=1;if(o62m_aca_ge_v12(one,mod,words))one[0]=0;
 unsigned abits=o62m_aca_bitlen_v12(a,s->operand_a_words);
 for(unsigned bit=abits;bit--;) {o62m_aca_addmod_v12(base,base,base,mod,words);if((a[bit/32]>>(bit%32))&1u)o62m_aca_addmod_v12(base,base,one,mod,words);}
 memcpy(result,one,words*4u);unsigned ebits=o62m_aca_bitlen_v12(exponent,words);
 for(unsigned bit=0;bit<ebits;bit++) {
  if((exponent[bit/32]>>(bit%32))&1u)o62m_aca_mulmod_v12(result,result,base,mod,words);
  if(bit+1<ebits)o62m_aca_mulmod_v12(base,base,base,mod,words);
 }
 for(unsigned i=0;i<out_words;i++){unsigned r=s->result_first+i;s->word[r]=i<words?result[i]:0;s->written[r/8]|=(uint8_t)(1u<<(r%8));}
 s->writeback_words+=out_words;
 o62m_aca_words_wipe_v12(mod,65);o62m_aca_words_wipe_v12(base,65);o62m_aca_words_wipe_v12(exponent,65);
 o62m_aca_words_wipe_v12(result,65);o62m_aca_words_wipe_v12(one,65);o62m_aca_words_wipe_v12(a,65);
}
static inline bool o62m_aca_op_entry_v12(O62MAcaSramV12 *s,uint32_t entry)
{
 if(!s||!s->initialized||s->fifo_occupancy||s->running||(entry&1u))return false;
 unsigned opcode=entry>>27,bits=s->length[(entry>>23)&15u],a=0,b=0,a_words=0,r=0;
 if(opcode==21u) {
  if(!o62m_aca_modexp_entry_v12(s,entry,bits))return false;
  a=s->operand_a;b=s->operand_b;r=s->result_first;a_words=s->operand_a_words;
 } else if(opcode==4u) {
  if(!bits||bits>4096u||(entry&((1u<<17)|(1u<<11))))return false;
  unsigned ids[5]={(entry>>18)&31u,(entry>>12)&31u,(entry>>6)&31u,
                   (s->selector>>10)&31u,(s->selector>>15)&31u};
  unsigned starts[5],spans[5],words=(bits+31u)/32u,allocated=((bits+127u)/128u)*4u;
  for(unsigned i=0;i<5;i++) {
   if(!(s->gr_configured&(UINT32_C(1)<<ids[i])))return false;
   starts[i]=s->gr[ids[i]]&0xfffu;spans[i]=allocated;
   if(i==1) {spans[i]=((s->gr[ids[i]]>>16)&127u)*4u;if(!spans[i])spans[i]=allocated;}
   if(starts[i]>=O62M_ACA_SRAM_WORDS_V12||spans[i]>O62M_ACA_SRAM_WORDS_V12-starts[i])return false;
   for(unsigned j=0;j<i;j++)if(starts[i]<starts[j]+spans[j]&&starts[j]<starts[i]+spans[i])return false;
  }
  unsigned source_words[2];uint32_t divisor_nonzero=0;
  for(unsigned k=0;k<2;k++) {
   source_words[k]=((s->gr[ids[k]]>>16)&127u)*4u;
   if(!source_words[k]||source_words[k]>words)source_words[k]=words;
   for(unsigned i=0;i<source_words[k];i++) {
    unsigned n=starts[k]+i;if(!(s->written[n/8]&(1u<<(n%8))))return false;
    if(k==1){uint32_t v=s->word[n];if(i+1==words&&(bits&31u))v&=(UINT32_C(1)<<(bits&31u))-1u;divisor_nonzero|=v;}
   }
  }
  if(!divisor_nonzero)return false;
  a=starts[0];b=starts[1];r=starts[2];a_words=source_words[0];
  s->operand_b_words=source_words[1];
 } else if(opcode==8u||opcode==1u||opcode==5u||(opcode==2u&&(entry&(1u<<17))&&!(entry&(1u<<11)))) {
  if(!bits||bits>4096u||(opcode!=8u&&((entry&(1u<<11))||!(entry&(1u<<17)))))return false;
  unsigned aid=(entry>>18)&31u,rid=(entry>>6)&31u,words=(bits+31u)/32u;
  unsigned allocated_words=((bits+127u)/128u)*4u;
  if(!(s->gr_configured&(UINT32_C(1)<<aid))||!(s->gr_configured&(UINT32_C(1)<<rid)))return false;
  a=s->gr[aid]&0xfffu;r=s->gr[rid]&0xfffu;a_words=((s->gr[aid]>>16)&127u)*4u;
  if(!a_words)a_words=words;
  if(!a_words||a>=O62M_ACA_SRAM_WORDS_V12||a_words>O62M_ACA_SRAM_WORDS_V12-a||
     r>=O62M_ACA_SRAM_WORDS_V12||allocated_words>O62M_ACA_SRAM_WORDS_V12-r)return false;
  if(a_words>words)a_words=words;
  for(unsigned i=0;i<a_words;i++)if(!(s->written[(a+i)/8]&(1u<<((a+i)%8))))return false;
  b=opcode==8u?((entry>>11)&127u)+1u:(entry>>12)&31u;
 } else {
  if((opcode!=7u&&opcode!=2u)||!(entry&(1u<<11))||(entry&(1u<<17)))return false;
  if(!o62m_aca_op_span_v12(s,(entry>>18)&31u,bits,&a)||
     !o62m_aca_op_span_v12(s,(entry>>12)&31u,bits,&b))return false;
  a_words=(bits+31u)/32u;
 }
 s->operand_a=a;s->operand_b=b;s->operand_a_words=a_words;s->result_first=r;
 s->operation_bits=bits;s->opcode=opcode;s->save_result=opcode==8u||!(entry&(1u<<11));s->fifo_occupancy=1;
 s->completed=false;s->generation++;s->requests++;return true;
}
static inline bool o62m_aca_op_run_v12(O62MAcaSramV12 *s,uint64_t now)
{
 if(!s||s->fifo_occupancy!=1||s->running||now>INT64_MAX-1000u)return false;
 s->running=true;s->deadline=now+1000u;return true;
}
/* The functional divider uses private working storage. T0/T1 reserve real
 * SRAM ranges; their implementation-specific intermediate layout is not exposed. */
static inline void o62m_aca_divide_v12(O62MAcaSramV12 *s)
{
 unsigned words=(s->operation_bits+31u)/32u;
 uint32_t quotient[128]={0},remainder[129]={0},divisor[128]={0};
 for(unsigned i=0;i<s->operand_b_words;i++)divisor[i]=s->word[s->operand_b+i];
 if(s->operation_bits&31u)divisor[words-1]&=(UINT32_C(1)<<(s->operation_bits&31u))-1u;
 for(unsigned bit=s->operation_bits;bit--;) {
  uint32_t carry=bit/32u<s->operand_a_words?(s->word[s->operand_a+bit/32u]>>(bit%32u))&1u:0;
  for(unsigned i=0;i<=words;i++){uint32_t next=remainder[i]>>31;remainder[i]=(remainder[i]<<1)|carry;carry=next;}
  bool ge=remainder[words]!=0;
  if(!ge) {
   ge=true;for(unsigned i=words;i--;)if(remainder[i]!=divisor[i]){ge=remainder[i]>divisor[i];break;}
  }
  if(ge) {
   uint32_t borrow=0;
   for(unsigned i=0;i<words;i++){uint64_t diff=(uint64_t)remainder[i]-divisor[i]-borrow;remainder[i]=(uint32_t)diff;borrow=(uint32_t)(diff>>63);}
   remainder[words]-=borrow;quotient[bit/32u]|=UINT32_C(1)<<(bit%32u);
  }
 }
 for(unsigned i=0;i<words;i++) {
  unsigned a=s->operand_a+i,r=s->result_first+i;
  s->word[a]=remainder[i];s->word[r]=quotient[i];
  s->written[a/8]|=(uint8_t)(1u<<(a%8));s->written[r/8]|=(uint8_t)(1u<<(r%8));
 }
 s->writeback_words+=2u*words;
 volatile uint32_t *q=quotient,*r=remainder,*b=divisor;
 for(unsigned i=0;i<128;i++){q[i]=0;r[i]=0;b[i]=0;}r[128]=0;
}
static inline bool o62m_aca_op_complete_v12(O62MAcaSramV12 *s,uint64_t generation,uint64_t now)
{
 if(!s||!s->running||s->fifo_occupancy!=1||s->generation!=generation||now<s->deadline)return false;
 if(s->opcode==21u)o62m_aca_modexp_v12(s);
 else if(s->opcode==4u)o62m_aca_divide_v12(s);
 else {
 unsigned words=(s->operation_bits+31u)/32u;uint32_t different=0,carry=0,result[128]={0};
 for(unsigned i=0;i<words;i++) {
  if(s->opcode==8u) {
   unsigned source=i+s->operand_b/32u,shift=s->operand_b%32u;
   uint32_t lo=source<s->operand_a_words?s->word[s->operand_a+source]:0;
   uint32_t hi=source+1<s->operand_a_words?s->word[s->operand_a+source+1]:0;
   if(s->operation_bits&31u) {
    uint32_t mask=(UINT32_C(1)<<(s->operation_bits&31u))-1u;
    if(source+1==words)lo&=mask;if(source+2==words)hi&=mask;
   }
   result[i]=lo>>shift;if(shift)result[i]|=hi<<(32u-shift);
   if(i+1==words&&(s->operation_bits&31u))result[i]&=(UINT32_C(1)<<(s->operation_bits&31u))-1u;
   continue;
  }
  uint32_t a=i<s->operand_a_words?s->word[s->operand_a+i]:0;
  uint32_t b=s->save_result?(i==0?s->operand_b:0):s->word[s->operand_b+i];
  unsigned valid=i+1==words&&(s->operation_bits&31u)?s->operation_bits&31u:32u;
  uint32_t mask=valid==32u?UINT32_MAX:(UINT32_C(1)<<valid)-1u;a&=mask;if(!s->save_result)b&=mask;
  if(s->opcode==7u)different|=a^b;
  else if(s->opcode==5u){result[i]=a&b;different|=result[i];}
  else if(s->opcode==2u){uint64_t difference=(uint64_t)a-b-carry;result[i]=(uint32_t)difference&mask;carry=(uint32_t)(difference>>63);}
  else {uint64_t sum=(uint64_t)a+b+carry;result[i]=(uint32_t)sum&mask;carry=(uint32_t)(sum>>valid);different|=result[i];}
 }
 if(s->opcode==7u)s->xor_zero=different==0;
 else {
  if(s->opcode==1u||s->opcode==2u)s->carry=carry!=0;
  if(s->save_result) {
   for(unsigned i=0;i<words;i++) {
    unsigned n=s->result_first+i;s->word[n]=result[i];s->written[n/8]|=(uint8_t)(1u<<(n%8));
   }
   s->writeback_words+=words;if(s->opcode==1u)s->add_zero=different==0;
   if(s->opcode==5u)s->and_zero=different==0;
  }
 }
 volatile uint32_t *wipe=result;for(unsigned i=0;i<128;i++)wipe[i]=0;
 }
 s->completed=true;s->running=false;s->fifo_occupancy=0;s->save_result=false;
 s->operand_a=s->operand_b=s->operation_bits=s->operand_a_words=s->operand_b_words=s->result_first=s->modulus_first=s->modulus_words=0;s->opcode=0;s->deadline=0;s->completions++;return true;
}
static inline void o62m_aca_op_cancel_v12(O62MAcaSramV12 *s)
{
 if(!s)return;
 s->generation++;s->fifo_occupancy=0;s->running=s->completed=s->xor_zero=s->carry=s->add_zero=s->and_zero=s->save_result=false;
 s->operand_a=s->operand_b=s->operation_bits=s->operand_a_words=s->operand_b_words=s->result_first=s->modulus_first=s->modulus_words=0;s->opcode=0;s->deadline=0;
}

#endif
