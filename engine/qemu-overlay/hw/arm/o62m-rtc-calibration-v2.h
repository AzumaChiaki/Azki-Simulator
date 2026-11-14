#ifndef O62M_RTC_CALIBRATION_V2_H
#define O62M_RTC_CALIBRATION_V2_H
#include <stdbool.h>
#include <stdint.h>
#include "o62m-rtc32k-reference-v1.h"
typedef struct O62MRTCCalibrationV1 {
    uint64_t generation, source_generation, requests, completions;
    uint32_t window_ticks, result, planned_result;
    int64_t deadline_ns;
    bool control, pending, valid;
} O62MRTCCalibrationV1;
static inline bool o62m_rtc_calibration_start_v2(O62MRTCCalibrationV1 *s,
        uint16_t config,uint8_t written,uint32_t window,
        const O62MRTC32kReferenceV1 *source,int64_t now)
{
    if(s->pending||s->generation==UINT64_MAX||config!=0x11||written!=7||!window||
       !source->source_available||!source->ready||now<0)return false;
    uint64_t duration=((uint64_t)window*UINT64_C(1000000000)+25999999)/26000000;
    if(duration>(uint64_t)(INT64_MAX-now))return false;
    s->generation++;s->source_generation=source->generation;s->requests++;
    s->window_ticks=window;s->planned_result=(uint64_t)window*32768/26000000;
    s->deadline_ns=now+(int64_t)duration;s->control=true;s->pending=true;s->valid=false;s->result=0;
    return true;
}
static inline void o62m_rtc_calibration_cancel_v1(O62MRTCCalibrationV1 *s)
{
    s->pending=false;s->valid=false;s->control=false;s->result=0;
    if(s->generation!=UINT64_MAX)s->generation++;
}
static inline bool o62m_rtc_calibration_complete_v1(O62MRTCCalibrationV1 *s,
        uint64_t generation,const O62MRTC32kReferenceV1 *source,int64_t now)
{
    if(!s->pending||generation!=s->generation||now<s->deadline_ns)return false;
    if(!source->source_available||!source->ready||source->generation!=s->source_generation) {
        o62m_rtc_calibration_cancel_v1(s);return false;
    }
    s->result=s->planned_result;s->pending=false;s->valid=true;s->completions++;return true;
}
#endif
