/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SD_O62M_HRS_INDEPENDENT_CONFIG_V1_H
#define HW_SD_O62M_HRS_INDEPENDENT_CONFIG_V1_H
#include "hw/sd/o62m-hrs-v1.h"

/* Optional virtual separation of independent host configuration/empty BOOT
 * state from an actual pending CARD_RESET_LINE job. The original core remains
 * the sole owner of all words/jobs. No phase, deadline or completion is forged.
 * The QEMU caller must still validate the actual card's live BOOT configuration
 * before a +0x90 read; this pure layer only knows its explicit profile fields. */
/* Permit only the three original AP independent standard-register words.
 * This predicate neither changes the model nor executes any register handler.
 * Offset is relative to SRS (outer +0x200); normal width/range and each actual
 * handler's constraints remain the caller's responsibility. */
static inline bool o62m_hrs_independent_srs_config_access_v1(
    const O62MHrsV1 *m, bool explicit_independent_enable, uint64_t srs_offset)
{
    uint64_t word = srs_offset & ~UINT64_C(3);
    return explicit_independent_enable && o62m_hrs_valid_v1(m) && m->enabled &&
           m->phase == O62M_HRS_PENDING_V1 &&
           m->job.kind == O62M_HRS_JOB_CARD_RESET_LINE_V1 &&
           (word == UINT64_C(0x28) || word == UINT64_C(0x30) ||
            word == UINT64_C(0x3c));
}

static inline O62MHrsResultV1 o62m_hrs_access_independent_config_v1(
    O62MHrsV1 *m, bool explicit_independent_enable, int64_t now_ns,
    uint64_t offset, unsigned size, bool write, uint64_t *value)
{
    O62MHrsV1 trial;
    bool config_write = write && offset == UINT64_C(4);
    bool boot_read = !write && offset == UINT64_C(0x90);

    if (!explicit_independent_enable || !m ||
        m->phase != O62M_HRS_PENDING_V1 ||
        m->job.kind != O62M_HRS_JOB_CARD_RESET_LINE_V1 ||
        (!config_write && !boot_read)) {
        return o62m_hrs_access_v1(m, now_ns, offset, size, write, value);
    }
    if (!value) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (o62m_hrs_overlap_v1(m, sizeof(*m), value, sizeof(*value))) {
        return O62M_HRS_ALIAS_V1;
    }
    if (!o62m_hrs_valid_v1(m)) { return O62M_HRS_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_HRS_POLICY_REQUIRED_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_HRS_BAD_TIME_V1; }
    if (size != 4 || (config_write && *value > UINT32_MAX)) {
        return O62M_HRS_BAD_ACCESS_V1;
    }
    if (boot_read) {
        if (!m->auto_boot_off || m->media_bootcfg) {
            return O62M_HRS_BOOT_STATE_UNKNOWN_V1;
        }
        /* Empty BootEngine is an independent owner fact, not card-line ready.
         * Read never completes/replaces/reschedules the pending reset job. */
        *value = m->hrs36_opaque;
        return O62M_HRS_OK_V1;
    }
    if (m->accepted_writes == UINT64_MAX) {
        return O62M_HRS_COUNT_OVERFLOW_V1;
    }
    trial = *m;
    trial.hrs01 = (m->hrs01 & UINT32_C(0xff000000)) |
                  ((uint32_t)*value & UINT32_C(0x00ffffff));
    trial.accepted_writes++;
    trial.last_mutation_ns = now_ns;
    if (!o62m_hrs_valid_v1(&trial)) { return O62M_HRS_BAD_STATE_V1; }
    *m = trial;
    return O62M_HRS_OK_V1;
}
#endif
