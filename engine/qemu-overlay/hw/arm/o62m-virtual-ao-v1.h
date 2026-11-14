#ifndef O62M_VIRTUAL_AO_V1_H
#define O62M_VIRTUAL_AO_V1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "o62m-trng-unkeyed-v1.h"

/* Optional research hardware, not a reconstruction of physical ring noise,
 * health tests, AO key length, reset contents or timing. No OS/provider call,
 * key export, hashing, logging, RNP or implicit time/read progress exists here.
 *
 * One configuration backing and one job per host construction. Caller owns
 * security/privilege/clock checks and an actual conditioned entropy provider.
 * Caller must wipe its own provider staging buffer on EVERY completion path;
 * this core owns and wipes only its private AO store. Construction/destruction
 * are host operations, never guest CTRL reset. Cancel old worker callbacks
 * before reinitializing and use distinct instance IDs for live constructions.
 * All passed pointer ranges must refer to valid objects of the stated sizes.
 */
#define O62M_VAO_MAX_BYTES_V1 256u
#define O62M_VAO_MAGIC_V1 UINT32_C(0x56414f31)

typedef enum O62MVirtualAoPhaseV1 {
    O62M_VAO_EMPTY_V1 = 0, O62M_VAO_BUSY_V1,
    O62M_VAO_VALID_V1, O62M_VAO_FAILED_V1
} O62MVirtualAoPhaseV1;

typedef enum O62MVirtualAoResultV1 {
    O62M_VAO_OK_V1 = 0,
    /* Accepted lifecycle failure: state becomes FAILED, owned store wiped.
     * Every other non-OK result leaves all model/output bytes unchanged. */
    O62M_VAO_SOURCE_FAILED_V1,
    O62M_VAO_BAD_ARGUMENT_V1, O62M_VAO_BAD_INITIAL_V1,
    O62M_VAO_BAD_STATE_V1, O62M_VAO_POLICY_REQUIRED_V1,
    O62M_VAO_BAD_ACCESS_V1, O62M_VAO_UNSUPPORTED_REGISTER_V1,
    O62M_VAO_UNWRITTEN_CONFIG_V1, O62M_VAO_UNSUPPORTED_CONFIG_V1,
    O62M_VAO_UNSUPPORTED_CONTROL_V1, O62M_VAO_CONFIG_REQUIRED_V1,
    O62M_VAO_ACTIVE_JOB_V1, O62M_VAO_TERMINAL_PHASE_V1,
    O62M_VAO_BAD_TIME_V1, O62M_VAO_DEADLINE_OVERFLOW_V1,
    O62M_VAO_NOT_DUE_V1, O62M_VAO_JOB_MISMATCH_V1,
    O62M_VAO_ALIAS_V1
} O62MVirtualAoResultV1;

typedef struct O62MVirtualAoSpecV1 {
    bool explicit_enable;
    uint64_t status_opaque; /* independent u32; bit0 MUST be clear */
    uint32_t key_bytes;    /* explicit research parameter, 1..256 */
    uint64_t latency_ns;   /* explicit positive virtual work delay */
    uint64_t instance_id;  /* host-owned nonzero identity */
    int64_t now_ns;
} O62MVirtualAoSpecV1;

typedef struct O62MVirtualAoJobV1 {
    uint64_t instance_id, id;
    uint32_t bytes;
    int64_t deadline_ns;
} O62MVirtualAoJobV1;

typedef struct O62MVirtualAoMetadataV1 {
    O62MVirtualAoPhaseV1 phase;
    bool material_present;
    uint32_t key_bytes, config_written_mask;
    uint64_t instance_id, jobs_started, jobs_completed, provider_reported_bytes;
    int64_t deadline_ns;
} O62MVirtualAoMetadataV1;

typedef struct O62MVirtualAoV1 {
    uint32_t magic, enabled, config[5], written_mask, status_opaque, key_bytes;
    uint64_t latency_ns, instance_id, jobs_started, jobs_completed;
    uint64_t provider_reported_bytes;
    uint32_t provider_ok, material_present;
    int64_t origin_ns, last_mutation_ns, start_ns, deadline_ns;
    O62MVirtualAoPhaseV1 phase;
    uint8_t ao_store[O62M_VAO_MAX_BYTES_V1]; /* host-private: NEVER guest-map */
} O62MVirtualAoV1;

