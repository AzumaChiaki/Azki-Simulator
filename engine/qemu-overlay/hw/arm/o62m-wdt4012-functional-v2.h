#ifndef O62M_WDT4012_FUNCTIONAL_V2_H
#define O62M_WDT4012_FUNCTIONAL_V2_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/* Functional DW watchdog with the original custom TOP periods.
 * RMOD first expiry raises IRQ and reloads the next TOP at the elapsed deadline.
 * EOI clears IRQ without extending that deadline; CRR reload clears the stage.
 * Reset-mode/second expiry is latched for the board reset boundary. */
#define O62M_WDT4012_MAGIC_V2 UINT32_C(0x57445431)
#define O62M_WDT4012_TOP_COUNT_V2 15u
#define O62M_WDT4012_EN_V2 UINT32_C(1)
#define O62M_WDT4012_CR_FIELDS_V2 UINT32_C(3)
#define O62M_WDT4012_NS_PER_SECOND_V2 UINT64_C(1000000000)

typedef enum O62MWdt4012ResultV2 {
    O62M_WDT4012_OK_V2 = 0,
    O62M_WDT4012_ARGUMENT_V2,
    O62M_WDT4012_DISABLED_V2,
    O62M_WDT4012_STATE_V2,
    O62M_WDT4012_TIME_V2,
    O62M_WDT4012_OVERFLOW_V2,
    O62M_WDT4012_ACCESS_V2,
    O62M_WDT4012_UNSUPPORTED_V2,
    O62M_WDT4012_RESET_EXPIRY_V2
} O62MWdt4012ResultV2;

typedef struct O62MWdt4012V2 {
    uint32_t magic, clock_hz, cr, torr;
    uint32_t top_cycles[O62M_WDT4012_TOP_COUNT_V2];
    uint32_t preload_cycles, loaded_cycles;
    uint64_t generation, starts, feeds, preloads, hardware_reloads, expirations, irq_clears;
    int64_t started_ns, deadline_ns, last_ns;
    bool profile_enabled, preload_valid, running, expired, irq_pending, second_stage;
} O62MWdt4012V2;

static inline bool o62m_wdt4012_overlap_v2(const void *a, size_t na,
                                         const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < na : x - y < nb;
}

static inline bool o62m_wdt4012_torr_valid_v2(uint32_t torr)
{
    return (torr & 15u) < O62M_WDT4012_TOP_COUNT_V2 &&
           ((torr >> 4) & 15u) < O62M_WDT4012_TOP_COUNT_V2;
}

static inline bool o62m_wdt4012_duration_v2(uint32_t cycles, uint32_t hz,
                                          int64_t *duration)
{
    if (!cycles || !hz || !duration) {
        return false;
    }
    /* Both operands are bounded u32: this product fits in u64. */
    uint64_t product = (uint64_t)cycles * O62M_WDT4012_NS_PER_SECOND_V2;
    uint64_t ns = product / hz + (product % hz != 0);
    if (!ns || ns > INT64_MAX) {
        return false;
    }
    *duration = (int64_t)ns;
    return true;
}

