#ifndef O62M_AP_TEE_MAILBOX_V2_H
#define O62M_AP_TEE_MAILBOX_V2_H
#include <stdbool.h>
#include <stdint.h>
/* Receiver0 is the AP low event bank; receiver1 is the TEE channel27 event.
 * Pending is a level notification; ring and service state belong to firmware. */
typedef struct O62MApTeeMailboxV1 {
    uint32_t pending[2];
    uint64_t delivered[2], coalesced[2], reads[2], clears[2], acknowledged[2], cancelled[2];
} O62MApTeeMailboxV1;
static inline uint32_t o62m_ap_tee_mbox_bit_v1(unsigned receiver)
{ return receiver ? UINT32_C(1) : UINT32_C(1)<<19; }
static inline bool o62m_ap_tee_mbox_deliver_mask_v2(O62MApTeeMailboxV1 *m,unsigned receiver,uint32_t bit)
{
    if(receiver>1||!bit||(bit&(bit-1))||(receiver&&bit!=1))return false;
    if(m->pending[receiver]&bit)++m->coalesced[receiver];
    m->pending[receiver]|=bit;++m->delivered[receiver];return true;
}
static inline void o62m_ap_tee_mbox_deliver_v1(O62MApTeeMailboxV1 *m,unsigned receiver)
{
    uint32_t bit=o62m_ap_tee_mbox_bit_v1(receiver);
    if(m->pending[receiver]&bit)++m->coalesced[receiver];
    m->pending[receiver]|=bit;++m->delivered[receiver];
}
static inline uint32_t o62m_ap_tee_mbox_read_v1(O62MApTeeMailboxV1 *m,unsigned receiver)
{ ++m->reads[receiver];return m->pending[receiver]; }
static inline bool o62m_ap_tee_mbox_clear_v1(O62MApTeeMailboxV1 *m,unsigned receiver,uint32_t value)
{
    uint32_t bit=o62m_ap_tee_mbox_bit_v1(receiver);
    if(receiver&&value&~bit)return false;
    ++m->clears[receiver];
    if(m->pending[receiver]&value)++m->acknowledged[receiver];
    m->pending[receiver]&=~value;return true;
}
static inline bool o62m_ap_tee_mbox_irq_v1(const O62MApTeeMailboxV1 *m,unsigned receiver,uint32_t enable)
{ return (m->pending[receiver]&enable)!=0; }
static inline void o62m_ap_tee_mbox_cancel_v1(O62MApTeeMailboxV1 *m)
{
    for(unsigned i=0;i<2;i++) {
        m->cancelled[i]+=m->pending[i]!=0;m->pending[i]=0;
    }
}
#endif