static inline void o62m_virtual_ao_wipe_v1(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) { *v++ = 0; }
}

static inline bool o62m_virtual_ao_overlap_v1(
    const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    if (an > UINTPTR_MAX - ap || bn > UINTPTR_MAX - bp) { return true; }
    return an && bn && ap < bp + bn && bp < ap + an;
}

static inline bool o62m_virtual_ao_zero_v1(const uint8_t *p, size_t n)
{
    uint8_t any = 0;
    for (size_t i = 0; i < n; ++i) { any |= p[i]; }
    return any == 0;
}

static inline bool o62m_virtual_ao_valid_v1(const O62MVirtualAoV1 *m)
{
    if (!m || m->magic != O62M_VAO_MAGIC_V1 || m->enabled > 1 ||
        (m->status_opaque & 1u) || !m->instance_id || !m->key_bytes ||
        m->key_bytes > O62M_VAO_MAX_BYTES_V1 || !m->latency_ns ||
        m->latency_ns > INT64_MAX || m->origin_ns < 0 ||
        m->last_mutation_ns < m->origin_ns || (m->written_mask & ~31u) ||
        m->provider_ok > 1 || m->material_present > 1 ||
        (m->phase != O62M_VAO_EMPTY_V1 && m->phase != O62M_VAO_BUSY_V1 &&
         m->phase != O62M_VAO_VALID_V1 && m->phase != O62M_VAO_FAILED_V1)) {
        return false;
    }
    for (unsigned i = 0; i < 5; ++i) {
        if (m->written_mask & (1u << i)) {
            if (m->config[i] != o62m_trng_unkeyed_defs[i].original_value) {
                return false;
            }
        } else if (m->config[i]) { return false; }
    }
    if (m->phase == O62M_VAO_EMPTY_V1) {
        return !m->jobs_started && !m->jobs_completed && !m->start_ns &&
               !m->deadline_ns && !m->provider_ok && !m->provider_reported_bytes &&
               !m->material_present && o62m_virtual_ao_zero_v1(m->ao_store, sizeof(m->ao_store));
    }
    if (m->written_mask != 31 || !m->enabled || m->jobs_started != 1 ||
        m->start_ns < m->origin_ns ||
        m->latency_ns > (uint64_t)(INT64_MAX - m->start_ns) ||
        m->deadline_ns != m->start_ns + (int64_t)m->latency_ns ||
        m->last_mutation_ns < m->start_ns) { return false; }
    if (m->phase == O62M_VAO_BUSY_V1) {
        return !m->jobs_completed && !m->provider_ok && !m->provider_reported_bytes &&
               !m->material_present && m->last_mutation_ns == m->start_ns &&
               o62m_virtual_ao_zero_v1(m->ao_store, sizeof(m->ao_store));
    }
    if (m->jobs_completed != 1) { return false; }
    if (m->phase == O62M_VAO_VALID_V1) {
        return m->provider_ok && m->provider_reported_bytes == m->key_bytes &&
               m->material_present && m->last_mutation_ns >= m->deadline_ns &&
               o62m_virtual_ao_zero_v1(m->ao_store + m->key_bytes,
                                      sizeof(m->ao_store) - m->key_bytes);
    }
    return !m->material_present &&
           !(m->provider_ok && m->provider_reported_bytes == m->key_bytes) &&
           o62m_virtual_ao_zero_v1(m->ao_store, sizeof(m->ao_store));
}

