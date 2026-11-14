#ifndef O62M_V4_DATA_V1_H
#define O62M_V4_DATA_V1_H

/* Pure transfer accounting, not a card, RAM, register file or IRQ generator.
 * Working implementation: freeze only with the parent-owned formal receipt. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    O62M_V4_DATA_OK_V1 = 0,
    O62M_V4_DATA_BAD_ARGUMENT_V1,
    O62M_V4_DATA_BAD_STATE_V1,
    O62M_V4_DATA_BAD_CONFIG_V1,
    O62M_V4_DATA_OPEN_ENDED_UNSUPPORTED_V1,
    O62M_V4_DATA_SINGLE_COUNT_UNSUPPORTED_V1,
    O62M_V4_DATA_NO_WORK_V1,
    O62M_V4_DATA_CLOCK_OFF_V1,
    O62M_V4_DATA_STALE_PLAN_V1,
    O62M_V4_DATA_BAD_COMMIT_V1,
    O62M_V4_DATA_ADDRESS_OVERFLOW_V1,
    O62M_V4_DATA_COUNTER_OVERFLOW_V1,
    O62M_V4_DATA_UNSUPPORTED_ROLE_V1,
    O62M_V4_DATA_ALIAS_V1
} O62MV4DataResultV1;

typedef enum {
    O62M_V4_ENGINE_PIO_V1 = 0,
    O62M_V4_ENGINE_SDMA_V1 = 1,
    O62M_V4_ENGINE_ADMA_V1 = 2
} O62MV4DataEngineV1;

typedef enum {
    O62M_V4_PHASE_IDLE_V1 = 0,
    O62M_V4_PHASE_RUNNING_V1,
    O62M_V4_PHASE_WAIT_BOUNDARY_V1,
    O62M_V4_PHASE_DONE_V1,
    O62M_V4_PHASE_ABORTED_V1,
    O62M_V4_PHASE_EMPTY_V1
} O62MV4DataPhaseV1;

typedef enum {
    O62M_V4_COUNT16_V1 = 0,
    O62M_V4_COUNT32_V1,
    O62M_V4_IMPLICIT_ONE_V1
} O62MV4DataCountSourceV1;

typedef struct {
    uint32_t saar;
    uint16_t bcct16;
    uint64_t dma58;
} O62MV4DataBankV1;

typedef struct {
    bool hv4e;
    bool a64b;
    bool bce;
    bool multiblock;
    uint16_t block_size;
    uint8_t boundary_code; /* Decoded value 0..7; never silently masked. */
    O62MV4DataEngineV1 engine;
} O62MV4DataConfigV1;

typedef struct {
    O62MV4DataBankV1 bank;
    O62MV4DataConfigV1 config;
    uint16_t max_block_size;
    bool supports64;
    uintptr_t owner; /* Stable constructor identity; preserved in local trials. */
    uint64_t session_tag;
    uint64_t generation;
    O62MV4DataPhaseV1 phase;
    O62MV4DataCountSourceV1 count_source;
    uint16_t block_offset;
    uint64_t bytes_committed;
    uint64_t blocks_committed;
    uint64_t commit_count;
    uint32_t magic;
} O62MV4DataV1;

typedef struct {
    uintptr_t owner;
    uint64_t session_tag;
    uint64_t generation;
    uint64_t address;
    uint32_t request_budget;
    uint32_t bytes;
    uint32_t blocks_left;
    uint16_t block_offset;
    O62MV4DataEngineV1 engine;
    bool address_valid;
} O62MV4DataPlanV1;

typedef struct {
    uint32_t bytes;
    uint32_t completed_blocks;
    bool boundary_hit;
    bool data_exhausted;
} O62MV4DataFactsV1;

/* Init is one object lifecycle. A new lifecycle needs a new nonzero session_tag
 * and invalidates every old external token. max_block_size is 512/1024/2048. */
static inline O62MV4DataResultV1 o62m_v4_data_init_v1(
    O62MV4DataV1 *, const O62MV4DataBankV1 *, uint16_t, bool, uint64_t);
/* Inactive canonical CSR programming. Active MMIO writes must use the narrower
 * count-write/resume contracts, or fail before any external CSR mutation. */
static inline O62MV4DataResultV1 o62m_v4_data_program_v1(
    O62MV4DataV1 *, const O62MV4DataBankV1 *);
/* !multiblock must correspond to an actual single-block card command. This is
 * a caller-validated command contract, not an inference from BCE alone. */
