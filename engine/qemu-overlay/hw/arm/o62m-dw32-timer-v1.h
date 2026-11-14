#ifndef O62M_DW32_TIMER_V1_H
#define O62M_DW32_TIMER_V1_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/* All output pointers require independent storage, not overlapping model or
 * one another. The BSP supplies separate transaction/deadline/IRQ locals. */
#if !defined(__SIZEOF_INT128__)
#error "This rational virtual timer requires a compiler with unsigned128 support"
#endif
__extension__ typedef unsigned __int128 O62MDw32WideV1;

typedef enum O62MDw32PolicyV1 {
    O62M_DW32_STRICT_FIRST_EXPIRY_V1 = 0,
    O62M_DW32_VIRTUAL_LOAD_PLUS_ONE_V1 = 1
} O62MDw32PolicyV1;

typedef enum O62MDw32ResultV1 {
    O62M_DW32_OK_V1 = 0,
    O62M_DW32_BAD_ARGUMENT_V1,
    O62M_DW32_UNINITIALIZED_V1,
    O62M_DW32_BAD_INITIAL_V1,
    O62M_DW32_INVALID_CLOCK_V1,
    O62M_DW32_INVALID_STATE_V1,
    O62M_DW32_TIME_REVERSED_V1,
    O62M_DW32_TIME_OVERFLOW_V1,
    O62M_DW32_UNSUPPORTED_ACCESS_V1,
    O62M_DW32_LOAD_RUNNING_V1,
    O62M_DW32_CONTROL_OPAQUE_V1,
    O62M_DW32_FIRST_EXPIRY_UNSUPPORTED_V1,
    O62M_DW32_INEXACT_PHASE_REBASE_V1,
    O62M_DW32_EXPIRY_COUNT_OVERFLOW_V1,
    O62M_DW32_PCLK_DISABLED_V1
} O62MDw32ResultV1;

typedef struct O62MDw32InputV1 {
    uint32_t load, current, control, eoi_value;
    bool raw_irq;
} O62MDw32InputV1;

typedef struct O62MDw32ClockV1 {
    uint32_t source_hz[2];
    uint16_t divisor[2];
    bool functional_gate[2];
    bool pclk_gate;
} O62MDw32ClockV1;

typedef struct O62MDw32ChannelV1 {
    uint32_t load, current, control, eoi_value;
    bool raw_irq;
    uint64_t phase, expiries;
} O62MDw32ChannelV1;

typedef struct O62MDw32V1 {
    O62MDw32ChannelV1 channel[2];
    O62MDw32ClockV1 clock;
    int64_t last_ns;
    bool initialized;
    O62MDw32PolicyV1 policy;
} O62MDw32V1;

static inline const char *o62m_dw32_reason_v1(O62MDw32ResultV1 r)
{
    switch(r) {
    case O62M_DW32_OK_V1: return "ok";
    case O62M_DW32_BAD_ARGUMENT_V1: return "bad-argument";
    case O62M_DW32_UNINITIALIZED_V1: return "explicit-inactive-inputs-required";
    case O62M_DW32_BAD_INITIAL_V1: return "inactive-controls-policy-and-time-required";
    case O62M_DW32_INVALID_CLOCK_V1: return "explicit-rational-clock-required";
    case O62M_DW32_INVALID_STATE_V1: return "invalid-virtual-timer-state";
    case O62M_DW32_TIME_REVERSED_V1: return "virtual-time-reversed";
    case O62M_DW32_TIME_OVERFLOW_V1: return "virtual-time-or-tick-count-overflow";
    case O62M_DW32_UNSUPPORTED_ACCESS_V1: return "only-proven32bit-load-current-control-eoi-subset";
    case O62M_DW32_LOAD_RUNNING_V1: return "load-write-requires-disabled-channel";
    case O62M_DW32_CONTROL_OPAQUE_V1: return "opaque-control-bits-must-be-preserved";
    case O62M_DW32_FIRST_EXPIRY_UNSUPPORTED_V1: return "first-expiry-needs-explicit-virtual-plusone-policy";
    case O62M_DW32_INEXACT_PHASE_REBASE_V1: return "inexact-divider-phase-rebase-unsupported";
    case O62M_DW32_EXPIRY_COUNT_OVERFLOW_V1: return "expiry-telemetry-overflow";
    case O62M_DW32_PCLK_DISABLED_V1: return "pclk-disabled-bus-response-unimplemented";
    }
    return "invalid-result";
}

