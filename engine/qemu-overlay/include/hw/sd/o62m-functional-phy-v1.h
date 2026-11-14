/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SD_O62M_FUNCTIONAL_PHY_V1_H
#define HW_SD_O62M_FUNCTIONAL_PHY_V1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Explicit functional PHY, not an analog DLL or an O62M reset-value claim.
 * Six real finite parameter words feed a captured initialization transaction.
 * apply_due performs the copy into the active private configuration itself;
 * callers cannot supply success/READY. No OS, I/O, timer or autonomous progress
 * occurs here. The adapter must supply actual monotonic time/reference clock,
 * serialize calls and retire its timer when assertion cancels the owned job.
 * Original data commands must be gated by ready(), not merely by reset release.
 * 0x2080 and undocumented word subfields are opaque virtual inputs. No whole
 * firmware word is compared to a magic allowed value to manufacture readiness.
 */
#define O62M_PHY_MAGIC_V1 UINT32_C(0x50485931)
#define O62M_PHY_WORDS_V1 6u
#define O62M_PHY_PREF_MASK_V1 UINT32_C(0x3e)
#define O62M_PHY_HRS09_RW_V1 UINT32_C(0xffc1800d)

typedef enum O62MPhyResultV1 {
    O62M_PHY_OK_V1 = 0,
    O62M_PHY_BAD_ARGUMENT_V1, O62M_PHY_ALIAS_V1,
    O62M_PHY_POLICY_REQUIRED_V1, O62M_PHY_BAD_INITIAL_V1,
    O62M_PHY_BAD_STATE_V1, O62M_PHY_BAD_TIME_V1,
    O62M_PHY_BAD_ACCESS_V1, O62M_PHY_UNKNOWN_REGISTER_V1,
    O62M_PHY_UNKNOWN_ADDRESS_V1, O62M_PHY_ADDRESS_NOT_SELECTED_V1,
    O62M_PHY_BAD_CONFIG_V1, O62M_PHY_RESET_REQUIRED_V1,
    O62M_PHY_PENDING_V1, O62M_PHY_MISSING_WRITES_V1,
    O62M_PHY_COUNTER_OVERFLOW_V1, O62M_PHY_DEADLINE_OVERFLOW_V1,
    O62M_PHY_STALE_JOB_V1, O62M_PHY_NOT_DUE_V1, O62M_PHY_CLOCK_OFF_V1
} O62MPhyResultV1;

typedef enum O62MPhyPhaseV1 {
    O62M_PHY_HELD_V1 = 0, O62M_PHY_UNINITIALIZED_V1,
    O62M_PHY_PENDING_PHASE_V1, O62M_PHY_READY_V1
} O62MPhyPhaseV1;

typedef struct O62MPhySpecV1 {
    bool explicit_enable, idealized_apply_enable;
    /* Original normal AP changes DLL/clock while reusing genuinely programmed
     * retained parameters. Default false preserves the v5 fresh-write policy. */
    bool retained_parameter_reuse_enable;
    uint32_t hrs04_initial_fullword, hrs07_initial_fullword;
    uint32_t hrs09_initial_fullword, hrs10_initial_fullword;
    uint32_t hrs16_initial_fullword;
    /* Exact indices: 2000,2004,2008,200c,2010,2080. */
    uint32_t phy_initial[O62M_PHY_WORDS_V1];
    uint64_t init_latency_ns, instance_id;
    int64_t now_ns;
} O62MPhySpecV1;

typedef struct O62MPhyJobV1 {
    uint64_t instance_id, generation;
    int64_t deadline_ns;
} O62MPhyJobV1;

typedef struct O62MPhyV1 {
    uint32_t magic;
    bool enabled, idealized_apply, address_selected;
    bool retained_parameter_reuse;
    O62MPhyPhaseV1 phase;
    /* HRS09.bit1 is always zero in storage; only READY projects it as one. */
    uint32_t hrs04, hrs07, hrs09, hrs10, hrs16;
    uint32_t backing[O62M_PHY_WORDS_V1];
    uint32_t pending_bank[O62M_PHY_WORDS_V1];
    uint32_t active_bank[O62M_PHY_WORDS_V1];
    uint32_t written_mask;
    uint64_t init_latency_ns, instance_id, generation;
    uint64_t accepted_writes, parameter_writes, held_assertions;
    uint64_t jobs_started, jobs_completed, jobs_cancelled;
    int64_t origin_ns, last_mutation_ns, job_start_ns;
    O62MPhyJobV1 job;
} O62MPhyV1;

