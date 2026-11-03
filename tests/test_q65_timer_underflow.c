/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct ptimer_state ptimer_state;
typedef void (*ptimer_cb)(void *);
typedef int QEMUClockType;
typedef int ClockEvent;
typedef struct { uint64_t period; } Clock;
static uint64_t clock_get(const Clock *c) { return c->period; }
#define QEMU_CLOCK_VIRTUAL 0
#define g_new0(type, count) ((type *)calloc(count, sizeof(type)))
#define g_free free
#define g_assert_not_reached() assert(0)
static int clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static uint64_t extract64(uint64_t v, int start, int length)
{ return (v >> start) & (UINT64_MAX >> (64 - length)); }
static uint32_t deposit32(uint32_t v, int start, int length, uint32_t value)
{
    uint32_t mask = (UINT32_MAX >> (32 - length)) << start;
    return (v & ~mask) | ((value << start) & mask);
}
static bool qtest_enabled(void) { return true; }
static bool icount_enabled(void) { return false; }
static int64_t now_ns;
static int64_t qemu_clock_get_ns(QEMUClockType type) { return now_ns; }
typedef struct QEMUTimer {
    int64_t deadline;
    bool pending;
    void (*callback)(void *);
    void *opaque;
    struct QEMUTimer *next;
} QEMUTimer;
static QEMUTimer *timers;
static QEMUTimer *timer_new_ns(QEMUClockType type, void (*cb)(void *), void *opaque)
{
    QEMUTimer *t = calloc(1, sizeof(*t));
    t->callback = cb; t->opaque = opaque; t->next = timers; timers = t;
    return t;
}
static void timer_mod(QEMUTimer *t, int64_t deadline)
{ t->deadline = deadline; t->pending = true; }
static void timer_del(QEMUTimer *t) { t->pending = false; }
static void timer_free(QEMUTimer *t)
{
    QEMUTimer **p = &timers;
    while (*p != t) { assert(*p); p = &(*p)->next; }
    *p = t->next; free(t);
}
static void advance(uint64_t ns)
{
    int64_t target = now_ns + ns;
    for (;;) {
        QEMUTimer *next = NULL;
        for (QEMUTimer *t = timers; t; t = t->next) {
            if (t->pending && t->deadline <= target &&
                (!next || t->deadline < next->deadline)) { next = t; }
        }
        if (!next) { break; }
        assert(next->deadline >= now_ns);
        now_ns = next->deadline; next->pending = false;
        next->callback(next->opaque);
    }
    now_ns = target;
}
/* Declarations supplied by QEMU's public ptimer header in the real build. */
static void ptimer_transaction_begin(ptimer_state *);
static void ptimer_transaction_commit(ptimer_state *);
#include "ptimer-production.inc"

typedef int MemoryRegion, SysBusDevice;
typedef uint64_t hwaddr;
typedef int *qemu_irq;
typedef struct CMSDKAPBDualTimer CMSDKAPBDualTimer;
#define OBJECT_DECLARE_SIMPLE_TYPE(type, name)
#define CMSDK_APB_DUALTIMER(p) ((CMSDKAPBDualTimer *)(p))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define REG32(name, offset) enum { A_ ## name = offset };
#define FIELD(reg, name, shift, bits) enum { R_ ## reg ## _ ## name ## _SHIFT = shift, \
    R_ ## reg ## _ ## name ## _MASK = ((1u << bits) - 1) << shift };
#define FIELD_EX32(v, reg, name) (((v) & R_ ## reg ## _ ## name ## _MASK) >> R_ ## reg ## _ ## name ## _SHIFT)
#define qemu_log_mask(...) ((void)0)
#define LOG_GUEST_ERROR 0
#define trace_cmsdk_apb_dualtimer_read(...) ((void)0)
#define trace_cmsdk_apb_dualtimer_write(...) ((void)0)
static void qemu_set_irq(qemu_irq irq, int level) { if (irq) { *irq = level; } }
#include "dualtimer-production.inc"

