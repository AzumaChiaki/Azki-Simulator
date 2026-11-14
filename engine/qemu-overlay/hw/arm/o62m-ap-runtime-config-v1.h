/* Exact original AP config table, bounded clockloop and timing config ports.
 * Explicit virtual retained configuration only: no silicon reset/ready/clock
 * output semantics. Existing335 ports excluded: single canonical owner.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef O62M_AP_RUNTIME_CONFIG_V1_H
#define O62M_AP_RUNTIME_CONFIG_V1_H
#include <stdint.h>
#include <stdbool.h>
#define O62M_AP_RUNTIME_CONFIG_WORDS_V1 67u
static const uint32_t o62m_ap_runtime_config_addresses_v1[O62M_AP_RUNTIME_CONFIG_WORDS_V1]={
    UINT32_C(0x40050020),
    UINT32_C(0x40050024),
    UINT32_C(0x40050028),
    UINT32_C(0x4005002c),
    UINT32_C(0x40050030),
    UINT32_C(0x40050034),
    UINT32_C(0x40050038),
    UINT32_C(0x4005003c),
    UINT32_C(0x40050040),
    UINT32_C(0x40050044),
    UINT32_C(0x40050048),
    UINT32_C(0x4005004c),
    UINT32_C(0x40050050),
    UINT32_C(0x40050054),
    UINT32_C(0x40050058),
    UINT32_C(0x4005005c),
    UINT32_C(0x40050060),
    UINT32_C(0x40050064),
    UINT32_C(0x40050068),
    UINT32_C(0x4005006c),
    UINT32_C(0x40050070),
    UINT32_C(0x40050074),
    UINT32_C(0x40050078),
    UINT32_C(0x4005007c),
    UINT32_C(0x40050088),
    UINT32_C(0x4005008c),
    UINT32_C(0x40050090),
    UINT32_C(0x40050094),
    UINT32_C(0x40050098),
    UINT32_C(0x400500d8),
    UINT32_C(0x400500dc),
    UINT32_C(0x400500e0),
    UINT32_C(0x4a000098),
    UINT32_C(0x4a00009c),
    UINT32_C(0x4a0000a0),
    UINT32_C(0x4a0000a4),
    UINT32_C(0x4a0000a8),
    UINT32_C(0x4a0000ac),
    UINT32_C(0x4a0000b0),
    UINT32_C(0x4a000120),
    UINT32_C(0x4a000124),
    UINT32_C(0x4a000200),
    UINT32_C(0x4a000250),
    UINT32_C(0x4a00025c),
    UINT32_C(0x4a000268),
    UINT32_C(0x4a000280),
    UINT32_C(0x4a098000),
    UINT32_C(0x4a098004),
    UINT32_C(0x4a098108),
    UINT32_C(0x4a0c0084),
    UINT32_C(0x4a0c0088),
    UINT32_C(0x4a0c008c),
    UINT32_C(0x4a0c0090),
    UINT32_C(0x4a0c0094),
    UINT32_C(0x4a0c0098),
    UINT32_C(0x4a0c009c),
    UINT32_C(0x4a0c00a0),
    UINT32_C(0x4a0c00a4),
    UINT32_C(0x4a0c00a8),
    UINT32_C(0x4a0c00ac),
    UINT32_C(0x4a0c00b0),
    UINT32_C(0x4a0c00b4),
    UINT32_C(0x4a0c00b8),
    UINT32_C(0x4a0c00bc),
    UINT32_C(0x4a0c00e4),
    UINT32_C(0x4a0c00e8),
    UINT32_C(0x4a0c00ec),
};
static inline bool o62m_ap_runtime_clear_v1(uint64_t address,uint64_t pc,uint32_t before,uint64_t value)
{
    if(value>UINT32_MAX)return false;
    uint32_t mask=0;
    if(pc==UINT64_C(0x60dbfbc8)&&address>=UINT64_C(0x4a098000)&&address<=UINT64_C(0x4a0981e0)&&!(address&3))mask=2;
    if(pc==UINT64_C(0x60dbfcb0)) {
        switch(address) {
        case UINT64_C(0x40060010):mask=UINT32_C(0x100000);break;
        case UINT64_C(0x40060014):mask=UINT32_C(0x100000);break;
        case UINT64_C(0x40060160):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a0900a0):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a090124):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a0900f8):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a0900b0):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a090164):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a090168):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a090178):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a090170):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a09016c):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a090174):mask=UINT32_C(0x200000);break;
        case UINT64_C(0x4a09017c):mask=UINT32_C(0x200000);break;
}
    }
    return mask&&value==(before&~mask);
}
#endif
