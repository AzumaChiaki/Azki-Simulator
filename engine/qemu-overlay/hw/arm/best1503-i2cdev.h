/* Simple register-file I2C peripherals. SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Firmware drivers address these chips with the common "write register
 * pointer, then read or write consecutive registers" convention. A device
 * may override individual registers with a hook (e.g. live charger status).
 */
#ifndef BEST1503_I2CDEV_H
#define BEST1503_I2CDEV_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BES_I2C_DEVICES 8

typedef struct BesI2cDevice BesI2cDevice;
struct BesI2cDevice {
    const char *name;
    uint8_t address;               /* 7-bit target */
    uint8_t controller;            /* index of the I2C controller it sits on */
    uint8_t regs[256];
    uint8_t pointer;
    bool expect_pointer;           /* first written byte of a transfer */
    uint64_t reads, writes;
    uint8_t (*read_hook)(BesI2cDevice *dev, uint8_t reg, void *opaque);
    void (*write_hook)(BesI2cDevice *dev, uint8_t reg, uint8_t value, void *opaque);
    void *opaque;
};

static inline void bes_i2c_device_init(BesI2cDevice *dev, const char *name,
                                       uint8_t controller, uint8_t address)
{
    memset(dev, 0, sizeof(*dev));
    dev->name = name;
    dev->controller = controller;
    dev->address = address;
    dev->expect_pointer = true;
}

static inline uint8_t bes_i2c_device_read(BesI2cDevice *dev)
{
    uint8_t reg = dev->pointer++;
    dev->reads++;
    return dev->read_hook ? dev->read_hook(dev, reg, dev->opaque) : dev->regs[reg];
}

static inline void bes_i2c_device_write(BesI2cDevice *dev, uint8_t value)
{
    dev->writes++;
    if (dev->expect_pointer) {
        dev->pointer = value;
        dev->expect_pointer = false;
        return;
    }
    uint8_t reg = dev->pointer++;
    dev->regs[reg] = value;
    if (dev->write_hook) {
        dev->write_hook(dev, reg, value, dev->opaque);
    }
}

static inline void bes_i2c_device_stop(BesI2cDevice *dev)
{
    dev->expect_pointer = true;
}

#endif
