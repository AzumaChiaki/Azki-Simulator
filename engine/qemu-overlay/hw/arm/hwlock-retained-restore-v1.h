#ifndef O62M_HWLOCK_RETAINED_RESTORE_V1_H
#define O62M_HWLOCK_RETAINED_RESTORE_V1_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "hwspinlock-model.h"

/* Canonical retained-state decoder only. The BSP separately pins capsuleSHA.
 * Layout88bytes LE: <8s6I2q4Qq>, magicO62MHLK1; all originalmodel fields plus
 * capturevirtualns. Only a completed quiescentreset is supported here.
 * History is rebased to a new virtualepoch0 by oldtime-capturetime, retaining
 * negative timestamps exactly. Existing ownership/reset operations unchanged.
 */
#define O62M_HWLOCK_CAPSULE_BYTES 88u

typedef enum O62MHwLockRestoreResult {
    O62M_HWLOCK_RESTORE_OK = 0,
    O62M_HWLOCK_RESTORE_BAD_ARGUMENT,
    O62M_HWLOCK_RESTORE_BAD_LENGTH,
    O62M_HWLOCK_RESTORE_BAD_MAGIC,
    O62M_HWLOCK_RESTORE_SEED_MISMATCH,
    O62M_HWLOCK_RESTORE_BAD_RESET_PHASE,
    O62M_HWLOCK_RESTORE_BAD_OWNERS,
    O62M_HWLOCK_RESTORE_BAD_SOURCE_TIME
} O62MHwLockRestoreResult;

static inline const char *o62m_hwlock_restore_reason(O62MHwLockRestoreResult result)
{
    switch (result) {
    case O62M_HWLOCK_RESTORE_OK: return "ok";
    case O62M_HWLOCK_RESTORE_BAD_ARGUMENT: return "bad-argument";
    case O62M_HWLOCK_RESTORE_BAD_LENGTH: return "canonical88byte-length-required";
    case O62M_HWLOCK_RESTORE_BAD_MAGIC: return "capsule-magic-or-version-mismatch";
    case O62M_HWLOCK_RESTORE_SEED_MISMATCH: return "explore-config-seed-mismatch";
    case O62M_HWLOCK_RESTORE_BAD_RESET_PHASE: return "completed-quiescent-reset-required";
    case O62M_HWLOCK_RESTORE_BAD_OWNERS: return "lock0to30-disjoint-owners-required";
    case O62M_HWLOCK_RESTORE_BAD_SOURCE_TIME: return "nonnegative-ordered-source-times-required";
    }
    return "invalid-result";
}

static inline uint32_t o62m_hwlock_capsule_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static inline uint64_t o62m_hwlock_capsule_le64(const uint8_t *bytes)
{
    return (uint64_t)o62m_hwlock_capsule_le32(bytes) |
           ((uint64_t)o62m_hwlock_capsule_le32(bytes + 4) << 32);
}

/* Any failure leaves all destinationmodel bytes unchanged. Source signed
 * fields must be nonnegative, so validate signbits in the rawu64 form before
 * converting; no implementation-defined negativeu64->i64 casts are needed.
 * No opaqueconfigbits or telemetry counters are truncated/reinitialized.
 */
static inline O62MHwLockRestoreResult o62m_hwlock_restore(O62MHwSpinlock *model,
                                                         const uint8_t *blob,
                                                         size_t length,
                                                         uint32_t expected_seed)
{
    static const uint8_t magic[8] = {'O', '6', '2', 'M', 'H', 'L', 'K', '1'};
    O62MHwSpinlock next = {0};
    uint32_t asserted, complete;
    uint64_t assert_time, release_time, capture_time;

    if (!model || !blob) {
        return O62M_HWLOCK_RESTORE_BAD_ARGUMENT;
    }
    if (length != O62M_HWLOCK_CAPSULE_BYTES) {
        return O62M_HWLOCK_RESTORE_BAD_LENGTH;
    }
    if (memcmp(blob, magic, sizeof(magic)) != 0) {
        return O62M_HWLOCK_RESTORE_BAD_MAGIC;
    }
    next.config_seed = o62m_hwlock_capsule_le32(blob + 8);
    next.config = o62m_hwlock_capsule_le32(blob + 12);
    next.owned = o62m_hwlock_capsule_le32(blob + 16);
    next.external_owned = o62m_hwlock_capsule_le32(blob + 20);
    asserted = o62m_hwlock_capsule_le32(blob + 24);
    complete = o62m_hwlock_capsule_le32(blob + 28);
    assert_time = o62m_hwlock_capsule_le64(blob + 32);
    release_time = o62m_hwlock_capsule_le64(blob + 40);
    next.reset_count = o62m_hwlock_capsule_le64(blob + 48);
    next.acquisitions = o62m_hwlock_capsule_le64(blob + 56);
    next.conflicts = o62m_hwlock_capsule_le64(blob + 64);
    next.releases = o62m_hwlock_capsule_le64(blob + 72);
    capture_time = o62m_hwlock_capsule_le64(blob + 80);
    if (next.config_seed != expected_seed) {
        return O62M_HWLOCK_RESTORE_SEED_MISMATCH;
    }
    if (asserted != 0u || complete != 1u || next.reset_count == 0) {
        return O62M_HWLOCK_RESTORE_BAD_RESET_PHASE;
    }
    if (((next.owned | next.external_owned) & UINT32_C(0x80000000)) ||
        (next.owned & next.external_owned)) {
        return O62M_HWLOCK_RESTORE_BAD_OWNERS;
    }
    if (assert_time > INT64_MAX || release_time > INT64_MAX ||
        capture_time > INT64_MAX || assert_time > release_time ||
        release_time > capture_time) {
        return O62M_HWLOCK_RESTORE_BAD_SOURCE_TIME;
    }
    next.reset_asserted = false;
    next.reset_complete = true;
    next.asserted_ns = (int64_t)assert_time - (int64_t)capture_time;
    next.released_ns = (int64_t)release_time - (int64_t)capture_time;
    *model = next;
    return O62M_HWLOCK_RESTORE_OK;
}

#endif