static int irq[3];
static Clock test_clock;
static void init(CMSDKAPBDualTimer *s, uint32_t mask, uint32_t hz)
{
    assert(timers == NULL);
    now_ns = 0; memset(s, 0, sizeof(*s)); memset(irq, 0, sizeof(irq));
    test_clock.period = ((uint64_t)1000000000 << 32) / hz;
    s->timclk = &test_clock; s->bes_oneshot_underflow_mask = mask;
    s->timerintc = &irq[0];
    for (unsigned i = 0; i < 2; i++) {
        CMSDKAPBDualTimerModule *m = &s->timermod[i];
        m->parent = s; m->timerint = &irq[i + 1];
        m->timer = ptimer_init(cmsdk_dualtimermod_tick, m, cmsdk_dualtimermod_policy(m));
        cmsdk_dualtimermod_reset(m);
    }
}
static void fini(CMSDKAPBDualTimer *s)
{ for (unsigned i = 0; i < 2; i++) { ptimer_free(s->timermod[i].timer); } }
static void put(CMSDKAPBDualTimer *s, uint32_t offset, uint32_t value)
{ cmsdk_apb_dualtimer_write(s, offset, value, 4); }
static uint32_t get(CMSDKAPBDualTimer *s, uint32_t offset)
{ return cmsdk_apb_dualtimer_read(s, offset, 4); }
static void start(CMSDKAPBDualTimer *s, uint32_t load, uint32_t ctrl)
{ put(s, 0x28, ctrl & ~0x80); put(s, 0x20, load); put(s, 0x28, ctrl); }

static void test_exact_underflow(void)
{
    CMSDKAPBDualTimer s; init(&s, 2, 1000000);
    start(&s, 3, 0xa3);
    assert(get(&s, 0x20) == 3 && get(&s, 0x24) == 3);
    advance(2999); assert(get(&s, 0x24) == 1 && !irq[2]);
    advance(1); assert(get(&s, 0x24) == 0 && !irq[2]);
    advance(999); assert(get(&s, 0x24) == 0 && !irq[2]);
    advance(1); assert(get(&s, 0x30) == 1 && irq[0] && irq[2]);
    assert(get(&s, 0x28) == 0xa3 && get(&s, 0x24) == 0);
    put(&s, 0x2c, 0); assert(!irq[0] && !irq[2]);
    /* CONTROL/INTEN/SIZE writes cannot revive the completed timer. */
    put(&s, 0x28, 0x83); put(&s, 0x28, 0xa1); put(&s, 0x28, 0xa3);
    advance(10000); assert(!irq[2] && get(&s, 0x30) == 0);
    /* Enabled LOAD rearms an expired mode=0 one-shot, including zero. */
    put(&s, 0x20, 0); assert(get(&s, 0x20) == 0 && get(&s, 0x24) == 0);
    advance(999); assert(!irq[2]); advance(1); assert(irq[2]);
    put(&s, 0x2c, 1); put(&s, 0x20, 1);
    advance(1000); assert(get(&s, 0x24) == 0 && !irq[2]);
    advance(1000); assert(irq[2]); fini(&s);
}

static void test_fractional_clock(void)
{
    CMSDKAPBDualTimer s; init(&s, 2, 16384);
    start(&s, 164, 0xa3);
    assert(get(&s, 0x24) == 164);
    advance(1); assert(get(&s, 0x24) == 164);
    advance(10009764); assert(!irq[2]); /* old N-tick deadline */
    advance(61034); assert(!irq[2]);
    advance(1); assert(!irq[2]); /* floor deadline is still too early */
    advance(1); assert(irq[2]); /* ceil(165 * 1e9 / 16384) */
    fini(&s);
}

static void test_rearm_mask_and_bgload(void)
{
    CMSDKAPBDualTimer s; init(&s, 2, 1000000);
    start(&s, 5, 0xe3); advance(2000);
    put(&s, 0x38, 99); assert(get(&s, 0x20) == 99 && get(&s, 0x24) == 3);
    put(&s, 0x20, 2); assert(get(&s, 0x24) == 2);
    put(&s, 0x28, 0xc3); /* mask IRQ without restarting countdown */
    advance(2999); assert(get(&s, 0x30) == 0);
    advance(1); assert(get(&s, 0x30) == 1 && get(&s, 0x34) == 0 && !irq[2]);
    put(&s, 0x28, 0xe3); assert(irq[2]);
    put(&s, 0x2c, 1); advance(10000); assert(!irq[2]);
    fini(&s);
}

