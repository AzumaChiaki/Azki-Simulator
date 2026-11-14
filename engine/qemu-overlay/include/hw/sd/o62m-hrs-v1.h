#ifndef O62M_HRS_V1_H
#define O62M_HRS_V1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Opt-in Cadence-compatible research envelope, not O62M silicon reset data.
 * No QEMU, card, file, OS, autonomous time progress or reset backend lives here.
 * A single externally owned operation must REALLY succeed before complete().
 * Controller RESET_ALL and the raw EMMC_RST field are independent operations.
 * Raw card levels do not specify dfi_rstbar electrical polarity. The adapter
 * must declare its virtual wire policy and synchronize the initial card level.
 *
 * Caller serializes every API, cancels owned callbacks before destroy/reinit,
 * uses distinct nonzero IDs for live constructions and calls guard_boot_request
 * before ANY external operation capable of starting BOOT or altering BootCFG.
 * Unknown BOOT triggers, PHY, ACK, command and read-clear semantics stay strict.
 * All pointers refer to valid objects; outputs and inputs must not overlap the
 * model or each other. Only BACKEND_FAILED is an accepted non-OK state change.
 */
#define O62M_HRS_MAGIC_V1 UINT32_C(0x48525331)
#define O62M_HRS_SRS_RESET_ALL_V1 UINT32_C(1)

typedef enum O62MHrsPhaseV1 {
    O62M_HRS_IDLE_V1 = 0, O62M_HRS_PENDING_V1, O62M_HRS_FAILED_V1
} O62MHrsPhaseV1;

typedef enum O62MHrsJobKindV1 {
    O62M_HRS_JOB_NONE_V1 = 0,
    O62M_HRS_JOB_SRS_RESET_ALL_V1,
    O62M_HRS_JOB_CARD_RESET_LINE_V1
} O62MHrsJobKindV1;

typedef enum O62MHrsResultV1 {
    O62M_HRS_OK_V1 = 0,
    /* Accepted external failure: FAILED and no fabricated completion. */
    O62M_HRS_BACKEND_FAILED_V1,
    O62M_HRS_BAD_ARGUMENT_V1, O62M_HRS_BAD_INITIAL_V1,
    O62M_HRS_BAD_STATE_V1, O62M_HRS_POLICY_REQUIRED_V1,
    O62M_HRS_BAD_ACCESS_V1, O62M_HRS_UNSUPPORTED_REGISTER_V1,
    O62M_HRS_READ_ONLY_V1, O62M_HRS_ACTIVE_JOB_V1,
    O62M_HRS_TERMINAL_PHASE_V1, O62M_HRS_BAD_TIME_V1,
    O62M_HRS_DEADLINE_OVERFLOW_V1, O62M_HRS_COUNT_OVERFLOW_V1,
    O62M_HRS_NOT_DUE_V1, O62M_HRS_JOB_MISMATCH_V1,
    O62M_HRS_BAD_COMPLETION_V1, O62M_HRS_BOOT_UNSUPPORTED_V1,
    O62M_HRS_BOOT_STATE_UNKNOWN_V1, O62M_HRS_ALIAS_V1
} O62MHrsResultV1;

typedef struct O62MHrsSpecV1 {
    bool explicit_enable;
    uint64_t hrs00_initial_fullword; /* bit0 MUST be clear: no inherited job */
    uint64_t hrs01_initial_fullword;
    uint64_t hrs01_reset_fullword;   /* explicit controller-reset result */
    uint64_t hrs11_initial_fullword;
    uint64_t hrs31_initial_fullword; /* full RO version profile, no inference */
    uint64_t hrs36_initial_fullword; /* low7 clear: fresh BootEngine only */
    bool card_initial_applied_valid;
    uint32_t card_initial_applied_level; /* exactly initial HRS11.bit0 */
    bool virtual_card_auto_boot_off;
    uint64_t media_bootcfg;         /* must be zero for inactive BOOT reads */
    uint64_t controller_latency_ns, card_latency_ns;
    uint64_t instance_id;
    int64_t now_ns;
} O62MHrsSpecV1;

