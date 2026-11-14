#ifndef O62M_SHOT_TIMER_RUNTIME_H
#define O62M_SHOT_TIMER_RUNTIME_H

/* Original mode0 timer access subset. No reset values, IRQ routing, MMIO aliases,
 * mode1, periodic reload or silicon terminal behavior are invented here.
 * The adapter must schedule next_event and check every Result from advance.
 * All failed transactions preserve the complete state and read output.
 * Public structs are inspectable; callers must not mutate state or alias outputs
 * into state. Each instance and its effective input clock remain independent. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef unsigned __int128 O62MShotWide;

typedef enum O62MShotPolicy {
    O62M_SHOT_FIRST_EXPIRY_UNSUPPORTED = 0,
    /* Explicit virtual one-shot choice, not independently verified silicon. */
    O62M_SHOT_HOLD_ZERO_WITH_IRQ = 1
} O62MShotPolicy;

typedef enum O62MShotResult {
    O62M_SHOT_OK = 0,
    O62M_SHOT_BAD_ARGUMENT,
    O62M_SHOT_BAD_INPUT,
    O62M_SHOT_BAD_STATE,
    O62M_SHOT_BAD_ACCESS,
    O62M_SHOT_UNSUPPORTED_REGISTER,
    O62M_SHOT_READ_ONLY,
    O62M_SHOT_INVALID_CONTROL,
    O62M_SHOT_BUSY,
    O62M_SHOT_ACTIVE_CLOCK_CHANGE,
    O62M_SHOT_TIME_REVERSED,
    O62M_SHOT_TIME_OVERFLOW,
    O62M_SHOT_FIRST_EXPIRY_BOUNDARY,
    O62M_SHOT_EXPIRATION_OVERFLOW
} O62MShotResult;

typedef struct O62MShotInit {
    uint64_t now_ns, load, snapshot, latency_ns, source_hz;
    uint32_t control, status_opaque, divisor;
    uint32_t snapshot_request_opaque, ack_request_opaque;
    bool gate, raw_irq;
    O62MShotPolicy policy;
} O62MShotInit;

typedef struct O62MShotTimer {
    uint64_t load, snapshot, now_ns, latency_ns, source_hz, fraction;
    uint64_t deadline[3], expirations;
    O62MShotWide remaining; /* load+1 cycles, including the UINT64_MAX load case */
    uint32_t control, status_opaque, divisor;
    uint32_t snapshot_request_opaque, ack_request_opaque;
    uint8_t pending; /* bit0=start/status2, bit1=snapshot/status1, bit2=ACK/status3 */
    bool gate, running, raw_irq, initialized;
    O62MShotPolicy policy;
} O62MShotTimer;

static inline bool o62m_shot_clock_valid(uint64_t hz, uint32_t divisor)
{ return hz && hz <= UINT64_C(1000000000) && divisor && divisor <= 256; }

static inline bool o62m_shot_state_valid(const O62MShotTimer *m)
{
    if (!m || !m->initialized || !o62m_shot_clock_valid(m->source_hz,m->divisor) ||
        !m->latency_ns || (m->control&4u) || (m->status_opaque&14u) ||
        (m->snapshot_request_opaque&1u) || (m->ack_request_opaque&1u) ||
        (m->pending&~7u) ||
        (m->policy!=O62M_SHOT_FIRST_EXPIRY_UNSUPPORTED &&
         m->policy!=O62M_SHOT_HOLD_ZERO_WITH_IRQ) ||
        m->fraction >= UINT64_C(1000000000)*m->divisor ||
        m->remaining > ((O62MShotWide)UINT64_MAX+1) ||
        (m->running && (!m->remaining || !(m->control&1u)))) return false;
    for (unsigned i=0;i<3;i++)
        if ((m->pending&(1u<<i)) && m->deadline[i]<m->now_ns) return false;
    return true;
}

