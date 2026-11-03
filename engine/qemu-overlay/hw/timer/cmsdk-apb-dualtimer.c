/*
 * ARM CMSDK APB dual-timer emulation
 *
 * Copyright (c) 2018 Linaro Limited
 * Written by Peter Maydell
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 or
 *  (at your option) any later version.
 */

/*
 * This is a model of the "APB dual-input timer" which is part of the Cortex-M
 * System Design Kit (CMSDK) and documented in the Cortex-M System
 * Design Kit Technical Reference Manual (ARM DDI0479C):
 * https://developer.arm.com/products/system-design/system-design-kits/cortex-m-system-design-kit
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "trace.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/qdev-clock.h"
#include "hw/timer/cmsdk-apb-dualtimer.h"
#include "migration/vmstate.h"

REG32(TIMER1LOAD, 0x0)
REG32(TIMER1VALUE, 0x4)
REG32(TIMER1CONTROL, 0x8)
    FIELD(CONTROL, ONESHOT, 0, 1)
    FIELD(CONTROL, SIZE, 1, 1)
    FIELD(CONTROL, PRESCALE, 2, 2)
    FIELD(CONTROL, INTEN, 5, 1)
    FIELD(CONTROL, MODE, 6, 1)
    FIELD(CONTROL, ENABLE, 7, 1)
#define R_CONTROL_VALID_MASK (R_CONTROL_ONESHOT_MASK | R_CONTROL_SIZE_MASK | \
                              R_CONTROL_PRESCALE_MASK | R_CONTROL_INTEN_MASK | \
                              R_CONTROL_MODE_MASK | R_CONTROL_ENABLE_MASK)
REG32(TIMER1INTCLR, 0xc)
REG32(TIMER1RIS, 0x10)
REG32(TIMER1MIS, 0x14)
REG32(TIMER1BGLOAD, 0x18)
REG32(TIMER2LOAD, 0x20)
REG32(TIMER2VALUE, 0x24)
REG32(TIMER2CONTROL, 0x28)
REG32(TIMER2INTCLR, 0x2c)
REG32(TIMER2RIS, 0x30)
REG32(TIMER2MIS, 0x34)
REG32(TIMER2BGLOAD, 0x38)
REG32(TIMERITCR, 0xf00)
    FIELD(TIMERITCR, ENABLE, 0, 1)
#define R_TIMERITCR_VALID_MASK R_TIMERITCR_ENABLE_MASK
REG32(TIMERITOP, 0xf04)
    FIELD(TIMERITOP, TIMINT1, 0, 1)
    FIELD(TIMERITOP, TIMINT2, 1, 1)
#define R_TIMERITOP_VALID_MASK (R_TIMERITOP_TIMINT1_MASK | \
                                R_TIMERITOP_TIMINT2_MASK)
REG32(PID4, 0xfd0)
REG32(PID5, 0xfd4)
REG32(PID6, 0xfd8)
REG32(PID7, 0xfdc)
REG32(PID0, 0xfe0)
REG32(PID1, 0xfe4)
REG32(PID2, 0xfe8)
REG32(PID3, 0xfec)
REG32(CID0, 0xff0)
REG32(CID1, 0xff4)
REG32(CID2, 0xff8)
REG32(CID3, 0xffc)

/* PID/CID values */
static const int timer_id[] = {
    0x04, 0x00, 0x00, 0x00, /* PID4..PID7 */
    0x23, 0xb8, 0x1b, 0x00, /* PID0..PID3 */
    0x0d, 0xf0, 0x05, 0xb1, /* CID0..CID3 */
};

static bool cmsdk_dualtimermod_intstatus(CMSDKAPBDualTimerModule *m)
{
    /* Return masked interrupt status for the timer module */
    return m->intstatus && (m->control & R_CONTROL_INTEN_MASK);
}