static inline O62MV4DataResultV1 o62m_v4_data_begin_v1(
    O62MV4DataV1 *, const O62MV4DataConfigV1 *);
static inline uint32_t o62m_v4_data_remaining_v1(const O62MV4DataV1 *);
static inline O62MV4DataResultV1 o62m_v4_data_plan_v1(
    const O62MV4DataV1 *, uint32_t, bool, O62MV4DataPlanV1 *);
static inline O62MV4DataResultV1 o62m_v4_data_validate_commit_v1(
    const O62MV4DataV1 *, const O62MV4DataPlanV1 *, uint32_t, bool);
/* actual_bytes is a positively acknowledged transferred prefix, not a request
 * length. Call validate before I/O, then commit exactly actual successful bytes.
 * No unknown-partial-I/O success is permitted. facts are not IRQ/status bits. */
static inline O62MV4DataResultV1 o62m_v4_data_commit_v1(
    O62MV4DataV1 *, const O62MV4DataPlanV1 *, uint32_t, bool,
    O62MV4DataFactsV1 *);
/* Only a completed, proven address-register transaction may call resume.
 * Legacy writes must include SAAR's highest byte. V4/64 multiword trigger/order
 * is NOT implemented by this semantic hook and must remain strict in MMIO. */
static inline O62MV4DataResultV1 o62m_v4_data_resume_v1(
    O62MV4DataV1 *, uint64_t);
/* count32=false is BCCT16. count32=true is SAAR in its count role only.
 * All active BCCT16 writes are ignored. Active SAAR32 writes are accepted as
 * ignored only when that register is the latched count source; other roles are
 * strict. Ignored writes do not invalidate tokens or advance anything. */
static inline O62MV4DataResultV1 o62m_v4_data_count_write_v1(
    O62MV4DataV1 *, bool, uint32_t, bool *);
static inline O62MV4DataResultV1 o62m_v4_data_abort_v1(O62MV4DataV1 *);
static inline const char *o62m_v4_data_reason_v1(O62MV4DataResultV1);

#define O62M_V4_DATA_MAGIC_V1 UINT32_C(0x56344431)

static inline bool o62m_v4_data_overlap_v1(
    const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    if (an == 0 || bn == 0) {
        return false;
    }
    if (av > UINTPTR_MAX - (an - 1) || bv > UINTPTR_MAX - (bn - 1)) {
        return true;
    }
    return av <= bv + (bn - 1) && bv <= av + (an - 1);
}

static inline bool o62m_v4_data_active_v1(const O62MV4DataV1 *m)
{
    return m->phase == O62M_V4_PHASE_RUNNING_V1 ||
           m->phase == O62M_V4_PHASE_WAIT_BOUNDARY_V1;
}

static inline uint32_t o62m_v4_data_selected_count_v1(const O62MV4DataV1 *m)
{
    if (m->count_source == O62M_V4_COUNT16_V1) {
        return m->bank.bcct16;
    }
    if (m->count_source == O62M_V4_COUNT32_V1) {
        return m->bank.saar;
    }
    return m->phase == O62M_V4_PHASE_DONE_V1 ? 0 : 1;
}

static inline bool o62m_v4_data_config_valid_v1(
    const O62MV4DataV1 *m, const O62MV4DataConfigV1 *c)
{
    return c->block_size != 0 && c->block_size <= m->max_block_size &&
           c->boundary_code <= 7 &&
           (c->engine == O62M_V4_ENGINE_PIO_V1 ||
            c->engine == O62M_V4_ENGINE_SDMA_V1 ||
            c->engine == O62M_V4_ENGINE_ADMA_V1) &&
           (!(c->hv4e && c->a64b) || m->supports64);
}

