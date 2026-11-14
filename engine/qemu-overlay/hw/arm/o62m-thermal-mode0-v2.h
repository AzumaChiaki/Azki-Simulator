/* Explicit virtual mode0 single-conversion profile. Not silicon timing,
 * calibration, status-bit, clock, reset or temperature measurement evidence.
 * The existing O62MThermalConfig remains the sole canonical config owner.
 * V2 requires the seven actual mode0 writes (mask0xef); optional threshold
 * word+0x40 has no value/provenance unless its existing written flag is true.
 */
#ifndef O62M_THERMAL_MODE0_V2_H
#define O62M_THERMAL_MODE0_V2_H
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "thermal-config-v1.h"

#define O62M_THERMAL_MODE0_MAGIC_V2 UINT32_C(0x54483031)
#define O62M_THERMAL_MODE0_CONFIG_WORDS_V2 8u
/* Actual quiet mode0 init may skip threshold+0x40 (canonical index4). */
#define O62M_THERMAL_MODE0_REQUIRED_MASK_V2 UINT32_C(0xef)

typedef enum O62MThermalMode0ResultV2 {
    O62M_THERMAL_MODE0_OK_V2 = 0,
    O62M_THERMAL_MODE0_BAD_ARGUMENT_V2,
    O62M_THERMAL_MODE0_ALIAS_V2,
    O62M_THERMAL_MODE0_POLICY_REQUIRED_V2,
    O62M_THERMAL_MODE0_BAD_INPUT_V2,
    O62M_THERMAL_MODE0_BAD_STATE_V2,
    O62M_THERMAL_MODE0_BAD_TIME_V2,
    O62M_THERMAL_MODE0_BAD_ACCESS_V2,
    O62M_THERMAL_MODE0_UNKNOWN_REGISTER_V2,
    O62M_THERMAL_MODE0_READ_ONLY_V2,
    O62M_THERMAL_MODE0_BAD_CONTROL_V2,
    O62M_THERMAL_MODE0_MISSING_CONFIG_V2,
    O62M_THERMAL_MODE0_PENDING_V2,
    O62M_THERMAL_MODE0_RESULT_INVALID_V2,
    O62M_THERMAL_MODE0_COUNTER_OVERFLOW_V2,
    O62M_THERMAL_MODE0_DEADLINE_OVERFLOW_V2,
    O62M_THERMAL_MODE0_STALE_JOB_V2,
    O62M_THERMAL_MODE0_NOT_DUE_V2,
    O62M_THERMAL_MODE0_CLOCK_OFF_V2
} O62MThermalMode0ResultV2;

typedef enum O62MThermalMode0PhaseV2 {
    O62M_THERMAL_MODE0_IDLE_V2 = 0,
    O62M_THERMAL_MODE0_PENDING_PHASE_V2,
    O62M_THERMAL_MODE0_READY_V2
} O62MThermalMode0PhaseV2;

typedef struct O62MThermalMode0SpecV2 {
    bool explicit_enable;
    uint64_t instance_id, latency_ns, raw_input_u32;
    int64_t now_ns;
} O62MThermalMode0SpecV2;

typedef struct O62MThermalMode0JobV2 {
    uint64_t instance_id, generation;
    int64_t deadline_ns;
} O62MThermalMode0JobV2;

typedef struct O62MThermalMode0V2 {
    uint32_t magic;
    bool initialized, result_valid, config_captured;
    O62MThermalMode0PhaseV2 phase;
    uint32_t control, status, result, raw_input, pending_raw;
    /* Immutable job/last-completed provenance, never a second MMIO owner. */
    uint32_t captured_config[O62M_THERMAL_MODE0_CONFIG_WORDS_V2];
    uint32_t captured_written_mask;
    uint64_t instance_id, latency_ns, generation;
    uint64_t accepted_writes, jobs_started, jobs_completed, jobs_cancelled, stops;
    int64_t origin_ns, last_mutation_ns, job_start_ns;
    O62MThermalMode0JobV2 job;
} O62MThermalMode0V2;