typedef struct O62MHrsJobV1 {
    uint64_t instance_id, id;
    O62MHrsJobKindV1 kind;
    uint32_t logical_card_level; /* only used by CARD_RESET_LINE */
    int64_t deadline_ns;
} O62MHrsJobV1;

typedef struct O62MHrsCompletionV1 {
    O62MHrsJobKindV1 kind;
    bool success;
    uint32_t completed_srs_mask; /* exactly RESET_ALL=1 for controller success */
    uint32_t applied_card_level; /* exactly requested logical field level */
} O62MHrsCompletionV1;

typedef struct O62MHrsMetadataV1 {
    O62MHrsPhaseV1 phase;
    O62MHrsJobKindV1 active_kind, failed_kind;
    uint64_t instance_id, jobs_started, jobs_completed, backend_failures;
    uint64_t controller_resets, card_transitions, accepted_writes;
    uint32_t requested_card_level, applied_card_level;
    bool applied_card_level_known, boot_inactive_provenance;
    int64_t deadline_ns, last_mutation_ns;
} O62MHrsMetadataV1;

typedef struct O62MHrsV1 {
    uint32_t magic, enabled, hrs00_opaque, swr;
    uint32_t hrs01, hrs01_reset, hrs11_opaque, hrs31, hrs36_opaque;
    uint32_t requested_card_level, applied_card_level, card_level_known;
    uint32_t auto_boot_off, media_bootcfg;
    uint64_t controller_latency_ns, card_latency_ns, instance_id;
    uint64_t jobs_started, jobs_completed, backend_failures;
    uint64_t controller_resets, card_transitions, accepted_writes;
    int64_t origin_ns, last_mutation_ns, job_start_ns;
    O62MHrsPhaseV1 phase;
    O62MHrsJobKindV1 failed_kind;
    O62MHrsJobV1 job;
} O62MHrsV1;

static inline bool o62m_hrs_overlap_v1(
    const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    if (an > UINTPTR_MAX - ap || bn > UINTPTR_MAX - bp) { return true; }
    return an && bn && ap < bp + bn && bp < ap + an;
}