static inline bool o62m_wdt4012_valid_v2(const O62MWdt4012V2 *m)
{
    if (!m || m->magic != O62M_WDT4012_MAGIC_V2) {
        return false;
    }
    if (!m->profile_enabled) {
        return !m->running && !m->expired && !m->preload_valid;
    }
    if (!m->clock_hz || m->last_ns < 0 || m->started_ns < 0 ||
        !o62m_wdt4012_torr_valid_v2(m->torr) || m->starts > 1 ||
        (m->preload_valid && (!m->preload_cycles || m->running || m->expired)) ||
        (m->running && m->expired) ||
        m->generation < m->preloads ||
        m->generation - m->preloads < m->feeds ||
        m->generation - m->preloads - m->feeds < m->hardware_reloads ||
        m->generation - m->preloads - m->feeds - m->hardware_reloads != m->starts ||
        (!!(m->cr & O62M_WDT4012_EN_V2) != (m->running || m->expired))) {
        return false;
    }
    if (!m->running && !m->expired &&
        (m->starts || m->feeds || m->loaded_cycles || m->deadline_ns != -1)) {
        return false;
    }
    for (unsigned i = 0; i < O62M_WDT4012_TOP_COUNT_V2; ++i) {
        if (!m->top_cycles[i]) {
            return false;
        }
    }
    if (m->running || m->expired) {
        int64_t duration;
        if (!m->loaded_cycles || m->starts != 1 || !m->generation ||
            !o62m_wdt4012_duration_v2(m->loaded_cycles, m->clock_hz, &duration) ||
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

static inline O62MWdt4012ResultV2 o62m_wdt4012_init_v2(
    O62MWdt4012V2 *m, bool profile_enabled, uint32_t hz,
    uint32_t initial_cr, uint32_t initial_torr,
    const uint32_t *top_cycles, size_t count, int64_t now_ns)
{
    if (!m) {
        return O62M_WDT4012_ARGUMENT_V2;
    }
    O62MWdt4012V2 next;
    memset(&next, 0, sizeof(next));
    next.magic = O62M_WDT4012_MAGIC_V2;
    if (!profile_enabled) {
        *m = next;
        return O62M_WDT4012_OK_V2;
    }
    if (!top_cycles || count != O62M_WDT4012_TOP_COUNT_V2 || !hz || now_ns < 0 ||
        (initial_cr & O62M_WDT4012_EN_V2) ||
        !o62m_wdt4012_torr_valid_v2(initial_torr)) {
        return O62M_WDT4012_ARGUMENT_V2;
    }
    for (unsigned i = 0; i < O62M_WDT4012_TOP_COUNT_V2; ++i) {
        if (!top_cycles[i]) {
            return O62M_WDT4012_ARGUMENT_V2;
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
    return O62M_WDT4012_OK_V2;
}

/* Expiry advances from the previous deadline, including a late callback. */
static inline O62MWdt4012ResultV2 o62m_wdt4012_advance_v2(
    O62MWdt4012V2 *m, int64_t now_ns)
{
    if (!o62m_wdt4012_valid_v2(m)) return O62M_WDT4012_STATE_V2;
    if (!m->profile_enabled) return O62M_WDT4012_DISABLED_V2;
    if (now_ns < m->last_ns) return O62M_WDT4012_TIME_V2;
    if (m->expired) return O62M_WDT4012_RESET_EXPIRY_V2;
    O62MWdt4012V2 next = *m;
    next.last_ns = now_ns;
    while (next.running && now_ns >= next.deadline_ns) {
        ++next.expirations;
        if ((next.cr & 2) && !next.second_stage) {
            int64_t duration;
            uint32_t cycles = next.top_cycles[next.torr & 15u];
            if (!o62m_wdt4012_duration_v2(cycles,next.clock_hz,&duration) ||
                next.deadline_ns > INT64_MAX-duration || next.generation==UINT64_MAX)
                return O62M_WDT4012_OVERFLOW_V2;
            next.started_ns = next.deadline_ns;
            next.deadline_ns += duration;
            next.loaded_cycles = cycles;
            next.irq_pending = next.second_stage = true;
            ++next.generation;
            ++next.hardware_reloads;
        } else {
            next.running = false;
            next.expired = true;
        }
    }
    *m = next;
    return next.expired ? O62M_WDT4012_RESET_EXPIRY_V2 : O62M_WDT4012_OK_V2;
}

/* Internal helpers require a previously validated local trial. They are not
 * public MMIO entry points; integrations use init/access/advance/next_deadline. */
static inline O62MWdt4012ResultV2 o62m_wdt4012_reload_v2(
    O62MWdt4012V2 *m, uint32_t cycles, int64_t now_ns)
{
    int64_t duration;
    if (!o62m_wdt4012_duration_v2(cycles, m->clock_hz, &duration) ||
        now_ns > INT64_MAX - duration || m->generation == UINT64_MAX) {
        return O62M_WDT4012_OVERFLOW_V2;
    }
    m->loaded_cycles = cycles;
    m->started_ns = now_ns;
    m->deadline_ns = now_ns + duration;
    ++m->generation;
    return O62M_WDT4012_OK_V2;
}

static inline O62MWdt4012ResultV2 o62m_wdt4012_remaining_v2(
    const O62MWdt4012V2 *m, int64_t now_ns, uint32_t *out)
{
    if (!m->running) {
        *out = m->preload_valid ? m->preload_cycles : 0;
        return O62M_WDT4012_OK_V2;
    }
    uint64_t elapsed = (uint64_t)(now_ns - m->started_ns);
    uint64_t seconds = elapsed / O62M_WDT4012_NS_PER_SECOND_V2;
    uint64_t sub_ns = elapsed % O62M_WDT4012_NS_PER_SECOND_V2;
    if (seconds > UINT64_MAX / m->clock_hz) {
        return O62M_WDT4012_OVERFLOW_V2;
    }
    uint64_t whole = seconds * m->clock_hz;
    uint64_t partial = (sub_ns * m->clock_hz) / O62M_WDT4012_NS_PER_SECOND_V2;
    if (whole > UINT64_MAX - partial || whole + partial >= m->loaded_cycles) {
        return O62M_WDT4012_STATE_V2;
    }
    *out = m->loaded_cycles - (uint32_t)(whole + partial);
    return O62M_WDT4012_OK_V2;
}

/* Offset API accepts CR00, TORR04, CCVR08, CRR0c, STAT10 and EOI14.
 * Invalid protocol and ordinary failures leave model/value unchanged.
 * A valid access at/past deadline instead commits RESET_EXPIRY and does not
 * modify value. This terminal event cannot be rescued by a late feed.
 */
static inline O62MWdt4012ResultV2 o62m_wdt4012_access_v2(
    O62MWdt4012V2 *m, int64_t now_ns, uint32_t offset,
    unsigned size, bool write, uint64_t *value)
{
    if (!m || !value || o62m_wdt4012_overlap_v2(m, sizeof(*m), value, sizeof(*value))) {
        return O62M_WDT4012_ARGUMENT_V2;
    }
    if (!o62m_wdt4012_valid_v2(m)) {
        return O62M_WDT4012_STATE_V2;
    }
    if (!m->profile_enabled) {
        return O62M_WDT4012_DISABLED_V2;
    }
    if (size != 4 || (offset & 3u) || (write && *value > UINT32_MAX)) {
        return O62M_WDT4012_ACCESS_V2;
    }
    uint32_t data = write ? (uint32_t)*value : 0;
    if (write) {
        if (offset == 0) {
            if (((data ^ m->cr) & ~O62M_WDT4012_CR_FIELDS_V2) ||
                ((m->cr & O62M_WDT4012_EN_V2) && !(data & O62M_WDT4012_EN_V2))) {
                return O62M_WDT4012_UNSUPPORTED_V2;
            }
        } else if (offset == 4) {
            if (((data ^ m->torr) & ~UINT32_C(255)) || !o62m_wdt4012_torr_valid_v2(data)) {
                return O62M_WDT4012_UNSUPPORTED_V2;
            }
        } else if (offset != 12 || data != UINT32_C(0x76)) {
            return O62M_WDT4012_UNSUPPORTED_V2;
        }
    } else if (offset != 0 && offset != 4 && offset != 8 && offset != 16 && offset != 20) {
        return O62M_WDT4012_UNSUPPORTED_V2;
    }
    O62MWdt4012V2 next = *m;
    O62MWdt4012ResultV2 result = o62m_wdt4012_advance_v2(&next, now_ns);
    if (result == O62M_WDT4012_RESET_EXPIRY_V2) {
        *m = next;
        return result;
    }
    if (result != O62M_WDT4012_OK_V2) {
        return result;
    }
    if (write && offset == 0) {
        if (!(next.cr & O62M_WDT4012_EN_V2) && (data & O62M_WDT4012_EN_V2)) {
            uint32_t cycles = next.preload_valid ? next.preload_cycles : next.top_cycles[next.torr & 15u];
            result = o62m_wdt4012_reload_v2(&next, cycles, now_ns);
            if (result != O62M_WDT4012_OK_V2) {
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
        next.irq_pending = next.second_stage = false;
        uint32_t cycles = next.top_cycles[next.torr & 15u];
        if (next.running) {
            if (next.feeds == UINT64_MAX) {
                return O62M_WDT4012_OVERFLOW_V2;
            }
            result = o62m_wdt4012_reload_v2(&next, cycles, now_ns);
            if (result != O62M_WDT4012_OK_V2) {
                return result;
            }
            ++next.feeds;
        } else {
            if (next.preloads == UINT64_MAX || next.generation == UINT64_MAX) {
                return O62M_WDT4012_OVERFLOW_V2;
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
    } else if (offset == 16) {
        data = next.irq_pending;
    } else if (offset == 20) {
        data = 0;
        next.irq_pending = false;
        ++next.irq_clears;
    } else {
        result = o62m_wdt4012_remaining_v2(&next, now_ns, &data);
        if (result != O62M_WDT4012_OK_V2) {
            return result;
        }
    }
    *m = next;
    if (!write) {
        *value = data;
    }
    return O62M_WDT4012_OK_V2;
}

static inline O62MWdt4012ResultV2 o62m_wdt4012_next_deadline_v2(
    const O62MWdt4012V2 *m, bool *present, int64_t *deadline_ns)
{
    if (!m || !present || !deadline_ns ||
        o62m_wdt4012_overlap_v2(m, sizeof(*m), present, sizeof(*present)) ||
        o62m_wdt4012_overlap_v2(m, sizeof(*m), deadline_ns, sizeof(*deadline_ns)) ||
        o62m_wdt4012_overlap_v2(present, sizeof(*present), deadline_ns, sizeof(*deadline_ns))) {
        return O62M_WDT4012_ARGUMENT_V2;
    }
    if (!o62m_wdt4012_valid_v2(m)) {
        return O62M_WDT4012_STATE_V2;
    }
    if (!m->profile_enabled) {
        return O62M_WDT4012_DISABLED_V2;
    }
    if (m->expired) {
        return O62M_WDT4012_RESET_EXPIRY_V2;
    }
    *present = m->running;
    *deadline_ns = m->running ? m->deadline_ns : 0;
    return O62M_WDT4012_OK_V2;
}

#endif
