/* Original DVFS PHY r1p0 configuration only: no request/status completion.
 * Explicit fullword initials, unobserved hardware reset/alias/effects unknown.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef O62M_AON120_CONFIG_V2_H
#define O62M_AON120_CONFIG_V2_H
#include "clock-controls.h"
#define O62M_AON120_CONFIG_COUNT_V2 25u
static const O62MClockControlDef o62m_aon120_config_defs_v2[O62M_AON120_CONFIG_COUNT_V2]={
    { UINT64_C(0x4a120000), { UINT32_C(0x78) } },
    { UINT64_C(0x4a120004), { UINT32_C(0x7) } },
    { UINT64_C(0x4a120008), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a12000c), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120010), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120014), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120018), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a12001c), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120020), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120024), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120028), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a12002c), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120030), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120050), { UINT32_C(0x7), UINT32_C(0x38), UINT32_C(0x1c0), UINT32_C(0xe00), UINT32_C(0x7000), UINT32_C(0x38000), UINT32_C(0x1c0000), UINT32_C(0xe00000), UINT32_C(0x7000000), UINT32_C(0x38000000) } },
    { UINT64_C(0x4a120054), { UINT32_C(0x7), UINT32_C(0x38), UINT32_C(0x1c0), UINT32_C(0xe00), UINT32_C(0x7000), UINT32_C(0x38000) } },
    { UINT64_C(0x4a120058), { UINT32_C(0x7), UINT32_C(0x38), UINT32_C(0x1c0), UINT32_C(0xe00), UINT32_C(0x7000), UINT32_C(0x38000), UINT32_C(0x1c0000), UINT32_C(0xe00000), UINT32_C(0x7000000), UINT32_C(0x38000000) } },
    { UINT64_C(0x4a12005c), { UINT32_C(0x7), UINT32_C(0x38), UINT32_C(0x1c0), UINT32_C(0xe00), UINT32_C(0x7000), UINT32_C(0x38000) } },
    { UINT64_C(0x4a120060), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120064), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120068), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a12006c), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120070), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120074), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a120078), { UINT32_C(0xffffffff) } },
    { UINT64_C(0x4a12007c), { UINT32_C(0xffffffff) } },
};
#endif
