#ifndef O62M_AON_PREDIV_QUERY_H
#define O62M_AON_PREDIV_QUERY_H

/* Original registration-query trace only; no PLL, gate/clock progress or ready. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define O62M_AON_PREDIV_QUERY_COUNT 26u

typedef enum O62MAonPredivQueryResult {
    O62M_AON_PREDIV_QUERY_OK = 0,
    O62M_AON_PREDIV_QUERY_BAD_ARGUMENT,
    O62M_AON_PREDIV_QUERY_BAD_SEEDS,
    O62M_AON_PREDIV_QUERY_DISABLED,
    O62M_AON_PREDIV_QUERY_BAD_ACCESS,
    O62M_AON_PREDIV_QUERY_UNKNOWN_ADDRESS,
    O62M_AON_PREDIV_QUERY_SEQUENCE_COMPLETE,
    O62M_AON_PREDIV_QUERY_WRONG_PHASE
} O62MAonPredivQueryResult;

typedef struct O62MAonPredivQueryProjection {
    bool enabled;
    /* Accepted queries, 0..26. Experiment trace, not a hardware read effect. */
    uint8_t phase;
    uint32_t word[O62M_AON_PREDIV_QUERY_COUNT];
} O62MAonPredivQueryProjection;

/* Firmware registrationQueryOrder, deliberately not numerically sorted. */
static inline uint64_t o62m_aon_prediv_query_address(unsigned index)
{
    static const uint64_t addresses[O62M_AON_PREDIV_QUERY_COUNT] = {
        UINT64_C(0x5a098144), UINT64_C(0x5a098018),
        UINT64_C(0x5a09801c), UINT64_C(0x5a098020),
        UINT64_C(0x5a098024), UINT64_C(0x5a098028),
        UINT64_C(0x5a09802c), UINT64_C(0x5a098030),
        UINT64_C(0x5a098034), UINT64_C(0x5a09803c),
        UINT64_C(0x5a098040), UINT64_C(0x5a098038),
        UINT64_C(0x5a098044), UINT64_C(0x5a098048),
        UINT64_C(0x5a09804c), UINT64_C(0x5a098050),
        UINT64_C(0x5a098054), UINT64_C(0x5a098058),
        UINT64_C(0x5a09805c), UINT64_C(0x5a098060),
        UINT64_C(0x5a098064), UINT64_C(0x5a098068),
        UINT64_C(0x5a09806c), UINT64_C(0x5a098070),
        UINT64_C(0x5a09809c), UINT64_C(0x5a0980a0)
    };
    return index < O62M_AON_PREDIV_QUERY_COUNT ? addresses[index] : UINT64_MAX;
}

/*
 * Default-off. Enabled start requires all 26 independently supplied full-u32
 * words, in query order; each address is unique. Explicit zeros/odd bit0 legal.
 * Typed u32 inputs must be range-checked by any wider host parser before call.
 * No implicit initial values. Invalid init preserves the old experiment.
 * Disabled init clears it; enabled init explicitly starts a new trace.
 * This host lifecycle is not hardware reset behavior or a reset-value claim.
 */
static inline O62MAonPredivQueryResult o62m_aon_prediv_query_init(
    O62MAonPredivQueryProjection *state, bool enabled,
    const uint32_t *seeds, size_t seed_count)
{
    if (state == NULL) {
        return O62M_AON_PREDIV_QUERY_BAD_ARGUMENT;
    }
    if (enabled && (seeds == NULL || seed_count != O62M_AON_PREDIV_QUERY_COUNT)) {
        return O62M_AON_PREDIV_QUERY_BAD_SEEDS;
    }
    O62MAonPredivQueryProjection next = {0};
    next.enabled = enabled;
    if (enabled) {
        for (unsigned i = 0; i < O62M_AON_PREDIV_QUERY_COUNT; ++i) {
            next.word[i] = seeds[i];
        }
    }
    *state = next;
    return O62M_AON_PREDIV_QUERY_OK;
}

/*
 * Only exact secure-address whitelist, aligned32, original order, once each.
 * Returns the entire explicit word unchanged; advances only the trace phase.
 * No write API. Root must strictly reject every write, never skip/synthesize it.
 * Root must make this group mutually exclusive with the one-off144 projection.
 * Valid initialized state and independent output required; no concurrent calls.
 * Every error preserves all state fields and readout.
 */
static inline O62MAonPredivQueryResult o62m_aon_prediv_query_read(
    O62MAonPredivQueryProjection *state, uint64_t address,
    unsigned size, uint32_t *readout)
{
    if (state == NULL || readout == NULL) {
        return O62M_AON_PREDIV_QUERY_BAD_ARGUMENT;
    }
    if (!state->enabled) {
        return O62M_AON_PREDIV_QUERY_DISABLED;
    }
    if (size != 4 || (address & UINT64_C(3)) != 0) {
        return O62M_AON_PREDIV_QUERY_BAD_ACCESS;
    }
    unsigned index = O62M_AON_PREDIV_QUERY_COUNT;
    for (unsigned i = 0; i < O62M_AON_PREDIV_QUERY_COUNT; ++i) {
        if (address == o62m_aon_prediv_query_address(i)) {
            index = i;
            break;
        }
    }
    if (index == O62M_AON_PREDIV_QUERY_COUNT) {
        return O62M_AON_PREDIV_QUERY_UNKNOWN_ADDRESS;
    }
    if (state->phase >= O62M_AON_PREDIV_QUERY_COUNT) {
        return O62M_AON_PREDIV_QUERY_SEQUENCE_COMPLETE;
    }
    if (index != state->phase) {
        return O62M_AON_PREDIV_QUERY_WRONG_PHASE;
    }
    const uint32_t value = state->word[index];
    ++state->phase;
    *readout = value;
    return O62M_AON_PREDIV_QUERY_OK;
}

static inline const char *o62m_aon_prediv_query_reason(O62MAonPredivQueryResult result)
{
    static const char *const reasons[] = {
        "ok", "bad-argument", "missing-or-wrong-count-seeds", "disabled",
        "bad-width-or-alignment", "unknown-address", "sequence-complete",
        "wrong-query-phase"
    };
    return (unsigned)result < sizeof(reasons) / sizeof(reasons[0])
        ? reasons[result] : "unknown-error";
}

#endif
