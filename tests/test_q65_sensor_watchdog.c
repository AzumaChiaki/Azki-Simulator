/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int MemoryRegion, MemTxResult, MemTxAttrs;
typedef uint64_t hwaddr;
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
typedef struct {
    uint64_t count, limit;
    bool running, transaction;
} ptimer_state;
static void ptimer_transaction_begin(ptimer_state *t)
{ assert(!t->transaction); t->transaction = true; }
static void ptimer_transaction_commit(ptimer_state *t)
{ assert(t->transaction); t->transaction = false; }
static void ptimer_stop(ptimer_state *t)
{ assert(t->transaction); t->running = false; }
static void ptimer_run(ptimer_state *t, int oneshot)
{ assert(t->transaction && !oneshot); t->running = true; }
static void ptimer_set_limit(ptimer_state *t, uint64_t value, int reload)
{ assert(t->transaction); t->limit = value; if (reload) { t->count = value; } }
static void ptimer_set_count(ptimer_state *t, uint64_t value)
{ assert(t->transaction); t->count = value; }
static uint64_t ptimer_get_limit(ptimer_state *t) { return t->limit; }
static uint64_t ptimer_get_count(ptimer_state *t) { return t->count; }

#include "q65-sensor-watchdog.inc"

static void put(BesSensorWatchdog *w, uint32_t reg, uint32_t value)
{ assert(sensor_watchdog_write(w, reg, value, 4, 0) == MEMTX_OK); }
static uint32_t get(BesSensorWatchdog *w, uint32_t reg)
{
    uint64_t value;
    assert(sensor_watchdog_read(w, reg, &value, 4, 0) == MEMTX_OK);
    return value;
}
static void expire(BesSensorWatchdog *w)
{
    /* QEMU invokes ptimer callbacks from inside a timer transaction. */
    assert(w->timer->running);
    ptimer_transaction_begin(w->timer);
    sensor_watchdog_tick(w);
    ptimer_transaction_commit(w->timer);
}

int main(void)
{
    ptimer_state timer = {0};
    BesSensorWatchdog w = {.timer = &timer};
    sensor_watchdog_reset(&w);
    assert(get(&w, 0) == UINT32_MAX && get(&w, 4) == UINT32_MAX);
    assert(get(&w, 8) == 0 && get(&w, 0xc00) == 0 && !timer.running);
    put(&w, 0xc00, 1);
    put(&w, 0, 1); put(&w, 8, 3);
    assert(get(&w, 0) == UINT32_MAX && !timer.running);
    put(&w, 0xc00, 0x1acce551);
    put(&w, 0, 100); put(&w, 8, 3);
    assert(timer.running && get(&w, 4) == 100);
    timer.count = 33;
    assert(get(&w, 4) == 33); /* VALUE must be the real timer count. */
    expire(&w);
    assert(get(&w, 0x10) == 1 && get(&w, 0x14) == 1);
    assert(w.irq_output && !w.reset_output && timer.running);
    put(&w, 0xc00, 1); put(&w, 0xc, 1);
    assert(w.intstatus == 1); /* Locked feed cannot acknowledge IRQ. */
    put(&w, 0xc00, 0x1acce551); put(&w, 0xc, 1);
    assert(!w.irq_output && !w.intstatus && get(&w, 4) == 100);
    expire(&w); expire(&w);
    assert(w.intstatus && w.resetstatus && w.reset_output && !timer.running);
    put(&w, 8, 0);
    assert(get(&w, 0x10) == 1 && get(&w, 0x14) == 0);
    assert(!w.irq_output && !w.reset_output);
    sensor_watchdog_reset(&w);
    assert(!w.resetstatus && !w.intstatus && !timer.running);
    put(&w, 0xf00, 1); put(&w, 0xf04, 3);
    assert(w.irq_output && w.reset_output && !timer.running);
    put(&w, 0xf00, 0);
    assert(!w.irq_output && !w.reset_output);
    uint64_t value;
    assert(sensor_watchdog_read(&w, 0x100, &value, 4, 0) == MEMTX_ERROR);
    assert(sensor_watchdog_write(&w, 0x100, 0, 4, 0) == MEMTX_ERROR);
    puts("Q65 sensor watchdog: counter, lock, feed, expiry and domain isolation passed");
    return 0;
}
