/* Partial virtual PSRAM delay-line calibration. */
#ifndef O62M_PSRAM_CALIBRATION_V1_H
#define O62M_PSRAM_CALIBRATION_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
typedef struct O62MPsramCalibrationV1 {
    uint64_t generation,requests,completions,cancelled,applies;
    uint32_t reference_hz,cell_ps;
    int64_t deadline;
    uint8_t code,copied;
    bool pending,valid,applied;
} O62MPsramCalibrationV1;
static inline bool o62m_psram_cal_parameters_v1(uint32_t hz,uint32_t cell,
                                               uint8_t *code,int64_t *duration)
{
    if(!hz||hz>1000000000u||!cell||cell>1000000u)return false;
    uint64_t denominator=UINT64_C(2)*hz*cell;
    uint64_t tap=UINT64_C(1000000000000)/denominator+
        (UINT64_C(1000000000000)%denominator!=0);
    if(!tap||tap>=255)return false;
    *code=(uint8_t)tap;
    *duration=(INT64_C(64000000000)+hz-1)/hz;
    return true;
}
static inline void o62m_psram_cal_cancel_v1(O62MPsramCalibrationV1 *s)
{
    ++s->generation;s->cancelled+=s->pending;s->pending=false;
    s->valid=false;s->applied=false;s->code=0;s->copied=0;
}
static inline void o62m_psram_cal_reset_v1(O62MPsramCalibrationV1 *s)
{
    uint64_t generation=s->generation+1;
    memset(s,0,sizeof(*s));s->generation=generation;
}
static inline bool o62m_psram_cal_request_v1(O62MPsramCalibrationV1 *s,
    uint32_t command,uint32_t hz,uint32_t cell,int64_t now,bool source)
{
    uint8_t code;int64_t duration;
    if(command!=UINT32_C(0xa208d400)||s->pending||!source||now<0||
       !o62m_psram_cal_parameters_v1(hz,cell,&code,&duration)||
       now>INT64_MAX-duration)return false;
    o62m_psram_cal_cancel_v1(s);
    s->reference_hz=hz;s->cell_ps=cell;s->deadline=now+duration;
    s->pending=true;++s->requests;return true;
}
static inline bool o62m_psram_cal_complete_v1(O62MPsramCalibrationV1 *s,
                                             uint64_t generation,int64_t now,bool source)
{
    uint8_t code;int64_t duration;
    if(!s->pending||generation!=s->generation||now<s->deadline||!source||
       !o62m_psram_cal_parameters_v1(s->reference_hz,s->cell_ps,&code,&duration))return false;
    s->pending=false;s->valid=true;s->code=code;++s->completions;return true;
}
static inline uint32_t o62m_psram_cal_status_v1(const O62MPsramCalibrationV1 *s)
{ return s->valid?(UINT32_C(1)<<28)|s->code:0; }
static inline bool o62m_psram_cal_copy_v1(O62MPsramCalibrationV1 *s,unsigned index,uint32_t value)
{
    if(index>=3||!s->valid||s->applied||value!=s->code)return false;
    s->copied|=(uint8_t)(1u<<index);return true;
}
static inline bool o62m_psram_cal_apply_v1(O62MPsramCalibrationV1 *s,uint32_t command)
{
    if(command!=UINT32_C(0xa208d800)||!s->valid||s->applied||s->copied!=7)return false;
    s->applied=true;++s->applies;return true;
}
#endif
