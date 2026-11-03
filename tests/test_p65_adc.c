/* SPDX-License-Identifier: GPL-2.0-or-later
 * Production P65 ISPI/GPADC functions with only host clock/IRQ/QOM stubs. */
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
typedef struct { uint32_t value; bool output, fail; } Visitor;
typedef struct Best1503State Best1503State;

#include "p65-ispi-state.inc"
struct Best1503State {
    bool power_adc_fixture;
    QemuMutex adc_lock;
    BesAdcInput adc_input;
    uint32_t adc_ntc_channels, adc_ntc_room;
    uint8_t charger_state;
    uint16_t pmu_charger_edges;
    bool pmu_charger_ac_on, pmu_charger_armed;
    BesIspi ispi;
};

#define QEMU_CLOCK_VIRTUAL 0
#define LOG_GUEST_ERROR 0
#define CPU_LOG_TB_IN_ASM 1
#define qemu_log_mask(...) ((void)0)
static bool qemu_loglevel_mask(int mask) { (void)mask; return false; }
static int64_t clock_ns;
static int64_t qemu_clock_get_ns(int clock) { (void)clock; return clock_ns; }
static int irq_level;
static void qemu_set_irq(qemu_irq irq, int level) { (void)irq; irq_level = level; }
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

static bool visit_type_uint32(Visitor *v, const char *name, uint32_t *value, Error **errp)
{
    (void)name;
    if (v->fail) { error_setg(errp, "invalid visitor input"); return false; }
    if (v->output) { v->value = *value; } else { *value = v->value; }
    return true;
}

#include "p65-adc-functions.inc"

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
    irq_level = 0;
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

static void target_channel_and_fixture_boundaries(void)
{
    Best1503State s;
    start(&s);
    BesIspi *spi = &s.ispi;
    Error *error = NULL;
    best1503_set_adc_raw((Object *)&s, "100,101,102,103,104,105,106,107", &error);
    CHECK(!error);
    transfer(spi, false, 0x1d, 0xff);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns = 125000;
    CHECK(transfer(spi, true, 0x50, 0) == 0x16c);
    for (unsigned channel=0; channel<8; channel++) {
        bool supported = (1u << channel) & 0x6c;
        CHECK(spi->adc_sample_valid[channel] == supported);
        CHECK(transfer(spi, true, 0x56+channel, 0) == (supported ? 100+channel : 0));
        CHECK(s.adc_input.reads[channel] == (supported ? 1 : 0));
    }
    BesAdcInput before = s.adc_input;
    /* CS2 and PMU page1 can contain identical bytes but are not ADC input. */
    spi->regs[1] = 2u << 7;
    spi->analog[2][0][0x59] = 8888;
    CHECK(transfer(spi, true, 0x59, 0) == 8888);
    CHECK(!memcmp(&before, &s.adc_input, sizeof(before)));
    spi->regs[1] = 3u << 7;
    transfer(spi, false, 0, 0xa010);
    CHECK(spi->page[3] == 1);
    spi->analog[3][1][0x59] = 7777;
    CHECK(transfer(spi, true, 0x59, 0) == 7777);
    CHECK(!memcmp(&before, &s.adc_input, sizeof(before)));
    transfer(spi, false, 0, 0xa000);
    CHECK(spi->page[3] == 0);
    s.power_adc_fixture = false;
    CHECK(transfer(spi, true, 0x59, 0) == 103);
    CHECK(!memcmp(&before, &s.adc_input, sizeof(before)));
    s.power_adc_fixture = true;
    /* Writes/status return SPI words but must never inherit a stale slot tag. */
    for (unsigned i=0; i<16; i++) {
        transfer(spi, false, 0x59, 0xabcd);
        transfer(spi, true, 0x50, 0);
    }
    CHECK(!memcmp(&before, &s.adc_input, sizeof(before)));
    CHECK(transfer(spi, true, 0x59, 0) == 103);
    CHECK(s.adc_input.reads[3] == before.reads[3]+1);
    /* Padding changes transport bits only; the ADC version remains latched. */
    spi->regs[0] = 31u << 16;
    ispi_write(spi, 8, ((1u << 24) | (0x5cu << 16)) << 7, 4);
    CHECK(s.adc_input.reads[6] == before.reads[6]);
    CHECK(ispi_read(spi, 8, 4) == 106u << 7);
    CHECK(s.adc_input.reads[6] == before.reads[6]+1 && s.adc_input.last_read_version[6] == 1);
    g_mutex_clear(&s.adc_lock);
}

