#ifndef O62M_PMIC_MODE_CONFIG_DEFS_V4_H
#define O62M_PMIC_MODE_CONFIG_DEFS_V4_H
#include "clock-controls.h"
#define O62M_PMIC_MODE_CONFIG_COUNT_V4 7u
/* PMIC register numbers, not CPU MMIO addresses. Original AP mode mask tables:
 * buck 6139bc08[5], ldo 6139bb98[28], lsw 6139bb80[6]; LLVM90/91 branches.
 * Each RMW changes ONE listed field. Independent full16 initial words only.
 * No GO50, IRQ/status, reset value or physical regulator transition modeled.
 */
static const O62MClockControlDef o62m_pmic_mode_config_defs_v4[O62M_PMIC_MODE_CONFIG_COUNT_V4] = {
    { UINT64_C(0x0040), { 0x800u, 0x400u, 0x200u, 0x100u, 0x80u,
        0x40u, 0x20u, 0x10u, 0x8u, 0x4u, 0x2u, 0x1u } },
    { UINT64_C(0x0044), { 0x8000u, 0x4000u, 0x2000u, 0x1000u,
        0x800u, 0x400u, 0x200u, 0x100u, 0x80u, 0x40u, 0x20u, 0x10u,
        0x8u, 0x4u, 0x2u, 0x1u } },
    { UINT64_C(0x0048), { 0x1000u, 0x800u, 0x400u, 0x80u, 0x40u,
        0x20u, 0x10u, 0x8u, 0x4u, 0x3u } },
    { UINT64_C(0x00f0), { 0x10u,0x4u,0x3u,0x2000u,0x1000u,0x100u,0x800u,0x80u } },
    { UINT64_C(0x00f4), { 0xc0u } },
    { UINT64_C(0x10f4), { 0 } },
    { UINT64_C(0x119c), { 0x3u,0xcu } },
};
static inline O62MClockControlsResult o62m_pmic_mode_write_v4(O62MClockControls *m,uint16_t reg,uint32_t value)
{
    if(value>UINT16_MAX)return O62M_CLOCK_CONTROLS_VALUE_RANGE;
    if(reg!=0x05f0&&reg!=0x0af0)return o62m_clock_controls_write(m,reg,4,value);
    size_t i;O62MClockControlsResult rc=o62m_clock_controls_index(m,0xf0,4,&i);
    if(rc!=O62M_CLOCK_CONTROLS_OK)return rc;
    bool allowed=false;
    for(unsigned f=0;f<O62M_CLOCK_CONTROL_FIELDS_MAX;f++) {
        uint32_t mask=m->defs[i].field_mask[f];
        if(value&&mask&&(value&~mask)==0)allowed=true;
    }
    if(!allowed)return O62M_CLOCK_CONTROLS_OPAQUE_CHANGE;
    uint32_t next=reg==0x05f0?(m->word[i]|value):(m->word[i]&~value);
    return o62m_clock_controls_write(m,0xf0,4,next);
}
#endif