static inline O62MShotResult o62m_shot_timer_create(
        O62MShotTimer *m, const O62MShotInit *in)
{
    if (!m || !in) return O62M_SHOT_BAD_ARGUMENT;
    /* Enabled/busy initial states need history, not fabricated completion. */
    if ((in->control&5u) || (in->status_opaque&14u) ||
        (in->snapshot_request_opaque&1u) || (in->ack_request_opaque&1u) ||
        !in->latency_ns || !o62m_shot_clock_valid(in->source_hz,in->divisor) ||
        (in->policy!=O62M_SHOT_FIRST_EXPIRY_UNSUPPORTED &&
         in->policy!=O62M_SHOT_HOLD_ZERO_WITH_IRQ)) return O62M_SHOT_BAD_INPUT;
    O62MShotTimer n;
    memset(&n,0,sizeof(n));
    n.load=in->load; n.snapshot=in->snapshot; n.now_ns=in->now_ns;
    n.latency_ns=in->latency_ns; n.source_hz=in->source_hz;
    n.control=in->control; n.status_opaque=in->status_opaque; n.divisor=in->divisor;
    n.snapshot_request_opaque=in->snapshot_request_opaque;
    n.ack_request_opaque=in->ack_request_opaque;
    n.gate=in->gate; n.raw_irq=in->raw_irq; n.policy=in->policy;
    n.initialized=true;
    *m=n;
    return O62M_SHOT_OK;
}

static inline uint64_t o62m_shot_count_(const O62MShotTimer *m)
{ return m->remaining ? (uint64_t)(m->remaining-1) : 0; }

/* True iff an expiry deadline exists in the uint64 ns time horizon. */
static inline bool o62m_shot_expiry_(const O62MShotTimer *m, uint64_t *when)
{
    if (!m->running || !m->gate) return false;
    O62MShotWide units=m->remaining*(UINT64_C(1000000000)*m->divisor)-m->fraction;
    O62MShotWide delta=(units+m->source_hz-1)/m->source_hz;
    if (delta>UINT64_MAX-m->now_ns) return false;
    *when=m->now_ns+(uint64_t)delta;
    return true;
}

/* Internal trial-state operation. Never call this directly from an adapter. */
static inline O62MShotResult o62m_shot_elapsed_(O62MShotTimer *m,
        uint64_t elapsed, bool *expired)
{
    *expired=false;
    if (!m->running || !m->gate) return O62M_SHOT_OK;
    uint64_t denom=UINT64_C(1000000000)*m->divisor;
    O62MShotWide units=(O62MShotWide)elapsed*m->source_hz+m->fraction;
    O62MShotWide ticks=units/denom;
    if (ticks>=m->remaining) {
        if (m->policy==O62M_SHOT_FIRST_EXPIRY_UNSUPPORTED)
            return O62M_SHOT_FIRST_EXPIRY_BOUNDARY;
        if (m->expirations==UINT64_MAX) return O62M_SHOT_EXPIRATION_OVERFLOW;
        m->remaining=0; m->fraction=0; m->running=false; m->expirations++;
        *expired=true;
    } else {
        m->remaining-=ticks; m->fraction=(uint64_t)(units%denom);
    }
    return O62M_SHOT_OK;
}

static inline O62MShotResult o62m_shot_advance_(O62MShotTimer *m, uint64_t now)
{
    if (now<m->now_ns) return O62M_SHOT_TIME_REVERSED;
    if (!m->gate) {
        uint64_t elapsed=now-m->now_ns;
        for (unsigned i=0;i<3;i++) if ((m->pending&(1u<<i)) &&
                elapsed>UINT64_MAX-m->deadline[i]) return O62M_SHOT_TIME_OVERFLOW;
        for (unsigned i=0;i<3;i++) if (m->pending&(1u<<i)) m->deadline[i]+=elapsed;
        m->now_ns=now;
        return O62M_SHOT_OK;
    }
    for (;;) {
        uint64_t next=now, expiry;
        for (unsigned i=0;i<3;i++)
            if ((m->pending&(1u<<i)) && m->deadline[i]<next) next=m->deadline[i];
        if (o62m_shot_expiry_(m,&expiry) && expiry<next) next=expiry;
        bool expired;
        O62MShotResult result=o62m_shot_elapsed_(m,next-m->now_ns,&expired);
        if (result!=O62M_SHOT_OK) return result;
        m->now_ns=next;
        /* ACK retires the old IRQ; an expiry in this same time slice is asserted
         * afterwards, so an unrelated/new event cannot be lost to old ACK. */
        if ((m->pending&4u) && m->deadline[2]<=next) {
            m->pending&=(uint8_t)~4u; m->raw_irq=false;
        }
        if ((m->pending&1u) && m->deadline[0]<=next) {
            m->pending&=(uint8_t)~1u;
            if (m->control&1u) {
                m->remaining=(O62MShotWide)m->load+1;
                m->fraction=0; m->running=true;
            }
        }
        if ((m->pending&2u) && m->deadline[1]<=next) {
            m->pending&=(uint8_t)~2u; m->snapshot=o62m_shot_count_(m);
        }
        if (expired) m->raw_irq=true;
        if (next==now) return O62M_SHOT_OK;
    }
}