static inline bool o62m_thermal_mode0_overlap_v2(
    const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    return an && bn && (ap <= bp ? bp - ap < an : ap - bp < bn);
}

static inline bool o62m_thermal_mode0_valid_v2(const O62MThermalMode0V2 *m)
{
    uint64_t retired, mutations;
    if (!m || m->magic != O62M_THERMAL_MODE0_MAGIC_V2 || !m->initialized ||
        !m->instance_id || !m->latency_ns || m->latency_ns > INT64_MAX ||
        m->origin_ns < 0 || m->last_mutation_ns < m->origin_ns ||
        (unsigned)m->phase > O62M_THERMAL_MODE0_READY_V2 ||
        m->jobs_started > UINT64_MAX - m->stops ||
        m->jobs_completed > UINT64_MAX - m->jobs_cancelled) {
        return false;
    }
    if ((m->captured_written_mask & ~UINT32_C(0xff)) ||
        (m->config_captured &&
         (m->captured_written_mask & O62M_THERMAL_MODE0_REQUIRED_MASK_V2) !=
             O62M_THERMAL_MODE0_REQUIRED_MASK_V2) ||
        (!m->config_captured && m->captured_written_mask)) {
        return false;
    }
    for (unsigned i = 0; i < O62M_THERMAL_MODE0_CONFIG_WORDS_V2; ++i) {
        if (!(m->captured_written_mask & (UINT32_C(1) << i)) &&
            m->captured_config[i]) { return false; }
    }
    mutations = m->jobs_started + m->stops;
    retired = m->jobs_completed + m->jobs_cancelled;
    if (mutations == UINT64_MAX || m->generation != mutations + 1 ||
        m->accepted_writes != mutations) {
        return false;
    }
    if (m->phase == O62M_THERMAL_MODE0_PENDING_PHASE_V2) {
        if (retired == UINT64_MAX || m->jobs_started != retired + 1 ||
            m->control != 1 || m->status || m->result_valid || !m->config_captured ||
            m->job.instance_id != m->instance_id ||
            m->job.generation != m->generation ||
            m->job_start_ns < m->origin_ns ||
            m->job_start_ns > m->last_mutation_ns ||
            m->latency_ns > (uint64_t)(INT64_MAX - m->job_start_ns) ||
            m->job.deadline_ns != m->job_start_ns + (int64_t)m->latency_ns) {
            return false;
        }
    } else {
        if (m->jobs_started != retired || m->pending_raw || m->job_start_ns ||
            m->job.instance_id || m->job.generation || m->job.deadline_ns) {
            return false;
        }
        if (m->phase == O62M_THERMAL_MODE0_READY_V2) {
            if (!m->jobs_completed || m->control != 1 || m->status != 1 ||
                !m->result_valid || !m->config_captured) { return false; }
        } else {
            if (m->control || m->status || m->result_valid || m->config_captured) {
                return false;
            }
            for (unsigned i = 0; i < O62M_THERMAL_MODE0_CONFIG_WORDS_V2; ++i) {
                if (m->captured_config[i]) { return false; }
            }
        }
    }
    return true;
}

