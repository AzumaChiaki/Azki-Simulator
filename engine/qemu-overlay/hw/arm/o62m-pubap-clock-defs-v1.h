#ifndef O62M_PUBAP_CLOCK_CONTROL_DEFS_V1_H
#define O62M_PUBAP_CLOCK_CONTROL_DEFS_V1_H
#include "clock-controls.h"
#define O62M_PUBAP_CLOCK_COUNT_V1 40u
/* Exact original AP pubap/domain4 descriptors; one owner per listed word.
 * No reset/ready value, automatic bit, bank expansion, or AP/TEE alias.
 * Initial fullword state must be supplied explicitly by the caller. */
static const O62MClockControlDef o62m_pubap_clock_defs_v1[O62M_PUBAP_CLOCK_COUNT_V1] = {
    { UINT64_C(0x4a8e0000), { 0x00000007u, 0x00000700u, 0x00010000u } }, /* clk_pubap_main */
    { UINT64_C(0x4a8e0004), { 0x00020000u } }, /* clk_pubap_xtal_26m */
    { UINT64_C(0x4a8e0008), { 0x00020000u } }, /* clk_pubap_2x */
    { UINT64_C(0x4a8e000c), { 0x00010000u } }, /* clk_pubap_1x */
    { UINT64_C(0x4a8e0010), { 0x00020000u } }, /* clk_pubap_axi_bridge_ap */
    { UINT64_C(0x4a8e0014), { 0x00020000u } }, /* clk_pubap_axi_bridge_vpu */
    { UINT64_C(0x4a8e0018), { 0x00020000u } }, /* clk_pubap_axi_bridge_audio */
    { UINT64_C(0x4a8e001c), { 0x00020000u } }, /* clk_pubap_axi_bridge_com */
    { UINT64_C(0x4a8e0020), { 0x00020000u } }, /* clk_pubap_axi_bridge_cp */
    { UINT64_C(0x4a8e0024), { 0x00020000u } }, /* clk_pubap_apb_busmon_0 */
    { UINT64_C(0x4a8e0028), { 0x00020000u } }, /* clk_pubap_apb_busmon_1 */
    { UINT64_C(0x4a8e002c), { 0x00020000u } }, /* clk_pubap_apb_busmon_2 */
    { UINT64_C(0x4a8e0030), { 0x00020000u } }, /* clk_pubap_apb_busmon_3 */
    { UINT64_C(0x4a8e0034), { 0x00020000u } }, /* clk_pubap_apb_busmon_4 */
    { UINT64_C(0x4a8e0038), { 0x00020000u } }, /* clk_pubap_axi_busmon_0 */
    { UINT64_C(0x4a8e003c), { 0x00020000u } }, /* clk_pubap_axi_busmon_1 */
    { UINT64_C(0x4a8e0040), { 0x00020000u } }, /* clk_pubap_axi_busmon_2 */
    { UINT64_C(0x4a8e0044), { 0x00020000u } }, /* clk_pubap_axi_busmon_3 */
    { UINT64_C(0x4a8e0048), { 0x00020000u } }, /* clk_pubap_axi_busmon_4 */
    { UINT64_C(0x4a8e004c), { 0x00020000u } }, /* clk_pubap_axi_0_ddrc */
    { UINT64_C(0x4a8e0050), { 0x00020000u } }, /* clk_pubap_axi_1_ddrc */
    { UINT64_C(0x4a8e0054), { 0x00020000u } }, /* clk_pubap_axi_2_ddrc */
    { UINT64_C(0x4a8e0058), { 0x00020000u } }, /* clk_pubap_axi_3_ddrc */
    { UINT64_C(0x4a8e005c), { 0x00020000u } }, /* clk_pubap_axi_4_ddrc */
    { UINT64_C(0x4a8e0060), { 0x00020000u } }, /* clk_pubap_apb_ddrc */
    { UINT64_C(0x4a8e0064), { 0x00020000u } }, /* clk_pubap_ddrc_core */
    { UINT64_C(0x4a8e0068), { 0x00020000u } }, /* clk_pubap_apb_mpu_0 */
    { UINT64_C(0x4a8e006c), { 0x00020000u } }, /* clk_pubap_apb_mpu_1 */
    { UINT64_C(0x4a8e0070), { 0x00020000u } }, /* clk_pubap_apb_mpu_2 */
    { UINT64_C(0x4a8e0074), { 0x00020000u } }, /* clk_pubap_apb_mpu_3 */
    { UINT64_C(0x4a8e0078), { 0x00020000u } }, /* clk_pubap_apb_mpu_4 */
    { UINT64_C(0x4a8e007c), { 0x00020000u } }, /* clk_pubap_axi_mpu_0 */
    { UINT64_C(0x4a8e0080), { 0x00020000u } }, /* clk_pubap_axi_mpu_1 */
    { UINT64_C(0x4a8e0084), { 0x00020000u } }, /* clk_pubap_axi_mpu_2 */
    { UINT64_C(0x4a8e0088), { 0x00020000u } }, /* clk_pubap_axi_mpu_3 */
    { UINT64_C(0x4a8e008c), { 0x00020000u } }, /* clk_pubap_axi_mpu_4 */
    { UINT64_C(0x4a8e0090), { 0x00020000u } }, /* clk_pubap_xtal_26m_dfs */
    { UINT64_C(0x4a8e0094), { 0x00020000u } }, /* clk_pubap_apb_dfs */
    { UINT64_C(0x4a8e0098), { 0x00020000u } }, /* clk_pubap_xtal_26m_slp_ctrl */
    { UINT64_C(0x4a8e009c), { 0x00020000u } }, /* clk_pubap_slp_ctrl */
};
#endif
