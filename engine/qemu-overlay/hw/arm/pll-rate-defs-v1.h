#ifndef O62M_PLL_RATE_DEFS_V1_H
#define O62M_PLL_RATE_DEFS_V1_H
#include "clock-controls.h"
/* Four original PLL descriptors; exact secure TEE words only. Initial values
 * supplied explicitly by the experiment. No writable PLL/lock/status subset. */
#define O62M_PLL_RATE_COUNT 12u
static const O62MClockControlDef o62m_pll_rate_defs[O62M_PLL_RATE_COUNT]={
 {UINT64_C(0x5a09c000),{0}},
 {UINT64_C(0x5a09c004),{0}},
 {UINT64_C(0x5a09c008),{0}},
 {UINT64_C(0x5a09c200),{0}},
 {UINT64_C(0x5a09c204),{0}},
 {UINT64_C(0x5a09c208),{0}},
 {UINT64_C(0x5a09c400),{0}},
 {UINT64_C(0x5a09c404),{0}},
 {UINT64_C(0x5a09c408),{0}},
 {UINT64_C(0x5a09c600),{0}},
 {UINT64_C(0x5a09c604),{0}},
 {UINT64_C(0x5a09c608),{0}},
};
static inline bool o62m_pll_rate_original_read_pc(uint64_t pc) {
 return pc==0x73e5b792||pc==0x73e5b796||pc==0x73e5b79c||pc==0x73e5b7aa||pc==0x73e5b7dc;
}
#endif
