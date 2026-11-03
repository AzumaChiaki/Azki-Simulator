/* SPDX-License-Identifier: GPL-2.0-or-later
 * Production Q65 ISPI/GPADC functions with only host clock/IRQ/QOM stubs. */
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

#include "q65-ispi-state.inc"
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
    if (!error) { return; }
    *error = g_new0(Error, 1);
    va_list args;
    va_start(args, format);
    (*error)->message = g_strdup_vprintf(format, args);
    va_end(args);
}
static void error_clear(Error **error)
{
    if (*error) {
        g_free((*error)->message);
        g_free(*error);
        *error = NULL;
    }
}

#include "q65-adc-functions.inc"

static unsigned checks;
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void start(Best1503State *s)
{
    memset(s, 0, sizeof(*s));
    best1503_instance_init((Object *)s);
    s->power_adc_fixture = true;
    s->ispi.machine = s;
    s->ispi.regs[0] = 24u << 16; /* 25-bit internal SPI frames. */
    s->ispi.regs[1] = 3u << 7;  /* PMU CS3, page 0. */
    clock_ns = 0;
}

static uint16_t transfer(BesIspi *spi, bool read, unsigned reg, uint16_t value)
{
    ispi_write(spi, 8, (read ? 1u << 24 : 0) | reg << 16 | value, 4);
    return ispi_read(spi, 8, 4);
}

static void defaults_and_atomic_input(void)
{
    Best1503State s;
    start(&s);
    for (unsigned i = 0; i < BES_ADC_CHANNELS; i++) {
        CHECK(s.adc_input.values[i] == (i == 3 ? 12090 : i == 6 ? 15968 : 21000));
        CHECK(s.adc_input.reads[i] == 0 && s.adc_input.last_read_version[i] == 0);
    }
    Error *error = NULL;
    best1503_set_adc_raw((Object *)&s, "0,1,2,3,4,5,6,65535", &error);
    CHECK(!error && s.adc_input.version == 1 && s.adc_input.values[7] == 65535);
    CHECK(s.adc_input.reads[3] == 0 && s.adc_input.reads[6] == 0);
    const char *bad[] = {
        "0,1,2,3,4,5,6", "0,1,2,3,4,5,6,7,8", "0,1,2,3,4,5,6,65536",
        "0,1,2,3,4,5,6,-1", "0,1,2,3,4,5,6, 7", "0,1,2,3,4,5,6,7x",
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(bad); i++) {
        BesAdcInput before = s.adc_input;
        best1503_set_adc_raw((Object *)&s, bad[i], &error);
        CHECK(error && !memcmp(&before, &s.adc_input, sizeof(before)));
        error_clear(&error);
    }
    s.power_adc_fixture = false;
    best1503_set_adc_raw((Object *)&s, "1,1,1,1,1,1,1,1", &error);
    CHECK(error && s.adc_input.version == 1);
    error_clear(&error);
    s.power_adc_fixture = true;
    s.adc_input.version = UINT64_MAX;
    best1503_set_adc_raw((Object *)&s, "1,1,1,1,1,1,1,1", &error);
    CHECK(error && s.adc_input.version == UINT64_MAX && s.adc_input.values[0] == 0);
    error_clear(&error);
    g_mutex_clear(&s.adc_lock);
}

