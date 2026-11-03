/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t hwaddr;
typedef int MemoryRegion, qemu_irq, MemTxResult, MemTxAttrs;
typedef struct Best1503State Best1503State;
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define qemu_log_mask(...) ((void)0)
static int irq_level[5];
static void qemu_set_irq(qemu_irq irq, int level) { irq_level[irq] = level; }
typedef struct {
    bool enabled, slave_reading;
    uint16_t pointer;
    uint8_t tx[16];
    unsigned tx_count, read_count;
} BesTouch;
typedef struct { bool reading; uint8_t pointer; unsigned tx_count; } BesCrown;
struct Best1503State { BesTouch touch; BesCrown crown; };
/* Sensor controllers must never borrow the AP's attached touch/crown. */
static bool touch_powered(Best1503State *s) { (void)s; abort(); }
static bool touch_write_packet(Best1503State *s) { (void)s; abort(); }
static bool touch_read_byte(Best1503State *s, unsigned p, uint8_t *b)
{ (void)s; (void)p; (void)b; abort(); }
static bool crown_read_byte(Best1503State *s, uint8_t *b)
{ (void)s; (void)b; abort(); }
static bool crown_write_byte(Best1503State *s, uint64_t b)
{ (void)s; (void)b; abort(); }

#include "q65-sensor-i2c.inc"

static void put(BesI2c *i, uint32_t reg, uint32_t value)
{ assert(i2c_write(i, reg, value, 4, 0) == MEMTX_OK); }
static uint32_t get(BesI2c *i, uint32_t reg)
{
    uint64_t value;
    assert(i2c_read(i, reg, &value, 4, 0) == MEMTX_OK);
    return value;
}

int main(void)
{
    Best1503State machine = {.touch.enabled = true};
    static const hwaddr bases[] = {0x50005000, 0x50006000, 0x5000e000, 0x5000f000};
    BesI2c sensor[4] = {0};
    BesI2c ap = {.base = 0x40063000, .machine = &machine};
    ap.regs[0] = 0xa5;
    ap.pending = 0x100;
    BesI2c original_ap = ap;
    for (unsigned n = 0; n < 4; n++) {
        BesI2c *i = &sensor[n];
        i->machine = &machine; i->base = bases[n]; i->irq = n + 1;
        put(i, 0, 1); /* Master */
        put(i, 4, n & 1 ? 0x74 : 0x5a); /* Same addresses as AP attachments. */
        put(i, 0x30, (1 << 6) | (1 << 9));
        put(i, 0x10, 0x300); /* Disabled controller cannot transact. */
        assert(get(i, 0x80) == 0 && !irq_level[n + 1]);
        put(i, 0x6c, 1);
        assert((get(i, 0x34) & (1 << 4)) && !irq_level[n + 1]);
        put(i, 0x10, 0x300); /* READ|STOP: absent target NACKs address. */
        assert(get(i, 0x80) == 1 && irq_level[n + 1]);
        assert(get(i, 0x2c) == ((1 << 6) | (1 << 9)));
        assert(get(i, 0x78) == 0 && i->rx_count == 0);
        get(i, 0x54); /* Clear TX_ABRT leaves STOP pending. */
        assert(get(i, 0x80) == 0 && irq_level[n + 1]);
        get(i, 0x60);
        assert(!irq_level[n + 1]);
        put(i, 4, (1 << 12) | 0x2a5);
        put(i, 0x10, 0);
        assert(get(i, 0x80) == 2); /* Ten-bit address NACK. */
        get(i, 0x40);
        assert(!irq_level[n + 1]);
        put(i, 0x6c, 3);
        assert(get(i, 0x80) == (1 << 16) && irq_level[n + 1]);
        put(i, 0x6c, 0);
        assert(!irq_level[n + 1] && get(i, 0x80) == 0);
        for (unsigned other = 0; other < n; other++) {
            assert(!sensor[other].pending && !sensor[other].abort_source);
        }
        assert(!memcmp(&ap, &original_ap, sizeof(ap)));
    }
    puts("Q65 sensor I2C: NACK, IRQ clear, absent samples and controller isolation passed");
    return 0;
}