static inline bool o62m_dw32_clock_valid_v1(const O62MDw32ClockV1 *clock)
{
    if(!clock) { return false; }
    for(unsigned i=0;i<2;++i) {
        if(clock->divisor[i]<1 || clock->divisor[i]>256 ||
           (clock->functional_gate[i] && !clock->source_hz[i])) { return false; }
    }
    return true;
}

static inline bool o62m_dw32_valid_v1(const O62MDw32V1 *model)
{
    if(!model || !model->initialized || model->last_ns<0 ||
       !o62m_dw32_clock_valid_v1(&model->clock) ||
       (model->policy!=O62M_DW32_STRICT_FIRST_EXPIRY_V1 &&
        model->policy!=O62M_DW32_VIRTUAL_LOAD_PLUS_ONE_V1)) { return false; }
    for(unsigned i=0;i<2;++i) {
        uint64_t denominator=UINT64_C(1000000000)*model->clock.divisor[i];
        if(model->channel[i].phase>=denominator) { return false; }
    }
    return true;
}

/* Every field is explicit. No silicon reset values are supplied. Initial
 * controls must be disabled; phase0 is unobservable until the next rising
 * enable reloads the counter and starts the selected virtual phase policy.
 */
static inline O62MDw32ResultV1 o62m_dw32_init_v1(O62MDw32V1 *model,
        const O62MDw32InputV1 input[2], const O62MDw32ClockV1 *clock,
        int64_t now_ns, O62MDw32PolicyV1 policy)
{
    O62MDw32V1 next={0};
    if(!model || !input || !clock) { return O62M_DW32_BAD_ARGUMENT_V1; }
    if(now_ns<0 || (input[0].control&1u) || (input[1].control&1u) ||
       (policy!=O62M_DW32_STRICT_FIRST_EXPIRY_V1 &&
        policy!=O62M_DW32_VIRTUAL_LOAD_PLUS_ONE_V1)) { return O62M_DW32_BAD_INITIAL_V1; }
    if(!o62m_dw32_clock_valid_v1(clock)) { return O62M_DW32_INVALID_CLOCK_V1; }
    for(unsigned i=0;i<2;++i) {
        next.channel[i].load=input[i].load;
        next.channel[i].current=input[i].current;
        next.channel[i].control=input[i].control;
        next.channel[i].eoi_value=input[i].eoi_value;
        next.channel[i].raw_irq=input[i].raw_irq;
    }
    next.clock=*clock;
    next.last_ns=now_ns;
    next.policy=policy;
    next.initialized=true;
    *model=next;
    return O62M_DW32_OK_V1;
}

/* Internal trial-only routine; callers must not pass the committed model.
 * Virtual plusone policy: starting at LOAD, expiration occurs after LOAD+1
 * clock ticks. Periodic mode reloads LOAD; free mode reloads all32bits. The
 * precise silicon terminal edge is not established by this policy.
 */