static inline bool o62m_hrs_valid_v1(const O62MHrsV1 *m)
{
    uint64_t done;
    if (!m || m->magic != O62M_HRS_MAGIC_V1 || m->enabled > 1 ||
        m->swr > 1 || (m->hrs00_opaque & 1) ||
        (m->hrs11_opaque & 1) || (m->hrs36_opaque & UINT32_C(0x7f)) ||
        (m->hrs01 & UINT32_C(0xff000000)) !=
            (m->hrs01_reset & UINT32_C(0xff000000)) ||
        m->requested_card_level > 1 || m->applied_card_level > 1 ||
        m->card_level_known > 1 || m->auto_boot_off > 1 ||
        (m->auto_boot_off && m->media_bootcfg) ||
        !m->controller_latency_ns || !m->card_latency_ns || !m->instance_id ||
        m->origin_ns < 0 || m->last_mutation_ns < m->origin_ns ||
        m->jobs_completed > UINT64_MAX - m->backend_failures ||
        m->controller_resets > UINT64_MAX - m->card_transitions ||
        m->controller_resets + m->card_transitions != m->jobs_completed ||
        m->jobs_started > m->accepted_writes) { return false; }
    done = m->jobs_completed + m->backend_failures;
    if (m->phase == O62M_HRS_PENDING_V1) {
        uint64_t latency;
        if (done == UINT64_MAX || m->jobs_started != done + 1 ||
            m->failed_kind != O62M_HRS_JOB_NONE_V1 ||
            m->job.instance_id != m->instance_id ||
            m->job.id != m->jobs_started || !m->job.id ||
            m->job_start_ns < m->origin_ns ||
            m->job_start_ns > m->last_mutation_ns ||
            m->job.deadline_ns < m->job_start_ns) { return false; }
        if (m->job.kind == O62M_HRS_JOB_SRS_RESET_ALL_V1) {
            latency = m->controller_latency_ns;
            if (!m->swr || m->job.logical_card_level || !m->card_level_known ||
                m->requested_card_level != m->applied_card_level) { return false; }
        } else if (m->job.kind == O62M_HRS_JOB_CARD_RESET_LINE_V1) {
            latency = m->card_latency_ns;
            if (m->swr || !m->card_level_known ||
                m->job.logical_card_level != m->requested_card_level ||
                m->requested_card_level == m->applied_card_level) { return false; }
        } else { return false; }
        if (latency > (uint64_t)(INT64_MAX - m->job_start_ns) ||
            m->job.deadline_ns != m->job_start_ns + (int64_t)latency) {
            return false;
        }
    } else {
        if (m->jobs_started != done || m->job_start_ns ||
            m->job.instance_id || m->job.id ||
            m->job.kind != O62M_HRS_JOB_NONE_V1 ||
            m->job.logical_card_level || m->job.deadline_ns) { return false; }
        if (m->phase == O62M_HRS_IDLE_V1) {
            if (m->failed_kind != O62M_HRS_JOB_NONE_V1 || m->swr ||
                !m->card_level_known ||
                m->requested_card_level != m->applied_card_level) { return false; }
        } else if (m->phase == O62M_HRS_FAILED_V1) {
            if (!m->backend_failures) { return false; }
            if (m->failed_kind == O62M_HRS_JOB_SRS_RESET_ALL_V1) {
                if (!m->swr || !m->card_level_known ||
                    m->requested_card_level != m->applied_card_level) { return false; }
            } else if (m->failed_kind == O62M_HRS_JOB_CARD_RESET_LINE_V1) {
                if (m->swr || m->card_level_known ||
                    m->requested_card_level == m->applied_card_level) { return false; }
            } else { return false; }
        } else { return false; }
    }
    return true;
}

static inline O62MHrsResultV1 o62m_hrs_init_v1(
    O62MHrsV1 *m, const O62MHrsSpecV1 *s)
{
    O62MHrsV1 t;
    if (!m || !s) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (o62m_hrs_overlap_v1(m, sizeof(*m), s, sizeof(*s))) {
        return O62M_HRS_ALIAS_V1;
    }
    if (s->hrs00_initial_fullword > UINT32_MAX ||
        s->hrs01_initial_fullword > UINT32_MAX ||
        s->hrs01_reset_fullword > UINT32_MAX ||
        s->hrs11_initial_fullword > UINT32_MAX ||
        s->hrs31_initial_fullword > UINT32_MAX ||
        s->hrs36_initial_fullword > UINT32_MAX || s->media_bootcfg > UINT32_MAX ||
        (s->hrs00_initial_fullword & 1) ||
        (s->hrs36_initial_fullword & UINT64_C(0x7f)) ||
        (s->hrs01_initial_fullword & UINT64_C(0xff000000)) !=
            (s->hrs01_reset_fullword & UINT64_C(0xff000000)) ||
        !s->card_initial_applied_valid || s->card_initial_applied_level > 1 ||
        s->card_initial_applied_level != (s->hrs11_initial_fullword & 1) ||
        (s->virtual_card_auto_boot_off && s->media_bootcfg) ||
        !s->controller_latency_ns || !s->card_latency_ns ||
        !s->instance_id || s->now_ns < 0) { return O62M_HRS_BAD_INITIAL_V1; }
    memset(&t, 0, sizeof(t));
    t.magic = O62M_HRS_MAGIC_V1;
    t.enabled = s->explicit_enable ? 1u : 0u;
    t.hrs00_opaque = (uint32_t)s->hrs00_initial_fullword;
    t.hrs01 = (uint32_t)s->hrs01_initial_fullword;
    t.hrs01_reset = (uint32_t)s->hrs01_reset_fullword;
    t.hrs11_opaque = (uint32_t)s->hrs11_initial_fullword & ~UINT32_C(1);
    t.hrs31 = (uint32_t)s->hrs31_initial_fullword;
    t.hrs36_opaque = (uint32_t)s->hrs36_initial_fullword;
    t.requested_card_level = s->card_initial_applied_level;
    t.applied_card_level = s->card_initial_applied_level;
    t.card_level_known = 1;
    t.auto_boot_off = s->virtual_card_auto_boot_off ? 1u : 0u;
    t.media_bootcfg = (uint32_t)s->media_bootcfg;
    t.controller_latency_ns = s->controller_latency_ns;
    t.card_latency_ns = s->card_latency_ns;
    t.instance_id = s->instance_id;
    t.origin_ns = s->now_ns;
    t.last_mutation_ns = s->now_ns;
    t.phase = O62M_HRS_IDLE_V1;
    *m = t;
    return O62M_HRS_OK_V1;
}