static inline O62MThermalMode0ResultV2 o62m_thermal_mode0_init_v2(
    O62MThermalMode0V2 *m, const O62MThermalMode0SpecV2 *spec)
{
    O62MThermalMode0V2 next;
    if (!m || !spec) { return O62M_THERMAL_MODE0_BAD_ARGUMENT_V2; }
    if (o62m_thermal_mode0_overlap_v2(m, sizeof(*m), spec, sizeof(*spec))) {
        return O62M_THERMAL_MODE0_ALIAS_V2;
    }
    if (!spec->explicit_enable) { return O62M_THERMAL_MODE0_POLICY_REQUIRED_V2; }
    if (!spec->instance_id || !spec->latency_ns || spec->latency_ns > INT64_MAX ||
        spec->now_ns < 0 || spec->raw_input_u32 > UINT32_MAX) {
        return O62M_THERMAL_MODE0_BAD_INPUT_V2;
    }
    memset(&next, 0, sizeof(next));
    next.magic = O62M_THERMAL_MODE0_MAGIC_V2;
    next.initialized = true;
    next.instance_id = spec->instance_id;
    next.latency_ns = spec->latency_ns;
    next.raw_input = (uint32_t)spec->raw_input_u32;
    next.generation = 1;
    next.origin_ns = next.last_mutation_ns = spec->now_ns;
    *m = next;
    return O62M_THERMAL_MODE0_OK_V2;
}

/* Changing a future explicit input never changes an already captured job. */
static inline O62MThermalMode0ResultV2 o62m_thermal_mode0_set_input_v2(
    O62MThermalMode0V2 *m, int64_t now_ns, uint64_t raw_input_u32)
{
    if (!m) { return O62M_THERMAL_MODE0_BAD_ARGUMENT_V2; }
    if (!o62m_thermal_mode0_valid_v2(m)) { return O62M_THERMAL_MODE0_BAD_STATE_V2; }
    if (now_ns < m->last_mutation_ns) { return O62M_THERMAL_MODE0_BAD_TIME_V2; }
    if (raw_input_u32 > UINT32_MAX) { return O62M_THERMAL_MODE0_BAD_INPUT_V2; }
    m->raw_input = (uint32_t)raw_input_u32;
    m->last_mutation_ns = now_ns;
    return O62M_THERMAL_MODE0_OK_V2;
}

/* All offsets are relative to the AP thermal base. Exact aligned32 only.
 * Reads never inspect the caller's old output bytes and never advance work.
 * The existing config owner is read only for an actual start request.
 */