static void test_size_max_and_modes(void)
{
    CMSDKAPBDualTimer s; init(&s, 2, 1000000);
    start(&s, UINT32_MAX, 0xa3);
    assert(s.timermod[1].timer->delta == UINT64_C(0x100000000));
    assert(get(&s, 0x24) == UINT32_MAX);
    advance(UINT64_C(0xffffffff) * 1000); assert(!irq[2] && get(&s, 0x24) == 0);
    advance(1000); assert(irq[2]);
    put(&s, 0x2c, 1); start(&s, 0xabcd0001, 0xa1);
    assert(get(&s, 0x24) == 0xabcd0001 && s.timermod[1].timer->delta == 2);
    advance(1000); assert(get(&s, 0x24) == 0xabcd0000 && !irq[2]);
    advance(1000); assert(irq[2]);
    put(&s, 0x2c, 1); start(&s, 0x12340003, 0x23);
    put(&s, 0x28, 0x21); assert(get(&s, 0x24) == 0x12340003);
    put(&s, 0x28, 0x23); assert(get(&s, 0x24) == 0x12340003);
    /* Remove and restore ONESHOT while paused: one bias, not two. */
    put(&s, 0x28, 0x22); assert(get(&s, 0x24) == 0x12340003);
    put(&s, 0x28, 0x23); assert(get(&s, 0x24) == 0x12340003);
    fini(&s);
}

static void test_prescale_pause_and_clock(void)
{
    CMSDKAPBDualTimer s; init(&s, 2, 1000000);
    start(&s, 1, 0xa7); advance(31999); assert(!irq[2]);
    advance(1); assert(irq[2]);
    put(&s, 0x2c, 1); start(&s, 1, 0xab);
    advance(511999); assert(!irq[2]); advance(1); assert(irq[2]);
    put(&s, 0x2c, 1); start(&s, 3, 0xa3); advance(1000);
    put(&s, 0x28, 0x23); assert(get(&s, 0x24) == 2);
    advance(10000); assert(get(&s, 0x24) == 2 && !irq[2]);
    put(&s, 0x28, 0xa7); /* three remaining ticks, divided by16 */
    advance(47999); assert(!irq[2]); advance(1); assert(irq[2]);
    put(&s, 0x2c, 1); start(&s, 3, 0xa3); advance(1000);
    test_clock.period = (uint64_t)2000 << 32;
    cmsdk_apb_dualtimer_clk_update(&s, 0);
    advance(5999); assert(!irq[2]); advance(1); assert(irq[2]);
    fini(&s);
    /* Same-instant fractional pause/rescale cannot lose one tick. */
    init(&s, 2, 16384); start(&s, 164, 0xa3);
    put(&s, 0x28, 0x23); assert(get(&s, 0x24) == 164);
    put(&s, 0x28, 0xa7); assert(get(&s, 0x24) == 164);
    advance(161132812); assert(!irq[2]); advance(1); assert(irq[2]);
    fini(&s);
}

static void test_default_and_migration(void)
{
    CMSDKAPBDualTimer s; init(&s, 0, 1000000);
    start(&s, 3, 0xa3); advance(2999); assert(!irq[2]);
    advance(1); assert(irq[2]); /* default retains N-tick behavior */
    assert(cmsdk_apb_dualtimer_post_load(&s, 2) == 0);
    fini(&s); init(&s, 2, 1000000);
    put(&s, 0, 3); put(&s, 8, 0xa3);
    advance(3000); assert(irq[1] && !irq[2]); /* unselected channel unchanged */
    assert(cmsdk_apb_dualtimer_post_load(&s, 2) == -EINVAL);
    start(&s, 3, 0xa3); advance(1000);
    int64_t deadline = s.timermod[1].timer->next_event;
    assert(cmsdk_apb_dualtimer_post_load(&s, 3) == 0);
    assert(s.timermod[1].timer->next_event == deadline);
    advance(2999); assert(!irq[2]); advance(1); assert(irq[2]);
    fini(&s);
}