static void cmsdk_apb_dualtimer_update(CMSDKAPBDualTimer *s)
{
    bool timint1, timint2, timintc;

    if (s->timeritcr) {
        /* Integration test mode: outputs driven directly from TIMERITOP bits */
        timint1 = s->timeritop & R_TIMERITOP_TIMINT1_MASK;
        timint2 = s->timeritop & R_TIMERITOP_TIMINT2_MASK;
    } else {
        timint1 = cmsdk_dualtimermod_intstatus(&s->timermod[0]);
        timint2 = cmsdk_dualtimermod_intstatus(&s->timermod[1]);
    }

    timintc = timint1 || timint2;

    qemu_set_irq(s->timermod[0].timerint, timint1);
    qemu_set_irq(s->timermod[1].timerint, timint2);
    qemu_set_irq(s->timerintc, timintc);
}

static int cmsdk_dualtimermod_divisor(CMSDKAPBDualTimerModule *m)
{
    /* Return the divisor set by the current CONTROL.PRESCALE value */
    switch (FIELD_EX32(m->control, CONTROL, PRESCALE)) {
    case 0:
        return 1;
    case 1:
        return 16;
    case 2:
    case 3: /* UNDEFINED, we treat like 2 (and complained when it was set) */
        return 256;
    default:
        g_assert_not_reached();
    }
}

static bool cmsdk_dualtimermod_bes_underflow(CMSDKAPBDualTimerModule *m,
                                           uint32_t control)
{
    unsigned index = m - m->parent->timermod;

    return (m->parent->bes_oneshot_underflow_mask & (1u << index)) &&
           (control & R_CONTROL_ONESHOT_MASK);
}

static uint64_t cmsdk_dualtimermod_raw_count(CMSDKAPBDualTimerModule *m)
{
    CMSDKAPBDualTimer *s = m->parent;
    unsigned index = m - s->timermod;

    if (cmsdk_dualtimermod_bes_underflow(m, m->control) &&
        s->bes_count_valid[index]) {
        uint64_t count = s->bes_count_value[index];
        __uint128_t period = ((__uint128_t)s->bes_count_period_ns[index] << 32) |
                            s->bes_count_period_frac[index];
        int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                          s->bes_count_time[index];

        if ((m->control & R_CONTROL_ENABLE_MASK) && period && elapsed > 0) {
            __uint128_t ticks = ((__uint128_t)elapsed << 32) / period;

            count = ticks >= count ? 0 : count - (uint64_t)ticks;
        }
        return count;
    }
    return ptimer_get_count(m->timer);
}

static uint64_t cmsdk_dualtimermod_decode_count(CMSDKAPBDualTimerModule *m,
                                              uint64_t raw, uint32_t control)
{
    return cmsdk_dualtimermod_bes_underflow(m, control) && raw ? raw - 1 : raw;
}

static void cmsdk_dualtimermod_latch_period(CMSDKAPBDualTimerModule *m,
                                          unsigned divisor)
{
    CMSDKAPBDualTimer *s = m->parent;
    unsigned index = m - s->timermod;
    __uint128_t period = (__uint128_t)clock_get(s->timclk) * divisor;

    s->bes_count_period_ns[index] = period >> 32;
    s->bes_count_period_frac[index] = period;
}

static void cmsdk_dualtimermod_set_raw_count(CMSDKAPBDualTimerModule *m,
                                           uint64_t raw)
{
    CMSDKAPBDualTimer *s = m->parent;
    unsigned index = m - s->timermod;

    ptimer_set_count(m->timer, raw);
    s->bes_count_time[index] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->bes_count_value[index] = raw;
    s->bes_count_valid[index] = true;
    cmsdk_dualtimermod_latch_period(m, cmsdk_dualtimermod_divisor(m));
}

static void cmsdk_dualtimermod_set_period(CMSDKAPBDualTimerModule *m,
                                        unsigned divisor)
{
    bool bes = cmsdk_dualtimermod_bes_underflow(m, m->control);
    uint64_t raw = bes ? cmsdk_dualtimermod_raw_count(m) : 0;

    ptimer_set_period_from_clock(m->timer, m->parent->timclk, divisor);
    if (bes) {
        /* Preserve the encoded remaining ticks, including the zero phase.
         * Do not add another tick when changing the clock or prescaler. */
        cmsdk_dualtimermod_set_raw_count(m, raw);
        cmsdk_dualtimermod_latch_period(m, divisor);
    }
}

