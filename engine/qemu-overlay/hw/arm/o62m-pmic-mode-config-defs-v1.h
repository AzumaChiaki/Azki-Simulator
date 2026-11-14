#ifndef O62M_PMIC_MODE_CONFIG_DEFS_V1_H
#define O62M_PMIC_MODE_CONFIG_DEFS_V1_H
#include "clock-controls.h"
#define O62M_PMIC_MODE_CONFIG_COUNT_V1 4u
/* PMIC register numbers, not CPU MMIO addresses. Original AP mode mask tables:
 * buck 6139bc08[5], ldo 6139bb98[28], lsw 6139bb80[6]; LLVM90/91 branches.
 * Each RMW changes ONE listed field. Independent full16 initial words only.
 * No GO50, IRQ/status, reset value or physical regulator transition modeled.
 */
static const O62MClockControlDef o62m_pmic_mode_config_defs_v1[O62M_PMIC_MODE_CONFIG_COUNT_V1] = {
    { UINT64_C(0x0040), { 0x800u, 0x400u, 0x200u, 0x100u, 0x80u,
        0x40u, 0x20u, 0x10u, 0x8u, 0x4u, 0x2u, 0x1u } },
    { UINT64_C(0x0044), { 0x8000u, 0x4000u, 0x2000u, 0x1000u,
        0x800u, 0x400u, 0x200u, 0x100u, 0x80u, 0x40u, 0x20u, 0x10u,
        0x8u, 0x4u, 0x2u, 0x1u } },
    { UINT64_C(0x0048), { 0x1000u, 0x800u, 0x400u, 0x80u, 0x40u,
        0x20u, 0x10u, 0x8u, 0x4u, 0x3u } },
    { UINT64_C(0x00f0), { 0x10u } },
};
#endif
