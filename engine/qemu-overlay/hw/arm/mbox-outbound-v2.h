#ifndef O62M_MBOX_OUTBOUND_V2_H
#define O62M_MBOX_OUTBOUND_V2_H
#include <stdbool.h>
#include <stdint.h>
#define O62M_MBOX_OUTBOUND_CAPACITY 4096u
/* Logical unconsumed outgoing request history. This is not a physical pending
 * register map or proof of delivery. No dequeue/peer/ACK/IRQ/status is provided.
 * The original API supports destinations0..4, events0..63 in10 command words.
 * Preserve each actual write; do not guess hardware coalescing semantics. */
typedef struct O62MMboxRequest {uint64_t now_ns;uint8_t destination,event;} O62MMboxRequest;
typedef struct O62MMboxOutbound {O62MMboxRequest request[O62M_MBOX_OUTBOUND_CAPACITY];unsigned count;} O62MMboxOutbound;
static inline bool o62m_mbox_outbound_notify(O62MMboxOutbound *m,uint64_t address,
 unsigned size,uint64_t value,uint64_t pc,uint64_t now_ns) {
 if(!m||size!=4||!value||value>UINT32_MAX||(value&(value-1))||pc!=0x73e579b8||
    m->count>=O62M_MBOX_OUTBOUND_CAPACITY||(m->count&&now_ns<m->request[m->count-1].now_ns))return false;
 for(unsigned d=0;d<5;d++)for(unsigned half=0;half<2;half++)
  if(address==UINT64_C(0x5c120010)+d*0x28u+half*4u) {
   unsigned bit=0;while((value>>bit)!=1u)bit++;
   O62MMboxRequest r={now_ns,d,bit+half*32};m->request[m->count++]=r;return true;
  }
 return false;
}
#endif