static inline O62MThermalMode0ResultV2 o62m_thermal_mode0_access_v2(
    O62MThermalMode0V2 *m, const O62MThermalConfig *config, int64_t now_ns,
    uint64_t offset, unsigned size, bool write, uint64_t *value)
{
    O62MThermalMode0V2 next;
    uint32_t input;
    if (!m || !value) { return O62M_THERMAL_MODE0_BAD_ARGUMENT_V2; }
    if (o62m_thermal_mode0_overlap_v2(m, sizeof(*m), value, sizeof(*value)) ||
        (config &&
         (o62m_thermal_mode0_overlap_v2(config, sizeof(*config), value, sizeof(*value)) ||
          o62m_thermal_mode0_overlap_v2(config, sizeof(*config), m, sizeof(*m))))) {
        return O62M_THERMAL_MODE0_ALIAS_V2;
    }
    if (!o62m_thermal_mode0_valid_v2(m)) { return O62M_THERMAL_MODE0_BAD_STATE_V2; }
    if (now_ns < m->last_mutation_ns) { return O62M_THERMAL_MODE0_BAD_TIME_V2; }
    if (size != 4 || (offset & 3)) { return O62M_THERMAL_MODE0_BAD_ACCESS_V2; }
    if (offset != 0x18 && offset != 0x30 && offset != 0x34) {
        return O62M_THERMAL_MODE0_UNKNOWN_REGISTER_V2;
    }
    if (!write) {
        if (offset == 0x34 && !m->result_valid) {
            return O62M_THERMAL_MODE0_RESULT_INVALID_V2;
        }
        *value = offset == 0x18 ? m->status : offset == 0x30 ? m->control : m->result;
        return O62M_THERMAL_MODE0_OK_V2;
    }
    if (offset != 0x30) { return O62M_THERMAL_MODE0_READ_ONLY_V2; }
    if (*value > UINT32_MAX) { return O62M_THERMAL_MODE0_BAD_ACCESS_V2; }
    input = (uint32_t)*value;
    if (input > 1) { return O62M_THERMAL_MODE0_BAD_CONTROL_V2; }
    if (m->accepted_writes == UINT64_MAX || m->generation == UINT64_MAX) {
        return O62M_THERMAL_MODE0_COUNTER_OVERFLOW_V2;
    }
    next = *m;
    if (input) {
        if (m->phase == O62M_THERMAL_MODE0_PENDING_PHASE_V2) {
            return O62M_THERMAL_MODE0_PENDING_V2;
        }
        if (!config) { return O62M_THERMAL_MODE0_MISSING_CONFIG_V2; }
        next.captured_written_mask = 0;
        for (unsigned i = 0; i < O62M_THERMAL_MODE0_CONFIG_WORDS_V2; ++i) {
            if (config->written[i]) {
                next.captured_written_mask |= UINT32_C(1) << i;
                next.captured_config[i] = config->word[i];
            } else {
                /* Invalid private placeholder: never read an unwritten word,
                 * never a physical initial/readback value or IRQ threshold. */
                next.captured_config[i] = 0;
            }
        }
        if ((next.captured_written_mask & O62M_THERMAL_MODE0_REQUIRED_MASK_V2) !=
            O62M_THERMAL_MODE0_REQUIRED_MASK_V2) {
            return O62M_THERMAL_MODE0_MISSING_CONFIG_V2;
        }
        if (m->jobs_started == UINT64_MAX) { return O62M_THERMAL_MODE0_COUNTER_OVERFLOW_V2; }
        if (m->latency_ns > (uint64_t)(INT64_MAX - now_ns)) {
            return O62M_THERMAL_MODE0_DEADLINE_OVERFLOW_V2;
        }
        next.jobs_started++;
        next.phase = O62M_THERMAL_MODE0_PENDING_PHASE_V2;
        next.config_captured = true;
        next.pending_raw = m->raw_input;
        next.job_start_ns = now_ns;
        next.job.instance_id = m->instance_id;
        next.job.generation = m->generation + 1;
        next.job.deadline_ns = now_ns + (int64_t)m->latency_ns;
    } else {
        if (m->stops == UINT64_MAX ||
            (m->phase == O62M_THERMAL_MODE0_PENDING_PHASE_V2 &&
             m->jobs_cancelled == UINT64_MAX)) {
            return O62M_THERMAL_MODE0_COUNTER_OVERFLOW_V2;
        }
        next.stops++;
        if (m->phase == O62M_THERMAL_MODE0_PENDING_PHASE_V2) { next.jobs_cancelled++; }
        next.phase = O62M_THERMAL_MODE0_IDLE_V2;
        next.config_captured = false;
        next.captured_written_mask = 0;
        memset(next.captured_config, 0, sizeof(next.captured_config));
        next.pending_raw = 0;
        next.job_start_ns = 0;
        memset(&next.job, 0, sizeof(next.job));
    }
    next.control = input;
    next.status = 0;
    next.result_valid = false;
    next.accepted_writes++;
    next.generation++;
    next.last_mutation_ns = now_ns;
    if (!o62m_thermal_mode0_valid_v2(&next)) { return O62M_THERMAL_MODE0_BAD_STATE_V2; }
    *m = next;
    return O62M_THERMAL_MODE0_OK_V2;
}

/* Observation only. No pending work returns a zero token and due=false. */
static inline O62MThermalMode0ResultV2 o62m_thermal_mode0_job_due_v2(
    const O62MThermalMode0V2 *m, int64_t now_ns,
    O62MThermalMode0JobV2 *job, bool *due)
{
    O62MThermalMode0JobV2 token = {0};
    if (!m || !job || !due) { return O62M_THERMAL_MODE0_BAD_ARGUMENT_V2; }
    if (o62m_thermal_mode0_overlap_v2(m, sizeof(*m), job, sizeof(*job)) ||
        o62m_thermal_mode0_overlap_v2(m, sizeof(*m), due, sizeof(*due)) ||
        o62m_thermal_mode0_overlap_v2(job, sizeof(*job), due, sizeof(*due))) {
        return O62M_THERMAL_MODE0_ALIAS_V2;
    }
    if (!o62m_thermal_mode0_valid_v2(m)) { return O62M_THERMAL_MODE0_BAD_STATE_V2; }
    if (now_ns < m->last_mutation_ns) { return O62M_THERMAL_MODE0_BAD_TIME_V2; }
    if (m->phase == O62M_THERMAL_MODE0_PENDING_PHASE_V2) { token = m->job; }
    *job = token;
    *due = token.generation && now_ns >= token.deadline_ns;
    return O62M_THERMAL_MODE0_OK_V2;
}

