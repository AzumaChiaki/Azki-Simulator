#ifndef O62M_VIRTUAL_DVS_JOB_V1_H
#define O62M_VIRTUAL_DVS_JOB_V1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/* Explicit opt-in VIRTUAL profile, not a silicon/TRM implementation.
 * SID=8 and packed reg/payload interpretation are source-supported choices.
 * Equal command pairs only: no hardware bank-selection rule is invented.
 * Only the peer's real shared-bus commit may enter SETTLE. No MMIO or I/O here.
 */
typedef enum {
    O62M_VDVS_V1_OK = 0,
    O62M_VDVS_V1_BAD_ARGUMENT,
    O62M_VDVS_V1_NOT_INITIALIZED,
    O62M_VDVS_V1_ALREADY_INITIALIZED,
    O62M_VDVS_V1_BAD_INITIAL,
    O62M_VDVS_V1_CLOCK_BACKWARD,
    O62M_VDVS_V1_BUSY,
    O62M_VDVS_V1_BAD_PHASE,
    O62M_VDVS_V1_STALE_TICKET,
    O62M_VDVS_V1_BAD_SID,
    O62M_VDVS_V1_BAD_COMMAND,
    O62M_VDVS_V1_TICKET_MISMATCH,
    O62M_VDVS_V1_COUNTER_OVERFLOW,
    O62M_VDVS_V1_DEADLINE_OVERFLOW,
    O62M_VDVS_V1_PMIC_NOT_INITIALIZED,
    O62M_VDVS_V1_PMIC_LAYOUT
} O62MVirtualDvsResultV1;

typedef enum {
    O62M_VDVS_IDLE_V1 = 0,
    O62M_VDVS_WAIT_BUS_V1,
    O62M_VDVS_TRANSFER_V1,
    O62M_VDVS_SETTLE_V1,
    O62M_VDVS_FAILED_V1
} O62MVirtualDvsPhaseV1;

typedef struct {
    bool initialized, bus_committed;
    O62MVirtualDvsPhaseV1 phase;
    uint64_t generation;
    uint32_t current_index, virtual_mv;
    uint32_t job_sid, job_index, job_ctrl00, job_cmd_a, job_cmd_b;
    uint16_t pending_reg, pending_payload;
    uint32_t pending_mv;
    int64_t settle_ns, settle_due_ns, last_ns;
    uint64_t begun, queued, committed, completed, cancelled, failed;
} O62MVirtualDvsJobV1;

static inline bool o62m_virtual_dvs_overlaps_v1(
    const void *a_ptr, size_t a_size, const void *b_ptr, size_t b_size)
{
    uintptr_t a = (uintptr_t)a_ptr, b = (uintptr_t)b_ptr;
    return b >= a ? b-a < a_size : a-b < b_size;
}

static inline bool o62m_virtual_dvs_mv_valid_v1(uint32_t mv)
{
    return mv >= 500u && mv <= 1130u && (mv-500u) % 10u == 0u;
}

static inline bool o62m_virtual_dvs_command_valid_v1(uint32_t cmd)
{
    uint32_t payload = cmd & UINT32_C(0xffff);
    return (cmd >> 16) == UINT32_C(0x0240) &&
           (payload & ~UINT32_C(0x703f)) == 0u &&
           ((payload >> 12) & 7u) == 1u;
}

static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_clock_v1(
    const O62MVirtualDvsJobV1 *m, int64_t now_ns)
{
    if (!m) { return O62M_VDVS_V1_BAD_ARGUMENT; }
    if (!m->initialized) { return O62M_VDVS_V1_NOT_INITIALIZED; }
    if (now_ns < 0 || now_ns < m->last_ns) { return O62M_VDVS_V1_CLOCK_BACKWARD; }
    return O62M_VDVS_V1_OK;
}

static inline bool o62m_virtual_dvs_active_v1(const O62MVirtualDvsJobV1 *m)
{
    return m->phase == O62M_VDVS_WAIT_BUS_V1 ||
           m->phase == O62M_VDVS_TRANSFER_V1 || m->phase == O62M_VDVS_SETTLE_V1;
}

/* Construction only: caller supplies a zero-initialized object. Never memset or
 * reinitialize an object with outstanding/previous tickets. init rejects every
 * initialized object, including FAILED/IDLE, to prevent generation ABA reuse.
 */
static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_init_v1(
    O62MVirtualDvsJobV1 *m, uint32_t initial_index, uint32_t initial_mv,
    int64_t settle_ns, int64_t now_ns)
{
    if (!m) { return O62M_VDVS_V1_BAD_ARGUMENT; }
    if (m->initialized) { return O62M_VDVS_V1_ALREADY_INITIALIZED; }
    if (initial_index > 7u || !o62m_virtual_dvs_mv_valid_v1(initial_mv) ||
        settle_ns <= 0 || now_ns < 0) { return O62M_VDVS_V1_BAD_INITIAL; }
    O62MVirtualDvsJobV1 trial;
    memset(&trial, 0, sizeof(trial));
    trial.initialized = true;
    trial.phase = O62M_VDVS_IDLE_V1;
    trial.current_index = initial_index;
    trial.virtual_mv = initial_mv;
    trial.settle_ns = settle_ns;
    trial.last_ns = now_ns;
    *m = trial;
    return O62M_VDVS_V1_OK;
}