static inline bool o62m_v4_data_valid_v1(const O62MV4DataV1 *m)
{
    uint32_t remaining;
    if (m == NULL || m->magic != O62M_V4_DATA_MAGIC_V1 ||
        (m->max_block_size != 512 && m->max_block_size != 1024 &&
         m->max_block_size != 2048) ||
        m->owner == 0 || m->session_tag == 0 || m->generation == 0 ||
        m->phase < O62M_V4_PHASE_IDLE_V1 ||
        m->phase > O62M_V4_PHASE_EMPTY_V1) {
        return false;
    }
    if (m->phase == O62M_V4_PHASE_IDLE_V1) {
        return m->block_offset == 0;
    }
    if (!o62m_v4_data_config_valid_v1(m, &m->config) ||
        m->count_source < O62M_V4_COUNT16_V1 ||
        m->count_source > O62M_V4_IMPLICIT_ONE_V1 ||
        m->block_offset >= m->config.block_size ||
        (!m->config.bce && m->config.multiblock)) {
        return false;
    }
    if ((m->config.bce && m->count_source == O62M_V4_IMPLICIT_ONE_V1) ||
        (!m->config.bce && m->count_source != O62M_V4_IMPLICIT_ONE_V1) ||
        (m->count_source == O62M_V4_COUNT32_V1 && !m->config.hv4e) ||
        (m->phase == O62M_V4_PHASE_WAIT_BOUNDARY_V1 &&
         m->config.engine != O62M_V4_ENGINE_SDMA_V1)) {
        return false;
    }
    remaining = o62m_v4_data_selected_count_v1(m);
    if (o62m_v4_data_active_v1(m)) {
        return remaining != 0 && (m->config.multiblock || remaining == 1);
    }
    if (m->phase == O62M_V4_PHASE_DONE_V1 ||
        m->phase == O62M_V4_PHASE_EMPTY_V1) {
        return m->block_offset == 0 && remaining == 0;
    }
    return m->phase == O62M_V4_PHASE_ABORTED_V1;
}

static inline void o62m_v4_data_idle_job_v1(O62MV4DataV1 *m)
{
    memset(&m->config, 0, sizeof(m->config));
    m->phase = O62M_V4_PHASE_IDLE_V1;
    m->count_source = O62M_V4_COUNT16_V1;
    m->block_offset = 0;
}

static inline O62MV4DataResultV1 o62m_v4_data_init_v1(
    O62MV4DataV1 *m, const O62MV4DataBankV1 *bank,
    uint16_t max_block_size, bool supports64, uint64_t session_tag)
{
    O62MV4DataV1 trial;
    if (m == NULL || bank == NULL || session_tag == 0 ||
        (max_block_size != 512 && max_block_size != 1024 &&
         max_block_size != 2048)) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (o62m_v4_data_overlap_v1(m, sizeof(*m), bank, sizeof(*bank))) {
        return O62M_V4_DATA_ALIAS_V1;
    }
    memset(&trial, 0, sizeof(trial));
    trial.bank = *bank;
    trial.max_block_size = max_block_size;
    trial.supports64 = supports64;
    trial.owner = (uintptr_t)m;
    trial.session_tag = session_tag;
    trial.generation = 1;
    trial.magic = O62M_V4_DATA_MAGIC_V1;
    o62m_v4_data_idle_job_v1(&trial);
    *m = trial;
    return O62M_V4_DATA_OK_V1;
}

static inline O62MV4DataResultV1 o62m_v4_data_program_v1(
    O62MV4DataV1 *m, const O62MV4DataBankV1 *bank)
{
    O62MV4DataV1 trial;
    if (m == NULL || bank == NULL) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (o62m_v4_data_overlap_v1(m, sizeof(*m), bank, sizeof(*bank))) {
        return O62M_V4_DATA_ALIAS_V1;
    }
    if (!o62m_v4_data_valid_v1(m) || o62m_v4_data_active_v1(m)) {
        return O62M_V4_DATA_BAD_STATE_V1;
    }
    if (m->generation == UINT64_MAX) {
        return O62M_V4_DATA_COUNTER_OVERFLOW_V1;
    }
    trial = *m;
    trial.bank = *bank;
    o62m_v4_data_idle_job_v1(&trial);
    ++trial.generation;
    *m = trial;
    return O62M_V4_DATA_OK_V1;
}

static inline O62MV4DataResultV1 o62m_v4_data_begin_v1(
    O62MV4DataV1 *m, const O62MV4DataConfigV1 *c)
{
    O62MV4DataV1 trial;
    uint32_t count;
    if (m == NULL || c == NULL) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (o62m_v4_data_overlap_v1(m, sizeof(*m), c, sizeof(*c))) {
        return O62M_V4_DATA_ALIAS_V1;
    }
    if (!o62m_v4_data_valid_v1(m) || o62m_v4_data_active_v1(m)) {
        return O62M_V4_DATA_BAD_STATE_V1;
    }
    if (!o62m_v4_data_config_valid_v1(m, c)) {
        return O62M_V4_DATA_BAD_CONFIG_V1;
    }
    if (!c->bce && c->multiblock) {
        return O62M_V4_DATA_OPEN_ENDED_UNSUPPORTED_V1;
    }
    if (m->generation == UINT64_MAX) {
        return O62M_V4_DATA_COUNTER_OVERFLOW_V1;
    }
    trial = *m;
    trial.config = *c;
    trial.block_offset = 0;
    trial.count_source = !c->bce ? O62M_V4_IMPLICIT_ONE_V1 :
        (c->hv4e && trial.bank.bcct16 == 0 ? O62M_V4_COUNT32_V1 :
                                             O62M_V4_COUNT16_V1);
    trial.phase = O62M_V4_PHASE_RUNNING_V1;
    count = o62m_v4_data_selected_count_v1(&trial);
    if (!c->multiblock && count > 1) {
        return O62M_V4_DATA_SINGLE_COUNT_UNSUPPORTED_V1;
    }
    if (count == 0) {
        trial.phase = O62M_V4_PHASE_EMPTY_V1;
    }
    ++trial.generation;
    *m = trial;
    return O62M_V4_DATA_OK_V1;
}