static void conversion_and_fifo_observation(void)
{
    Best1503State s;
    start(&s);
    BesIspi *spi = &s.ispi;
    Error *error = NULL;
    transfer(spi, false, 0x1d, 0xff);
    transfer(spi, false, 0x1f, 0x1ff);
    transfer(spi, false, 0x20, 0x1ff);
    transfer(spi, false, 0x4f, 0x20);
    CHECK(spi->adc_deadline == 125000);
    clock_ns = 124999;
    CHECK(transfer(spi, true, 0x59, 0) == 0 && s.adc_input.reads[3] == 0);
    CHECK(!spi->adc_sample_valid[3]);
    clock_ns = 125000;
    CHECK(transfer(spi, true, 0x50, 0) == 0x16c);
    CHECK(spi->analog[3][0][0x59] == 12090 && spi->analog[3][0][0x5c] == 15968);
    CHECK(spi->analog[3][0][0x58] == 21000 && spi->analog[3][0][0x5b] == 21000);
    CHECK(!spi->adc_deadline && !(spi->analog[3][0][0x4f] & 0x20));
    CHECK(s.adc_input.reads[3] == 0 && s.adc_input.reads[6] == 0);
    CHECK(!spi->adc_sample_valid[0] && !spi->adc_sample_valid[7]);
    best1503_set_adc_raw((Object *)&s, "0,1,2,300,4,5,600,7", &error);
    CHECK(!error && s.adc_input.version == 1);
    /* Read command acceptance alone is not guest consumption. */
    ispi_write(spi, 8, (1u << 24) | (0x59u << 16), 4);
    CHECK(spi->count == 1 && s.adc_input.reads[3] == 0);
    best1503_set_adc_raw((Object *)&s, "0,1,2,301,4,5,601,7", &error);
    CHECK(!error && s.adc_input.version == 2);
    CHECK(ispi_read(spi, 8, 4) == 12090);
    CHECK(s.adc_input.reads[3] == 1 && s.adc_input.last_read_version[3] == 0);
    CHECK(ispi_read(spi, 8, 4) == 0 && s.adc_input.reads[3] == 1);
    /* W1C clears completion, while the conversion's result remains latched. */
    transfer(spi, false, 0x51, 0x1ff);
    CHECK(transfer(spi, true, 0x50, 0) == 0);
    CHECK(transfer(spi, true, 0x59, 0) == 12090 && s.adc_input.last_read_version[3] == 0);
    transfer(spi, false, 0x1d, 0x48);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns += 125000;
    CHECK(transfer(spi, true, 0x59, 0) == 301);
    CHECK(s.adc_input.last_read_version[3] == 2);
    best1503_set_adc_raw((Object *)&s, "0,1,2,302,4,5,602,7", &error);
    CHECK(!error && s.adc_input.version == 3);
    CHECK(transfer(spi, true, 0x5c, 0) == 601 && s.adc_input.last_read_version[6] == 2);
    CHECK(transfer(spi, true, 0x5d, 0) == 0 && s.adc_input.reads[7] == 0);
    /* Full RX FIFO drops the new response; it must not claim observation. */
    uint64_t before_reads = s.adc_input.reads[6];
    for (unsigned i = 0; i < 8; i++) {
        ispi_write(spi, 8, (1u << 24) | (0x50u << 16), 4);
    }
    ispi_write(spi, 8, (1u << 24) | (0x5cu << 16), 4);
    CHECK(spi->count == 8 && s.adc_input.reads[6] == before_reads);
    for (unsigned i = 0; i < 8; i++) { ispi_read(spi, 8, 4); }
    CHECK(s.adc_input.reads[6] == before_reads);
    CHECK(transfer(spi, true, 0x5c, 0) == 601 && s.adc_input.reads[6] == before_reads + 1);
    /* Periodic scans sample at the next conversion, not at host update. */
    transfer(spi, false, 0x18, 0x1000);
    transfer(spi, false, 0x1c, 122);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns += 125000;
    CHECK(transfer(spi, true, 0x59, 0) == 302 && s.adc_input.last_read_version[3] == 3);
    CHECK(spi->adc_deadline == clock_ns + 122 * 1024);
    best1503_set_adc_raw((Object *)&s, "0,1,2,303,4,5,603,7", &error);
    CHECK(!error && transfer(spi, true, 0x5c, 0) == 602);
    CHECK(s.adc_input.last_read_version[6] == 3);
    clock_ns += 122 * 1024;
    CHECK(transfer(spi, true, 0x5c, 0) == 603 && s.adc_input.last_read_version[6] == 4);
    uint64_t reads = s.adc_input.reads[6];
    char *state = best1503_get_adc_state((Object *)&s, &error);
    CHECK(!error && strstr(state, "\"schema\":1,\"fixtureEnabled\":true,\"version\":4"));
    CHECK(strstr(state, "\"values\":[0,1,2,303,4,5,603,7]"));
    CHECK(s.adc_input.reads[6] == reads);
    g_free(state);
    g_mutex_clear(&s.adc_lock);
}

int main(void)
{
    defaults_and_atomic_input();
    conversion_and_fifo_observation();
    printf("Q65 ADC production regression: %u checks passed\n", checks);
    return 0;
}
