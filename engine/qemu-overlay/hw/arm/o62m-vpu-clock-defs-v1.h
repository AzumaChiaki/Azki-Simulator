#ifndef O62M_VPU_CLOCK_CONTROL_DEFS_V1_H
#define O62M_VPU_CLOCK_CONTROL_DEFS_V1_H
#include "clock-controls.h"
#define O62M_VPU_CLOCK_COUNT_V1 15u
/* Exact root-index7/PMdomain7 original AP VPU descriptor fields.
 * Each listed word has one backing. Explicit fullword initial inputs required.
 * Static lookahead only: group callbacks are not modeled by this header.
 * No reset/ready, automatic-bit, bank-alias or physical behavior claim. */
static const O62MClockControlDef o62m_vpu_clock_defs_v1[O62M_VPU_CLOCK_COUNT_V1] = {
    { UINT64_C(0x41050000), { 0x00000007u, 0x00000700u, 0x00010000u } }, /* clk_vpu_codec */
    { UINT64_C(0x41050004), { 0x00020000u } }, /* clk_vpu_axi_s */
    { UINT64_C(0x41050008), { 0x00020000u } }, /* clk_vpu_axi_mtx */
    { UINT64_C(0x4105000c), { 0x00020000u } }, /* clk_vpu_axi_mtx_r */
    { UINT64_C(0x41050010), { 0x00020000u } }, /* clk_vpu_gpv_apb */
    { UINT64_C(0x41050014), { 0x00020000u } }, /* clk_vpu_dcamif_ahb */
    { UINT64_C(0x41050018), { 0x00020000u } }, /* clk_vpu_busmon_apb */
    { UINT64_C(0x4105001c), { 0x00020000u } }, /* clk_vpu_glbreg_apb */
    { UINT64_C(0x41050020), { 0x00020000u } }, /* clk_vpu_enc */
    { UINT64_C(0x41050024), { 0x00020000u } }, /* clk_vpu_enc_apb */
    { UINT64_C(0x41050028), { 0x00020000u } }, /* clk_vpu_dec */
    { UINT64_C(0x4105002c), { 0x00020000u } }, /* clk_vpu_dec_apb */
    { UINT64_C(0x41050030), { 0x00000007u, 0x00000700u, 0x00010000u } }, /* clk_vpu_dcamif_main */
    { UINT64_C(0x41050034), { 0x00020000u } }, /* clk_vpu_dcamif_main_cg */
    { UINT64_C(0x41050038), { 0x00000007u, 0x00000700u, 0x00010000u } }, /* clk_vpu_dcamif_rclk */
};
#endif