static inline bool o62m_phy_overlap_v1(const void *a, size_t an,
                                      const void *b, size_t bn)
{
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    if (an > UINTPTR_MAX - ap || bn > UINTPTR_MAX - bp) { return true; }
    return an && bn && ap < bp + bn && bp < ap + an;
}

static inline int o62m_phy_index_v1(uint32_t address)
{
    switch (address) {
    case 0x2000: return 0;
    case 0x2004: return 1;
    case 0x2008: return 2;
    case 0x200c: return 3;
    case 0x2010: return 4;
    case 0x2080: return 5;
    default: return -1;
    }
}

static inline bool o62m_phy_zero_bank_v1(const uint32_t *bank)
{
    for (unsigned i = 0; i < O62M_PHY_WORDS_V1; i++) {
        if (bank[i]) { return false; }
    }
    return true;
}

static inline bool o62m_phy_valid_v1(const O62MPhyV1 *m)
{
    uint64_t retired;
    uint32_t address;
    if (!m || m->magic != O62M_PHY_MAGIC_V1 || !m->enabled ||
        !m->idealized_apply || !m->instance_id || !m->init_latency_ns ||
        m->init_latency_ns > INT64_MAX || !m->generation ||
        m->origin_ns < 0 || m->last_mutation_ns < m->origin_ns ||
        (m->hrs09 & 2) || ((m->hrs09 >> 22) & 63) > 47 ||
        (m->written_mask & ~O62M_PHY_PREF_MASK_V1) ||
        (unsigned)m->phase > (unsigned)O62M_PHY_READY_V1 ||
        m->parameter_writes > m->accepted_writes ||
        m->held_assertions > m->accepted_writes ||
        m->jobs_started > m->accepted_writes ||
        m->jobs_completed > UINT64_MAX - m->jobs_cancelled ||
        m->jobs_started > UINT64_MAX - m->held_assertions ||
        m->jobs_started + m->held_assertions == UINT64_MAX ||
        m->generation != 1 + m->jobs_started + m->held_assertions) {
        return false;
    }
    address = m->hrs04 & UINT32_C(0xffff);
    if ((address && o62m_phy_index_v1(address) < 0) ||
        (m->address_selected && o62m_phy_index_v1(address) < 0)) {
        return false;
    }
    retired = m->jobs_completed + m->jobs_cancelled;
    if (m->phase == O62M_PHY_PENDING_PHASE_V1) {
        if (!(m->hrs09 & 1) || retired == UINT64_MAX ||
            m->jobs_started != retired + 1 ||
            m->written_mask != O62M_PHY_PREF_MASK_V1 ||
            m->job.instance_id != m->instance_id ||
            m->job.generation != m->generation ||
            m->job_start_ns < m->origin_ns ||
            m->job_start_ns > m->last_mutation_ns ||
            m->init_latency_ns > (uint64_t)(INT64_MAX - m->job_start_ns) ||
            m->job.deadline_ns != m->job_start_ns + (int64_t)m->init_latency_ns ||
            memcmp(m->pending_bank, m->backing, sizeof(m->backing))) {
            return false;
        }
    } else {
        if (m->jobs_started != retired || m->job_start_ns ||
            m->job.instance_id || m->job.generation || m->job.deadline_ns ||
            !o62m_phy_zero_bank_v1(m->pending_bank)) {
            return false;
        }
    }
    if (m->phase == O62M_PHY_READY_V1) {
        if (!(m->hrs09 & 1) || !m->jobs_completed ||
            m->written_mask != O62M_PHY_PREF_MASK_V1 ||
            memcmp(m->active_bank, m->backing, sizeof(m->backing))) {
            return false;
        }
    } else if (!o62m_phy_zero_bank_v1(m->active_bank)) {
        return false;
    }
    if (m->phase == O62M_PHY_HELD_V1 && (m->hrs09 & 1)) { return false; }
    if (m->phase == O62M_PHY_UNINITIALIZED_V1 &&
        (!(m->hrs09 & 1) || m->written_mask || m->jobs_started)) {
        return false;
    }
    return true;
}

