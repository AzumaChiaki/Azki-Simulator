#ifndef O62M_MIPI_ULPS_V2_H
#define O62M_MIPI_ULPS_V2_H
#include <stdbool.h>
#include <stdint.h>
/* Virtual PHY control, lock and ULPS lifecycle. */
typedef struct O62MMipiUlpsV2 {
    uint32_t rstz, lane_encoding, ulps_control, test_control, test_data;
    uint8_t test_address, pending_test_data;
    bool address_clock, address_valid, data_clock, test_1f_valid;
    bool pll_pending, pll_locked, ulps_pending, ulps_qualified, lanes_active;
    bool pll_stopping, ulps_entering;
    uint64_t pll_generation, ulps_generation, test_commits;
    uint64_t pll_requests, pll_completions, ulps_requests, ulps_qualifications, ulps_exits;
    uint64_t ulps_entry_requests, ulps_entries, pll_stop_requests, pll_stops;
    int64_t pll_deadline_ns, ulps_deadline_ns;
} O62MMipiUlpsV2;
static inline void o62m_mipi_invalidate_v2(O62MMipiUlpsV2 *s)
{
    s->pll_pending=false;s->pll_locked=false;s->ulps_pending=false;
    s->ulps_qualified=false;s->lanes_active=false;
    s->pll_stopping=false;s->ulps_entering=false;
    s->address_clock=false;s->address_valid=false;s->data_clock=false;s->test_1f_valid=false;
    if(s->pll_generation!=UINT64_MAX)++s->pll_generation;
    if(s->ulps_generation!=UINT64_MAX)++s->ulps_generation;
}
static inline void o62m_mipi_reset_v2(O62MMipiUlpsV2 *s)
{
    o62m_mipi_invalidate_v2(s);
    s->rstz=7;s->lane_encoding=0;s->ulps_control=0;s->test_control=0;s->test_data=0;
    s->test_address=0;s->pending_test_data=0;
}
static inline uint32_t o62m_mipi_status_v2(const O62MMipiUlpsV2 *s)
{
    uint32_t word=s->pll_locked?1u:0u;
    if(s->lanes_active) {
        static const unsigned shifts[4]={5,8,10,12};
        word|=1u<<3;
        for(unsigned i=0;i<=s->lane_encoding;i++)word|=1u<<shifts[i];
    }
    return word;
}
static inline bool o62m_mipi_read_v2(const O62MMipiUlpsV2 *s,unsigned off,uint32_t *word)
{
    switch(off) {
    case 0xa0:*word=s->rstz;return true;
    case 0xa4:*word=s->lane_encoding;return true;
    case 0xa8:*word=s->ulps_control;return true;
    case 0xb0:*word=o62m_mipi_status_v2(s);return true;
    case 0xb4:*word=s->test_control;return true;
    case 0xb8:*word=s->test_data;return true;
    default:return false;
    }
}
static inline bool o62m_mipi_write_candidate_v2(O62MMipiUlpsV2 *s,unsigned off,
        uint32_t value,int64_t now)
{
    if(off==0xb8) {
        if((value^s->test_data)&~UINT32_C(0x100ff))return false;
        s->test_data=value;return true;
    }
    if(off==0xb4) {
        if((value^s->test_control)&~2u)return false;
        bool was=(s->test_control&2)!=0,next=(value&2)!=0,enable=(s->test_data&0x10000)!=0;
        if(!was&&next) {
            if(enable)return false;
            if(s->address_valid) {s->data_clock=true;s->pending_test_data=s->test_data&0xff;}
            else s->address_clock=true;
        } else if(was&&!next) {
            if(enable&&s->address_clock&&!s->address_valid) {
                if((s->test_data&0xff)!=0x1f)return false;
                s->test_address=0x1f;s->address_valid=true;s->address_clock=false;
            } else if(!enable&&s->address_valid&&s->data_clock) {
                if(s->test_address!=0x1f||s->pending_test_data!=(s->test_data&0xff)||
                   s->pending_test_data>1)return false;
                if(s->pending_test_data==0&&(!s->ulps_entries||s->lanes_active||s->pll_locked||
                    s->pll_pending||s->ulps_pending||s->ulps_control!=5))return false;
                s->test_1f_valid=s->pending_test_data==1;++s->test_commits;
                s->address_valid=false;s->data_clock=false;
            } else return false;
        }
        s->test_control=value;return true;
    }
    if(off==0xa0) {
        if((s->rstz&7)!=7||(value&7)!=7||((value^s->rstz)&~8u))return false;
        if(value&8) {
            if(s->pll_pending||s->pll_locked||s->lanes_active||!s->test_1f_valid||
               s->pll_generation==UINT64_MAX||now>INT64_MAX-1000)return false;
            ++s->pll_generation;++s->pll_requests;s->pll_deadline_ns=now+1000;
            s->pll_pending=true;s->pll_stopping=false;
        } else if(!s->pll_locked||!s->lanes_active||s->ulps_pending)return false;
        s->rstz=value;return true;
    }
    if(off==0xa8) {
        uint32_t changed=value^s->ulps_control;
        if(value>15||!s->pll_locked||(changed&&(changed&(changed-1))))return false;
        if(s->lanes_active) {
            if(s->pll_pending||s->ulps_pending||(s->rstz&8)||
               (value!=1&&value!=5)||(s->ulps_control&~value)||(value&~5u))return false;
            if(value==5) {
                if(s->ulps_generation==UINT64_MAX||now>INT64_MAX-1000)return false;
                ++s->ulps_generation;++s->ulps_entry_requests;s->ulps_deadline_ns=now+1000;
                s->ulps_pending=true;s->ulps_entering=true;
            }
            s->ulps_control=value;return true;
        }
        if(s->ulps_pending)return false;
        if(s->ulps_qualified) {
            if(value&~s->ulps_control)return false;
            s->ulps_control=value;
            if(!value){s->lanes_active=true;s->ulps_qualified=false;++s->ulps_exits;}
            return true;
        }
        if((s->ulps_control&~value)||((value&2)&&!(value&1))||((value&8)&&!(value&4)))return false;
        s->ulps_control=value;
        if(value==15) {
            if(s->ulps_generation==UINT64_MAX||now>INT64_MAX-1000000)return false;
            ++s->ulps_generation;++s->ulps_requests;s->ulps_deadline_ns=now+1000000;
            s->ulps_pending=true;s->ulps_entering=false;
        }
        return true;
    }
    return false;
}
static inline bool o62m_mipi_write_v2(O62MMipiUlpsV2 *s,unsigned off,uint64_t value,
        int64_t now,bool source_available)
{
    if(value>UINT32_MAX||now<0||!source_available)return false;
    O62MMipiUlpsV2 next=*s;
    if(!o62m_mipi_write_candidate_v2(&next,off,(uint32_t)value,now))return false;
    *s=next;return true;
}
static inline bool o62m_mipi_pll_complete_v2(O62MMipiUlpsV2 *s,uint64_t generation,
        int64_t now,bool source_available)
{
    if(!s->pll_pending||generation!=s->pll_generation||now<s->pll_deadline_ns||!source_available)return false;
    if(s->pll_stopping) {
        if((s->rstz&8)||s->lanes_active||s->ulps_pending||s->ulps_control!=5)return false;
        s->pll_pending=false;s->pll_stopping=false;s->pll_locked=false;++s->pll_stops;return true;
    }
    if((s->rstz&15)!=15||!s->test_1f_valid)return false;
    s->pll_pending=false;s->pll_locked=true;++s->pll_completions;return true;
}
static inline bool o62m_mipi_ulps_qualify_v2(O62MMipiUlpsV2 *s,uint64_t generation,
        int64_t now,bool source_available)
{
    if(!s->ulps_pending||generation!=s->ulps_generation||now<s->ulps_deadline_ns||
       !source_available||!s->pll_locked)return false;
    if(s->ulps_entering) {
        if(s->ulps_control!=5||(s->rstz&8)||!s->lanes_active||s->pll_pending||
           s->pll_generation==UINT64_MAX||now>INT64_MAX-1000)return false;
        s->ulps_pending=false;s->ulps_entering=false;s->lanes_active=false;++s->ulps_entries;
        ++s->pll_generation;++s->pll_stop_requests;s->pll_pending=true;s->pll_stopping=true;
        s->pll_deadline_ns=now+1000;return true;
    }
    if(s->ulps_control!=15)return false;
    s->ulps_pending=false;s->ulps_qualified=true;++s->ulps_qualifications;return true;
}
#endif
