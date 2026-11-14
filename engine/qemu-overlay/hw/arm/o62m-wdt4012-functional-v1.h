#ifndef O62M_WDT4012_FUNCTIONAL_V1_H
#define O62M_WDT4012_FUNCTIONAL_V1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/* Explicit functional research policy, not a silicon reset/clock model.
 * Parent supplies the original firmware's 15 custom TOP periods in cycles.
 * CRR=0x76 captures low-TOP even while disabled; EN uses that preload, or
 * current low-TOP if none exists. TOP_INIT is retained configuration only.
 * TORR changes do not reload an already running/preloaded counter. CCVR is
 * remaining cycles (not a claim that physical CCVR reads N instead of N-1).
 * The first actual deadline latches EXPIRED and returns FIRST_EXPIRY. No IRQ,
 * reset, EOI/STAT behavior, EN-clear stop, or success is synthesized.
 * Caller serializes access and schedules next_deadline on its virtual clock.
 */
#define O62M_WDT4012_MAGIC_V1 UINT32_C(0x57445431)
#define O62M_WDT4012_TOP_COUNT_V1 15u
#define O62M_WDT4012_EN_V1 UINT32_C(1)
#define O62M_WDT4012_CR_FIELDS_V1 UINT32_C(3)
#define O62M_WDT4012_NS_PER_SECOND_V1 UINT64_C(1000000000)

typedef enum O62MWdt4012ResultV1 {
    O62M_WDT4012_OK_V1 = 0,
    O62M_WDT4012_ARGUMENT_V1,
    O62M_WDT4012_DISABLED_V1,
    O62M_WDT4012_STATE_V1,
    O62M_WDT4012_TIME_V1,
    O62M_WDT4012_OVERFLOW_V1,
    O62M_WDT4012_ACCESS_V1,
    O62M_WDT4012_UNSUPPORTED_V1,
    O62M_WDT4012_FIRST_EXPIRY_V1
} O62MWdt4012ResultV1;

typedef struct O62MWdt4012V1 {
    uint32_t magic, clock_hz, cr, torr;
    uint32_t top_cycles[O62M_WDT4012_TOP_COUNT_V1];
    uint32_t preload_cycles, loaded_cycles;
    uint64_t generation, starts, feeds, preloads;
    int64_t started_ns, deadline_ns, last_ns;
    bool profile_enabled, preload_valid, running, expired;
} O62MWdt4012V1;

static inline bool o62m_wdt4012_overlap_v1(const void *a, size_t na,
                                         const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < na : x - y < nb;
}

static inline bool o62m_wdt4012_torr_valid_v1(uint32_t torr)
{
    return (torr & 15u) < O62M_WDT4012_TOP_COUNT_V1 &&
           ((torr >> 4) & 15u) < O62M_WDT4012_TOP_COUNT_V1;
}

static inline bool o62m_wdt4012_duration_v1(uint32_t cycles, uint32_t hz,
                                          int64_t *duration)
{
    if (!cycles || !hz || !duration) {
        return false;
    }
    /* Both operands are bounded u32: this product fits in u64. */
    uint64_t product = (uint64_t)cycles * O62M_WDT4012_NS_PER_SECOND_V1;
    uint64_t ns = product / hz + (product % hz != 0);
    if (!ns || ns > INT64_MAX) {
        return false;
    }
    *duration = (int64_t)ns;
    return true;
}

static inline bool o62m_wdt4012_valid_v1(const O62MWdt4012V1 *m)
{
    if (!m || m->magic != O62M_WDT4012_MAGIC_V1) {
        return false;
    }
    if (!m->profile_enabled) {
        return !m->running && !m->expired && !m->preload_valid;
    }
    if (!m->clock_hz || m->last_ns < 0 || m->started_ns < 0 ||
        !o62m_wdt4012_torr_valid_v1(m->torr) || m->starts > 1 ||
        (m->preload_valid && (!m->preload_cycles || m->running || m->expired)) ||
        (m->running && m->expired) ||
        m->generation < m->preloads ||
        m->generation - m->preloads < m->feeds ||
        m->generation - m->preloads - m->feeds != m->starts ||
        (!!(m->cr & O62M_WDT4012_EN_V1) != (m->running || m->expired))) {
        return false;
    }
    if (!m->running && !m->expired &&
        (m->starts || m->feeds || m->loaded_cycles || m->deadline_ns != -1)) {
        return false;
    }
    for (unsigned i = 0; i < O62M_WDT4012_TOP_COUNT_V1; ++i) {
        if (!m->top_cycles[i]) {
            return false;
        }
    }
    if (m->running || m->expired) {
        int64_t duration;
        if (!m->loaded_cycles || m->starts != 1 || !m->generation ||
            !o62m_wdt4012_duration_v1(m->loaded_cycles, m->clock_hz, &duration) ||
            m->started_ns > INT64_MAX - duration ||
            m->deadline_ns != m->started_ns + duration ||
            m->last_ns < m->started_ns ||
            (m->running && m->last_ns >= m->deadline_ns) ||
            (m->expired && m->last_ns < m->deadline_ns)) {
            return false;
        }
    }
    return true;
}

