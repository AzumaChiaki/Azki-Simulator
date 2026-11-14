#ifndef O62M_MEMPLL_VOTE_V1_H
#define O62M_MEMPLL_VOTE_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct O62MMemPllVoteV1 {
    uint32_t vote[2];
    uint64_t generation, writes, requests, completions, cancelled;
    int64_t deadline;
    bool pending, locked;
} O62MMemPllVoteV1;
static inline void o62m_mempll_cancel_v1(O62MMemPllVoteV1 *m)
{
    ++m->generation;m->cancelled+=m->pending;m->pending=false;m->locked=false;
}
static inline void o62m_mempll_reset_v1(O62MMemPllVoteV1 *m)
{
    uint64_t generation=m->generation+1;
    memset(m,0,sizeof(*m));m->generation=generation;
}
static inline bool o62m_mempll_write_v1(O62MMemPllVoteV1 *m,unsigned index,
                                      bool set,uint32_t value,int64_t now,bool source)
{
    if(index>1||(value&~UINT32_C(0x1ffe))||now<0||now>INT64_MAX-1000)return false;
    uint32_t next=set?m->vote[index]|value:m->vote[index]&~value;
    ++m->writes;
    if(next==m->vote[index])return true;
    o62m_mempll_cancel_v1(m);m->vote[index]=next;
    if(source&&(m->vote[0]&m->vote[1])) {
        m->pending=true;m->deadline=now+1000;++m->requests;
    }
    return true;
}
static inline bool o62m_mempll_complete_v1(O62MMemPllVoteV1 *m,uint64_t generation,
                                         int64_t now,bool source)
{
    if(!m->pending||generation!=m->generation||now<m->deadline||!source||
       !(m->vote[0]&m->vote[1]))return false;
    m->pending=false;m->locked=true;++m->completions;return true;
}
#endif