static inline O62MShotResult o62m_shot_timer_advance(O62MShotTimer *m,uint64_t now)
{
    if (!m) return O62M_SHOT_BAD_ARGUMENT;
    if (!o62m_shot_state_valid(m)) return O62M_SHOT_BAD_STATE;
    O62MShotTimer trial=*m;
    O62MShotResult r=o62m_shot_advance_(&trial,now);
    if (r==O62M_SHOT_OK) *m=trial;
    return r;
}

static inline O62MShotResult o62m_shot_timer_set_clock(O62MShotTimer *m,
        uint64_t now,uint64_t hz,uint32_t divisor,bool gate)
{
    if (!m) return O62M_SHOT_BAD_ARGUMENT;
    if (!o62m_shot_state_valid(m)) return O62M_SHOT_BAD_STATE;
    if (!o62m_shot_clock_valid(hz,divisor)) return O62M_SHOT_BAD_INPUT;
    if (now<m->now_ns) return O62M_SHOT_TIME_REVERSED;
    if ((m->running || (m->pending&1u)) &&
        (hz!=m->source_hz || divisor!=m->divisor)) return O62M_SHOT_ACTIVE_CLOCK_CHANGE;
    O62MShotTimer trial=*m;
    O62MShotResult r=o62m_shot_advance_(&trial,now);
    if (r!=O62M_SHOT_OK) return r;
    if (hz!=trial.source_hz || divisor!=trial.divisor) trial.fraction=0;
    trial.source_hz=hz; trial.divisor=divisor; trial.gate=gate;
    *m=trial;
    return O62M_SHOT_OK;
}

static inline O62MShotResult o62m_shot_access_(uint32_t offset,unsigned size,bool write)
{
    if (size!=4 || (offset&3u)) return O62M_SHOT_BAD_ACCESS;
    switch(offset) {
    case 0: case 4: case 16: case 24: case 28:return O62M_SHOT_OK;
    case 8: case 12: case 32:return write?O62M_SHOT_READ_ONLY:O62M_SHOT_OK;
    default:return O62M_SHOT_UNSUPPORTED_REGISTER;
    }
}

static inline O62MShotResult o62m_shot_timer_read(O62MShotTimer *m,uint64_t now,
        uint32_t offset,unsigned size,uint32_t *value)
{
    if (!m || !value) return O62M_SHOT_BAD_ARGUMENT;
    if (!o62m_shot_state_valid(m)) return O62M_SHOT_BAD_STATE;
    O62MShotResult r=o62m_shot_access_(offset,size,false);
    if (r!=O62M_SHOT_OK) return r;
    O62MShotTimer trial=*m;
    r=o62m_shot_advance_(&trial,now);
    if (r!=O62M_SHOT_OK) return r;
    uint32_t out=0;
    switch(offset) {
    case 0:out=(uint32_t)trial.load;break;
    case 4:out=(uint32_t)(trial.load>>32);break;
    case 8:out=(uint32_t)trial.snapshot;break;
    case 12:out=(uint32_t)(trial.snapshot>>32);break;
    case 16:out=trial.control;break;
    case 24:out=trial.snapshot_request_opaque|!!(trial.pending&2u);break;
    case 28:out=trial.ack_request_opaque|!!(trial.pending&4u);break;
    case 32:out=trial.status_opaque|((trial.pending&1u)?4u:0)|
                      ((trial.pending&2u)?2u:0)|((trial.pending&4u)?8u:0);break;
    }
    *m=trial; *value=out;
    return O62M_SHOT_OK;
}