/* offset is relative to the exact controller HRS base, never truncated. RO
 * bits in writable words ignore written values and retain explicit opaque
 * inputs. Every rejected access leaves both model and *value byte-identical. */
static inline O62MHrsResultV1 o62m_hrs_access_v1(
    O62MHrsV1 *m, int64_t now_ns, uint64_t offset, unsigned size,
    bool write, uint64_t *value)
{
    O62MHrsV1 t;
    uint32_t v;
    O62MHrsJobKindV1 kind = O62M_HRS_JOB_NONE_V1;
    uint64_t latency = 0;
    if (!m || !value) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (o62m_hrs_overlap_v1(m, sizeof(*m), value, sizeof(*value))) {
        return O62M_HRS_ALIAS_V1;
    }
    if (!o62m_hrs_valid_v1(m)) { return O62M_HRS_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_HRS_POLICY_REQUIRED_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_HRS_BAD_TIME_V1; }
    if (size != 4 || (offset & 3) || (write && *value > UINT32_MAX)) {
        return O62M_HRS_BAD_ACCESS_V1;
    }
    if (offset != 0 && offset != 4 && offset != UINT64_C(0x2c) &&
        offset != UINT64_C(0x7c) && offset != UINT64_C(0x90)) {
        return O62M_HRS_UNSUPPORTED_REGISTER_V1;
    }
    if (!write) {
        if (offset == UINT64_C(0x90)) {
            if (m->phase == O62M_HRS_PENDING_V1) { return O62M_HRS_ACTIVE_JOB_V1; }
            if (m->phase == O62M_HRS_FAILED_V1) { return O62M_HRS_TERMINAL_PHASE_V1; }
            if (!m->auto_boot_off || m->media_bootcfg) {
                return O62M_HRS_BOOT_STATE_UNKNOWN_V1;
            }
            v = m->hrs36_opaque; /* actual empty BootEngine: no accepted BOOT */
        } else if (offset == 0) { v = m->hrs00_opaque | m->swr; }
        else if (offset == 4) { v = m->hrs01; }
        else if (offset == UINT64_C(0x2c)) {
            v = m->hrs11_opaque | m->requested_card_level;
        } else { v = m->hrs31; }
        *value = v;
        return O62M_HRS_OK_V1;
    }
    if (offset == UINT64_C(0x7c) || offset == UINT64_C(0x90)) {
        return O62M_HRS_READ_ONLY_V1;
    }
    if (m->phase == O62M_HRS_PENDING_V1) { return O62M_HRS_ACTIVE_JOB_V1; }
    if (m->phase == O62M_HRS_FAILED_V1) { return O62M_HRS_TERMINAL_PHASE_V1; }
    if (m->accepted_writes == UINT64_MAX) { return O62M_HRS_COUNT_OVERFLOW_V1; }
    v = (uint32_t)*value;
    t = *m;
    if (offset == 0 && (v & 1)) {
        kind = O62M_HRS_JOB_SRS_RESET_ALL_V1;
        latency = m->controller_latency_ns;
        t.swr = 1;
    } else if (offset == UINT64_C(0x2c) &&
               (v & 1) != m->requested_card_level) {
        kind = O62M_HRS_JOB_CARD_RESET_LINE_V1;
        latency = m->card_latency_ns;
        t.requested_card_level = v & 1;
    } else if (offset == 4) {
        t.hrs01 = (m->hrs01 & UINT32_C(0xff000000)) | (v & UINT32_C(0xffffff));
    }
    if (kind != O62M_HRS_JOB_NONE_V1) {
        if (m->jobs_started == UINT64_MAX) { return O62M_HRS_COUNT_OVERFLOW_V1; }
        if (latency > (uint64_t)(INT64_MAX - now_ns)) {
            return O62M_HRS_DEADLINE_OVERFLOW_V1;
        }
        t.jobs_started++;
        t.phase = O62M_HRS_PENDING_V1;
        t.job.instance_id = m->instance_id;
        t.job.id = t.jobs_started;
        t.job.kind = kind;
        t.job.logical_card_level = kind == O62M_HRS_JOB_CARD_RESET_LINE_V1 ?
            t.requested_card_level : 0;
        t.job_start_ns = now_ns;
        t.job.deadline_ns = now_ns + (int64_t)latency;
    }
    t.accepted_writes++;
    t.last_mutation_ns = now_ns;
    *m = t;
    return O62M_HRS_OK_V1;
}