/* CLOCK_OFF leaves the original pending token/deadline unchanged. The BSP
 * pauses/re-arms its timer; no read, retry count or caller success sets READY.
 */
static inline O62MThermalMode0ResultV2 o62m_thermal_mode0_apply_due_v2(
    O62MThermalMode0V2 *m, int64_t now_ns,
    const O62MThermalMode0JobV2 *job, bool functional_clock_available)
{
    O62MThermalMode0V2 next;
    if (!m || !job) { return O62M_THERMAL_MODE0_BAD_ARGUMENT_V2; }
    if (o62m_thermal_mode0_overlap_v2(m, sizeof(*m), job, sizeof(*job))) {
        return O62M_THERMAL_MODE0_ALIAS_V2;
    }
    if (!o62m_thermal_mode0_valid_v2(m)) { return O62M_THERMAL_MODE0_BAD_STATE_V2; }
    if (now_ns < m->last_mutation_ns) { return O62M_THERMAL_MODE0_BAD_TIME_V2; }
    if (m->phase != O62M_THERMAL_MODE0_PENDING_PHASE_V2 ||
        job->instance_id != m->instance_id || job->generation != m->generation ||
        job->deadline_ns != m->job.deadline_ns) { return O62M_THERMAL_MODE0_STALE_JOB_V2; }
    if (now_ns < job->deadline_ns) { return O62M_THERMAL_MODE0_NOT_DUE_V2; }
    if (!functional_clock_available) { return O62M_THERMAL_MODE0_CLOCK_OFF_V2; }
    if (m->jobs_completed == UINT64_MAX) { return O62M_THERMAL_MODE0_COUNTER_OVERFLOW_V2; }
    next = *m;
    next.result = m->pending_raw; /* Actual virtual conversion result copy. */
    next.result_valid = true;
    next.pending_raw = 0;
    next.jobs_completed++;
    next.phase = O62M_THERMAL_MODE0_READY_V2;
    next.status = 1; /* Declared virtual nonzero-valid code, not a physical bit. */
    next.job_start_ns = 0;
    memset(&next.job, 0, sizeof(next.job));
    next.last_mutation_ns = now_ns;
    if (!o62m_thermal_mode0_valid_v2(&next)) { return O62M_THERMAL_MODE0_BAD_STATE_V2; }
    *m = next;
    return O62M_THERMAL_MODE0_OK_V2;
}

static inline const char *o62m_thermal_mode0_reason_v2(O62MThermalMode0ResultV2 result)
{
    static const char *const reason[] = {
        "ok", "bad-argument", "overlapping-thermal-input-output", "explicit-mode0-profile-required",
        "invalid-explicit-input", "invalid-thermal-state", "time-before-last-mutation",
        "requires-aligned32bit", "unknown-mode0-register", "read-only-status-or-result",
        "control-must-be-zero-or-one", "required-seven-mode0-config-writes-missing", "conversion-pending",
        "no-current-conversion-result", "counter-or-generation-overflow", "deadline-overflow",
        "stale-or-unowned-job", "job-not-due", "declared-functional-clock-off"
    };
    return (unsigned)result < sizeof(reason) / sizeof(reason[0]) ? reason[result] : "unknown-result";
}
#endif