static inline O62MWdt4012ResultV1 o62m_wdt4012_init_v1(
    O62MWdt4012V1 *m, bool profile_enabled, uint32_t hz,
    uint32_t initial_cr, uint32_t initial_torr,
    const uint32_t *top_cycles, size_t count, int64_t now_ns)
{
    if (!m) {
        return O62M_WDT4012_ARGUMENT_V1;
    }
    O62MWdt4012V1 next;
    memset(&next, 0, sizeof(next));
    next.magic = O62M_WDT4012_MAGIC_V1;
    if (!profile_enabled) {
        *m = next;
        return O62M_WDT4012_OK_V1;
    }
    if (!top_cycles || count != O62M_WDT4012_TOP_COUNT_V1 || !hz || now_ns < 0 ||
        (initial_cr & O62M_WDT4012_EN_V1) ||
        !o62m_wdt4012_torr_valid_v1(initial_torr)) {
        return O62M_WDT4012_ARGUMENT_V1;
    }
    for (unsigned i = 0; i < O62M_WDT4012_TOP_COUNT_V1; ++i) {
        if (!top_cycles[i]) {
            return O62M_WDT4012_ARGUMENT_V1;
        }
        next.top_cycles[i] = top_cycles[i];
    }
    next.profile_enabled = true;
    next.clock_hz = hz;
    next.cr = initial_cr;
    next.torr = initial_torr;
    next.started_ns = next.last_ns = now_ns;
    next.deadline_ns = -1;
    *m = next;
    return O62M_WDT4012_OK_V1;
}

/* FIRST_EXPIRY is a committed terminal event, not an ordinary failure.
 * All other non-OK results leave model unchanged. No IRQ/reset is issued. */
static inline O62MWdt4012ResultV1 o62m_wdt4012_advance_v1(
    O62MWdt4012V1 *m, int64_t now_ns)
{
    if (!o62m_wdt4012_valid_v1(m)) {
        return O62M_WDT4012_STATE_V1;
    }
    if (!m->profile_enabled) {
        return O62M_WDT4012_DISABLED_V1;
    }
    if (now_ns < m->last_ns) {
        return O62M_WDT4012_TIME_V1;
    }
    if (m->expired) {
        return O62M_WDT4012_FIRST_EXPIRY_V1;
    }
    m->last_ns = now_ns;
    if (m->running && now_ns >= m->deadline_ns) {
        m->running = false;
        m->expired = true;
        return O62M_WDT4012_FIRST_EXPIRY_V1;
    }
    return O62M_WDT4012_OK_V1;
}

/* Internal helpers require a previously validated local trial. They are not
 * public MMIO entry points; integrations use init/access/advance/next_deadline. */
static inline O62MWdt4012ResultV1 o62m_wdt4012_reload_v1(
    O62MWdt4012V1 *m, uint32_t cycles, int64_t now_ns)
{
    int64_t duration;
    if (!o62m_wdt4012_duration_v1(cycles, m->clock_hz, &duration) ||
        now_ns > INT64_MAX - duration || m->generation == UINT64_MAX) {
        return O62M_WDT4012_OVERFLOW_V1;
    }
    m->loaded_cycles = cycles;
    m->started_ns = now_ns;
    m->deadline_ns = now_ns + duration;
    ++m->generation;
    return O62M_WDT4012_OK_V1;
}

static inline O62MWdt4012ResultV1 o62m_wdt4012_remaining_v1(
    const O62MWdt4012V1 *m, int64_t now_ns, uint32_t *out)
{
    if (!m->running) {
        *out = m->preload_valid ? m->preload_cycles : 0;
        return O62M_WDT4012_OK_V1;
    }
    uint64_t elapsed = (uint64_t)(now_ns - m->started_ns);
    uint64_t seconds = elapsed / O62M_WDT4012_NS_PER_SECOND_V1;
    uint64_t sub_ns = elapsed % O62M_WDT4012_NS_PER_SECOND_V1;
    if (seconds > UINT64_MAX / m->clock_hz) {
        return O62M_WDT4012_OVERFLOW_V1;
    }
    uint64_t whole = seconds * m->clock_hz;
    uint64_t partial = (sub_ns * m->clock_hz) / O62M_WDT4012_NS_PER_SECOND_V1;
    if (whole > UINT64_MAX - partial || whole + partial >= m->loaded_cycles) {
        return O62M_WDT4012_STATE_V1;
    }
    *out = m->loaded_cycles - (uint32_t)(whole + partial);
    return O62M_WDT4012_OK_V1;
}

/* Offset API accepts 32-bit MMIO only: CR00, TORR04, CCVR08(read), CRR0c(write).
 * Invalid protocol and ordinary failures leave model/value unchanged.
 * A valid access at/past deadline instead commits FIRST_EXPIRY and does not
 * modify value. This terminal event cannot be rescued by a late feed.
 */