static inline uint32_t o62m_v4_data_remaining_v1(const O62MV4DataV1 *m)
{
    if (!o62m_v4_data_valid_v1(m) || m->phase == O62M_V4_PHASE_IDLE_V1) {
        return 0;
    }
    return o62m_v4_data_selected_count_v1(m);
}

static inline uint64_t o62m_v4_data_address_v1(const O62MV4DataV1 *m)
{
    if (!m->config.hv4e) {
        return m->bank.saar;
    }
    return m->config.a64b ? m->bank.dma58 : (uint32_t)m->bank.dma58;
}

static inline uint64_t o62m_v4_data_address_max_v1(const O62MV4DataV1 *m)
{
    return m->config.hv4e && m->config.a64b ? UINT64_MAX : UINT32_MAX;
}

static inline uint32_t o62m_v4_data_boundary_v1(const O62MV4DataV1 *m)
{
    return UINT32_C(1) << (12 + m->config.boundary_code);
}

static inline O62MV4DataResultV1 o62m_v4_data_plan_v1(
    const O62MV4DataV1 *m, uint32_t budget, bool functional_clock,
    O62MV4DataPlanV1 *out)
{
    O62MV4DataPlanV1 p;
    uint32_t bytes, boundary_left;
    if (m == NULL || out == NULL) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (o62m_v4_data_overlap_v1(m, sizeof(*m), out, sizeof(*out))) {
        return O62M_V4_DATA_ALIAS_V1;
    }
    if (!o62m_v4_data_valid_v1(m)) {
        return O62M_V4_DATA_BAD_STATE_V1;
    }
    if (m->phase != O62M_V4_PHASE_RUNNING_V1 || budget == 0) {
        return O62M_V4_DATA_NO_WORK_V1;
    }
    if (!functional_clock) {
        return O62M_V4_DATA_CLOCK_OFF_V1;
    }
    memset(&p, 0, sizeof(p));
    p.owner = m->owner;
    p.session_tag = m->session_tag;
    p.generation = m->generation;
    p.request_budget = budget;
    p.blocks_left = o62m_v4_data_selected_count_v1(m);
    p.block_offset = m->block_offset;
    p.engine = m->config.engine;
    bytes = m->config.block_size - m->block_offset;
    if (bytes > budget) {
        bytes = budget;
    }
    if (m->config.engine == O62M_V4_ENGINE_SDMA_V1) {
        uint32_t boundary = o62m_v4_data_boundary_v1(m);
        p.address_valid = true;
        p.address = o62m_v4_data_address_v1(m);
        boundary_left = boundary - (uint32_t)(p.address & (boundary - 1));
        if (bytes > boundary_left) {
            bytes = boundary_left;
        }
        if (p.address > o62m_v4_data_address_max_v1(m) - bytes) {
            return O62M_V4_DATA_ADDRESS_OVERFLOW_V1;
        }
    }
    p.bytes = bytes;
    memcpy(out, &p, sizeof(p));
    return O62M_V4_DATA_OK_V1;
}