/* Metadata only, including when !due; this NEVER executes or completes work. */
static inline O62MHrsResultV1 o62m_hrs_job_due_v1(
    const O62MHrsV1 *m, int64_t now_ns, O62MHrsJobV1 *job, bool *due)
{
    if (!m || !job || !due) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (o62m_hrs_overlap_v1(m, sizeof(*m), job, sizeof(*job)) ||
        o62m_hrs_overlap_v1(m, sizeof(*m), due, sizeof(*due)) ||
        o62m_hrs_overlap_v1(job, sizeof(*job), due, sizeof(*due))) {
        return O62M_HRS_ALIAS_V1;
    }
    if (!o62m_hrs_valid_v1(m)) { return O62M_HRS_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_HRS_POLICY_REQUIRED_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_HRS_BAD_TIME_V1; }
    *job = m->job;
    *due = m->phase == O62M_HRS_PENDING_V1 && now_ns >= m->job.deadline_ns;
    return O62M_HRS_OK_V1;
}

static inline O62MHrsResultV1 o62m_hrs_job_check_v1(
    const O62MHrsV1 *m, int64_t now_ns, const O62MHrsJobV1 *job)
{
    if (!m || !job) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (o62m_hrs_overlap_v1(m, sizeof(*m), job, sizeof(*job))) {
        return O62M_HRS_ALIAS_V1;
    }
    if (!o62m_hrs_valid_v1(m)) { return O62M_HRS_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_HRS_POLICY_REQUIRED_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_HRS_BAD_TIME_V1; }
    if (m->phase != O62M_HRS_PENDING_V1 ||
        job->instance_id != m->job.instance_id || job->id != m->job.id ||
        job->kind != m->job.kind ||
        job->logical_card_level != m->job.logical_card_level ||
        job->deadline_ns != m->job.deadline_ns) { return O62M_HRS_JOB_MISMATCH_V1; }
    return O62M_HRS_OK_V1;
}

static inline void o62m_hrs_clear_job_v1(O62MHrsV1 *m)
{
    memset(&m->job, 0, sizeof(m->job));
    m->job_start_ns = 0;
}

/* Actual backend failure may be reported before deadline; it never clears
 * SWR, pretends the card applied its line, or synthesizes BOOT error bits. */