static void test_hal_deadline_comparison(void)
{
    /* AON channel0 is an unmodified free-running clocksource.  The HAL
     * compares its complemented VALUE with the saved target after channel1
     * fires.  Exercise nonzero phases after clocksource initialization. */
    static const uint32_t phases[] = {1, 100, 999, 1000, 1001, 61034, 61035,
                                     61036, 122070, 122071, 999999};
    static const uint32_t deltas[] = {2, 3, 165, 1000};
    for (unsigned h = 0; h < 2; h++) {
        uint32_t hz = h ? 16384 : 1000000;
        for (unsigned p = 0; p < ARRAY_SIZE(phases); p++) {
            for (unsigned d = 0; d < ARRAY_SIZE(deltas); d++) {
                CMSDKAPBDualTimer s; init(&s, 2, hz);
                put(&s, 0, UINT32_MAX); put(&s, 8, 0x82);
                advance(phases[p]);
                uint32_t saved = ~get(&s, 4);
                start(&s, deltas[d] - 1, 0xa3);
                int64_t deadline = s.timermod[1].timer->next_event;
                advance(deadline - now_ns - 1); assert(!irq[2]);
                advance(1); assert(irq[2]);
                uint32_t current = ~get(&s, 4);
                if (current < saved + deltas[d]) {
                    fprintf(stderr, "HAL early hz=%u phase=%u delta=%u saved=%u current=%u\n",
                            hz, phases[p], deltas[d], saved, current);
                    abort();
                }
                fini(&s);
            }
        }
    }
}

static void test_floor_negative_control(void)
{
    CMSDKAPBDualTimer s; init(&s, 2, 16384);
    /* Preserve the failing vector with the new policy explicitly absent.
     * This is the original ptimer rounding path, not a second arithmetic
     * approximation of the observed failure. */
    s.timermod[1].timer->policy_mask &= ~PTIMER_POLICY_ONESHOT_ROUND_UP;
    put(&s, 0, UINT32_MAX); put(&s, 8, 0x82); advance(61035);
    uint32_t saved = ~get(&s, 4);
    start(&s, 164, 0xa3);
    advance(s.timermod[1].timer->next_event - now_ns);
    uint32_t current = ~get(&s, 4);
    assert(saved == 1 && current == 165 && current < saved + 165);
    puts("negative control retained: floor policy gives current165 < target166");
    fini(&s);
}

static void test_fractional_product_limits(void)
{
    /* Policy is inactive in periodic mode, including on a selected channel.
     * Its product must retain all 64 bits at the largest BES counter value. */
    CMSDKAPBDualTimer s; init(&s, 2, 16384);
    start(&s, UINT32_MAX, 0xa3);
    assert(s.timermod[1].timer->next_event == INT64_C(262144000000000));
    assert(get(&s, 0x24) == UINT32_MAX);
    fini(&s); init(&s, 2, 16384);
    start(&s, UINT32_MAX - 1, 0xa3);
    assert(s.timermod[1].timer->next_event == INT64_C(262143999938965));
    fini(&s);
    init(&s, 2, 16384);
    start(&s, 3, 0xe2); /* periodic, no underflow-count encoding */
    assert(s.timermod[1].timer->next_event == 183105); /* floor unchanged */
    assert(s.timermod[1].timer->enabled == 1);
    fini(&s);
    init(&s, 2, 16384);
    now_ns = INT64_MAX - 122070;
    start(&s, 1, 0xa3);
    assert(s.timermod[1].timer->next_event == INT64_MAX); /* ceil saturates */
    fini(&s);
}

int main(void)
{
    test_exact_underflow(); test_fractional_clock(); test_rearm_mask_and_bgload();
    test_size_max_and_modes(); test_prescale_pause_and_clock(); test_default_and_migration();
    test_hal_deadline_comparison();
    test_floor_negative_control(); test_fractional_product_limits();
    puts("Q65 production ptimer underflow tests passed (no guest)");
    return 0;
}
