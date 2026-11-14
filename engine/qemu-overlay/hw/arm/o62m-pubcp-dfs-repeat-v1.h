/* Original PUBCP retained reconfiguration after the initial boot sequence. */
#ifndef O62M_PUBCP_DFS_REPEAT_V1_H
#define O62M_PUBCP_DFS_REPEAT_V1_H
#include "dfs_config.h"
#include <stdint.h>
#include <stdbool.h>
typedef struct O62MPubcpDfsRepeatV1 {uint32_t value[8];unsigned step;bool read_seen;} O62MPubcpDfsRepeatV1;
static inline bool o62m_pubcp_dfs_repeat_v1(struct dfs_config_state *owner,
    O62MPubcpDfsRepeatV1 *r,bool write,uint32_t offset,uint64_t value,uint32_t *out)
{
    static const uint32_t offsets[]={4,8,12,16,20,24,0,44};
    static const uint32_t fixed[]={0xfff,0xfff,0x10babe,0,0,0,1,1};
    if(!owner->enabled||owner->phase!=16||r->step>=8||offset!=offsets[r->step])return false;
    int seed=r->step<2?(int)r->step:r->step==7?DFS_CONFIG_SEED_2C:-1;
    if(!write) {
        if(seed<0||!out)return false;
        *out=owner->retained[DFS_CONFIG_PUBCP][seed];r->read_seen=true;return true;
    }
    uint32_t expected=fixed[r->step];
    if(seed>=0) {
        if(!r->read_seen)return false;
        expected|=owner->retained[DFS_CONFIG_PUBCP][seed]&(r->step==7?UINT32_C(0xfffffff0):UINT32_C(0xffff0000));
    }
    if(value!=expected)return false;
    if(seed>=0)owner->retained[DFS_CONFIG_PUBCP][seed]=(uint32_t)value;
    r->value[r->step++]=(uint32_t)value;r->read_seen=false;return true;
}
#endif