static inline O62MHrsResultV1 o62m_hrs_fail_v1(
    O62MHrsV1 *m, int64_t now_ns, const O62MHrsJobV1 *job)
{
    O62MHrsV1 t;
    O62MHrsResultV1 r = o62m_hrs_job_check_v1(m, now_ns, job);
    if (r != O62M_HRS_OK_V1) { return r; }
    if (m->backend_failures == UINT64_MAX) { return O62M_HRS_COUNT_OVERFLOW_V1; }
    t = *m;
    t.failed_kind = t.job.kind;
    if (t.job.kind == O62M_HRS_JOB_CARD_RESET_LINE_V1) { t.card_level_known = 0; }
    t.backend_failures++;
    t.phase = O62M_HRS_FAILED_V1;
    t.last_mutation_ns = now_ns;
    o62m_hrs_clear_job_v1(&t);
    *m = t;
    return O62M_HRS_BACKEND_FAILED_V1;
}

static inline O62MHrsResultV1 o62m_hrs_complete_v1(
    O62MHrsV1 *m, int64_t now_ns, const O62MHrsJobV1 *job,
    const O62MHrsCompletionV1 *completion)
{
    O62MHrsV1 t;
    O62MHrsResultV1 r;
    if (!m || !job || !completion) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (o62m_hrs_overlap_v1(m, sizeof(*m), completion, sizeof(*completion)) ||
        o62m_hrs_overlap_v1(job, sizeof(*job), completion, sizeof(*completion))) {
        return O62M_HRS_ALIAS_V1;
    }
    r = o62m_hrs_job_check_v1(m, now_ns, job);
    if (r != O62M_HRS_OK_V1) { return r; }
    if (completion->kind != job->kind ||
        (job->kind == O62M_HRS_JOB_SRS_RESET_ALL_V1 &&
         (completion->applied_card_level ||
          (completion->success && completion->completed_srs_mask !=
           O62M_HRS_SRS_RESET_ALL_V1) ||
          (!completion->success && completion->completed_srs_mask))) ||
        (job->kind == O62M_HRS_JOB_CARD_RESET_LINE_V1 &&
         (completion->completed_srs_mask || completion->applied_card_level > 1 ||
          (completion->success &&
           completion->applied_card_level != job->logical_card_level)))) {
        return O62M_HRS_BAD_COMPLETION_V1;
    }
    if (now_ns < job->deadline_ns) { return O62M_HRS_NOT_DUE_V1; }
    if (!completion->success) { return o62m_hrs_fail_v1(m, now_ns, job); }
    if (m->jobs_completed == UINT64_MAX ||
        (job->kind == O62M_HRS_JOB_SRS_RESET_ALL_V1 && m->controller_resets == UINT64_MAX) ||
        (job->kind == O62M_HRS_JOB_CARD_RESET_LINE_V1 && m->card_transitions == UINT64_MAX)) {
        return O62M_HRS_COUNT_OVERFLOW_V1;
    }
    t = *m;
    if (job->kind == O62M_HRS_JOB_SRS_RESET_ALL_V1) {
        t.swr = 0;
        t.hrs01 = t.hrs01_reset;
        t.controller_resets++;
    } else {
        t.applied_card_level = job->logical_card_level;
        t.card_level_known = 1;
        t.card_transitions++;
    }
    t.jobs_completed++;
    t.phase = O62M_HRS_IDLE_V1;
    t.last_mutation_ns = now_ns;
    o62m_hrs_clear_job_v1(&t);
    *m = t;
    return O62M_HRS_OK_V1;
}

