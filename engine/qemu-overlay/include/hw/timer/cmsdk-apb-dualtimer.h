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
 *
 * QEMU interface:
 *  + Clock input "TIMCLK": clock (for both timers)
 *  + sysbus MMIO region 0: the register bank
 *  + sysbus IRQ 0: combined timer interrupt TIMINTC
 *  + sysbus IRO 1: timer block 1 interrupt TIMINT1
 *  + sysbus IRQ 2: timer block 2 interrupt TIMINT2
 */

#ifndef CMSDK_APB_DUALTIMER_H
#define CMSDK_APB_DUALTIMER_H

#include "hw/sysbus.h"
#include "hw/ptimer.h"
#include "hw/clock.h"
#include "qom/object.h"

#define TYPE_CMSDK_APB_DUALTIMER "cmsdk-apb-dualtimer"
OBJECT_DECLARE_SIMPLE_TYPE(CMSDKAPBDualTimer, CMSDK_APB_DUALTIMER)


/* One of the two identical timer modules in the dual-timer module */
typedef struct CMSDKAPBDualTimerModule {
    CMSDKAPBDualTimer *parent;
    struct ptimer_state *timer;
    qemu_irq timerint;
    /*
     * We must track the guest LOAD and VALUE register state by hand
     * rather than leaving this state only in the ptimer limit/count,
     * because if CONTROL.SIZE is 0 then only the low 16 bits of the
     * counter actually counts, but the high half is still guest
     * accessible.
     */
    uint32_t load;
    uint32_t value;
    uint32_t control;
    uint32_t intstatus;
} CMSDKAPBDualTimerModule;

#define CMSDK_APB_DUALTIMER_NUM_MODULES 2

struct CMSDKAPBDualTimer {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq timerintc;
    Clock *timclk;

    CMSDKAPBDualTimerModule timermod[CMSDK_APB_DUALTIMER_NUM_MODULES];
    uint32_t timeritcr;
    uint32_t timeritop;

    /* Board-selected BES channels count through zero in one-shot mode. */
    uint32_t bes_oneshot_underflow_mask;
    /* Inclusive remaining ticks at a count-changing MMIO/clock event.
     * ptimer drives expiry/IRQ; these retain exact register readback at
     * integer and fractional tick boundaries without changing ptimer. */
    int64_t bes_count_time[CMSDK_APB_DUALTIMER_NUM_MODULES];
    uint64_t bes_count_value[CMSDK_APB_DUALTIMER_NUM_MODULES];
    uint64_t bes_count_period_ns[CMSDK_APB_DUALTIMER_NUM_MODULES];
    uint32_t bes_count_period_frac[CMSDK_APB_DUALTIMER_NUM_MODULES];
    bool bes_count_valid[CMSDK_APB_DUALTIMER_NUM_MODULES];
};

#endif