static inline O62MWdt4012ResultV1 o62m_wdt4012_access_v1(
    O62MWdt4012V1 *m, int64_t now_ns, uint32_t offset,
    unsigned size, bool write, uint64_t *value)
{
    if (!m || !value || o62m_wdt4012_overlap_v1(m, sizeof(*m), value, sizeof(*value))) {
        return O62M_WDT4012_ARGUMENT_V1;
    }
    if (!o62m_wdt4012_valid_v1(m)) {
        return O62M_WDT4012_STATE_V1;
    }
    if (!m->profile_enabled) {
        return O62M_WDT4012_DISABLED_V1;
    }
    if (size != 4 || (offset & 3u) || (write && *value > UINT32_MAX)) {
        return O62M_WDT4012_ACCESS_V1;
    }
    uint32_t data = write ? (uint32_t)*value : 0;
    if (write) {
        if (offset == 0) {
            if (((data ^ m->cr) & ~O62M_WDT4012_CR_FIELDS_V1) ||
                ((m->cr & O62M_WDT4012_EN_V1) && !(data & O62M_WDT4012_EN_V1))) {
                return O62M_WDT4012_UNSUPPORTED_V1;
            }
        } else if (offset == 4) {
            if (((data ^ m->torr) & ~UINT32_C(255)) || !o62m_wdt4012_torr_valid_v1(data)) {
                return O62M_WDT4012_UNSUPPORTED_V1;
            }
        } else if (offset != 12 || data != UINT32_C(0x76)) {
            return O62M_WDT4012_UNSUPPORTED_V1;
        }
    } else if (offset != 0 && offset != 4 && offset != 8) {
        return O62M_WDT4012_UNSUPPORTED_V1;
    }
    O62MWdt4012V1 next = *m;
    O62MWdt4012ResultV1 result = o62m_wdt4012_advance_v1(&next, now_ns);
    if (result == O62M_WDT4012_FIRST_EXPIRY_V1) {
        *m = next;
        return result;
    }
    if (result != O62M_WDT4012_OK_V1) {
        return result;
    }
    if (write && offset == 0) {
        if (!(next.cr & O62M_WDT4012_EN_V1) && (data & O62M_WDT4012_EN_V1)) {
            uint32_t cycles = next.preload_valid ? next.preload_cycles : next.top_cycles[next.torr & 15u];
            result = o62m_wdt4012_reload_v1(&next, cycles, now_ns);
            if (result != O62M_WDT4012_OK_V1) {
                return result;
            }
            next.preload_valid = false;
            next.running = true;
            ++next.starts;
        }
        next.cr = data;
    } else if (write && offset == 4) {
        next.torr = data;
    } else if (write) {
        uint32_t cycles = next.top_cycles[next.torr & 15u];
        if (next.running) {
            if (next.feeds == UINT64_MAX) {
                return O62M_WDT4012_OVERFLOW_V1;
            }
            result = o62m_wdt4012_reload_v1(&next, cycles, now_ns);
            if (result != O62M_WDT4012_OK_V1) {
                return result;
            }
            ++next.feeds;
        } else {
            if (next.preloads == UINT64_MAX || next.generation == UINT64_MAX) {
                return O62M_WDT4012_OVERFLOW_V1;
            }
            next.preload_cycles = cycles;
            next.preload_valid = true;
            ++next.preloads;
            ++next.generation;
        }
    } else if (offset == 0) {
        data = next.cr;
    } else if (offset == 4) {
        data = next.torr;
    } else {
        result = o62m_wdt4012_remaining_v1(&next, now_ns, &data);
        if (result != O62M_WDT4012_OK_V1) {
            return result;
        }
    }
    *m = next;
    if (!write) {
        *value = data;
    }
    return O62M_WDT4012_OK_V1;
}

static inline O62MWdt4012ResultV1 o62m_wdt4012_next_deadline_v1(
    const O62MWdt4012V1 *m, bool *present, int64_t *deadline_ns)
{
    if (!m || !present || !deadline_ns ||
        o62m_wdt4012_overlap_v1(m, sizeof(*m), present, sizeof(*present)) ||
        o62m_wdt4012_overlap_v1(m, sizeof(*m), deadline_ns, sizeof(*deadline_ns)) ||
        o62m_wdt4012_overlap_v1(present, sizeof(*present), deadline_ns, sizeof(*deadline_ns))) {
        return O62M_WDT4012_ARGUMENT_V1;
    }
    if (!o62m_wdt4012_valid_v1(m)) {
        return O62M_WDT4012_STATE_V1;
    }
    if (!m->profile_enabled) {
        return O62M_WDT4012_DISABLED_V1;
    }
    if (m->expired) {
        return O62M_WDT4012_FIRST_EXPIRY_V1;
    }
    *present = m->running;
    *deadline_ns = m->running ? m->deadline_ns : 0;
    return O62M_WDT4012_OK_V1;
}

#endif
