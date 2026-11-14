#ifndef O62M_MIPI_CLOCK_CONTROL_DEFS_V1_H
#define O62M_MIPI_CLOCK_CONTROL_DEFS_V1_H
#include "clock-controls.h"
#define O62M_MIPI_CLOCK_COUNT_V1 6u
/* Root index8 is MIPI PMdomain8; next root slot is NULL.
 * Six exact original descriptor words; no bank expansion, ready or auto fields. */
static const O62MClockControlDef o62m_mipi_clock_defs_v1[O62M_MIPI_CLOCK_COUNT_V1] = {
    { UINT64_C(0x40e20000), { 0x00020000u } }, /* clk_mipi_cfg_20m */
    { UINT64_C(0x40e20004), { 0x00000300u, 0x00010000u } }, /* clk_mipi_scanclk */
    { UINT64_C(0x40e20008), { 0x00020000u } }, /* clk_mipi_pll_scanclk */
    { UINT64_C(0x40e2000c), { 0x00000001u, 0x00000700u, 0x00010000u } }, /* clk_mipi_scanbyteclk */
    { UINT64_C(0x40e20010), { 0x00020000u } }, /* clk_mipi_scanrxclkesc */
    { UINT64_C(0x40e20014), { 0x00020000u } }, /* clk_mipi_scantxclkesc */
};
#endif