static void cmsdk_dualtimermod_write_control(CMSDKAPBDualTimerModule *m,
                                             uint32_t newctrl)
{
    /* Handle a write to the CONTROL register */
    uint32_t changed;

    ptimer_transaction_begin(m->timer);

    newctrl &= R_CONTROL_VALID_MASK;

    changed = m->control ^ newctrl;

    if (changed & ~newctrl & R_CONTROL_ENABLE_MASK) {
        /* ENABLE cleared, stop timer before any further changes */
        bool bes = cmsdk_dualtimermod_bes_underflow(m, m->control);
        uint64_t raw = bes ? cmsdk_dualtimermod_raw_count(m) : 0;

        ptimer_stop(m->timer);
        if (bes) {
            cmsdk_dualtimermod_set_raw_count(m, raw);
        }
    }

    if (changed & R_CONTROL_PRESCALE_MASK) {
        int divisor;

        switch (FIELD_EX32(newctrl, CONTROL, PRESCALE)) {
        case 0:
            divisor = 1;
            break;
        case 1:
            divisor = 16;
            break;
        case 2:
            divisor = 256;
            break;
        case 3:
            /* UNDEFINED; complain, and arbitrarily treat like 2 */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CMSDK APB dual-timer: CONTROL.PRESCALE==0b11"
                          " is undefined behaviour\n");
            divisor = 256;
            break;
        default:
            g_assert_not_reached();
        }
        cmsdk_dualtimermod_set_period(m, divisor);
    }

    if (changed & R_CONTROL_MODE_MASK) {
        uint32_t load;
        if (newctrl & R_CONTROL_MODE_MASK) {
            /* Periodic: the limit is the LOAD register value */
            load = m->load;
        } else {
            /* Free-running: counter wraps around */
            load = ptimer_get_limit(m->timer);
            if (!(m->control & R_CONTROL_SIZE_MASK)) {
                load = deposit32(m->load, 0, 16, load);
            }
            m->load = load;
            load = 0xffffffff;
        }
        if (!(m->control & R_CONTROL_SIZE_MASK)) {
            load &= 0xffff;
        }
        ptimer_set_limit(m->timer, load, 0);
    }

    if ((changed & R_CONTROL_SIZE_MASK) ||
        ((changed & R_CONTROL_ONESHOT_MASK) &&
         (cmsdk_dualtimermod_bes_underflow(m, m->control) ||
          cmsdk_dualtimermod_bes_underflow(m, newctrl)))) {
        /* Timer switched between 16 and 32 bit count */
        uint32_t value, load;
        uint64_t raw = cmsdk_dualtimermod_raw_count(m);
        bool completed = cmsdk_dualtimermod_bes_underflow(m, m->control) &&
                         raw == 0;

        value = cmsdk_dualtimermod_decode_count(m, raw, m->control);
        load = ptimer_get_limit(m->timer);
        if (!(changed & R_CONTROL_SIZE_MASK)) {
            /* Only the one-shot encoding changed; retain the counter width. */
        } else if (newctrl & R_CONTROL_SIZE_MASK) {
            /* 16 -> 32, top half of VALUE is in struct field */
            value = deposit32(m->value, 0, 16, value);
        } else {
            /* 32 -> 16: save top half to struct field and truncate */
            m->value = value;
            value &= 0xffff;
        }

        if (newctrl & R_CONTROL_MODE_MASK) {
            /* Periodic, timer limit has LOAD value */
            if (newctrl & R_CONTROL_SIZE_MASK) {
                load = deposit32(m->load, 0, 16, load);
            } else {
                m->load = load;
                load &= 0xffff;
            }
        } else {
            /* Free-running, timer limit is set to give wraparound */
            if (newctrl & R_CONTROL_SIZE_MASK) {
                load = 0xffffffff;
            } else {
                load = 0xffff;
            }
        }
        if (cmsdk_dualtimermod_bes_underflow(m, newctrl)) {
            /* A completed one-shot stays completed until LOAD rearms it.
             * In particular, changing SIZE must not manufacture a tick. */
            cmsdk_dualtimermod_set_raw_count(m,
                                            completed ? 0 : (uint64_t)value + 1);
        } else {
            cmsdk_dualtimermod_set_raw_count(m, value);
        }
        ptimer_set_limit(m->timer, load, 0);
    }

    if ((changed & newctrl & R_CONTROL_ENABLE_MASK) &&
        cmsdk_dualtimermod_bes_underflow(m, newctrl)) {
        /* Resume from the paused remaining count, not from its old epoch. */
        cmsdk_dualtimermod_set_raw_count(m, cmsdk_dualtimermod_raw_count(m));
    }

    m->control = newctrl;
    if (cmsdk_dualtimermod_bes_underflow(m, newctrl)) {
        cmsdk_dualtimermod_latch_period(m, cmsdk_dualtimermod_divisor(m));
    }

    if ((newctrl & R_CONTROL_ENABLE_MASK) &&
        !(cmsdk_dualtimermod_bes_underflow(m, newctrl) &&
          cmsdk_dualtimermod_raw_count(m) == 0)) {
        /*
         * ENABLE is set; start the timer after all other changes.
         * We start it even if the ENABLE bit didn't actually change,
         * in case the timer was an expired one-shot timer that has
         * now been changed into a free-running or periodic timer.
         */
        ptimer_run(m->timer, !!(newctrl & R_CONTROL_ONESHOT_MASK));
    }

    ptimer_transaction_commit(m->timer);
}

