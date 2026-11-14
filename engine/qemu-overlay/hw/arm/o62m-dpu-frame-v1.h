#ifndef O62M_DPU_FRAME_V1_H
#define O62M_DPU_FRAME_V1_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint32_t source,stride,width,height;uint64_t span,bytes;} O62MDpuFrameV1;
static inline bool o62m_dpu_frame_plan_v1(O62MDpuFrameV1 *p,uint32_t control,const uint32_t w[6])
{
    O62MDpuFrameV1 t={.source=w[0],.stride=w[1]&0x1ffff,.width=w[2]&0xfff,.height=(w[2]>>16)&0xfff};
    if((control&7)!=4||(control&0xe0000)||w[3]||w[4]||w[5]||!t.width||!t.height||
       t.width>1024||t.height>1024||t.stride<t.width*4)return false;
    t.span=(uint64_t)(t.height-1)*t.stride+t.width*4;t.bytes=(uint64_t)t.width*t.height*4;
    if((uint64_t)t.source+t.span>UINT64_C(0x100000000))return false;
    *p=t;return true;
}
#endif