static inline O62MHrsResultV1 o62m_hrs_query_v1(
    const O62MHrsV1 *m, O62MHrsMetadataV1 *out)
{
    O62MHrsMetadataV1 t;
    if (!m || !out) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (o62m_hrs_overlap_v1(m, sizeof(*m), out, sizeof(*out))) {
        return O62M_HRS_ALIAS_V1;
    }
    if (!o62m_hrs_valid_v1(m)) { return O62M_HRS_BAD_STATE_V1; }
    memset(&t, 0, sizeof(t));
    t.phase = m->phase;
    t.active_kind = m->job.kind;
    t.failed_kind = m->failed_kind;
    t.instance_id = m->instance_id;
    t.jobs_started = m->jobs_started;
    t.jobs_completed = m->jobs_completed;
    t.backend_failures = m->backend_failures;
    t.controller_resets = m->controller_resets;
    t.card_transitions = m->card_transitions;
    t.accepted_writes = m->accepted_writes;
    t.requested_card_level = m->requested_card_level;
    t.applied_card_level = m->applied_card_level;
    t.applied_card_level_known = m->card_level_known != 0;
    t.boot_inactive_provenance = m->auto_boot_off && !m->media_bootcfg &&
        m->phase == O62M_HRS_IDLE_V1;
    t.deadline_ns = m->job.deadline_ns;
    t.last_mutation_ns = m->last_mutation_ns;
    *out = t;
    return O62M_HRS_OK_V1;
}

/* Every possible BOOT-start or BootCFG mutation is unsupported by this core.
 * The adapter MUST stop that request, not continue then read natural idle. */
static inline O62MHrsResultV1 o62m_hrs_guard_boot_request_v1(const O62MHrsV1 *m)
{
    if (!m) { return O62M_HRS_BAD_ARGUMENT_V1; }
    if (!o62m_hrs_valid_v1(m)) { return O62M_HRS_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_HRS_POLICY_REQUIRED_V1; }
    return O62M_HRS_BOOT_UNSUPPORTED_V1;
}

static inline void o62m_hrs_destroy_v1(O62MHrsV1 *m)
{
    if (m) {
        volatile unsigned char *p = (volatile unsigned char *)m;
        for (size_t i = 0; i < sizeof(*m); ++i) { p[i] = 0; }
    }
}

static inline const char *o62m_hrs_reason_v1(O62MHrsResultV1 r)
{
    switch (r) {
    case O62M_HRS_OK_V1: return "ok";
    case O62M_HRS_BACKEND_FAILED_V1: return "accepted actual backend failure";
    case O62M_HRS_BAD_ARGUMENT_V1: return "bad argument";
    case O62M_HRS_BAD_INITIAL_V1: return "bad explicit initial state";
    case O62M_HRS_BAD_STATE_V1: return "invalid model state";
    case O62M_HRS_POLICY_REQUIRED_V1: return "explicit research policy required";
    case O62M_HRS_BAD_ACCESS_V1: return "unsupported access width alignment or value";
    case O62M_HRS_UNSUPPORTED_REGISTER_V1: return "unknown HRS register";
    case O62M_HRS_READ_ONLY_V1: return "read-only register";
    case O62M_HRS_ACTIVE_JOB_V1: return "external operation still owns controller";
    case O62M_HRS_TERMINAL_PHASE_V1: return "actual external operation failed";
    case O62M_HRS_BAD_TIME_V1: return "time before accepted mutation";
    case O62M_HRS_DEADLINE_OVERFLOW_V1: return "unrepresentable deadline";
    case O62M_HRS_COUNT_OVERFLOW_V1: return "counter overflow";
    case O62M_HRS_NOT_DUE_V1: return "explicit minimum work delay not reached";
    case O62M_HRS_JOB_MISMATCH_V1: return "unowned stale or mismatched job";
    case O62M_HRS_BAD_COMPLETION_V1: return "wrong actual completion kind or result";
    case O62M_HRS_BOOT_UNSUPPORTED_V1: return "BOOT request unsupported";
    case O62M_HRS_BOOT_STATE_UNKNOWN_V1: return "BOOT inactive prerequisites absent";
    case O62M_HRS_ALIAS_V1: return "overlapping model input or output";
    default: return "unknown HRS result";
    }
}

#endif