/* generation is the ticket; read m->generation only after an OK begin. */
static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_begin_v1(
    O62MVirtualDvsJobV1 *m, uint32_t index, uint32_t ctrl00,
    uint32_t cmd_a, uint32_t cmd_b, int64_t now_ns)
{
    O62MVirtualDvsResultV1 r = o62m_virtual_dvs_clock_v1(m, now_ns);
    if (r != O62M_VDVS_V1_OK) { return r; }
    if (m->phase != O62M_VDVS_IDLE_V1 && m->phase != O62M_VDVS_FAILED_V1) {
        return O62M_VDVS_V1_BUSY;
    }
    if (index > 7u) { return O62M_VDVS_V1_BAD_ARGUMENT; }
    if (((ctrl00 >> 3) & 15u) != 8u) { return O62M_VDVS_V1_BAD_SID; }
    if (cmd_a != cmd_b || !o62m_virtual_dvs_command_valid_v1(cmd_a)) {
        return O62M_VDVS_V1_BAD_COMMAND;
    }
    if (m->generation == UINT64_MAX || m->begun == UINT64_MAX) {
        return O62M_VDVS_V1_COUNTER_OVERFLOW;
    }
    ++m->generation;
    ++m->begun;
    m->job_sid = 8u;
    m->job_index = index;
    m->job_ctrl00 = ctrl00;
    m->job_cmd_a = cmd_a;
    m->job_cmd_b = cmd_b;
    m->pending_reg = (uint16_t)(cmd_a >> 16);
    m->pending_payload = (uint16_t)cmd_a;
    m->pending_mv = 500u + 10u * (cmd_a & 63u);
    m->settle_due_ns = 0;
    m->bus_committed = false;
    m->phase = O62M_VDVS_WAIT_BUS_V1;
    m->last_ns = now_ns;
    return O62M_VDVS_V1_OK;
}

static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_mark_queued_v1(
    O62MVirtualDvsJobV1 *m, uint64_t generation, int64_t now_ns)
{
    O62MVirtualDvsResultV1 r = o62m_virtual_dvs_clock_v1(m, now_ns);
    if (r != O62M_VDVS_V1_OK) { return r; }
    if (generation != m->generation) { return O62M_VDVS_V1_STALE_TICKET; }
    if (m->phase != O62M_VDVS_WAIT_BUS_V1) { return O62M_VDVS_V1_BAD_PHASE; }
    if (m->queued == UINT64_MAX) { return O62M_VDVS_V1_COUNTER_OVERFLOW; }
    ++m->queued;
    m->phase = O62M_VDVS_TRANSFER_V1;
    m->last_ns = now_ns;
    return O62M_VDVS_V1_OK;
}

/* Cancel invalidates old callbacks. A committed PMIC word is never rolled back. */
static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_cancel_v1(
    O62MVirtualDvsJobV1 *m, uint64_t generation, int64_t now_ns)
{
    O62MVirtualDvsResultV1 r = o62m_virtual_dvs_clock_v1(m, now_ns);
    if (r != O62M_VDVS_V1_OK) { return r; }
    if (generation != m->generation) { return O62M_VDVS_V1_STALE_TICKET; }
    if (!o62m_virtual_dvs_active_v1(m)) { return O62M_VDVS_V1_BAD_PHASE; }
    if (m->generation == UINT64_MAX || m->cancelled == UINT64_MAX) {
        return O62M_VDVS_V1_COUNTER_OVERFLOW;
    }
    ++m->generation;
    ++m->cancelled;
    m->phase = O62M_VDVS_FAILED_V1;
    m->last_ns = now_ns;
    return O62M_VDVS_V1_OK;
}

static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_fail_v1(
    O62MVirtualDvsJobV1 *m, uint64_t generation, int64_t now_ns)
{
    O62MVirtualDvsResultV1 r = o62m_virtual_dvs_clock_v1(m, now_ns);
    if (r != O62M_VDVS_V1_OK) { return r; }
    if (generation != m->generation) { return O62M_VDVS_V1_STALE_TICKET; }
    if (!o62m_virtual_dvs_active_v1(m)) { return O62M_VDVS_V1_BAD_PHASE; }
    if (m->failed == UINT64_MAX) { return O62M_VDVS_V1_COUNTER_OVERFLOW; }
    ++m->failed;
    m->phase = O62M_VDVS_FAILED_V1;
    m->last_ns = now_ns;
    return O62M_VDVS_V1_OK;
}

static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_advance_v1(
    O62MVirtualDvsJobV1 *m, int64_t now_ns)
{
    O62MVirtualDvsResultV1 r = o62m_virtual_dvs_clock_v1(m, now_ns);
    if (r != O62M_VDVS_V1_OK) { return r; }
    if (m->phase != O62M_VDVS_SETTLE_V1 || !m->bus_committed) {
        return O62M_VDVS_V1_BAD_PHASE;
    }
    if (now_ns >= m->settle_due_ns) {
        if (m->completed == UINT64_MAX) { return O62M_VDVS_V1_COUNTER_OVERFLOW; }
        m->current_index = m->job_index;
        m->virtual_mv = m->pending_mv;
        ++m->completed;
        m->phase = O62M_VDVS_IDLE_V1;
    }
    m->last_ns = now_ns;
    return O62M_VDVS_V1_OK;
}

/* Firmware runs its original <= predicate. FAILED stays non-ready as well. */
static inline O62MVirtualDvsResultV1 o62m_virtual_dvs_status_v1(
    const O62MVirtualDvsJobV1 *m, uint32_t *out)
{
    if (!m || !out || o62m_virtual_dvs_overlaps_v1(m, sizeof(*m), out, sizeof(*out))) {
        return O62M_VDVS_V1_BAD_ARGUMENT;
    }
    if (!m->initialized) { return O62M_VDVS_V1_NOT_INITIALIZED; }
    *out = m->current_index | (m->phase == O62M_VDVS_IDLE_V1 ? 0u : 3u << 11);
    return O62M_VDVS_V1_OK;
}
#endif
