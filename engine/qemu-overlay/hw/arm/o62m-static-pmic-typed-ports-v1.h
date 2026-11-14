#ifndef O62M_STATIC_PMIC_TYPED_PORTS_V1_H
#define O62M_STATIC_PMIC_TYPED_PORTS_V1_H
#include <stdint.h>
#include <stddef.h>
/* Classification only; no raw runtime arguments/addresses printed. */
typedef struct {uint16_t reg; uint8_t kind; int8_t owner_index;} O62MStaticPmicTypedPortV1;
#define O62M_STATIC_PMIC_TYPED_PORT_COUNT_V1 50u
static const O62MStaticPmicTypedPortV1 o62m_static_pmic_typed_ports_v1[] = {
    {0x0040u, 4u, -1},
    {0x0044u, 4u, -1},
    {0x0048u, 4u, -1},
    {0x0050u, 5u, -1},
    {0x0060u, 1u, 0},
    {0x0064u, 1u, 1},
    {0x0068u, 1u, 2},
    {0x006cu, 1u, 3},
    {0x00f0u, 4u, -1},
    {0x0240u, 1u, 4},
    {0x0244u, 1u, 5},
    {0x024cu, 1u, 6},
    {0x0254u, 1u, 7},
    {0x0258u, 1u, 8},
    {0x0364u, 1u, 9},
    {0x0388u, 1u, 10},
    {0x03b0u, 1u, 11},
    {0x03b4u, 1u, 12},
    {0x03b8u, 1u, 13},
    {0x03bcu, 1u, 14},
    {0x03c4u, 1u, 15},
    {0x03d0u, 1u, 16},
    {0x03d4u, 1u, 17},
    {0x03d8u, 1u, 18},
    {0x03dcu, 1u, 19},
    {0x03e0u, 1u, 20},
    {0x03e4u, 1u, 21},
    {0x03e8u, 1u, 22},
    {0x03ecu, 1u, 23},
    {0x03f0u, 1u, 24},
    {0x03f4u, 1u, 25},
    {0x03f8u, 1u, 26},
    {0x03fcu, 1u, 27},
    {0x0400u, 1u, 28},
    {0x0404u, 1u, 29},
    {0x0408u, 1u, 30},
    {0x040cu, 1u, 31},
    {0x0410u, 1u, 32},
    {0x0414u, 1u, 33},
    {0x0418u, 1u, 34},
    {0x0420u, 1u, 35},
    {0x0560u, 2u, 0},
    {0x0564u, 2u, 1},
    {0x0568u, 2u, 2},
    {0x056cu, 2u, 3},
    {0x0a60u, 3u, 0},
    {0x0a64u, 3u, 1},
    {0x0a68u, 3u, 2},
    {0x0a6cu, 3u, 3},
    {0x1284u, 1u, 36},
};
static inline int o62m_static_pmic_typed_port_index_v1(uint32_t reg)
{
    for(size_t i=0;i<O62M_STATIC_PMIC_TYPED_PORT_COUNT_V1;++i) {
        if((uint32_t)o62m_static_pmic_typed_ports_v1[i].reg==reg) {return (int)i;}
    }
    return -1;
}
#endif