static uint64_t cmsdk_apb_dualtimer_read(void *opaque, hwaddr offset,
                                          unsigned size)
{
    CMSDKAPBDualTimer *s = CMSDK_APB_DUALTIMER(opaque);
    uint64_t r;

    if (offset >= A_TIMERITCR) {
        switch (offset) {
        case A_TIMERITCR:
            r = s->timeritcr;
            break;
        case A_PID4 ... A_CID3:
            r = timer_id[(offset - A_PID4) / 4];
            break;
        default:
        bad_offset:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CMSDK APB dual-timer read: bad offset %x\n",
                          (int) offset);
            r = 0;
            break;
        }
    } else {
        int timer = offset >> 5;
        CMSDKAPBDualTimerModule *m;

        if (timer >= ARRAY_SIZE(s->timermod)) {
            goto bad_offset;
        }

        m = &s->timermod[timer];

        switch (offset & 0x1F) {
        case A_TIMER1LOAD:
        case A_TIMER1BGLOAD:
            if (m->control & R_CONTROL_MODE_MASK) {
                /*
                 * Periodic: the ptimer limit is the LOAD register value, (or
                 * just the low 16 bits of it if the timer is in 16-bit mode)
                 */
                r = ptimer_get_limit(m->timer);
                if (!(m->control & R_CONTROL_SIZE_MASK)) {
                    r = deposit32(m->load, 0, 16, r);
                }
            } else {
                /* Free-running: LOAD register value is just in m->load */
                r = m->load;
            }
            break;
        case A_TIMER1VALUE:
            r = cmsdk_dualtimermod_decode_count(m,
                    cmsdk_dualtimermod_raw_count(m), m->control);
            if (!(m->control & R_CONTROL_SIZE_MASK)) {
                r = deposit32(m->value, 0, 16, r);
            }
            break;
        case A_TIMER1CONTROL:
            r = m->control;
            break;
        case A_TIMER1RIS:
            r = m->intstatus;
            break;
        case A_TIMER1MIS:
            r = cmsdk_dualtimermod_intstatus(m);
            break;
        case A_TIMER1INTCLR:
            /* Write-only interrupt clear register reads as zero. */
            r = 0;
            break;
        default:
            goto bad_offset;
        }
    }

    trace_cmsdk_apb_dualtimer_read(offset, r, size);
    return r;
}