static inline O62MPhyResultV1 o62m_phy_init_v1(O62MPhyV1 *m,
                                              const O62MPhySpecV1 *s)
{
    O62MPhyV1 t;
    uint32_t address;
    if (!m || !s) { return O62M_PHY_BAD_ARGUMENT_V1; }
    if (o62m_phy_overlap_v1(m, sizeof(*m), s, sizeof(*s))) {
        return O62M_PHY_ALIAS_V1;
    }
    if (!s->explicit_enable || !s->idealized_apply_enable) {
        return O62M_PHY_POLICY_REQUIRED_V1;
    }
    address = s->hrs04_initial_fullword & UINT32_C(0xffff);
    if ((s->hrs09_initial_fullword & 2) ||
        ((s->hrs09_initial_fullword >> 22) & 63) > 47 ||
        (address && o62m_phy_index_v1(address) < 0) ||
        !s->init_latency_ns || s->init_latency_ns > INT64_MAX ||
        !s->instance_id || s->now_ns < 0) {
        return O62M_PHY_BAD_INITIAL_V1;
    }
    memset(&t, 0, sizeof(t));
    t.magic = O62M_PHY_MAGIC_V1;
    t.enabled = t.idealized_apply = true;
    t.retained_parameter_reuse = s->retained_parameter_reuse_enable;
    t.hrs04 = s->hrs04_initial_fullword;
    t.hrs07 = s->hrs07_initial_fullword;
    t.hrs09 = s->hrs09_initial_fullword;
    t.hrs10 = s->hrs10_initial_fullword;
    t.hrs16 = s->hrs16_initial_fullword;
    memcpy(t.backing, s->phy_initial, sizeof(t.backing));
    /* Even an explicitly named initial address is not a software port-select
     * operation: HRS05 requires an actual accepted HRS04 write first. */
    t.address_selected = false;
    t.phase = (t.hrs09 & 1) ? O62M_PHY_UNINITIALIZED_V1 : O62M_PHY_HELD_V1;
    t.init_latency_ns = s->init_latency_ns;
    t.instance_id = s->instance_id;
    t.generation = 1;
    t.origin_ns = t.last_mutation_ns = s->now_ns;
    if (!o62m_phy_valid_v1(&t)) { return O62M_PHY_BAD_INITIAL_V1; }
    *m = t;
    return O62M_PHY_OK_V1;
}

static inline void o62m_phy_clear_job_v1(O62MPhyV1 *m)
{
    memset(&m->job, 0, sizeof(m->job));
    memset(m->pending_bank, 0, sizeof(m->pending_bank));
    m->job_start_ns = 0;
}

/* Exact offsets relative to HRS base: 10/14 ports,1c/24/28/40 controls.
 * Every rejection leaves model and caller output unchanged. */
