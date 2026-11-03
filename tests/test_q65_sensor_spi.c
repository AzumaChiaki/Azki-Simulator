/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int MemoryRegion, MemTxResult, MemTxAttrs;
typedef uint64_t hwaddr;
#include "azki-q65-imu.h"
typedef struct { bool spi0_unconnected, gh3x2x_fixture; } Best1503State;
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define qemu_log_mask(...) ((void)0)
#include "q65-spi-state.inc"
static bool gh_transfer(BesSpiConfig *spi, uint32_t value)
{ (void)spi; (void)value; abort(); } /* Sensor bus cannot reach AP GH backend. */
static Q65ImuByte sensor_imu_transfer(BesSpiConfig *spi, uint8_t value)
{ (void)spi; (void)value; return (Q65ImuByte){0xff,0,0}; }
static void sensor_imu_observe(BesSpiConfig *spi, Q65ImuByte value)
{ (void)spi; assert(!value.observation); }
#include "q65-spi-functions.inc"
static void put(BesSpiConfig *s, uint32_t reg, uint32_t value)
{ assert(spi0_write(s, reg, value, 4, 0) == MEMTX_OK); }
static uint32_t get(BesSpiConfig *s, uint32_t reg)
{
    uint64_t value;
    assert(spi0_read(s, reg, &value, 4, 0) == MEMTX_OK);
    return value;
}
int main(void)
{
    Best1503State machine = {.gh3x2x_fixture = true};
    BesSpiConfig spi[2] = {0};
    for (unsigned n = 0; n < 2; n++) {
        BesSpiConfig *s = &spi[n];
        s->machine = &machine; s->unconnected_only = true;
        s->base = 0x50007000 + n * 0x1000;
        put(s, 0, 0x70100); put(s, 4, 2); put(s, 0x10, 2);
        put(s, 0x24, 0); put(s, 0x14, 0);
        assert(get(s, 0xc) == 3 && get(s, 0x1c) == 0);
        for (unsigned cs = 0; cs < 4; cs++) {
            put(s, 4, 2 | cs << 7); put(s, 8, 0x9f);
            assert(get(s, 0xc) == 7 && get(s, 8) == 0xff);
            assert(get(s, 0xc) == 3 && !spi[1 - n].rx_count);
        }
        put(s, 4, 2);
        for (unsigned i = 0; i < 9; i++) { put(s, 8, i); }
        assert(s->rx_count == 8 && s->rx_overrun && get(s, 0xc) == 15);
        assert(get(s, 0x18) & 1);
        for (unsigned i = 0; i < 8; i++) { assert(get(s, 8) == 0xff); }
        assert(get(s, 8) == 0 && s->rx_count == 0);
        assert(get(s, 0xc) == 3); /* Empty reads do not fabricate FIFO data. */
        put(s, 8, 1); /* The exact odd-byte drain used by 0606efc2..efcc. */
        assert(get(s, 0xc) & 4);
        assert(get(s, 8) == 0xff && get(s, 8) == 0);
        assert(!(get(s, 0xc) & 4));
        put(s, 0x20, 1);
        assert(!s->rx_overrun && !(get(s, 0x18) & 1));
        assert(spi0_write(s, 0x14, 1, 4, 0) == MEMTX_ERROR); /* IRQ unsupported. */
        put(s, 0x24, 1);
        assert(spi0_write(s, 8, 1, 4, 0) == MEMTX_ERROR); /* DMA unsupported. */
        put(s, 0x24, 0); put(s, 0, 0xf0100);
        assert(spi0_write(s, 8, 1, 4, 0) == MEMTX_ERROR); /* Not proven 16-bit mode. */
        put(s, 0, 0x701c0); put(s, 4, 0);
        assert(spi0_write(s, 8, 1, 4, 0) == MEMTX_ERROR);
        put(s, 4, 2); put(s, 8, 0xff);
        assert(get(s, 8) == 0xff); /* Supported CPOL/CPHA still open bus. */
        put(s, 8, 0xffffffff); /* HAL receive dummy clocks: low eight bits. */
        assert(get(s, 8) == 0xff && s->rx_count == 0);
        put(s, 0x88, 1); /* RX extension is not chip select. */
        assert(spi0_write(s, 8, 1, 4, 0) == MEMTX_ERROR);
        put(s, 0x88, 0); put(s, 4, 2 | 4 << 7);
        assert(spi0_write(s, 8, 1, 4, 0) == MEMTX_ERROR);
    }
    puts("Q65 sensor SPI: open bus, FIFO, CS, isolation and unsupported modes passed");
    return 0;
}