static void cmsdk_apb_dualtimer_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size)
{
    CMSDKAPBDualTimer *s = CMSDK_APB_DUALTIMER(opaque);

    trace_cmsdk_apb_dualtimer_write(offset, value, size);

    if (offset >= A_TIMERITCR) {
        switch (offset) {
        case A_TIMERITCR:
            s->timeritcr = value & R_TIMERITCR_VALID_MASK;
            cmsdk_apb_dualtimer_update(s);
            break;
        case A_TIMERITOP:
            s->timeritop = value & R_TIMERITOP_VALID_MASK;
            cmsdk_apb_dualtimer_update(s);
            break;
        default:
        bad_offset:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CMSDK APB dual-timer write: bad offset %x\n",
                          (int) offset);
            break;
        }
    } else {
        int timer = offset >> 5;
        CMSDKAPBDualTimerModule *m;

        if (timer >= ARRAY_SIZE(s->timermod)) {
            goto bad_offset;
        }

        m = &s->timermod[timer];

        switch (offset & 0x1F) {
        case A_TIMER1LOAD:
            /* Set the limit, and immediately reload the count from it */
            m->load = value;
            m->value = value;
            if (!(m->control & R_CONTROL_SIZE_MASK)) {
                value &= 0xffff;
            }
            ptimer_transaction_begin(m->timer);
            if (cmsdk_dualtimermod_bes_underflow(m, m->control)) {
                if (m->control & R_CONTROL_MODE_MASK) {
                    ptimer_set_limit(m->timer, value, 0);
                }
                /* Keep LOAD/limit unmodified.  0xffffffff needs a 33-bit
                 * internal count, and LOAD=0 expires after one tick. */
                cmsdk_dualtimermod_set_raw_count(m, value + 1);
                if (m->control & R_CONTROL_ENABLE_MASK) {
                    ptimer_run(m->timer, 1);
                }
            } else if (!(m->control & R_CONTROL_MODE_MASK)) {
                /*
                 * In free-running mode this won't set the limit but will
                 * still change the current count value.
                 */
                ptimer_set_count(m->timer, value);
            } else {
                if (!value) {
                    ptimer_stop(m->timer);
                }
                ptimer_set_limit(m->timer, value, 1);
                if (value && (m->control & R_CONTROL_ENABLE_MASK)) {
                    /* Force possibly-expired oneshot timer to restart */
                    ptimer_run(m->timer, 1);
                }
            }
            ptimer_transaction_commit(m->timer);
            break;
        case A_TIMER1BGLOAD:
            /* Set the limit, but not the current count */
            m->load = value;
            if (!(m->control & R_CONTROL_MODE_MASK)) {
                /* In free-running mode there is no limit */
                break;
            }
            if (!(m->control & R_CONTROL_SIZE_MASK)) {
                value &= 0xffff;
            }
            ptimer_transaction_begin(m->timer);
            ptimer_set_limit(m->timer, value, 0);
            ptimer_transaction_commit(m->timer);
            break;
        case A_TIMER1CONTROL:
            cmsdk_dualtimermod_write_control(m, value);
            cmsdk_apb_dualtimer_update(s);
            break;
        case A_TIMER1INTCLR:
            m->intstatus = 0;
            cmsdk_apb_dualtimer_update(s);
            break;
        default:
            goto bad_offset;
        }
    }
}