static inline O62MPhyResultV1 o62m_phy_access_v1(
    O62MPhyV1 *m, int64_t now_ns, uint64_t offset, unsigned size,
    bool write, uint64_t *value)
{
    O62MPhyV1 t;
    uint32_t word, mask;
    int index;
    if (!m || !value) { return O62M_PHY_BAD_ARGUMENT_V1; }
    if (o62m_phy_overlap_v1(m, sizeof(*m), value, sizeof(*value))) {
        return O62M_PHY_ALIAS_V1;
    }
    if (!o62m_phy_valid_v1(m)) { return O62M_PHY_BAD_STATE_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_PHY_BAD_TIME_V1; }
    if (size != 4 || (offset & 3) || (write && *value > UINT32_MAX)) {
        return O62M_PHY_BAD_ACCESS_V1;
    }
    switch (offset) {
    case 0x10: word = m->hrs04; mask = UINT32_C(0xffff); break;
    case 0x14:
        if (!m->address_selected) { return O62M_PHY_ADDRESS_NOT_SELECTED_V1; }
        index = o62m_phy_index_v1(m->hrs04 & UINT32_C(0xffff));
        if (index < 0) { return O62M_PHY_UNKNOWN_ADDRESS_V1; }
        word = m->backing[index]; mask = UINT32_MAX; break;
    case 0x1c: word = m->hrs07; mask = UINT32_C(0x001f001f); break;
    case 0x24:
        word = m->hrs09 | (m->phase == O62M_PHY_READY_V1 ? 2u : 0u);
        mask = O62M_PHY_HRS09_RW_V1; break;
    case 0x28: word = m->hrs10; mask = UINT32_C(0x000f0000); break;
    case 0x40: word = m->hrs16; mask = UINT32_C(0x0f0f0f0f); break;
    default: return O62M_PHY_UNKNOWN_REGISTER_V1;
    }
    if (!write) {
        *value = word;
        return O62M_PHY_OK_V1;
    }
    if (m->accepted_writes == UINT64_MAX) {
        return O62M_PHY_COUNTER_OVERFLOW_V1;
    }
    word = (word & ~mask) | ((uint32_t)*value & mask);
    t = *m;
    switch (offset) {
    case 0x10:
        if (o62m_phy_index_v1(word & UINT32_C(0xffff)) < 0) {
            return O62M_PHY_UNKNOWN_ADDRESS_V1;
        }
        t.hrs04 = word;
        t.address_selected = true;
        break;
    case 0x14:
        index = o62m_phy_index_v1(m->hrs04 & UINT32_C(0xffff));
        if (m->phase == O62M_PHY_PENDING_PHASE_V1) { return O62M_PHY_PENDING_V1; }
        if (m->phase != O62M_PHY_HELD_V1 &&
            !(m->phase == O62M_PHY_READY_V1 && index == 0)) {
            return O62M_PHY_RESET_REQUIRED_V1;
        }
        if (m->parameter_writes == UINT64_MAX) {
            return O62M_PHY_COUNTER_OVERFLOW_V1;
        }
        t.backing[index] = word;
        t.parameter_writes++;
        if (index != 0) { t.written_mask |= UINT32_C(1) << index; }
        if (m->phase == O62M_PHY_READY_V1) {
            t.active_bank[0] = word; /* Actual post-init DQ configuration copy. */
        }
        break;
    case 0x1c: t.hrs07 = word; break;
    case 0x28: t.hrs10 = word; break;
    case 0x40: t.hrs16 = word; break;
    case 0x24:
        word &= ~UINT32_C(2); /* Ignore writes to the derived RO complete bit. */
        if (((word >> 22) & 63) > 47) { return O62M_PHY_BAD_CONFIG_V1; }
        if (!(word & 1) && (m->hrs09 & 1)) {
            if (m->generation == UINT64_MAX || m->held_assertions == UINT64_MAX ||
                (m->phase == O62M_PHY_PENDING_PHASE_V1 &&
                 m->jobs_cancelled == UINT64_MAX)) {
                return O62M_PHY_COUNTER_OVERFLOW_V1;
            }
            t.generation++;
            t.held_assertions++;
            if (m->phase == O62M_PHY_PENDING_PHASE_V1) { t.jobs_cancelled++; }
            t.phase = O62M_PHY_HELD_V1;
            /* DLL assertion clears active calibration, not the retained
             * parameter backing. With explicit reuse, only actual historical
             * writes retain their provenance; initial words never set a bit. */
            if (!m->retained_parameter_reuse) {
                t.written_mask = 0;
            }
            memset(t.active_bank, 0, sizeof(t.active_bank));
            o62m_phy_clear_job_v1(&t);
        } else if ((word & 1) && !(m->hrs09 & 1)) {
            if (m->phase != O62M_PHY_HELD_V1) { return O62M_PHY_BAD_STATE_V1; }
            if (m->written_mask != O62M_PHY_PREF_MASK_V1) {
                return O62M_PHY_MISSING_WRITES_V1;
            }
            if (m->generation == UINT64_MAX || m->jobs_started == UINT64_MAX) {
                return O62M_PHY_COUNTER_OVERFLOW_V1;
            }
            if (m->init_latency_ns > (uint64_t)(INT64_MAX - now_ns)) {
                return O62M_PHY_DEADLINE_OVERFLOW_V1;
            }
            t.generation++;
            t.jobs_started++;
            t.phase = O62M_PHY_PENDING_PHASE_V1;
            t.job.instance_id = m->instance_id;
            t.job.generation = t.generation;
            t.job.deadline_ns = now_ns + (int64_t)m->init_latency_ns;
            t.job_start_ns = now_ns;
            memcpy(t.pending_bank, m->backing, sizeof(t.pending_bank));
        }
        t.hrs09 = word;
        break;
    }
    t.accepted_writes++;
    t.last_mutation_ns = now_ns;
    if (!o62m_phy_valid_v1(&t)) { return O62M_PHY_BAD_STATE_V1; }
    *m = t;
    return O62M_PHY_OK_V1;
}

/* Metadata only. A non-pending model returns a zero token and due=false. */
static inline O62MPhyResultV1 o62m_phy_job_due_v1(
    const O62MPhyV1 *m, int64_t now_ns, O62MPhyJobV1 *job, bool *due)
{
    O62MPhyJobV1 token = {0};
    if (!m || !job || !due) { return O62M_PHY_BAD_ARGUMENT_V1; }
    if (o62m_phy_overlap_v1(m, sizeof(*m), job, sizeof(*job)) ||
        o62m_phy_overlap_v1(m, sizeof(*m), due, sizeof(*due)) ||
        o62m_phy_overlap_v1(job, sizeof(*job), due, sizeof(*due))) {
        return O62M_PHY_ALIAS_V1;
    }
    if (!o62m_phy_valid_v1(m)) { return O62M_PHY_BAD_STATE_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_PHY_BAD_TIME_V1; }
    if (m->phase == O62M_PHY_PENDING_PHASE_V1) { token = m->job; }
    *job = token;
    *due = token.generation && now_ns >= token.deadline_ns;
    return O62M_PHY_OK_V1;
}

