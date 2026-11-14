#ifndef O62M_SOC4005_CONFIG_DEFS_V1_H
#define O62M_SOC4005_CONFIG_DEFS_V1_H
#include "clock-controls.h"
#define O62M_SOC4005_CONFIG_COUNT_V1 3u
static const O62MClockControlDef o62m_soc4005_config_defs_v1[3] = {
    {UINT64_C(0x400500a4), {UINT32_C(0x00ff0000)}},
    {UINT64_C(0x400500b4), {UINT32_C(0x00000003)}},
    {UINT64_C(0x400502c0), {UINT32_C(0x0000ff00)}},
};
static inline bool o62m_soc4005_config_original_pc_v1(uint64_t address, uint64_t pc, bool write) {
    switch (address) {
    case UINT64_C(0x400500a4): return pc == (write ? UINT64_C(0x605f4c4e) : UINT64_C(0x605f4c42));
    case UINT64_C(0x400500b4): return pc == (write ? UINT64_C(0x605ecb78) : UINT64_C(0x605ecb6e));
    case UINT64_C(0x400502c0): return pc == (write ? UINT64_C(0x605ee6da) : UINT64_C(0x605ee6ce));
    default: return false;
    }
}
#endif