static const MemoryRegionOps cmsdk_apb_dualtimer_ops = {
    .read = cmsdk_apb_dualtimer_read,
    .write = cmsdk_apb_dualtimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* byte/halfword accesses are just zero-padded on reads and writes */
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void cmsdk_dualtimermod_tick(void *opaque)
{
    CMSDKAPBDualTimerModule *m = opaque;

    m->parent->bes_count_valid[m - m->parent->timermod] = false;
    m->intstatus = 1;
    cmsdk_apb_dualtimer_update(m->parent);
}

static void cmsdk_dualtimermod_reset(CMSDKAPBDualTimerModule *m)
{
    m->control = R_CONTROL_INTEN_MASK;
    m->intstatus = 0;
    m->load = 0;
    m->value = 0xffffffff;
    m->parent->bes_count_valid[m - m->parent->timermod] = false;
    ptimer_transaction_begin(m->timer);
    ptimer_stop(m->timer);
    /*
     * We start in free-running mode, with VALUE at 0xffffffff, and
     * in 16-bit counter mode. This means that the ptimer count and
     * limit must both be set to 0xffff, so we wrap at 16 bits.
     */
    ptimer_set_limit(m->timer, 0xffff, 1);
    ptimer_set_period_from_clock(m->timer, m->parent->timclk,
                                 cmsdk_dualtimermod_divisor(m));
    ptimer_transaction_commit(m->timer);
}

static void cmsdk_apb_dualtimer_reset(DeviceState *dev)
{
    CMSDKAPBDualTimer *s = CMSDK_APB_DUALTIMER(dev);
    int i;

    trace_cmsdk_apb_dualtimer_reset();

    for (i = 0; i < ARRAY_SIZE(s->timermod); i++) {
        cmsdk_dualtimermod_reset(&s->timermod[i]);
    }
    s->timeritcr = 0;
    s->timeritop = 0;
}

static void cmsdk_apb_dualtimer_clk_update(void *opaque, ClockEvent event)
{
    CMSDKAPBDualTimer *s = CMSDK_APB_DUALTIMER(opaque);
    int i;

    for (i = 0; i < ARRAY_SIZE(s->timermod); i++) {
        CMSDKAPBDualTimerModule *m = &s->timermod[i];
        ptimer_transaction_begin(m->timer);
        cmsdk_dualtimermod_set_period(m, cmsdk_dualtimermod_divisor(m));
        ptimer_transaction_commit(m->timer);
    }
}

static void cmsdk_apb_dualtimer_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    CMSDKAPBDualTimer *s = CMSDK_APB_DUALTIMER(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &cmsdk_apb_dualtimer_ops,
                          s, "cmsdk-apb-dualtimer", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->timerintc);

    for (i = 0; i < ARRAY_SIZE(s->timermod); i++) {
        sysbus_init_irq(sbd, &s->timermod[i].timerint);
    }
    s->timclk = qdev_init_clock_in(DEVICE(s), "TIMCLK",
                                   cmsdk_apb_dualtimer_clk_update, s,
                                   ClockUpdate);
}

static uint8_t cmsdk_dualtimermod_policy(CMSDKAPBDualTimerModule *m)
{
    unsigned index = m - m->parent->timermod;
    uint8_t policy = PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
                     PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT |
                     PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                     PTIMER_POLICY_NO_COUNTER_ROUND_DOWN;

    if (m->parent->bes_oneshot_underflow_mask & (1u << index)) {
        policy |= PTIMER_POLICY_ONESHOT_ROUND_UP;
    }
    return policy;
}

static void cmsdk_apb_dualtimer_realize(DeviceState *dev, Error **errp)
{
    CMSDKAPBDualTimer *s = CMSDK_APB_DUALTIMER(dev);
    int i;

    if (!clock_has_source(s->timclk)) {
        error_setg(errp, "CMSDK APB dualtimer: TIMCLK clock must be connected");
        return;
    }
    if (s->bes_oneshot_underflow_mask & ~3u) {
        error_setg(errp, "CMSDK APB dualtimer: invalid BES channel mask");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(s->timermod); i++) {
        CMSDKAPBDualTimerModule *m = &s->timermod[i];

        m->parent = s;
        m->timer = ptimer_init(cmsdk_dualtimermod_tick, m,
                               cmsdk_dualtimermod_policy(m));
    }
}

