/* O62M timer64_1, bounded from original AP drivers. No recovered reset defaults.
 * Register helpers 60c37250..60c373c6, ops60f23368, oneshot60c36770/60c367e8.
 * Mode0 is used by the original oneshot client. Mode1 and +14 are unsupported.
 * The synchronizer delay is an explicit experimental parameter, not silicon data.
 * First expiration is modeled after load+1 cycles; subsequent terminal behavior
 * is explicitly selected HOLD_FIRST_EXPIRY. Firmware evidence does not establish
 * physical self-disable/reload behavior. No periodic behavior is claimed.
 * IRQ is NVIC external line10 (NuttX26). Parent must schedule next_event_ns and
 * call advance, then drive irq_level; MMIO polling alone is insufficient.
 */
#ifndef O62M_TIMER64_MODEL_H
#define O62M_TIMER64_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef unsigned __int128 O62MTimerWide;
enum { O62M_TIMER64_HOLD_FIRST_EXPIRY = 1 };
typedef struct O62MTimer64 {
    uint64_t load, snapshot, now_ns, latency_ns, deadline[3], expirations;
    uint64_t source_hz, fraction;
    O62MTimerWide remaining;
    uint32_t control, status_opaque, divisor;
    uint8_t pending; /* bit0=start/status2, bit1=snapshot/status1, bit2=ack/status3 */
    bool gate, running, raw_irq;
} O62MTimer64;

static inline bool o62m_timer64_irq(const O62MTimer64 *m)
{ return m->raw_irq && !(m->control & 2); }

static inline uint64_t o62m_timer64_count(const O62MTimer64 *m)
{ return m->remaining ? (uint64_t)(m->remaining - 1) : 0; }

static inline bool o62m_timer64_create(O62MTimer64 *m, uint64_t now,
        uint64_t explicit_load, uint64_t explicit_snapshot, uint32_t explicit_ctrl,
        uint32_t explicit_status, uint64_t explicit_latency_ns,
        uint64_t source_hz, uint32_t divisor, bool gate, unsigned explicit_terminal_policy)
{
    /* Initial enabled/mode1 or busy states need prior history, not fabricated completion. */
    if ((explicit_ctrl & 5) || (explicit_status & 14) || !explicit_latency_ns ||
        !source_hz || source_hz > 1000000000 || !divisor || divisor > 256 ||
        explicit_terminal_policy != O62M_TIMER64_HOLD_FIRST_EXPIRY) return false;
    memset(m, 0, sizeof(*m));
    m->now_ns = now; m->load = explicit_load; m->snapshot = explicit_snapshot;
    m->control = explicit_ctrl; m->status_opaque = explicit_status;
    m->latency_ns = explicit_latency_ns; m->source_hz = source_hz;
    m->divisor = divisor; m->gate = gate;
    return true;
}

static inline void o62m_timer64_elapsed(O62MTimer64 *m, uint64_t elapsed)
{
    if (!m->running || !m->gate) return;
    uint64_t denominator = (uint64_t)1000000000 * m->divisor;
    O62MTimerWide units = (O62MTimerWide)elapsed * m->source_hz + m->fraction;
    O62MTimerWide ticks = units / denominator;
    m->fraction = (uint64_t)(units % denominator);
    if (ticks >= m->remaining) {
        m->remaining = 0; m->running = false; m->raw_irq = true; m->expirations++;
    } else m->remaining -= ticks;
}

static inline bool o62m_timer64_advance(O62MTimer64 *m, uint64_t now)
{
    if (now < m->now_ns) return false;
    /* Synchronization cannot complete while the functional clock is stopped. */
    if (!m->gate) {
        uint64_t elapsed = now - m->now_ns;
        for (unsigned i=0;i<3;++i) if (m->pending&(1u<<i)) {
            if (elapsed>UINT64_MAX-m->deadline[i]) return false;
        }
        for (unsigned i=0;i<3;++i) if (m->pending&(1u<<i)) m->deadline[i]+=elapsed;
        m->now_ns=now; return true;
    }
    for (;;) {
        uint64_t next = now;
        for (unsigned i=0; i<3; ++i)
            if ((m->pending & (1u<<i)) && m->deadline[i] < next) next=m->deadline[i];
        o62m_timer64_elapsed(m, next - m->now_ns); m->now_ns = next;
        /* Start then snapshot then ACK is explicit same-time ordering. */
        for (unsigned i=0; i<3; ++i) if ((m->pending & (1u<<i)) && m->deadline[i] <= next) {
            m->pending &= ~(1u<<i);
            if (i==0 && (m->control & 1)) {
                m->remaining = (O62MTimerWide)m->load + 1;
                m->fraction = 0; m->running = true;
            } else if (i==1) m->snapshot = o62m_timer64_count(m);
            else if (i==2) m->raw_irq = false;
        }
        if (next == now) return true;
    }
}

