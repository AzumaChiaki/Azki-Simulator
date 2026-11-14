/* Explicit virtual PM configuration policy; NOT recovered O62M silicon.
 * Original AP proves address/argument geometry, not set/clear hardware meaning.
 * No pending/status/clock/CPU wake/IRQ is synthesized by this helper.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef O62M_PM_FUNCTIONAL_V3_H
#define O62M_PM_FUNCTIONAL_V3_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct O62MPMFunctionalV1 {
    uint32_t config[4][5]; /* virtual raw14 and raw20/24/28/2c */
    uint32_t domain_request[4], source_commit_request[4];
    uint32_t config_6c;
    uint64_t accesses;
} O62MPMFunctionalV1;
static inline bool o62m_pm_functional_access_v1(O62MPMFunctionalV1 *s,
    uint64_t address,unsigned size,bool write,uint64_t *value)
{
    static const uint32_t base[4]={0,0x98,0x11c,0x190};
    static const uint32_t raw[5]={0x14,0x20,0x24,0x28,0x2c};
    static const uint32_t commit[4]={0x94,0x118,0x18c,0x20c};
    if(!s||!value||size!=4||(address&3)||address<UINT64_C(0x4c030000)||
       address>=UINT64_C(0x4c030800)||(write&&*value>UINT32_MAX)||
       s->accesses==UINT64_MAX)return false;
    uint32_t off=(uint32_t)(address-UINT64_C(0x4c030000));
    if(off==0x36c||off==0x66c) {
        if(!write||*value!=0x30)return false;
        if(off==0x36c)s->config_6c|=(uint32_t)*value;
        else s->config_6c&=~(uint32_t)*value;
        ++s->accesses;return true;
    }
    for(unsigned d=0;d<4;d++) {
        for(unsigned r=0;r<5;r++) {
            uint32_t origin=base[d]+raw[r];
            if(off==origin||off==origin+0x300||off==origin+0x600) {
                /* Alias reads are unproved, and deliberately strict. */
                if(!write&&off!=origin)return false;
                if(write) {
                    if(off==origin)s->config[d][r]=(uint32_t)*value;
                    else if(off==origin+0x300)s->config[d][r]|=(uint32_t)*value;
                    else s->config[d][r]&=~(uint32_t)*value;
                } else *value=s->config[d][r];
                s->accesses++;return true;
            }
        }
        if(off==base[d]+4||off==commit[d]) {
            /* Exact original startup command=1 is retained as a request.
             * It is not an acknowledgement/completion or a wake event. */
            if(!write||*value!=1)return false;
            if(off==base[d]+4)s->domain_request[d]=1;
            else s->source_commit_request[d]=1;
            s->accesses++;return true;
        }
    }
    return false;
}
#endif