static void reset_and_restart_boundaries(void)
{
    Best1503State s;
    start(&s);
    BesIspi *spi = &s.ispi;
    Error *error = NULL;
    best1503_set_adc_raw((Object *)&s, "0,1,2,300,4,5,600,7", &error);
    CHECK(!error);
    transfer(spi, false, 0x1d, 0x48);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns = 125000;
    CHECK(transfer(spi, true, 0x59, 0) == 300);
    CHECK(s.adc_input.reads[3] == 1 && s.adc_input.last_read_version[3] == 1);
    /* A conversion stop cancels the deadline; it does not publish new input. */
    transfer(spi, false, 0x4f, 0x20);
    best1503_set_adc_raw((Object *)&s, "0,1,2,301,4,5,601,7", &error);
    transfer(spi, false, 0x4f, 0);
    CHECK(!error && spi->adc_deadline == 0);
    clock_ns += 250000;
    CHECK(transfer(spi, true, 0x59, 0) == 300 && s.adc_input.last_read_version[3] == 1);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns += 125000;
    CHECK(transfer(spi, true, 0x59, 0) == 301 && s.adc_input.last_read_version[3] == 2);
    ispi_write(spi, 8, (1u << 24) | (0x5cu << 16), 4);
    CHECK(spi->count == 1 && spi->adc_sample_valid[6]);
    BesAdcInput before = s.adc_input;
    test_production_ispi_reset(&s);
    CHECK(spi->count == 0 && spi->head == 0 && spi->adc_deadline == 0);
    CHECK(spi->adc_pending == 0 && spi->key_pending == 0);
    for (unsigned channel=0; channel<8; channel++) {
        CHECK(spi->adc_channel[channel] == -1 && spi->adc_version[channel] == 0);
        CHECK(!spi->adc_sample_valid[channel] && spi->adc_sample_version[channel] == 0);
    }
    CHECK(!memcmp(&before, &s.adc_input, sizeof(before)));
    CHECK(ispi_read(spi, 8, 4) == 0 && s.adc_input.reads[6] == before.reads[6]);
    spi->regs[0] = 24u << 16;
    spi->regs[1] = 3u << 7;
    CHECK(transfer(spi, true, 0x5c, 0) == 0 && s.adc_input.reads[6] == before.reads[6]);
    transfer(spi, false, 0x1d, 0x40);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns += 124999;
    CHECK(transfer(spi, true, 0x5c, 0) == 0 && s.adc_input.reads[6] == before.reads[6]);
    clock_ns++;
    CHECK(transfer(spi, true, 0x5c, 0) == 601);
    CHECK(s.adc_input.reads[6] == before.reads[6]+1 && s.adc_input.last_read_version[6] == 2);
    g_mutex_clear(&s.adc_lock);
}

static void fixture_toggle_does_not_relabel_guest_register_writes(void)
{
    Best1503State s;
    start(&s);
    BesIspi *spi = &s.ispi;
    Error *error = NULL;
    best1503_set_adc_raw((Object *)&s, "100,101,102,103,104,105,106,107", &error);
    CHECK(!error);
    transfer(spi, false, 0x1d, 0x08);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns = 125000;
    transfer(spi, true, 0x50, 0);
    CHECK(spi->adc_sample_valid[3] && s.adc_input.reads[3] == 0);
    ispi_write(spi, 8, (1u << 24) | (0x59u << 16), 4);
    CHECK(spi->count == 1 && spi->adc_channel[spi->head] == 3);
    best1503_set_power_adc_fixture((Object *)&s, false, &error);
    ispi_write(spi, 8, (0x59u << 16) | 7777u, 4);
    CHECK(spi->analog[3][0][0x59] == 7777);
    best1503_set_power_adc_fixture((Object *)&s, true, &error);
    CHECK(!error);
    /* A response already clocked into RX before disable remains old evidence. */
    CHECK(ispi_read(spi, 8, 4) == 103);
    CHECK(s.adc_input.reads[3] == 1 && s.adc_input.last_read_version[3] == 1);
    CHECK(ispi_read(spi, 8, 4) == 0 && s.adc_input.reads[3] == 1);
    /* This byte was written by the guest, not converted from host raw input. */
    CHECK(transfer(spi, true, 0x59, 0) == 7777);
    CHECK(s.adc_input.reads[3] == 1);
    CHECK(!spi->adc_sample_valid[3]);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns += 125000;
    CHECK(transfer(spi, true, 0x59, 0) == 103);
    CHECK(spi->adc_sample_valid[3] && s.adc_input.reads[3] == 2);
    g_mutex_clear(&s.adc_lock);
}