static inline uint64_t o62m_timer64_next_event_ns(const O62MTimer64 *m)
{
    uint64_t next = UINT64_MAX;
    for (unsigned i=0;m->gate && i<3;++i)
        if ((m->pending & (1u<<i)) && m->deadline[i] < next) next=m->deadline[i];
    if (m->running && m->gate) {
        O62MTimerWide units=m->remaining*((uint64_t)1000000000*m->divisor)-m->fraction;
        O62MTimerWide delta=(units+m->source_hz-1)/m->source_hz;
        if (delta <= UINT64_MAX-m->now_ns && m->now_ns+(uint64_t)delta < next)
            next=m->now_ns+(uint64_t)delta;
    }
    return next;
}

static inline bool o62m_timer64_set_clock(O62MTimer64 *m, uint64_t now,
        uint64_t source_hz, uint32_t divisor, bool gate)
{
    if (!source_hz || source_hz>1000000000 || !divisor || divisor>256 || now<m->now_ns)
        return false;
    /* Live source/divisor transition behavior is not evidenced; gate pause is supported. */
    if ((m->running || (m->pending&1)) &&
        (source_hz!=m->source_hz || divisor!=m->divisor)) return false;
    o62m_timer64_advance(m,now);
    if (source_hz!=m->source_hz || divisor!=m->divisor) m->fraction=0;
    m->source_hz=source_hz; m->divisor=divisor; m->gate=gate;
    return true;
}

static inline bool o62m_timer64_read(O62MTimer64 *m, uint64_t now,
        uint32_t offset, uint32_t *value)
{
    if (!o62m_timer64_advance(m,now)) return false;
    switch(offset) {
    case 0: *value=(uint32_t)m->load; return true;
    case 4: *value=m->load>>32; return true;
    case 8: *value=(uint32_t)m->snapshot; return true;
    case 12: *value=m->snapshot>>32; return true;
    case 16: *value=m->control; return true;
    case 24: *value=!!(m->pending&2); return true;
    case 28: *value=!!(m->pending&4); return true;
    case 32: *value=m->status_opaque | ((m->pending&1)?4:0) |
                   ((m->pending&2)?2:0) | ((m->pending&4)?8:0); return true;
    default: return false;
    }
}

static inline bool o62m_timer64_write(O62MTimer64 *m, uint64_t now,
        uint32_t offset, uint32_t value)
{
    if (!o62m_timer64_advance(m,now)) return false;
    switch(offset) {
    case 0: case 4:
        /* Normal firmware always stops before changing load. */
        if (m->running || (m->pending&1)) return false;
        if (offset==0) m->load=(m->load&UINT64_C(0xffffffff00000000))|value;
        else m->load=(m->load&UINT64_C(0xffffffff))|((uint64_t)value<<32);
        return true;
    case 16:
        if (((value^m->control)&~3u) || (value&4)) return false;
        if ((value&1) && !(m->control&1)) {
            if (m->latency_ns>UINT64_MAX-now) return false;
            m->pending|=1; m->deadline[0]=now+m->latency_ns;
        } else if (!(value&1)) { m->running=false; m->pending&=~1u; }
        m->control=value; return true;
    case 24: case 28: {
        unsigned i=offset==24?1:2;
        if (value!=1 || (m->pending&(1u<<i)) || m->latency_ns>UINT64_MAX-now) return false;
        m->pending|=1u<<i; m->deadline[i]=now+m->latency_ns; return true;
    }
    default: return false;
    }
}
#endif
