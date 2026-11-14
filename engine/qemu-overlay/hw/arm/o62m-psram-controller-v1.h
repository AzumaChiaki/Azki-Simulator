/* Partial virtual PSRAM controller configuration commit and link activation. */
#ifndef O62M_PSRAM_CONTROLLER_V1_H
#define O62M_PSRAM_CONTROLLER_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#define O62M_PSRAM_CONTROLLER_WORDS_V1 51u
#define O62M_PSRAM_CONTROLLER_REQUIRED_V1 ((UINT64_C(1)<<45)-1)
typedef struct O62MPsramControllerV1 {
    uint32_t pending_words[51],active_words[51],pending_phy[3],active_phy[3];
    uint64_t pending_valid,active_valid,generation,requests,copies,initializations,cancelled;
    int64_t deadline;
    unsigned phase;
    uint8_t ports;
    bool update_open,config_ready,operating;
} O62MPsramControllerV1;
static inline void o62m_psram_controller_cancel_v1(O62MPsramControllerV1 *s)
{
    ++s->generation;s->cancelled+=s->phase!=0;s->phase=0;
    s->update_open=false;s->config_ready=false;s->operating=false;s->ports=0;
    memset(s->pending_words,0,sizeof(s->pending_words));memset(s->active_words,0,sizeof(s->active_words));
    memset(s->pending_phy,0,sizeof(s->pending_phy));memset(s->active_phy,0,sizeof(s->active_phy));
    s->pending_valid=s->active_valid=0;
}
static inline void o62m_psram_controller_reset_v1(O62MPsramControllerV1 *s)
{
    uint64_t generation=s->generation+1;
    memset(s,0,sizeof(*s));s->generation=generation;
}
static inline void o62m_psram_controller_open_v1(O62MPsramControllerV1 *s)
{ o62m_psram_controller_cancel_v1(s);s->update_open=true; }
static inline bool o62m_psram_controller_submit_v1(O62MPsramControllerV1 *s,
    const uint32_t words[51],uint64_t valid,const uint32_t phy[3],uint8_t code,int64_t now,bool source)
{
    if(!s->update_open||s->phase||!source||now<0||now>INT64_MAX-1000||
       (valid&O62M_PSRAM_CONTROLLER_REQUIRED_V1)!=O62M_PSRAM_CONTROLLER_REQUIRED_V1||
       (valid>>51)||!code||code==255||phy[0]!=code||phy[1]!=code||phy[2]!=code)return false;
    memcpy(s->pending_words,words,sizeof(s->pending_words));
    memcpy(s->pending_phy,phy,sizeof(s->pending_phy));s->pending_valid=valid;
    s->update_open=false;s->phase=1;s->deadline=now+1000;++s->generation;++s->requests;return true;
}
static inline bool o62m_psram_controller_complete_v1(O62MPsramControllerV1 *s,
    uint64_t generation,int64_t now,bool source)
{
    if(!s->phase||generation!=s->generation||now<s->deadline||!source)return false;
    if(s->phase==1) {
        if(now>INT64_MAX-1000)return false;
        memcpy(s->active_words,s->pending_words,sizeof(s->active_words));
        s->active_valid=s->pending_valid;memset(s->pending_words,0,sizeof(s->pending_words));s->pending_valid=0;
        s->config_ready=true;s->phase=2;s->deadline=now+1000;++s->copies;return true;
    }
    if(s->phase!=2||!s->config_ready||
       (s->active_valid&O62M_PSRAM_CONTROLLER_REQUIRED_V1)!=O62M_PSRAM_CONTROLLER_REQUIRED_V1)return false;
    memcpy(s->active_phy,s->pending_phy,sizeof(s->active_phy));memset(s->pending_phy,0,sizeof(s->pending_phy));
    s->operating=true;s->phase=0;++s->initializations;return true;
}
static inline bool o62m_psram_controller_port_v1(O62MPsramControllerV1 *s,unsigned port,uint32_t value)
{
    if(!s->operating||port>=4||value!=1)return false;
    s->ports|=(uint8_t)(1u<<port);return true;
}
#endif