static void ntc_updates_preserve_sample_provenance(void)
{
    Best1503State s;
    start(&s);
    BesIspi *spi = &s.ispi;
    Error *error = NULL;
    Visitor value = { .value = 1u << 3 };
    CHECK(s.adc_ntc_channels == 0 && s.adc_ntc_room == 12090);
    best1503_set_adc_ntc((Object *)&s, &value, "ntc", NULL, &error);
    CHECK(!error && s.adc_ntc_channels == 8 && s.adc_input.version == 0);
    transfer(spi, false, 0x1d, 8);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns = 125000;
    ispi_write(spi, 8, (1u << 24) | (0x59u << 16), 4);
    CHECK(spi->count == 1 && s.adc_input.reads[3] == 0);
    value.value = 10000;
    best1503_set_adc_ntc_room((Object *)&s, &value, "room", NULL, &error);
    CHECK(!error && s.adc_input.version == 1 && s.adc_input.values[3] == 10000);
    CHECK(s.adc_input.values[6] == 15968 && s.adc_input.reads[3] == 0);
    CHECK(ispi_read(spi, 8, 4) == 12090 && s.adc_input.last_read_version[3] == 0);
    transfer(spi, false, 0x4f, 0x20);
    clock_ns += 125000;
    CHECK(transfer(spi, true, 0x59, 0) == 10000 && s.adc_input.last_read_version[3] == 1);
    best1503_set_adc_ntc_room((Object *)&s, &value, "room", NULL, &error);
    CHECK(!error && s.adc_input.version == 1); /* Unchanged vector has no new epoch. */
    value.value = 0x28;
    best1503_set_adc_ntc((Object *)&s, &value, "ntc", NULL, &error);
    CHECK(!error && s.adc_ntc_channels == 0x28 && s.adc_input.version == 2);
    CHECK(s.adc_input.values[3] == 10000 && s.adc_input.values[5] == 10000);
    value.output = true;
    best1503_get_adc_ntc((Object *)&s, &value, "ntc", NULL, &error);
    CHECK(value.value == 0x28);
    best1503_get_adc_ntc_room((Object *)&s, &value, "room", NULL, &error);
    CHECK(value.value == 10000);
    value.output = false;
    BesAdcInput before = s.adc_input;
    value.value = 0x100;
    best1503_set_adc_ntc((Object *)&s, &value, "ntc", NULL, &error);
    CHECK(error && s.adc_ntc_channels == 0x28 && !memcmp(&before, &s.adc_input, sizeof(before)));
    error_clear(&error);
    value.value = 65536;
    best1503_set_adc_ntc_room((Object *)&s, &value, "room", NULL, &error);
    CHECK(error && s.adc_ntc_room == 10000 && !memcmp(&before, &s.adc_input, sizeof(before)));
    error_clear(&error);
    value.fail = true;
    best1503_set_adc_ntc((Object *)&s, &value, "ntc", NULL, &error);
    CHECK(error && !memcmp(&before, &s.adc_input, sizeof(before)));
    error_clear(&error);
    value.fail = false;
    s.adc_input.version = UINT64_MAX;
    before = s.adc_input;
    value.value = 9000;
    best1503_set_adc_ntc_room((Object *)&s, &value, "room", NULL, &error);
    CHECK(error && s.adc_ntc_room == 10000 && !memcmp(&before, &s.adc_input, sizeof(before)));
    error_clear(&error);
    value.value = 0x68;
    best1503_set_adc_ntc((Object *)&s, &value, "ntc", NULL, &error);
    CHECK(error && s.adc_ntc_channels == 0x28 && !memcmp(&before, &s.adc_input, sizeof(before)));
    error_clear(&error);
    g_mutex_clear(&s.adc_lock);
}