static inline O62MV4DataResultV1 o62m_v4_data_validate_commit_v1(
    const O62MV4DataV1 *m, const O62MV4DataPlanV1 *p,
    uint32_t actual_bytes, bool functional_clock)
{
    O62MV4DataPlanV1 expected;
    O62MV4DataResultV1 r;
    if (m == NULL || p == NULL) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (o62m_v4_data_overlap_v1(m, sizeof(*m), p, sizeof(*p))) {
        return O62M_V4_DATA_ALIAS_V1;
    }
    if (!o62m_v4_data_valid_v1(m)) {
        return O62M_V4_DATA_BAD_STATE_V1;
    }
    if (!functional_clock) {
        return O62M_V4_DATA_CLOCK_OFF_V1;
    }
    r = o62m_v4_data_plan_v1(m, p->request_budget, functional_clock, &expected);
    if (r != O62M_V4_DATA_OK_V1 ||
        p->owner != expected.owner || p->session_tag != expected.session_tag ||
        p->generation != expected.generation || p->address != expected.address ||
        p->request_budget != expected.request_budget || p->bytes != expected.bytes ||
        p->blocks_left != expected.blocks_left || p->block_offset != expected.block_offset ||
        p->engine != expected.engine || p->address_valid != expected.address_valid) {
        return O62M_V4_DATA_STALE_PLAN_V1;
    }
    if (actual_bytes == 0 || actual_bytes > p->bytes) {
        return O62M_V4_DATA_BAD_COMMIT_V1;
    }
    if (m->generation == UINT64_MAX || m->commit_count == UINT64_MAX ||
        m->bytes_committed > UINT64_MAX - actual_bytes ||
        ((uint32_t)m->block_offset + actual_bytes == m->config.block_size &&
         m->blocks_committed == UINT64_MAX)) {
        return O62M_V4_DATA_COUNTER_OVERFLOW_V1;
    }
    return O62M_V4_DATA_OK_V1;
}

static inline O62MV4DataResultV1 o62m_v4_data_commit_v1(
    O62MV4DataV1 *m, const O62MV4DataPlanV1 *p, uint32_t actual_bytes,
    bool functional_clock, O62MV4DataFactsV1 *facts)
{
    O62MV4DataV1 trial;
    O62MV4DataFactsV1 f;
    O62MV4DataResultV1 r;
    uint32_t new_offset;
    if (m == NULL || p == NULL || facts == NULL) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (o62m_v4_data_overlap_v1(m, sizeof(*m), facts, sizeof(*facts)) ||
        o62m_v4_data_overlap_v1(p, sizeof(*p), facts, sizeof(*facts))) {
        return O62M_V4_DATA_ALIAS_V1;
    }
    r = o62m_v4_data_validate_commit_v1(m, p, actual_bytes, functional_clock);
    if (r != O62M_V4_DATA_OK_V1) {
        return r;
    }
    trial = *m;
    memset(&f, 0, sizeof(f));
    f.bytes = actual_bytes;
    if (trial.config.engine == O62M_V4_ENGINE_SDMA_V1) {
        uint64_t new_address = p->address + actual_bytes;
        if (!trial.config.hv4e) {
            trial.bank.saar = (uint32_t)new_address;
        } else if (trial.config.a64b) {
            trial.bank.dma58 = new_address;
        } else {
            trial.bank.dma58 = (trial.bank.dma58 & UINT64_C(0xffffffff00000000)) |
                               (uint32_t)new_address;
        }
        f.boundary_hit = (new_address & (o62m_v4_data_boundary_v1(&trial) - 1)) == 0;
    }
    new_offset = (uint32_t)trial.block_offset + actual_bytes;
    if (new_offset == trial.config.block_size) {
        trial.block_offset = 0;
        f.completed_blocks = 1;
        ++trial.blocks_committed;
        if (trial.count_source == O62M_V4_COUNT16_V1) {
            --trial.bank.bcct16;
        } else if (trial.count_source == O62M_V4_COUNT32_V1) {
            --trial.bank.saar;
        }
        f.data_exhausted = trial.count_source == O62M_V4_IMPLICIT_ONE_V1 ||
                           o62m_v4_data_selected_count_v1(&trial) == 0;
    } else {
        trial.block_offset = (uint16_t)new_offset;
    }
    trial.bytes_committed += actual_bytes;
    ++trial.commit_count;
    ++trial.generation;
    trial.phase = f.data_exhausted ? O62M_V4_PHASE_DONE_V1 :
                  f.boundary_hit ? O62M_V4_PHASE_WAIT_BOUNDARY_V1 :
                                   O62M_V4_PHASE_RUNNING_V1;
    *m = trial;
    *facts = f;
    return O62M_V4_DATA_OK_V1;
}

