#ifndef O62M_VIRTUAL_DVS_PMIC_PEER_V1_H
#define O62M_VIRTUAL_DVS_PMIC_PEER_V1_H
#include "o62m-virtual-dvs-job-v1.h"
#include "o62m-pmic-regulators-v1.h"

/* Explicit virtual DVSHW agent only. Caller must invoke this after its OWN
 * shared SPMI pending queue reaches a genuine commit deadline, with slave 8
 * selected and the ticket still owned. This helper schedules no bus or timer.
 * Ordinary CPU PMIC-v1 writes to 0240 remain READ_ONLY, including unchanged.
 * This alters canonical command configuration, never fc0 voltage feedback.
 */
static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_pmic_commit_v1(
    O62MVirtualDvsJobV1 *m, O62MPmicRegulatorsV1 *pmic, uint64_t generation,
    uint32_t sid, uint32_t reg, uint32_t payload, int64_t now_ns)
{
    if (!m || !pmic || o62m_virtual_dvs_overlaps_v1(m, sizeof(*m), pmic, sizeof(*pmic))) {
        return O62M_VDVS_V1_BAD_ARGUMENT;
    }
    O62MVirtualDvsResultV1 r = o62m_virtual_dvs_clock_v1(m, now_ns);
    if (r != O62M_VDVS_V1_OK) { return r; }
    if (generation != m->generation) { return O62M_VDVS_V1_STALE_TICKET; }
    if (m->phase != O62M_VDVS_TRANSFER_V1 || m->bus_committed) {
        return O62M_VDVS_V1_BAD_PHASE;
    }
    if (sid != 8u || sid != m->job_sid) { return O62M_VDVS_V1_BAD_SID; }
    if (reg != m->pending_reg || payload != m->pending_payload ||
        reg != UINT32_C(0x0240) || payload > UINT16_MAX ||
        m->job_cmd_a != m->job_cmd_b ||
        !o62m_virtual_dvs_command_valid_v1(m->job_cmd_a) ||
        m->job_cmd_a != ((reg << 16) | payload) ||
        ((m->job_ctrl00 >> 3) & 15u) != sid || m->job_index > 7u ||
        m->pending_mv != 500u + 10u * (payload & 63u)) {
        return O62M_VDVS_V1_TICKET_MISMATCH;
    }
    if (!pmic->initialized) { return O62M_VDVS_V1_PMIC_NOT_INITIALIZED; }
    int index = o62m_pmic_regulator_index_v1(UINT32_C(0x0240));
    if (index < 0 || o62m_pmic_regulator_defs_v1[index].selector_mask != 0u) {
        return O62M_VDVS_V1_PMIC_LAYOUT;
    }
    if (m->settle_ns <= 0 || now_ns > INT64_MAX - m->settle_ns) {
        return O62M_VDVS_V1_DEADLINE_OVERFLOW;
    }
    if (m->committed == UINT64_MAX || pmic->writes == UINT64_MAX ||
        pmic->normal_writes == UINT64_MAX || pmic->word_writes[index] == UINT64_MAX) {
        return O62M_VDVS_V1_COUNTER_OVERFLOW;
    }
    /* All rejection paths precede both mutations; caller supplies serialization. */
    pmic->word[index] = (uint16_t)(((uint32_t)pmic->word[index] & UINT32_C(0x8fc0)) | payload);
    ++pmic->writes;
    ++pmic->normal_writes;
    ++pmic->word_writes[index];
    ++m->committed;
    m->bus_committed = true;
    m->phase = O62M_VDVS_SETTLE_V1;
    m->settle_due_ns = now_ns + m->settle_ns;
    m->last_ns = now_ns;
    return O62M_VDVS_V1_OK;
}
#endif
