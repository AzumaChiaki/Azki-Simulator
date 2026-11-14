/* SPDX-License-Identifier: MIT
 * Finite public-original Thumb BL encoding candidates to 0x60c4d0b8.
 * Static classification only: no memory reads or generic stack unwinding.
 * Caller must establish the exact direct wrapper role before using a token.
 */
#ifndef O62M_STATIC_SPMI_WRAPPER_CALLERS_V1_H
#define O62M_STATIC_SPMI_WRAPPER_CALLERS_V1_H
#include <stdint.h>
#define O62M_STATIC_SPMI_WRAPPER_CALLERS_V1_COUNT 54u
static inline int o62m_static_spmi_wrapper_caller_index_v1(uint32_t saved_return)
{
    static const uint32_t returns[O62M_STATIC_SPMI_WRAPPER_CALLERS_V1_COUNT]={
        UINT32_C(0x609d8051),UINT32_C(0x609d80d1),UINT32_C(0x609da7cb),UINT32_C(0x609da7e3),UINT32_C(0x609e1857),UINT32_C(0x60c4d14b),
        UINT32_C(0x60c4d1c7),UINT32_C(0x60c4d20d),UINT32_C(0x60c4d21f),UINT32_C(0x60c4d233),UINT32_C(0x60c4d243),UINT32_C(0x60c4d60b),
        UINT32_C(0x60c4d61d),UINT32_C(0x60c4d63f),UINT32_C(0x60c4d64b),UINT32_C(0x60c4d665),UINT32_C(0x60c4d679),UINT32_C(0x60c4d6cf),
        UINT32_C(0x60c4d6e5),UINT32_C(0x60c4d72b),UINT32_C(0x60c4d76d),UINT32_C(0x60c4d7a9),UINT32_C(0x60c4d7ef),UINT32_C(0x60c4d819),
        UINT32_C(0x60c4da69),UINT32_C(0x60c4df47),UINT32_C(0x60c4df51),UINT32_C(0x60c53d91),UINT32_C(0x60c54c6f),UINT32_C(0x60c54c79),
        UINT32_C(0x60c54c83),UINT32_C(0x60c54c8d),UINT32_C(0x60c54ca1),UINT32_C(0x60c54cb9),UINT32_C(0x60c54cd1),UINT32_C(0x60c54ce9),
        UINT32_C(0x60c54cf9),UINT32_C(0x60c54d05),UINT32_C(0x60c54d17),UINT32_C(0x60c54d2b),UINT32_C(0x60c54d33),UINT32_C(0x60c54d55),
        UINT32_C(0x60c54da9),UINT32_C(0x60c54db7),UINT32_C(0x60c54dd1),UINT32_C(0x60c54ddd),UINT32_C(0x60c55017),UINT32_C(0x60c5540b),
        UINT32_C(0x60c5f8e1),UINT32_C(0x60dade59),UINT32_C(0x60dc0b8b),UINT32_C(0x60dc0bcd),UINT32_C(0x60dcc0af),UINT32_C(0x60dd6213),
    };
    for(unsigned i=0;i<O62M_STATIC_SPMI_WRAPPER_CALLERS_V1_COUNT;i++)
        if(saved_return==returns[i])return (int)i;
    return -1;
}
#endif
