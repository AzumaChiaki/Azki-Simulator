#ifndef O62M_PUBCP_CLOCK_CONTROL_DEFS_V2_H
#define O62M_PUBCP_CLOCK_CONTROL_DEFS_V2_H
#include "clock-controls.h"
#define O62M_PUBCP_CLOCK_COUNT_V1 34u
/* Full34-word descriptor catalog; eight entries reuse boot_clocks backing.
 * Exact original AP pubcp/domain5 records only; no reset/ready or bank alias.
 * Original relocated LPS59bc provides the auto-control RMW fields. */
static const O62MClockControlDef o62m_pubcp_clock_defs_v1[O62M_PUBCP_CLOCK_COUNT_V1] = {
    { UINT64_C(0x4a9c0000), { 0x00000007u, 0x00000700u, 0x00010000u, 0x00100000u } }, /* clk_pubcp_main */
    { UINT64_C(0x4a9c0004), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_xtal_26m */
    { UINT64_C(0x4a9c0008), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_2x */
    { UINT64_C(0x4a9c000c), { 0x00010000u, 0x00100000u } }, /* clk_pubcp_1x */
    { UINT64_C(0x4a9c0010), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_bridge_ap */
    { UINT64_C(0x4a9c0014), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_bridge_lte */
    { UINT64_C(0x4a9c0018), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_bridge_com */
    { UINT64_C(0x4a9c001c), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_bridge_cp */
    { UINT64_C(0x4a9c0020), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_busmon_0 */
    { UINT64_C(0x4a9c0024), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_busmon_1 */
    { UINT64_C(0x4a9c0028), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_busmon_2 */
    { UINT64_C(0x4a9c002c), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_busmon_3 */
    { UINT64_C(0x4a9c0030), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_busmon_0 */
    { UINT64_C(0x4a9c0034), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_busmon_1 */
    { UINT64_C(0x4a9c0038), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_busmon_2 */
    { UINT64_C(0x4a9c003c), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_busmon_3 */
    { UINT64_C(0x4a9c0040), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_0_ddrc */
    { UINT64_C(0x4a9c0044), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_1_ddrc */
    { UINT64_C(0x4a9c0048), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_2_ddrc */
    { UINT64_C(0x4a9c004c), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_3_ddrc */
    { UINT64_C(0x4a9c0050), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_ddrc */
    { UINT64_C(0x4a9c0054), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_ddrc_core */
    { UINT64_C(0x4a9c0058), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_mpu_0 */
    { UINT64_C(0x4a9c005c), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_mpu_1 */
    { UINT64_C(0x4a9c0060), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_mpu_2 */
    { UINT64_C(0x4a9c0064), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_mpu_3 */
    { UINT64_C(0x4a9c0068), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_mpu_0 */
    { UINT64_C(0x4a9c006c), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_mpu_1 */
    { UINT64_C(0x4a9c0070), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_mpu_2 */
    { UINT64_C(0x4a9c0074), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_axi_mpu_3 */
    { UINT64_C(0x4a9c0078), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_xtal_26m_dfs */
    { UINT64_C(0x4a9c007c), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_apb_dfs */
    { UINT64_C(0x4a9c0080), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_xtal_26m_slp_ctrl */
    { UINT64_C(0x4a9c0084), { 0x00020000u, 0x00200000u } }, /* clk_pubcp_slp_ctrl */
};
#endif