static const VMStateDescription cmsdk_dualtimermod_vmstate = {
    .name = "cmsdk-apb-dualtimer-module",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, CMSDKAPBDualTimerModule),
        VMSTATE_UINT32(load, CMSDKAPBDualTimerModule),
        VMSTATE_UINT32(value, CMSDKAPBDualTimerModule),
        VMSTATE_UINT32(control, CMSDKAPBDualTimerModule),
        VMSTATE_UINT32(intstatus, CMSDKAPBDualTimerModule),
        VMSTATE_END_OF_LIST()
    }
};

static int cmsdk_apb_dualtimer_post_load(void *opaque, int version_id)
{
    CMSDKAPBDualTimer *s = opaque;

    /* Old streams contain unbiased ptimer counts.  Never silently interpret
     * one as the BES remaining-count format.  Version 3 also checks masks. */
    if (version_id < 3 && s->bes_oneshot_underflow_mask) {
        return -EINVAL;
    }
    if (version_id < 3) {
        memset(s->bes_count_valid, 0, sizeof(s->bes_count_valid));
    }
    cmsdk_apb_dualtimer_update(s);
    return 0;
}

static const VMStateDescription cmsdk_apb_dualtimer_vmstate = {
    .name = "cmsdk-apb-dualtimer",
    .version_id = 3,
    .minimum_version_id = 2,
    .post_load = cmsdk_apb_dualtimer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_CLOCK(timclk, CMSDKAPBDualTimer),
        VMSTATE_STRUCT_ARRAY(timermod, CMSDKAPBDualTimer,
                             CMSDK_APB_DUALTIMER_NUM_MODULES,
                             1, cmsdk_dualtimermod_vmstate,
                             CMSDKAPBDualTimerModule),
        VMSTATE_UINT32(timeritcr, CMSDKAPBDualTimer),
        VMSTATE_UINT32(timeritop, CMSDKAPBDualTimer),
        VMSTATE_UINT32_EQUAL_V(bes_oneshot_underflow_mask, CMSDKAPBDualTimer,
                              3, "BES one-shot channel mask differs"),
        VMSTATE_INT64_ARRAY_V(bes_count_time, CMSDKAPBDualTimer,
                             CMSDK_APB_DUALTIMER_NUM_MODULES, 3),
        VMSTATE_UINT64_ARRAY_V(bes_count_value, CMSDKAPBDualTimer,
                              CMSDK_APB_DUALTIMER_NUM_MODULES, 3),
        VMSTATE_UINT64_ARRAY_V(bes_count_period_ns, CMSDKAPBDualTimer,
                              CMSDK_APB_DUALTIMER_NUM_MODULES, 3),
        VMSTATE_UINT32_ARRAY_V(bes_count_period_frac, CMSDKAPBDualTimer,
                              CMSDK_APB_DUALTIMER_NUM_MODULES, 3),
        VMSTATE_BOOL_ARRAY_V(bes_count_valid, CMSDKAPBDualTimer,
                            CMSDK_APB_DUALTIMER_NUM_MODULES, 3),
        VMSTATE_END_OF_LIST()
    }
};

static const Property cmsdk_apb_dualtimer_properties[] = {
    DEFINE_PROP_UINT32("bes-oneshot-underflow-mask", CMSDKAPBDualTimer,
                       bes_oneshot_underflow_mask, 0),
};

static void cmsdk_apb_dualtimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = cmsdk_apb_dualtimer_realize;
    dc->vmsd = &cmsdk_apb_dualtimer_vmstate;
    device_class_set_props(dc, cmsdk_apb_dualtimer_properties);
    device_class_set_legacy_reset(dc, cmsdk_apb_dualtimer_reset);
}

static const TypeInfo cmsdk_apb_dualtimer_info = {
    .name = TYPE_CMSDK_APB_DUALTIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CMSDKAPBDualTimer),
    .instance_init = cmsdk_apb_dualtimer_init,
    .class_init = cmsdk_apb_dualtimer_class_init,
};

static void cmsdk_apb_dualtimer_register_types(void)
{
    type_register_static(&cmsdk_apb_dualtimer_info);
}

type_init(cmsdk_apb_dualtimer_register_types);
