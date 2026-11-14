#ifndef O62M_AP_IPCMSG_SLOTS_V2_H
#define O62M_AP_IPCMSG_SLOTS_V2_H
#include <stdbool.h>
#include <stdint.h>
#include "o62m-virtual-otp-v1.h"
typedef struct O62MAPIPCSlotsV1 {
    uint32_t payload[32],staged,busy;
    uint64_t writes,commits,reads,cancelled,payload_reads,releases;
} O62MAPIPCSlotsV1;
static inline void o62m_ap_ipc_slots_reset_v2(O62MAPIPCSlotsV1 *s)
{
    uint32_t busy=s->busy;
    while(busy){s->cancelled++;busy&=busy-1;}
    o62m_virtual_otp_wipe_v1(s->payload,sizeof(s->payload));
    s->staged=s->busy=0;
}
static inline bool o62m_ap_ipc_slots_access_v2(O62MAPIPCSlotsV1 *s,
        uint64_t address,unsigned size,bool write,uint64_t *value,uint64_t pc)
{
    if(size!=4)return false;
    if(address==UINT64_C(0x4c1200c8)) {
        if(write||(pc!=0x60c2fd72&&pc!=0x641a97a0))return false;
        *value=s->busy;s->reads++;return true;
    }
    if(!write) {
        if(address<UINT64_C(0x4c1200dc)||address>UINT64_C(0x4c120158)||(address&3)||pc!=0x60c276f6)return false;
        unsigned slot=(address-0x4c1200dc)/4;uint32_t bit=UINT32_C(1)<<slot;
        if(!(s->busy&bit))return false;
        *value=s->payload[slot];s->payload_reads++;return true;
    }
    if(*value>UINT32_MAX)return false;
    if(address==UINT64_C(0x4c1200d8)) {
        uint32_t bit=*value;
        if(pc!=0x60c27700||!bit||(bit&(bit-1))||!(s->busy&bit))return false;
        unsigned slot=0;while((bit>>slot)!=1u)slot++;
        s->payload[slot]=0;s->busy&=~bit;s->staged&=~bit;s->releases++;return true;
    }
    if(address>=UINT64_C(0x4c1200dc)&&address<=UINT64_C(0x4c120158)&&!(address&3)) {
        unsigned slot=(address-0x4c1200dc)/4;uint32_t bit=UINT32_C(1)<<slot;
        if((pc!=0x60c2ffbc&&pc!=0x640d4380)||(s->busy&bit))return false;
        s->payload[slot]=*value;s->staged|=bit;s->writes++;return true;
    }
    if(address==UINT64_C(0x4c1200d0)) {
        uint32_t bit=*value;
        if((pc!=0x60c2ffc4&&pc!=0x640d438a)||!bit||(bit&(bit-1))||!(s->staged&bit)||(s->busy&bit))return false;
        s->busy|=bit;s->commits++;return true;
    }
    return false;
}
#endif