static inline O62MVirtualAoResultV1 o62m_virtual_ao_init_v1(
    O62MVirtualAoV1 *m, const O62MVirtualAoSpecV1 *spec)
{
    O62MVirtualAoV1 next = {0};
    if (!m || !spec) { return O62M_VAO_BAD_ARGUMENT_V1; }
    if (o62m_virtual_ao_overlap_v1(m, sizeof(*m), spec, sizeof(*spec))) {
        return O62M_VAO_ALIAS_V1;
    }
    if (spec->status_opaque > UINT32_MAX || (spec->status_opaque & 1u) ||
        !spec->key_bytes || spec->key_bytes > O62M_VAO_MAX_BYTES_V1 ||
        !spec->latency_ns || spec->latency_ns > INT64_MAX ||
        !spec->instance_id || spec->now_ns < 0) { return O62M_VAO_BAD_INITIAL_V1; }
    next.magic = O62M_VAO_MAGIC_V1;
    next.enabled = spec->explicit_enable ? 1u : 0u;
    next.status_opaque = (uint32_t)spec->status_opaque;
    next.key_bytes = spec->key_bytes;
    next.latency_ns = spec->latency_ns;
    next.instance_id = spec->instance_id;
    next.origin_ns = next.last_mutation_ns = spec->now_ns;
    next.phase = O62M_VAO_EMPTY_V1;
    o62m_virtual_ao_wipe_v1(m, sizeof(*m));
    *m = next;
    return O62M_VAO_OK_V1;
}

static inline O62MVirtualAoResultV1 o62m_virtual_ao_access_v1(
    O62MVirtualAoV1 *m, int64_t now_ns, uint64_t address,
    unsigned size, bool write, uint64_t *value)
{
    if (!m || !value) { return O62M_VAO_BAD_ARGUMENT_V1; }
    if (o62m_virtual_ao_overlap_v1(m, sizeof(*m), value, sizeof(*value))) {
        return O62M_VAO_ALIAS_V1;
    }
    if (!o62m_virtual_ao_valid_v1(m)) { return O62M_VAO_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_VAO_POLICY_REQUIRED_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_VAO_BAD_TIME_V1; }
    if (size != 4 || (address & 3) || (write && *value > UINT32_MAX)) {
        return O62M_VAO_BAD_ACCESS_V1;
    }
    if (address == O62M_TRNG_UNKEYED_STAT) {
        if (write) { return O62M_VAO_UNSUPPORTED_CONTROL_V1; }
        *value = m->status_opaque | (m->phase == O62M_VAO_VALID_V1 ? 1u : 0u);
        return O62M_VAO_OK_V1;
    }
    if (address == O62M_TRNG_UNKEYED_CTL) {
        if (!write || *value != 1) { return O62M_VAO_UNSUPPORTED_CONTROL_V1; }
        if (m->phase == O62M_VAO_BUSY_V1) { return O62M_VAO_ACTIVE_JOB_V1; }
        if (m->phase != O62M_VAO_EMPTY_V1) { return O62M_VAO_TERMINAL_PHASE_V1; }
        if (m->written_mask != 31) { return O62M_VAO_CONFIG_REQUIRED_V1; }
        if (m->latency_ns > (uint64_t)(INT64_MAX - now_ns)) {
            return O62M_VAO_DEADLINE_OVERFLOW_V1;
        }
        m->start_ns = m->last_mutation_ns = now_ns;
        m->deadline_ns = now_ns + (int64_t)m->latency_ns;
        m->jobs_started = 1;
        m->phase = O62M_VAO_BUSY_V1;
        return O62M_VAO_OK_V1;
    }
    for (unsigned i = 0; i < 5; ++i) {
        if (address != o62m_trng_unkeyed_defs[i].address) { continue; }
        if (!write) {
            if (!(m->written_mask & (1u << i))) { return O62M_VAO_UNWRITTEN_CONFIG_V1; }
            *value = m->config[i];
            return O62M_VAO_OK_V1;
        }
        if (m->phase == O62M_VAO_BUSY_V1) { return O62M_VAO_ACTIVE_JOB_V1; }
        if (m->phase != O62M_VAO_EMPTY_V1) { return O62M_VAO_TERMINAL_PHASE_V1; }
        if (*value != o62m_trng_unkeyed_defs[i].original_value) {
            return O62M_VAO_UNSUPPORTED_CONFIG_V1;
        }
        m->config[i] = (uint32_t)*value;
        m->written_mask |= 1u << i;
        m->last_mutation_ns = now_ns;
        return O62M_VAO_OK_V1;
    }
    return O62M_VAO_UNSUPPORTED_REGISTER_V1;
}

