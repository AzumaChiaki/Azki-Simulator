/* SPDX-License-Identifier: GPL-2.0-or-later
 * Targeted Q65 production-function regression, with host-only clock/QOM stubs.
 * Baseline and candidate compile the same vector against their actual source. */
#include <glib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "best1503-adc.h"

typedef uint64_t hwaddr;
typedef int MemoryRegion;
typedef int qemu_irq;
typedef GMutex QemuMutex;
typedef void Object;
typedef struct { char *message; } Error;
typedef struct Best1503State Best1503State;
#include "q65-toggle-ispi-state.inc"
struct Best1503State {
    bool power_adc_fixture;
    QemuMutex adc_lock;
    BesAdcInput adc_input;
    BesIspi ispi;
};
#define QEMU_CLOCK_VIRTUAL 0
#define LOG_GUEST_ERROR 0
#define CPU_LOG_TB_IN_ASM 1
#define qemu_log_mask(...) ((void)0)
static bool qemu_loglevel_mask(int mask) { (void)mask; return false; }
static int64_t clock_ns;
static int64_t qemu_clock_get_ns(int clock) { (void)clock; return clock_ns; }
static void qemu_set_irq(qemu_irq irq, int level) { (void)irq; (void)level; }
static void qemu_mutex_init(QemuMutex *lock) { g_mutex_init(lock); }
static void qemu_mutex_lock(QemuMutex *lock) { g_mutex_lock(lock); }
static void qemu_mutex_unlock(QemuMutex *lock) { g_mutex_unlock(lock); }
static void error_setg(Error **error, const char *format, ...)
{
    if (!error) return;
    *error = g_new0(Error, 1);
    va_list args; va_start(args, format);
    (*error)->message = g_strdup_vprintf(format, args); va_end(args);
}
#include "q65-toggle-functions.inc"
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL check %u line %d: %s\n", checks, __LINE__, #c); exit(1); \
} } while (0)

static uint16_t transfer(BesIspi *spi, bool read, unsigned reg, uint16_t value)
{
    ispi_write(spi, 8, (read ? 1u << 24 : 0) | reg << 16 | value, 4);
    return ispi_read(spi, 8, 4);
}

int main(void)
{
    Best1503State s = {0};
    best1503_instance_init((Object *)&s);
    Error *error = NULL;
    best1503_set_power_adc_fixture((Object *)&s, true, &error);
    s.ispi.machine = &s;
    BesIspi *spi = &s.ispi;
    spi->regs[0] = 24u << 16;
    spi->regs[1] = 3u << 7;
    best1503_set_adc_raw((Object *)&s, "100,101,102,103,104,105,106,107", &error);
    CHECK(!error && s.adc_input.version == 1);
    transfer(spi, false, 0x1d, 0x08);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns = 125000;
    CHECK(transfer(spi, true, 0x50, 0) == 0x108);
    CHECK(spi->adc_sample_valid[3]);
    /* A repeated bool is not a mode transition; do not discard conversion. */
    best1503_set_power_adc_fixture((Object *)&s, true, &error);
    CHECK(spi->adc_sample_valid[3]);
    CHECK(transfer(spi, true, 0x59, 0) == 103);
    CHECK(s.adc_input.reads[3] == 1 && s.adc_input.last_read_version[3] == 1);
    ispi_write(spi, 8, (1u << 24) | (0x59u << 16), 4);
    CHECK(spi->count == 1 && s.adc_input.reads[3] == 1);
    best1503_set_power_adc_fixture((Object *)&s, false, &error);
    ispi_write(spi, 8, (0x59u << 16) | 7777u, 4);
    CHECK(spi->analog[3][0][0x59] == 7777);
    best1503_set_power_adc_fixture((Object *)&s, true, &error);
    CHECK(!error);
    /* The queued word predates mode change and remains valid old evidence. */
    CHECK(ispi_read(spi, 8, 4) == 103);
    CHECK(s.adc_input.reads[3] == 2 && s.adc_input.last_read_version[3] == 1);
    CHECK(ispi_read(spi, 8, 4) == 0 && s.adc_input.reads[3] == 2);
    /* A guest register write must not inherit the prior conversion version. */
    CHECK(transfer(spi, true, 0x59, 0) == 7777);
    CHECK(s.adc_input.reads[3] == 2);
    CHECK(!spi->adc_sample_valid[3]);
    best1503_set_adc_raw((Object *)&s, "100,101,102,301,104,105,106,107", &error);
    CHECK(!error && s.adc_input.version == 2);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns += 124999;
    CHECK(transfer(spi, true, 0x59, 0) == 7777 && s.adc_input.reads[3] == 2);
    clock_ns++;
    CHECK(transfer(spi, true, 0x59, 0) == 301);
    CHECK(spi->adc_sample_valid[3]);
    CHECK(s.adc_input.reads[3] == 3 && s.adc_input.last_read_version[3] == 2);
    best1503_set_power_adc_fixture((Object *)&s, true, &error);
    CHECK(spi->adc_sample_valid[3]);
    CHECK(transfer(spi, true, 0x59, 0) == 301);
    CHECK(s.adc_input.reads[3] == 4 && s.adc_input.last_read_version[3] == 2);
    g_mutex_clear(&s.adc_lock);
    printf("Q65 ADC toggle production regression: %u checks passed; host-only, no guest run\n", checks);
    return 0;
}
