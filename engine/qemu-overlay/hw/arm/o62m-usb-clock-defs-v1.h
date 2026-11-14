#ifndef O62M_USB_CLOCK_CONTROL_DEFS_V1_H
#define O62M_USB_CLOCK_CONTROL_DEFS_V1_H
#include "clock-controls.h"
#define O62M_USB_CLOCK_COUNT_V1 9u
/* Static lookahead: root index6 is USB PMdomain9; actual execution unclaimed.
 * Nine exact MMIO words, not the entire bank. The fixed rate record adds no MMIO.
 * No reset/ready, automatic field, or address alias is inferred. */
static const O62MClockControlDef o62m_usb_clock_defs_v1[O62M_USB_CLOCK_COUNT_V1] = {
    { UINT64_C(0x4aa40000), { 0x00000003u, 0x00000f00u, 0x00010000u } }, /* clk_usb_main */
    { UINT64_C(0x4aa40004), { 0x00020000u } }, /* clk_usb_xtal_26m */
    { UINT64_C(0x4aa4000c), { 0x00020000u } }, /* clk_usb_phy_100m_occ0 */
    { UINT64_C(0x4aa40010), { 0x00020000u } }, /* clk_usb_phy_100m_occ1 */
    { UINT64_C(0x4aa40014), { 0x00020000u } }, /* clk_usb_phy_100m_occ2 */
    { UINT64_C(0x4aa40018), { 0x00020000u } }, /* clk_usb_phy_100m_occ3 */
    { UINT64_C(0x4aa4001c), { 0x00020000u } }, /* clk_usb_phy_480m_occ0 */
    { UINT64_C(0x4aa40020), { 0x00020000u } }, /* clk_usb_phy_480m_occ1 */
    { UINT64_C(0x4aa40024), { 0x00020000u } }, /* clk_usb_phy_480m_occ2 */
};
#endif