static inline O62MDw32ResultV1 o62m_dw32_advance_trial_v1(O62MDw32V1 *trial,
                                                       int64_t now_ns)
{
    if(now_ns<trial->last_ns) { return O62M_DW32_TIME_REVERSED_V1; }
    uint64_t delta=(uint64_t)(now_ns-trial->last_ns);
    for(unsigned i=0;i<2;++i) {
        O62MDw32ChannelV1 *ch=&trial->channel[i];
        if(!(ch->control&1u) || !trial->clock.functional_gate[i]) { continue; }
        uint64_t denominator=UINT64_C(1000000000)*trial->clock.divisor[i];
        O62MDw32WideV1 elapsed=(O62MDw32WideV1)delta*trial->clock.source_hz[i]+ch->phase;
        O62MDw32WideV1 wide_ticks=elapsed/denominator;
        if(wide_ticks>UINT64_MAX) { return O62M_DW32_TIME_OVERFLOW_V1; }
        uint64_t ticks=(uint64_t)wide_ticks;
        ch->phase=(uint64_t)(elapsed%denominator);
        uint64_t first=(uint64_t)ch->current+1u;
        if(ticks<first) { ch->current-=(uint32_t)ticks; continue; }
        if(trial->policy==O62M_DW32_STRICT_FIRST_EXPIRY_V1) {
            return O62M_DW32_FIRST_EXPIRY_UNSUPPORTED_V1;
        }
        uint64_t remaining=ticks-first;
        uint64_t reload=(ch->control&2u)?ch->load:UINT32_MAX;
        uint64_t period=reload+1u;
        uint64_t expired=1u+remaining/period;
        if(expired>UINT64_MAX-ch->expiries) { return O62M_DW32_EXPIRY_COUNT_OVERFLOW_V1; }
        ch->expiries+=expired;
        ch->raw_irq=true;
        ch->current=(uint32_t)(reload-remaining%period);
    }
    trial->last_ns=now_ns;
    return O62M_DW32_OK_V1;
}

static inline O62MDw32ResultV1 o62m_dw32_advance_v1(O62MDw32V1 *model,int64_t now_ns)
{
    if(!model) { return O62M_DW32_BAD_ARGUMENT_V1; }
    if(!model->initialized) { return O62M_DW32_UNINITIALIZED_V1; }
    if(!o62m_dw32_valid_v1(model)) { return O62M_DW32_INVALID_STATE_V1; }
    O62MDw32V1 next=*model;
    O62MDw32ResultV1 result=o62m_dw32_advance_trial_v1(&next,now_ns);
    if(result!=O62M_DW32_OK_V1) { return result; }
    *model=next;
    return O62M_DW32_OK_V1;
}

static inline O62MDw32ResultV1 o62m_dw32_set_clock_v1(O62MDw32V1 *model,
        int64_t now_ns,const O62MDw32ClockV1 *clock)
{
    if(!model || !clock) { return O62M_DW32_BAD_ARGUMENT_V1; }
    if(!model->initialized) { return O62M_DW32_UNINITIALIZED_V1; }
    if(!o62m_dw32_valid_v1(model)) { return O62M_DW32_INVALID_STATE_V1; }
    if(!o62m_dw32_clock_valid_v1(clock)) { return O62M_DW32_INVALID_CLOCK_V1; }
    O62MDw32V1 next=*model;
    O62MDw32ResultV1 result=o62m_dw32_advance_trial_v1(&next,now_ns);
    if(result!=O62M_DW32_OK_V1) { return result; }
    for(unsigned i=0;i<2;++i) {
        O62MDw32WideV1 numerator=(O62MDw32WideV1)next.channel[i].phase*clock->divisor[i];
        if(numerator%next.clock.divisor[i]) { return O62M_DW32_INEXACT_PHASE_REBASE_V1; }
        next.channel[i].phase=(uint64_t)(numerator/next.clock.divisor[i]);
    }
    next.clock=*clock;
    *model=next;
    return O62M_DW32_OK_V1;
}

/* value must not overlap model storage. Unknown/status/global registers and
 * writes to CURRENT/EOI remain strict. Invalid transactions cannot advance
 * time, modify observations or consume an interrupt. PCLK controls only bus
 * access; functional clock gates pause counting without discarding phase.
 */