static inline O62MPhyResultV1 o62m_phy_apply_due_v1(
    O62MPhyV1 *m, int64_t now_ns, const O62MPhyJobV1 *job,
    bool reference_clock_available)
{
    O62MPhyV1 t;
    if (!m || !job) { return O62M_PHY_BAD_ARGUMENT_V1; }
    if (o62m_phy_overlap_v1(m, sizeof(*m), job, sizeof(*job))) {
        return O62M_PHY_ALIAS_V1;
    }
    if (!o62m_phy_valid_v1(m)) { return O62M_PHY_BAD_STATE_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_PHY_BAD_TIME_V1; }
    if (m->phase != O62M_PHY_PENDING_PHASE_V1 ||
        job->instance_id != m->instance_id || job->generation != m->generation ||
        job->deadline_ns != m->job.deadline_ns) {
        return O62M_PHY_STALE_JOB_V1;
    }
    if (now_ns < job->deadline_ns) { return O62M_PHY_NOT_DUE_V1; }
    if (!reference_clock_available) { return O62M_PHY_CLOCK_OFF_V1; }
    if (m->jobs_completed == UINT64_MAX) { return O62M_PHY_COUNTER_OVERFLOW_V1; }
    /* Every required register was really written: since assertion for v5's
     * default policy, or retained actual write history for the explicit v6
     * reuse policy. Initial backing alone never supplies that provenance.
     * All six addresses are fixed, finite, actual retained backing words. The
     * virtual PHY accepts their raw subfields; no analog tuning is claimed.
     * This is the actual configuration-application operation, not a caller
     * completion flag or an initially pre-populated active bank. */
    t = *m;
    for (unsigned i = 0; i < O62M_PHY_WORDS_V1; i++) {
        t.active_bank[i] = m->pending_bank[i];
    }
    t.jobs_completed++;
    t.phase = O62M_PHY_READY_V1;
    t.last_mutation_ns = now_ns;
    o62m_phy_clear_job_v1(&t);
    if (!o62m_phy_valid_v1(&t)) { return O62M_PHY_BAD_STATE_V1; }
    *m = t;
    return O62M_PHY_OK_V1;
}

static inline bool o62m_phy_ready_v1(const O62MPhyV1 *m)
{
    return o62m_phy_valid_v1(m) && m->phase == O62M_PHY_READY_V1;
}

static inline const char *o62m_phy_reason_v1(O62MPhyResultV1 r)
{
    switch (r) {
    case O62M_PHY_OK_V1: return "ok";
    case O62M_PHY_BAD_ARGUMENT_V1: return "bad PHY argument";
    case O62M_PHY_ALIAS_V1: return "overlapping PHY input or output";
    case O62M_PHY_POLICY_REQUIRED_V1: return "explicit idealized PHY policy required";
    case O62M_PHY_BAD_INITIAL_V1: return "invalid explicit PHY initial state";
    case O62M_PHY_BAD_STATE_V1: return "invalid finite PHY state";
    case O62M_PHY_BAD_TIME_V1: return "time before accepted PHY mutation";
    case O62M_PHY_BAD_ACCESS_V1: return "PHY requires exact aligned 32bit access";
    case O62M_PHY_UNKNOWN_REGISTER_V1: return "unknown host PHY register";
    case O62M_PHY_UNKNOWN_ADDRESS_V1: return "unknown finite PHY parameter address";
    case O62M_PHY_ADDRESS_NOT_SELECTED_V1: return "PHY data port has no actual address selection";
    case O62M_PHY_BAD_CONFIG_V1: return "invalid documented PHY host field";
    case O62M_PHY_RESET_REQUIRED_V1: return "PHY parameter write requires reset assertion";
    case O62M_PHY_PENDING_V1: return "initialization owns captured PHY parameters";
    case O62M_PHY_MISSING_WRITES_V1: return "PHY release lacks fresh parameter writes";
    case O62M_PHY_COUNTER_OVERFLOW_V1: return "PHY counter overflow";
    case O62M_PHY_DEADLINE_OVERFLOW_V1: return "PHY deadline overflow";
    case O62M_PHY_STALE_JOB_V1: return "stale or unowned PHY initialization job";
    case O62M_PHY_NOT_DUE_V1: return "PHY initialization delay not reached";
    case O62M_PHY_CLOCK_OFF_V1: return "actual PHY reference clock is gated";
    default: return "unknown PHY result";
    }
}
#endif
