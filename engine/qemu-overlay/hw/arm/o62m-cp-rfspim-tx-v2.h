#ifndef O62M_CP_RFSPIM_TX_V2_H
#define O62M_CP_RFSPIM_TX_V2_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#define O62M_CP_RFSPIM_TX_CAPACITY_V2 32u
typedef struct {
 uint32_t word[O62M_CP_RFSPIM_TX_CAPACITY_V2];
 unsigned head,count;
 uint64_t enqueued,completed,cancelled,generation,status_reads;
 int64_t deadline;
 bool running;
} O62MCPRFSPIMTxV2;
static inline bool o62m_cp_rfspim_push_v2(O62MCPRFSPIMTxV2 *q,uint32_t word)
{
 if(!(word&UINT32_C(0x80000000))||q->count>=O62M_CP_RFSPIM_TX_CAPACITY_V2)return false;
 q->word[(q->head+q->count)%O62M_CP_RFSPIM_TX_CAPACITY_V2]=word;
 ++q->count;++q->enqueued;return true;
}
static inline uint32_t o62m_cp_rfspim_status_v2(const O62MCPRFSPIMTxV2 *q)
{
 return (q->count<O62M_CP_RFSPIM_TX_CAPACITY_V2?2u:0u)|(q->count==0?4u:0u)|(q->running?1u:0u);
}
static inline bool o62m_cp_rfspim_start_v2(O62MCPRFSPIMTxV2 *q,int64_t now,uint64_t frame_ns)
{
 if(q->running||!q->count||now<0||!frame_ns||frame_ns>(uint64_t)(INT64_MAX-now))return false;
 ++q->generation;q->deadline=now+(int64_t)frame_ns;q->running=true;return true;
}
static inline bool o62m_cp_rfspim_complete_v2(O62MCPRFSPIMTxV2 *q,int64_t now,uint64_t generation,uint32_t *frame)
{
 if(!q->running||!q->count||generation!=q->generation||now<q->deadline)return false;
 *frame=q->word[q->head];q->word[q->head]=0;
 q->head=(q->head+1)%O62M_CP_RFSPIM_TX_CAPACITY_V2;--q->count;++q->completed;
 q->running=false;q->deadline=0;return true;
}
static inline void o62m_cp_rfspim_cancel_v2(O62MCPRFSPIMTxV2 *q)
{
 ++q->generation;q->cancelled+=q->count;q->head=0;q->count=0;q->running=false;q->deadline=0;
 volatile uint32_t *w=q->word;for(unsigned i=0;i<O62M_CP_RFSPIM_TX_CAPACITY_V2;++i)w[i]=0;
}
static inline void o62m_cp_rfspim_reset_v2(O62MCPRFSPIMTxV2 *q)
{
 uint64_t generation=q->generation+1;
 volatile unsigned char *b=(volatile unsigned char *)q;for(size_t i=0;i<sizeof(*q);++i)b[i]=0;
 q->generation=generation;
}
#endif