static inline O62MDw32ResultV1 o62m_dw32_access_v1(O62MDw32V1 *model,
        int64_t now_ns,uint32_t offset,unsigned size,bool write,uint64_t *value)
{
    unsigned index,reg;
    if(!model || !value) { return O62M_DW32_BAD_ARGUMENT_V1; }
    if(!model->initialized) { return O62M_DW32_UNINITIALIZED_V1; }
    if(!o62m_dw32_valid_v1(model)) { return O62M_DW32_INVALID_STATE_V1; }
    if(size!=4) { return O62M_DW32_UNSUPPORTED_ACCESS_V1; }
    switch(offset) {
    case 0x00:index=0;reg=0;break;
    case 0x04:index=0;reg=1;break;
    case 0x08:index=0;reg=2;break;
    case 0x0c:index=0;reg=3;break;
    case 0x14:index=1;reg=0;break;
    case 0x18:index=1;reg=1;break;
    case 0x1c:index=1;reg=2;break;
    case 0x20:index=1;reg=3;break;
    default:return O62M_DW32_UNSUPPORTED_ACCESS_V1;
    }
    if(write && (reg==1 || reg==3 || *value>UINT32_MAX)) {
        return O62M_DW32_UNSUPPORTED_ACCESS_V1;
    }
    if(write && reg==0 && (model->channel[index].control&1u)) {
        return O62M_DW32_LOAD_RUNNING_V1;
    }
    if(write && reg==2 && (((uint32_t)*value^model->channel[index].control)&~UINT32_C(7))) {
        return O62M_DW32_CONTROL_OPAQUE_V1;
    }
    if(!model->clock.pclk_gate) { return O62M_DW32_PCLK_DISABLED_V1; }
    O62MDw32V1 next=*model;
    O62MDw32ResultV1 result=o62m_dw32_advance_trial_v1(&next,now_ns);
    if(result!=O62M_DW32_OK_V1) { return result; }
    O62MDw32ChannelV1 *ch=&next.channel[index];
    uint64_t output=*value;
    if(write) {
        if(reg==0) { ch->load=(uint32_t)*value; }
        else {
            uint32_t control=(uint32_t)*value;
            if(!(ch->control&1u) && (control&1u)) {
                ch->current=ch->load;
                ch->phase=0;
            }
            ch->control=control;
        }
    } else {
        switch(reg) {
        case 0:output=ch->load;break;
        case 1:output=ch->current;break;
        case 2:output=ch->control;break;
        default:output=ch->eoi_value;ch->raw_irq=false;break;
        }
    }
    *model=next;
    *value=output;
    return O62M_DW32_OK_V1;
}

static inline O62MDw32ResultV1 o62m_dw32_next_event_v1(const O62MDw32V1 *model,
        int64_t *deadline_ns,bool *has_event)
{
    if(!model || !deadline_ns || !has_event) { return O62M_DW32_BAD_ARGUMENT_V1; }
    if(!model->initialized) { return O62M_DW32_UNINITIALIZED_V1; }
    if(!o62m_dw32_valid_v1(model)) { return O62M_DW32_INVALID_STATE_V1; }
    int64_t selected=0;
    bool found=false;
    for(unsigned i=0;i<2;++i) {
        const O62MDw32ChannelV1 *ch=&model->channel[i];
        if(!(ch->control&1u) || !model->clock.functional_gate[i]) { continue; }
        uint64_t denominator=UINT64_C(1000000000)*model->clock.divisor[i];
        O62MDw32WideV1 needed=((O62MDw32WideV1)ch->current+1u)*denominator-ch->phase;
        O62MDw32WideV1 delta=(needed+model->clock.source_hz[i]-1u)/model->clock.source_hz[i];
        if(delta>(uint64_t)(INT64_MAX-model->last_ns)) { return O62M_DW32_TIME_OVERFLOW_V1; }
        int64_t candidate=model->last_ns+(int64_t)delta;
        if(!found || candidate<selected) { found=true;selected=candidate; }
    }
    *deadline_ns=selected;
    *has_event=found;
    return O62M_DW32_OK_V1;
}

static inline O62MDw32ResultV1 o62m_dw32_irq_level_v1(const O62MDw32V1 *model,
        unsigned channel,bool *level)
{
    if(!model || !level || channel>1) { return O62M_DW32_BAD_ARGUMENT_V1; }
    if(!model->initialized) { return O62M_DW32_UNINITIALIZED_V1; }
    if(!o62m_dw32_valid_v1(model)) { return O62M_DW32_INVALID_STATE_V1; }
    *level=model->channel[channel].raw_irq && !(model->channel[channel].control&4u);
    return O62M_DW32_OK_V1;
}

#endif
