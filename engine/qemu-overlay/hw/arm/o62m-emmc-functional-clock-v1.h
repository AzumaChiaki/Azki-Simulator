/* Explicit virtual eMMC input-clock availability only. Read existing canonical
 * owners; add no word, seed, clock edges, PLL lock, rate completion or timer.
 * A true result is a declared functional policy, not measured silicon state.
 */
#ifndef O62M_EMMC_FUNCTIONAL_CLOCK_V1_H
#define O62M_EMMC_FUNCTIONAL_CLOCK_V1_H

#include "clock-controls.h"

typedef enum O62MEmmcVirtualSourcePolicyV1 {
    O62M_EMMC_SOURCE_POLICY_NONE_V1 = 0,
    O62M_EMMC_VIRTUAL_SOURCES_DECLARED_V1 = 1
} O62MEmmcVirtualSourcePolicyV1;

static inline bool o62m_emmc_clock_overlap_v1(
    const void *first, size_t first_size, const void *second, size_t second_size)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;
    return a <= b ? b - a < first_size : a - b < second_size;
}

static inline bool o62m_emmc_clock_output_alias_v1(
    const O62MClockControls *owner, const bool *available)
{
    if (!owner) {
        return false;
    }
    if (o62m_emmc_clock_overlap_v1(available, sizeof(*available),
                                  owner, sizeof(*owner))) {
        return true;
    }
    if (owner->defs && owner->count > 0 &&
        owner->count <= O62M_CLOCK_CONTROLS_MAX &&
        owner->count <= SIZE_MAX / sizeof(*owner->defs)) {
        return o62m_emmc_clock_overlap_v1(
            available, sizeof(*available), owner->defs,
            owner->count * sizeof(*owner->defs));
    }
    return false;
}

/* Exact address lookup, never a range alias or an assumed array index. */
static inline int o62m_emmc_clock_owner_index_v1(
    const O62MClockControls *owner, uint64_t address)
{
    int found = -1;
    if (!owner || !owner->enabled || !owner->defs || owner->count == 0 ||
        owner->count > O62M_CLOCK_CONTROLS_MAX) {
        return -1;
    }
    for (size_t i = 0; i < owner->count; ++i) {
        if (owner->defs[i].address == address) {
            if (found >= 0) {
                return -1;
            }
            found = (int)i;
        }
    }
    return found;
}

/* Return value means known. On false, available and both owners stay intact.
 * A valid but gated path returns true with available=false. Unused PLLs need
 * not be initialized. Only an ungated, selected PLL source is inspected.
 */
static inline bool o62m_emmc_functional_clock_query_v1(
    const O62MClockControls *clock335, const O62MClockControls *pll12,
    O62MEmmcVirtualSourcePolicyV1 policy, bool *available)
{
    static const uint64_t clock_addresses[4] = {
        UINT64_C(0x4006001c), UINT64_C(0x400600b4),
        UINT64_C(0x400600b8), UINT64_C(0x400600bc)
    };
    uint32_t word[4];
    unsigned mux;
    uint64_t pll_base;
    int mode_index, post_index, ref_index;
    uint32_t mode_word, post_word, ref_word, n, fraction;
    if (!available ||
        o62m_emmc_clock_output_alias_v1(clock335, available) ||
        o62m_emmc_clock_output_alias_v1(pll12, available)) {
        return false;
    }
    for (unsigned i = 0; i < 4; ++i) {
        int index = o62m_emmc_clock_owner_index_v1(clock335, clock_addresses[i]);
        if (index < 0) {
            return false;
        }
        word[i] = clock335->word[index];
    }
    mux = word[0] & 7u;
    if (mux > 5) {
        return false;
    }
    if (!(word[0] & UINT32_C(0x10000)) ||
        !(word[1] & UINT32_C(0x20000)) ||
        !(word[2] & UINT32_C(0x20000)) ||
        !(word[3] & UINT32_C(0x20000))) {
        *available = false;
        return true;
    }
    if (policy != O62M_EMMC_VIRTUAL_SOURCES_DECLARED_V1) {
        return false;
    }
    if (mux <= 1) {
        /* Declared virtual ap32k or xtal26m; no invented physical availability. */
        *available = true;
        return true;
    }
    pll_base = (mux == 2 || mux == 4)
        ? UINT64_C(0x5a09c200) : UINT64_C(0x5a09c000);
    mode_index = o62m_emmc_clock_owner_index_v1(pll12, pll_base);
    post_index = o62m_emmc_clock_owner_index_v1(pll12, pll_base + 4);
    ref_index = o62m_emmc_clock_owner_index_v1(pll12, pll_base + 8);
    if (mode_index < 0 || post_index < 0 || ref_index < 0) {
        return false;
    }
    mode_word = pll12->word[mode_index];
    post_word = pll12->word[post_index];
    ref_word = pll12->word[ref_index];
    n = (ref_word >> 6) & UINT32_C(0xfff);
    fraction = (mode_word & 8) ? post_word & UINT32_C(0xffffff) : 0;
    /* The original PLL getter uses raw divisors, with no encoded +1. */
    if (!(ref_word & UINT32_C(0x3f)) ||
        !((post_word >> 24) & 7u) || !((post_word >> 27) & 7u) ||
        !(n || fraction)) {
        return false;
    }
    *available = true;
    return true;
}

#endif