/* Metadata outputs contain no material. A due job is still BUSY/invalid. */
static inline O62MVirtualAoResultV1 o62m_virtual_ao_job_due_v1(
    const O62MVirtualAoV1 *m, int64_t now_ns,
    O62MVirtualAoJobV1 *out_job, bool *out_due)
{
    O62MVirtualAoJobV1 job = {0};
    if (!m || !out_job || !out_due) { return O62M_VAO_BAD_ARGUMENT_V1; }
    if (o62m_virtual_ao_overlap_v1(m,sizeof(*m),out_job,sizeof(*out_job)) ||
        o62m_virtual_ao_overlap_v1(m,sizeof(*m),out_due,sizeof(*out_due)) ||
        o62m_virtual_ao_overlap_v1(out_job,sizeof(*out_job),out_due,sizeof(*out_due))) {
        return O62M_VAO_ALIAS_V1;
    }
    if (!o62m_virtual_ao_valid_v1(m)) { return O62M_VAO_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_VAO_POLICY_REQUIRED_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_VAO_BAD_TIME_V1; }
    if (m->phase != O62M_VAO_BUSY_V1) { return O62M_VAO_TERMINAL_PHASE_V1; }
    job.instance_id=m->instance_id;job.id=1;job.bytes=m->key_bytes;job.deadline_ns=m->deadline_ns;
    *out_job=job;
    *out_due=now_ns>=m->deadline_ns;
    return O62M_VAO_OK_V1;
}

static inline O62MVirtualAoResultV1 o62m_virtual_ao_check_job_v1(
    const O62MVirtualAoV1 *m, int64_t now_ns, const O62MVirtualAoJobV1 *job)
{
    if (!m || !job) { return O62M_VAO_BAD_ARGUMENT_V1; }
    if (o62m_virtual_ao_overlap_v1(m,sizeof(*m),job,sizeof(*job))) { return O62M_VAO_ALIAS_V1; }
    if (!o62m_virtual_ao_valid_v1(m)) { return O62M_VAO_BAD_STATE_V1; }
    if (!m->enabled) { return O62M_VAO_POLICY_REQUIRED_V1; }
    if (now_ns < m->last_mutation_ns) { return O62M_VAO_BAD_TIME_V1; }
    if (m->phase != O62M_VAO_BUSY_V1) { return O62M_VAO_TERMINAL_PHASE_V1; }
    if (job->instance_id!=m->instance_id || job->id!=1 ||
        job->bytes!=m->key_bytes || job->deadline_ns!=m->deadline_ns) { return O62M_VAO_JOB_MISMATCH_V1; }
    return O62M_VAO_OK_V1;
}

static inline O62MVirtualAoResultV1 o62m_virtual_ao_complete_v1(
    O62MVirtualAoV1 *m, int64_t now_ns, const O62MVirtualAoJobV1 *job,
    const uint8_t *material, size_t actual_bytes, bool source_ok)
{
    O62MVirtualAoResultV1 r=o62m_virtual_ao_check_job_v1(m,now_ns,job);
    if (r!=O62M_VAO_OK_V1) { return r; }
    if (now_ns<m->deadline_ns) { return O62M_VAO_NOT_DUE_V1; }
    if (source_ok && actual_bytes==m->key_bytes) {
        if (!material) { return O62M_VAO_BAD_ARGUMENT_V1; }
        if (o62m_virtual_ao_overlap_v1(m,sizeof(*m),material,actual_bytes) ||
            o62m_virtual_ao_overlap_v1(job,sizeof(*job),material,actual_bytes)) {
            return O62M_VAO_ALIAS_V1;
        }
        /* All possible rejections precede the actual private-store commit. */
        memcpy(m->ao_store,material,actual_bytes);
        m->provider_reported_bytes=actual_bytes;
        m->provider_ok=m->material_present=1;
        m->jobs_completed=1;m->last_mutation_ns=now_ns;m->phase=O62M_VAO_VALID_V1;
        return O62M_VAO_OK_V1;
    }
    o62m_virtual_ao_wipe_v1(m->ao_store,sizeof(m->ao_store));
    m->provider_reported_bytes=(uint64_t)actual_bytes;
    m->provider_ok=source_ok?1u:0u;m->material_present=0;
    m->jobs_completed=1;m->last_mutation_ns=now_ns;m->phase=O62M_VAO_FAILED_V1;
    return O62M_VAO_SOURCE_FAILED_V1;
}