static inline O62MShotResult o62m_shot_timer_write(O62MShotTimer *m,uint64_t now,
        uint32_t offset,unsigned size,uint64_t value)
{
    if (!m) return O62M_SHOT_BAD_ARGUMENT;
    if (!o62m_shot_state_valid(m)) return O62M_SHOT_BAD_STATE;
    O62MShotResult r=o62m_shot_access_(offset,size,true);
    if (r!=O62M_SHOT_OK) return r;
    if (value>UINT32_MAX) return O62M_SHOT_BAD_INPUT;
    if (offset==16 && (((value^m->control)&~UINT64_C(3)) || (value&4u)))
        return O62M_SHOT_INVALID_CONTROL;
    if ((offset==24 && value!=(m->snapshot_request_opaque|1u)) ||
        (offset==28 && value!=(m->ack_request_opaque|1u))) return O62M_SHOT_BAD_INPUT;
    O62MShotTimer trial=*m;
    r=o62m_shot_advance_(&trial,now);
    if (r!=O62M_SHOT_OK) return r;
    switch(offset) {
    case 0: case 4:
        if (trial.running || (trial.pending&1u)) return O62M_SHOT_BUSY;
        if (!offset) trial.load=(trial.load&UINT64_C(0xffffffff00000000))|value;
        else trial.load=(trial.load&UINT64_C(0xffffffff))|(value<<32);
        break;
    case 16:
        if ((value&1u) && !(trial.control&1u)) {
            if (trial.latency_ns>UINT64_MAX-now) return O62M_SHOT_TIME_OVERFLOW;
            trial.pending|=1u; trial.deadline[0]=now+trial.latency_ns;
        } else if (!(value&1u)) {
            trial.running=false; trial.pending&=(uint8_t)~1u;
        }
        trial.control=(uint32_t)value;
        break;
    case 24: case 28: {
        unsigned index=offset==24?1u:2u;
        if (trial.pending&(1u<<index)) return O62M_SHOT_BUSY;
        if (trial.latency_ns>UINT64_MAX-now) return O62M_SHOT_TIME_OVERFLOW;
        trial.pending|=(uint8_t)(1u<<index); trial.deadline[index]=now+trial.latency_ns;
        break;
    }
    }
    *m=trial;
    return O62M_SHOT_OK;
}

static inline O62MShotResult o62m_shot_timer_next_event(const O62MShotTimer *m,
        uint64_t *when,bool *present)
{
    if (!m || !when || !present) return O62M_SHOT_BAD_ARGUMENT;
    if (!o62m_shot_state_valid(m)) return O62M_SHOT_BAD_STATE;
    bool have=false; uint64_t next=0,expiry;
    if (m->gate) {
        for (unsigned i=0;i<3;i++) if (m->pending&(1u<<i)) {
            if (!have || m->deadline[i]<next) next=m->deadline[i];
            have=true;
        }
        if (o62m_shot_expiry_(m,&expiry) && (!have || expiry<next)) {
            next=expiry; have=true;
        }
    }
    *when=next; *present=have;
    return O62M_SHOT_OK;
}

static inline O62MShotResult o62m_shot_timer_irq_level(const O62MShotTimer *m,bool *level)
{
    if (!m || !level) return O62M_SHOT_BAD_ARGUMENT;
    if (!o62m_shot_state_valid(m)) return O62M_SHOT_BAD_STATE;
    *level=m->raw_irq && !(m->control&2u);
    return O62M_SHOT_OK;
}

static inline const char *o62m_shot_timer_reason(O62MShotResult r)
{
    static const char *const reason[]={"ok","bad-argument","bad-input","bad-state",
        "bad-width-or-alignment","unsupported-register","read-only","invalid-control",
        "busy","active-source-or-divisor-change","time-reversed","time-overflow",
        "first-expiry-unsupported","expiration-overflow"};
    return (unsigned)r<sizeof(reason)/sizeof(reason[0])?reason[r]:"unknown-error";
}
#endif