static inline O62MV4DataResultV1 o62m_v4_data_resume_v1(
    O62MV4DataV1 *m, uint64_t address)
{
    O62MV4DataV1 trial;
    if (m == NULL) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (!o62m_v4_data_valid_v1(m) ||
        m->phase != O62M_V4_PHASE_WAIT_BOUNDARY_V1 ||
        m->config.engine != O62M_V4_ENGINE_SDMA_V1) {
        return O62M_V4_DATA_BAD_STATE_V1;
    }
    if (address > o62m_v4_data_address_max_v1(m)) {
        return O62M_V4_DATA_ADDRESS_OVERFLOW_V1;
    }
    if (m->generation == UINT64_MAX) {
        return O62M_V4_DATA_COUNTER_OVERFLOW_V1;
    }
    trial = *m;
    if (!trial.config.hv4e) {
        trial.bank.saar = (uint32_t)address;
    } else if (trial.config.a64b) {
        trial.bank.dma58 = address;
    } else {
        trial.bank.dma58 = (trial.bank.dma58 & UINT64_C(0xffffffff00000000)) |
                           (uint32_t)address;
    }
    trial.phase = O62M_V4_PHASE_RUNNING_V1;
    ++trial.generation;
    *m = trial;
    return O62M_V4_DATA_OK_V1;
}

static inline O62MV4DataResultV1 o62m_v4_data_count_write_v1(
    O62MV4DataV1 *m, bool count32, uint32_t value, bool *ignored)
{
    O62MV4DataV1 trial;
    if (m == NULL || ignored == NULL || (!count32 && value > UINT16_MAX)) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (o62m_v4_data_overlap_v1(m, sizeof(*m), ignored, sizeof(*ignored))) {
        return O62M_V4_DATA_ALIAS_V1;
    }
    if (!o62m_v4_data_valid_v1(m)) {
        return O62M_V4_DATA_BAD_STATE_V1;
    }
    if (o62m_v4_data_active_v1(m)) {
        if (count32 && m->count_source != O62M_V4_COUNT32_V1) {
            return O62M_V4_DATA_UNSUPPORTED_ROLE_V1;
        }
        *ignored = true;
        return O62M_V4_DATA_OK_V1;
    }
    if (m->generation == UINT64_MAX) {
        return O62M_V4_DATA_COUNTER_OVERFLOW_V1;
    }
    trial = *m;
    if (count32) {
        trial.bank.saar = value;
    } else {
        trial.bank.bcct16 = (uint16_t)value;
    }
    o62m_v4_data_idle_job_v1(&trial);
    ++trial.generation;
    *m = trial;
    *ignored = false;
    return O62M_V4_DATA_OK_V1;
}

static inline O62MV4DataResultV1 o62m_v4_data_abort_v1(O62MV4DataV1 *m)
{
    if (m == NULL) {
        return O62M_V4_DATA_BAD_ARGUMENT_V1;
    }
    if (!o62m_v4_data_valid_v1(m) || !o62m_v4_data_active_v1(m)) {
        return O62M_V4_DATA_BAD_STATE_V1;
    }
    if (m->generation == UINT64_MAX) {
        return O62M_V4_DATA_COUNTER_OVERFLOW_V1;
    }
    ++m->generation;
    m->phase = O62M_V4_PHASE_ABORTED_V1;
    return O62M_V4_DATA_OK_V1;
}

static inline const char *o62m_v4_data_reason_v1(O62MV4DataResultV1 r)
{
    switch (r) {
    case O62M_V4_DATA_OK_V1: return "ok";
    case O62M_V4_DATA_BAD_ARGUMENT_V1: return "bad-argument";
    case O62M_V4_DATA_BAD_STATE_V1: return "bad-state";
    case O62M_V4_DATA_BAD_CONFIG_V1: return "bad-config";
    case O62M_V4_DATA_OPEN_ENDED_UNSUPPORTED_V1: return "open-ended-unsupported";
    case O62M_V4_DATA_SINGLE_COUNT_UNSUPPORTED_V1: return "single-count-unsupported";
    case O62M_V4_DATA_NO_WORK_V1: return "no-work";
    case O62M_V4_DATA_CLOCK_OFF_V1: return "clock-off";
    case O62M_V4_DATA_STALE_PLAN_V1: return "stale-plan";
    case O62M_V4_DATA_BAD_COMMIT_V1: return "bad-commit";
    case O62M_V4_DATA_ADDRESS_OVERFLOW_V1: return "address-overflow";
    case O62M_V4_DATA_COUNTER_OVERFLOW_V1: return "counter-overflow";
    case O62M_V4_DATA_UNSUPPORTED_ROLE_V1: return "unsupported-role";
    case O62M_V4_DATA_ALIAS_V1: return "alias";
    default: return "unknown-result";
    }
}

#endif
