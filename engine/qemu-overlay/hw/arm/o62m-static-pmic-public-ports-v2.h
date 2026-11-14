#ifndef O62M_STATIC_PMIC_PUBLIC_PORTS_V2_H
#define O62M_STATIC_PMIC_PUBLIC_PORTS_V2_H
#include <stdint.h>
#include <stddef.h>
/* Classification only: no emulation, ownership, alias, mask, initial value,
 * read/write permission, IRQ, completion, or sample semantics are granted.
 * Includes known typed/ADC/clock ports and byte-matched formal instruction
 * constants. Unproved raw byte candidates are excluded. Exact uint32_t
 * equality only: unknown input returns -1; do not truncate or normalize. */
#define O62M_STATIC_PMIC_PUBLIC_PORT_COUNT_V2 78u
static const uint32_t o62m_static_pmic_public_ports_v2[O62M_STATIC_PMIC_PUBLIC_PORT_COUNT_V2] = {
    UINT32_C(0x0004), /* 0 */
    UINT32_C(0x0018), /* 1 */
    UINT32_C(0x001c), /* 2 */
    UINT32_C(0x0040), /* 3 */
    UINT32_C(0x0044), /* 4 */
    UINT32_C(0x0048), /* 5 */
    UINT32_C(0x0050), /* 6 */
    UINT32_C(0x0060), /* 7 */
    UINT32_C(0x0064), /* 8 */
    UINT32_C(0x0068), /* 9 */
    UINT32_C(0x006c), /* 10 */
    UINT32_C(0x00b0), /* 11 */
    UINT32_C(0x00b4), /* 12 */
    UINT32_C(0x00b8), /* 13 */
    UINT32_C(0x00bc), /* 14 */
    UINT32_C(0x00cc), /* 15 */
    UINT32_C(0x00f0), /* 16 */
    UINT32_C(0x0170), /* 17 */
    UINT32_C(0x0240), /* 18 */
    UINT32_C(0x0244), /* 19 */
    UINT32_C(0x024c), /* 20 */
    UINT32_C(0x0254), /* 21 */
    UINT32_C(0x0258), /* 22 */
    UINT32_C(0x0280), /* 23 */
    UINT32_C(0x028c), /* 24 */
    UINT32_C(0x0290), /* 25 */
    UINT32_C(0x0294), /* 26 */
    UINT32_C(0x0298), /* 27 */
    UINT32_C(0x02a0), /* 28 */
    UINT32_C(0x02a4), /* 29 */
    UINT32_C(0x02a8), /* 30 */
    UINT32_C(0x0364), /* 31 */
    UINT32_C(0x0388), /* 32 */
    UINT32_C(0x03b0), /* 33 */
    UINT32_C(0x03b4), /* 34 */
    UINT32_C(0x03b8), /* 35 */
    UINT32_C(0x03bc), /* 36 */
    UINT32_C(0x03c4), /* 37 */
    UINT32_C(0x03d0), /* 38 */
    UINT32_C(0x03d4), /* 39 */
    UINT32_C(0x03d8), /* 40 */
    UINT32_C(0x03dc), /* 41 */
    UINT32_C(0x03e0), /* 42 */
    UINT32_C(0x03e4), /* 43 */
    UINT32_C(0x03e8), /* 44 */
    UINT32_C(0x03ec), /* 45 */
    UINT32_C(0x03f0), /* 46 */
    UINT32_C(0x03f4), /* 47 */
    UINT32_C(0x03f8), /* 48 */
    UINT32_C(0x03fc), /* 49 */
    UINT32_C(0x0400), /* 50 */
    UINT32_C(0x0404), /* 51 */
    UINT32_C(0x0408), /* 52 */
    UINT32_C(0x040c), /* 53 */
    UINT32_C(0x0410), /* 54 */
    UINT32_C(0x0414), /* 55 */
    UINT32_C(0x0418), /* 56 */
    UINT32_C(0x0420), /* 57 */
    UINT32_C(0x0430), /* 58 */
    UINT32_C(0x0434), /* 59 */
    UINT32_C(0x0514), /* 60 */
    UINT32_C(0x0560), /* 61 */
    UINT32_C(0x0564), /* 62 */
    UINT32_C(0x0568), /* 63 */
    UINT32_C(0x056c), /* 64 */
    UINT32_C(0x05b0), /* 65 */
    UINT32_C(0x0780), /* 66 */
    UINT32_C(0x0a14), /* 67 */
    UINT32_C(0x0a60), /* 68 */
    UINT32_C(0x0a64), /* 69 */
    UINT32_C(0x0a68), /* 70 */
    UINT32_C(0x0a6c), /* 71 */
    UINT32_C(0x0af0), /* 72 */
    UINT32_C(0x0c80), /* 73 */
    UINT32_C(0x1000), /* 74 */
    UINT32_C(0x1004), /* 75 */
    UINT32_C(0x1014), /* 76 */
    UINT32_C(0x1284), /* 77 */
};
static inline int o62m_static_pmic_public_port_index_v2(uint32_t reg)
{
    for (size_t i = 0; i < O62M_STATIC_PMIC_PUBLIC_PORT_COUNT_V2; ++i) {
        if (o62m_static_pmic_public_ports_v2[i] == reg) { return (int)i; }
    }
    return -1;
}
#endif
