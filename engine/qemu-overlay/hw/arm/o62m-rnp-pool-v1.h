/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef O62M_RNP_POOL_V1_H
#define O62M_RNP_POOL_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
typedef struct {
    uint8_t pool[32];
    uint32_t control;
    uint64_t generation, requests, completions, cancelled, reads;
    int64_t deadline;
    bool pending, done, valid;
} O62MRnpPoolV1;
static inline void o62m_rnp_wipe_v1(void *p, size_t n)
{
    volatile uint8_t *b=p;
    while(n--)*b++=0;
}
static inline void o62m_rnp_cancel_v1(O62MRnpPoolV1 *m)
{
    if(m->pending)m->cancelled++;
    m->generation++;
    m->pending=m->done=m->valid=false;
    m->control=0;m->deadline=0;
    o62m_rnp_wipe_v1(m->pool,sizeof(m->pool));
}
static inline bool o62m_rnp_control_v1(O62MRnpPoolV1 *m,uint32_t v,int64_t now)
{
    if((v!=0&&v!=2&&v!=3)||now<0||m->generation==UINT64_MAX)return false;
    if(v==3) {
        if(m->pending||now>INT64_MAX-3000||m->requests==UINT64_MAX)return false;
        o62m_rnp_wipe_v1(m->pool,sizeof(m->pool));
        m->generation++;m->requests++;m->control=v;
        m->pending=true;m->done=m->valid=false;m->deadline=now+3000;
    } else {
        o62m_rnp_cancel_v1(m);m->control=v;
    }
    return true;
}
static inline bool o62m_rnp_complete_v1(O62MRnpPoolV1 *m,uint64_t generation,
                                        int64_t now,const uint8_t source[32],bool ok)
{
    if(!m->pending||generation!=m->generation||now<m->deadline||
       m->control!=3||!source||!ok||m->completions==UINT64_MAX)return false;
    memcpy(m->pool,source,sizeof(m->pool));
    m->pending=false;m->valid=m->done=true;m->completions++;
    return true;
}
static inline bool o62m_rnp_access_v1(O62MRnpPoolV1 *m,uint32_t offset,
                                     bool write,uint32_t *value,int64_t now)
{
    if(!m||!value||(offset&3))return false;
    if(offset==0) {
        if(write)return o62m_rnp_control_v1(m,*value,now);
        *value=m->control;return true;
    }
    if(offset==4&&!write){*value=m->pending?4:0;return true;}
    if(offset==8) {
        if(write){if(*value>1)return false;if(*value&1)m->done=false;}
        else *value=m->done?1:0;
        return true;
    }
    if(!write&&offset>=0x10&&offset<=0x2c&&m->valid&&m->reads!=UINT64_MAX) {
        unsigned i=offset-0x10;
        *value=((uint32_t)m->pool[i]<<24)|((uint32_t)m->pool[i+1]<<16)|
               ((uint32_t)m->pool[i+2]<<8)|m->pool[i+3];
        m->reads++;return true;
    }
    return false;
}
#endif
