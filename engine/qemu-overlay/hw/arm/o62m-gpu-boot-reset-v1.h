/* SPDX-License-Identifier: MIT */
#ifndef O62M_GPU_BOOT_RESET_V1_H
#define O62M_GPU_BOOT_RESET_V1_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
enum { O62M_GPU_NEW, O62M_GPU_ASSERTED, O62M_GPU_RELEASED, O62M_GPU_RESET_WORK, O62M_GPU_IDLE };
typedef struct O62MGpuBootResetV1 {
    uint32_t control, opaque_status;
    uint32_t command_address, command_size, fetch_position, interrupt_pending, interrupt_enable;
    unsigned phase;
    uint64_t generation, resets_completed;
    int64_t latency_ns, deadline_ns, now_ns;
} O62MGpuBootResetV1;
static inline bool o62m_gpu_boot_open_v1(uint32_t control)
{ return !(control & UINT32_C(0x81002)); }
static inline bool o62m_gpu_boot_init_v1(O62MGpuBootResetV1 *s,uint32_t control,uint32_t status,int64_t latency,int64_t now)
{
    if(!s||latency<=0||now<0||(control&0x1000)||(status&0xb05))return false;
    *s=(O62MGpuBootResetV1){.control=control,.opaque_status=status,.latency_ns=latency,.now_ns=now};
    return true;
}
static inline uint32_t o62m_gpu_boot_status_v1(const O62MGpuBootResetV1 *s)
{ return s->opaque_status|((s->phase==O62M_GPU_IDLE&&o62m_gpu_boot_open_v1(s->control))?UINT32_C(0xb05):0); }
static inline bool o62m_gpu_boot_control_v1(O62MGpuBootResetV1 *s,uint32_t word,int64_t now)
{
    if(!s||now<s->now_ns||((word^s->control)&~UINT32_C(0x81bfe)))return false;
    O62MGpuBootResetV1 t=*s;
    bool rising=!(t.control&0x1000)&&(word&0x1000);
    bool falling=(t.control&0x1000)&&!(word&0x1000);
    if(rising) {
        if(t.generation==UINT64_MAX)return false;
        ++t.generation;t.phase=O62M_GPU_ASSERTED;t.deadline_ns=0;
    } else if(falling) {
        if(t.phase!=O62M_GPU_ASSERTED)return false;
        t.phase=O62M_GPU_RELEASED;
    }
    if(!o62m_gpu_boot_open_v1(word)) {
        if(t.phase==O62M_GPU_RESET_WORK) {t.phase=O62M_GPU_RELEASED;t.deadline_ns=0;}
        else if(t.phase==O62M_GPU_IDLE)t.phase=O62M_GPU_NEW;
    } else if(t.phase==O62M_GPU_RELEASED) {
        if(t.generation==UINT64_MAX||t.latency_ns>INT64_MAX-now)return false;
        ++t.generation;t.phase=O62M_GPU_RESET_WORK;t.deadline_ns=now+t.latency_ns;
    }
    t.control=word;t.now_ns=now;*s=t;return true;
}
static inline bool o62m_gpu_boot_complete_v1(O62MGpuBootResetV1 *s,uint64_t generation,int64_t now)
{
    if(!s||s->phase!=O62M_GPU_RESET_WORK||generation!=s->generation||
       now<s->now_ns||now<s->deadline_ns||!o62m_gpu_boot_open_v1(s->control)||s->resets_completed==UINT64_MAX)return false;
    /* Reset the supported command front-end. Unsupported command submissions
     * are rejected by the bus adapter before acquiring any engine work. */
    s->command_address=0;s->command_size=0;s->fetch_position=0;
    s->interrupt_pending=0;s->interrupt_enable=0;
    ++s->resets_completed;s->phase=O62M_GPU_IDLE;s->deadline_ns=0;s->now_ns=now;
    return true;
}
#endif