static void charger_edges_and_key_irq_are_independent(void)
{
    Best1503State s;
    start(&s);
    BesIspi *spi = &s.ispi;
    Error *error = NULL;
    best1503_set_battery_charger((Object *)&s, "charging", &error);
    CHECK(!error && s.charger_state == 1 && !s.pmu_charger_armed);
    CHECK(transfer(spi, true, 0x5e, 0) == 0 && irq_level == 0);
    transfer(spi, false, 0x05, 0x1800);
    CHECK(s.pmu_charger_armed && s.pmu_charger_ac_on && irq_level == 1);
    CHECK(transfer(spi, true, 0x5e, 0) == 0x0c);
    transfer(spi, false, 0x41, 0x4000);
    spi->key_pending = 0x8000;
    pmu_update_irq(spi);
    transfer(spi, false, 0x5e, 8); /* Charger W1C leaves key IRQ asserted. */
    CHECK(s.pmu_charger_edges == 0 && spi->key_pending == 0x8000 && irq_level == 1);
    transfer(spi, false, 0x5e, 0x8000);
    CHECK(spi->key_pending == 0 && irq_level == 0);
    CHECK(transfer(spi, true, 0x5e, 0) == 4); /* W1C cannot clear AC_ON. */
    best1503_set_battery_charger((Object *)&s, "full", &error);
    CHECK(!error && s.charger_state == 2 && s.pmu_charger_edges == 0 && irq_level == 0);
    char *text = best1503_get_battery_charger((Object *)&s, &error);
    CHECK(!strcmp(text, "full")); g_free(text);
    best1503_set_battery_charger((Object *)&s, "unplugged", &error);
    CHECK(!error && !s.pmu_charger_ac_on && s.pmu_charger_edges == 0x10 && irq_level == 1);
    spi->key_pending = 0x4000;
    transfer(spi, false, 0x5e, 0x4000); /* Key W1C leaves charger IRQ asserted. */
    CHECK(spi->key_pending == 0 && s.pmu_charger_edges == 0x10 && irq_level == 1);
    transfer(spi, false, 0x5e, 0x10);
    CHECK(s.pmu_charger_edges == 0 && irq_level == 0);
    best1503_set_battery_charger((Object *)&s, "invalid", &error);
    CHECK(error && s.charger_state == 0 && !s.pmu_charger_ac_on); error_clear(&error);
    transfer(spi, false, 0x05, 0); /* Mask edges without discarding their status. */
    best1503_set_battery_charger((Object *)&s, "charging", &error);
    CHECK(!error && s.pmu_charger_ac_on && s.pmu_charger_edges == 8 && irq_level == 0);
    transfer(spi, false, 0x05, 0x0800);
    CHECK(irq_level == 1);
    test_production_ispi_reset(&s);
    CHECK(s.charger_state == 1 && !s.pmu_charger_ac_on && !s.pmu_charger_armed);
    CHECK(!s.pmu_charger_edges && !spi->key_pending && irq_level == 0);
    text = best1503_get_battery_gauge((Object *)&s, &error);
    CHECK(!strcmp(text, "soft")); g_free(text);
    best1503_set_battery_gauge((Object *)&s, "soft", &error);
    CHECK(!error);
    best1503_set_battery_gauge((Object *)&s, "hp4570", &error);
    CHECK(error); error_clear(&error);
    g_mutex_clear(&s.adc_lock);
}

int main(void)
{
    ntc_updates_preserve_sample_provenance();
    charger_edges_and_key_irq_are_independent();
    defaults_and_atomic_input();
    conversion_and_fifo_observation();
    target_channel_and_fixture_boundaries();
    reset_and_restart_boundaries();
    fixture_toggle_does_not_relabel_guest_register_writes();
    printf("P65 ADC production regression: %u checks passed\n", checks);
    return 0;
}