/* Explicit owned source/job failure may occur before its success deadline. */
static inline O62MVirtualAoResultV1 o62m_virtual_ao_fail_v1(
    O62MVirtualAoV1 *m, int64_t now_ns, const O62MVirtualAoJobV1 *job)
{
    O62MVirtualAoResultV1 r=o62m_virtual_ao_check_job_v1(m,now_ns,job);
    if (r!=O62M_VAO_OK_V1) { return r; }
    o62m_virtual_ao_wipe_v1(m->ao_store,sizeof(m->ao_store));
    m->provider_ok=m->material_present=0;m->provider_reported_bytes=0;
    m->jobs_completed=1;m->last_mutation_ns=now_ns;m->phase=O62M_VAO_FAILED_V1;
    return O62M_VAO_SOURCE_FAILED_V1;
}

static inline O62MVirtualAoResultV1 o62m_virtual_ao_query_v1(
    const O62MVirtualAoV1 *m, O62MVirtualAoMetadataV1 *out)
{
    O62MVirtualAoMetadataV1 next={0};
    if (!m || !out) { return O62M_VAO_BAD_ARGUMENT_V1; }
    if (o62m_virtual_ao_overlap_v1(m,sizeof(*m),out,sizeof(*out))) { return O62M_VAO_ALIAS_V1; }
    if (!o62m_virtual_ao_valid_v1(m)) { return O62M_VAO_BAD_STATE_V1; }
    next.phase=m->phase;next.material_present=m->material_present!=0;
    next.key_bytes=m->key_bytes;next.config_written_mask=m->written_mask;
    next.instance_id=m->instance_id;next.jobs_started=m->jobs_started;
    next.jobs_completed=m->jobs_completed;next.provider_reported_bytes=m->provider_reported_bytes;
    next.deadline_ns=m->deadline_ns;
    *out=next;
    return O62M_VAO_OK_V1;
}

/* Also wipes malformed/uninitialized host storage; no fields are read. */
static inline O62MVirtualAoResultV1 o62m_virtual_ao_destroy_v1(O62MVirtualAoV1 *m)
{
    if (!m) { return O62M_VAO_BAD_ARGUMENT_V1; }
    o62m_virtual_ao_wipe_v1(m,sizeof(*m));
    return O62M_VAO_OK_V1;
}

static inline const char *o62m_virtual_ao_reason_v1(O62MVirtualAoResultV1 r)
{
    switch(r) {
    case O62M_VAO_OK_V1:return "ok";
    case O62M_VAO_SOURCE_FAILED_V1:return "accepted-source-failure-private-store-wiped";
    case O62M_VAO_BAD_ARGUMENT_V1:return "bad-argument";
    case O62M_VAO_BAD_INITIAL_V1:return "bad-explicit-initial-profile";
    case O62M_VAO_BAD_STATE_V1:return "invalid-private-state";
    case O62M_VAO_POLICY_REQUIRED_V1:return "explicit-virtual-AO-policy-required";
    case O62M_VAO_BAD_ACCESS_V1:return "aligned-u32-fullword-required";
    case O62M_VAO_UNSUPPORTED_REGISTER_V1:return "unknown-AO-register";
    case O62M_VAO_UNWRITTEN_CONFIG_V1:return "configuration-not-yet-written";
    case O62M_VAO_UNSUPPORTED_CONFIG_V1:return "outside-exact-original-config-profile";
    case O62M_VAO_UNSUPPORTED_CONTROL_V1:return "unknown-control-reset-or-status-write";
    case O62M_VAO_CONFIG_REQUIRED_V1:return "all-five-real-config-writes-required";
    case O62M_VAO_ACTIVE_JOB_V1:return "owned-job-active";
    case O62M_VAO_TERMINAL_PHASE_V1:return "rekey-retry-or-consumer-unimplemented";
    case O62M_VAO_BAD_TIME_V1:return "time-before-last-mutation";
    case O62M_VAO_DEADLINE_OVERFLOW_V1:return "unrepresentable-job-deadline";
    case O62M_VAO_NOT_DUE_V1:return "material-completion-before-deadline";
    case O62M_VAO_JOB_MISMATCH_V1:return "wrong-instance-or-job-metadata";
    case O62M_VAO_ALIAS_V1:return "model-input-output-ranges-overlap";
    }
    return "unknown-result";
}
#endif
