/* Board motion/optical sensor models driven by host health input.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The firmware computes steps and heart rate itself from raw sensor data.
 * These models only produce physically plausible raw samples:
 *  - an IMU (accelerometer + gyroscope) whose samples describe a wrist that
 *    is at rest or walking at a host-chosen cadence for a host-chosen number
 *    of steps;
 *  - (later) a PPG front end whose green channel pulses at a host-chosen rate.
 * Which chip is fitted, on which bus, chip-select and interrupt pin, comes
 * from the board ("sensor-board" machine property); nothing here guesses it.
 */
#ifndef BEST1503_SENSORS_H
#define BEST1503_SENSORS_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    BES_IMU_NONE = 0,
    BES_IMU_LSM6DSO,
    BES_IMU_ICM42607,
} BesImuKind;

typedef enum {
    BES_BUS_NONE = 0,
    BES_BUS_SPI0,
    BES_BUS_SPI1,
    BES_BUS_I2C,
} BesSensorBusKind;

#define BES_IMU_FIFO_WORDS 438   /* LSM6DSO: 3 KiB / 7-byte words */

typedef struct BesSensorHub BesSensorHub;

typedef struct {
    BesSensorBusKind bus;
    int cs_pin;                  /* SPI chip select GPIO, active low */
    int irq_pin;                 /* interrupt GPIO, -1 when not wired */
    unsigned i2c_controller, i2c_address;
    bool selected;
    unsigned byte_index;
    uint8_t address;
    bool reading;
} BesSensorPort;

typedef struct {
    BesImuKind kind;
    BesSensorPort port;
    int8_t mount[3];             /* chip axis i = sign * body axis |mount[i]|-1 */
    uint8_t regs[256];
    uint8_t bank[2][256];        /* LSM6DSO embedded-function / sensor-hub pages */
    /* sampling */
    int64_t next_sample_ns;
    int64_t ts_base_ns;
    uint8_t fifo[BES_IMU_FIFO_WORDS][7];
    unsigned fifo_head, fifo_count;
    unsigned fifo_read_pos;      /* byte within the current word, 0..6 */
    uint8_t tag_counter;
    bool fifo_overrun;
    bool irq_level;
    int16_t accel_raw[3], gyro_raw[3];
    bool xl_ready, gy_ready;
    uint64_t samples;
    /* ICM-42607: MREG banks (BLK_SEL 0x00/0x28/0x50 -> 1..3) and byte FIFO */
    uint8_t mreg[4][256];
    uint8_t bfifo[2048];
    unsigned bfifo_head, bfifo_count;
} BesImu;

typedef enum {
    BES_PPG_NONE = 0,
    BES_PPG_HX3695,
    BES_PPG_HX3697,
} BesPpgKind;

#define BES_PPG_FIFO_WORDS 512

typedef struct {
    BesPpgKind kind;
    BesSensorPort port;
    uint8_t regs[256];
    uint8_t slot[8][8];          /* HX3695 0x44..0x4b, banked by 0x43 */
    uint8_t page[4][256];        /* HX3697 register pages, selected by 0x00 */
    uint32_t fifo[BES_PPG_FIFO_WORDS];
    unsigned fifo_head, fifo_count;
    unsigned read_byte;          /* byte within the current FIFO word */
    int64_t next_sample_ns;
    double beat_phase;           /* completed heart beats, fractional */
    double t;                    /* seconds of sampling, for baseline drift */
    unsigned channels;           /* channels pushed per conversion */
    unsigned clock_hz;           /* sample-period tick clock (board option) */
    bool irq_sent;
    uint64_t samples;
} BesPpg;

typedef struct {
    bool (*cs_low)(void *opaque, int pin);
    void (*set_input)(void *opaque, int pin, bool level);
    int64_t (*now_ns)(void *opaque);
    void (*schedule)(void *opaque, int64_t when_ns);
    void (*log)(void *opaque, const char *message);
    void *opaque;
} BesSensorHost;

struct BesSensorHub {
    BesSensorHost host;
    BesImu imu;
    BesPpg ppg;
    /* Host health input. walk_target is the number of steps the wearer walks
     * after boot (cumulative; raising it walks the difference). */
    bool health_enabled;
    uint32_t walk_target;
    uint32_t cadence;            /* steps per minute while walking */
    uint32_t heart_rate;         /* beats per minute, 0 = off wrist */
    double walk_phase;           /* completed steps, fractional */
    double step_rate, step_gain; /* per-step gait variation */
    uint32_t noise;
};

/* ---- motion model ------------------------------------------------------ */

static inline double bes_noise(BesSensorHub *h)
{
    h->noise = h->noise * 1664525u + 1013904223u;
    return ((h->noise >> 8) & 0xffff) / 65536.0 - 0.5;
}

static inline bool bes_walking(BesSensorHub *h)
{
    return h->health_enabled && h->cadence && h->walk_phase < h->walk_target;
}

/* One sample, advanced by dt seconds. Units: accel in g, gyro in dps. The
 * band lies on a left wrist, display up: gravity on +Z. Walking adds a
 * vertical heel-strike pulse once per step and an arm swing every two steps. */
static inline void bes_motion_sample(BesSensorHub *h, double dt, double a[3], double g[3])
{
    /* At rest: forearm level, display up. */
    a[0] = 0.02; a[1] = -0.05; a[2] = 0.99;
    g[0] = g[1] = g[2] = 0;
    if (bes_walking(h)) {
        /* Natural gait varies stride to stride: a new rate and intensity are
         * drawn at each completed step (+-6 % period, +-15 % amplitude). */
        double before = h->walk_phase;
        h->walk_phase += h->cadence / 60.0 * h->step_rate * dt;
        if (floor(h->walk_phase) != floor(before)) {
            h->step_rate = 1 + bes_noise(h) * 0.12;
            h->step_gain = 1 + bes_noise(h) * 0.3;
        }
        if (h->walk_phase > h->walk_target) {
            h->walk_phase = h->walk_target;
        }
        double k = h->step_gain;
        double p = h->walk_phase - floor(h->walk_phase);     /* step phase */
        double swing = M_PI * h->walk_phase;                   /* arm cycle */
        /* Arm hanging and swinging: gravity mostly along the forearm (body
         * Y), a heel-strike jolt each step and a swing every two steps. */
        double tilt = 0.35 * sin(swing);
        double strike = exp(-pow((p - 0.15) / 0.07, 2)) * 0.9 -
                        exp(-pow((p - 0.45) / 0.15, 2)) * 0.35;
        a[0] = 0.25 + 0.30 * sin(swing + 0.4) * k + 0.10 * strike * k;
        a[1] = -0.85 * cos(tilt) + 0.45 * strike * k;
        a[2] = 0.45 * sin(tilt) + 0.25 + 0.30 * strike * k;
        g[0] = 30 * cos(swing + 0.3) * k;
        g[1] = 80 * cos(swing) * k;
        g[2] = 15 * sin(swing) * k;
    }
    for (int i = 0; i < 3; i++) {
        a[i] += bes_noise(h) * 0.02;
        g[i] += bes_noise(h) * 0.6;
    }
}

/* Body-frame motion to the chip's axes, as the board mounts the part. */
static inline void bes_mount(BesImu *m, double v[3])
{
    double b[3] = {v[0], v[1], v[2]};
    for (int i = 0; i < 3; i++) {
        int src = m->mount[i] ? abs(m->mount[i]) - 1 : i;
        v[i] = m->mount[i] < 0 ? -b[src] : b[src];
    }
}

/* ---- LSM6DSO ----------------------------------------------------------- */

#define LSM_FIFO_CTRL1   0x07
#define LSM_FIFO_CTRL2   0x08
#define LSM_FIFO_CTRL3   0x09
#define LSM_FIFO_CTRL4   0x0a
#define LSM_INT1_CTRL    0x0d
#define LSM_WHO_AM_I     0x0f
#define LSM_CTRL1_XL     0x10
#define LSM_CTRL2_G      0x11
#define LSM_CTRL3_C      0x12
#define LSM_STATUS       0x1e
#define LSM_FIFO_STATUS1 0x3a
#define LSM_FIFO_STATUS2 0x3b
#define LSM_TIMESTAMP0   0x40
#define LSM_FIFO_TAG     0x78

static inline double lsm_odr_hz(unsigned code)
{
    static const double hz[16] = {0, 12.5, 26, 52, 104, 208, 416, 833,
                                  1666, 3332, 6667, 1.6, 0, 0, 0, 0};
    return hz[code & 15];
}

static inline void lsm_reset(BesImu *m)
{
    memset(m->regs, 0, sizeof(m->regs));
    memset(m->bank, 0, sizeof(m->bank));
    m->regs[LSM_WHO_AM_I] = 0x6c;
    m->regs[LSM_CTRL3_C] = 0x04;          /* IF_INC */
    m->regs[LSM_FIFO_CTRL1] = 0x00;
    m->fifo_head = m->fifo_count = m->fifo_read_pos = 0;
    m->fifo_overrun = false;
    m->xl_ready = m->gy_ready = false;
    m->next_sample_ns = 0;
}

static inline double lsm_accel_lsb_per_g(BesImu *m)
{
    static const double mg[4] = {0.061, 0.488, 0.122, 0.244};
    return 1000.0 / mg[(m->regs[LSM_CTRL1_XL] >> 2) & 3];
}

static inline double lsm_gyro_lsb_per_dps(BesImu *m)
{
    unsigned fs = (m->regs[LSM_CTRL2_G] >> 2) & 3;
    static const double mdps[4] = {8.75, 17.5, 35, 70};
    if (m->regs[LSM_CTRL2_G] & 2) {
        return 1000.0 / 4.375;            /* 125 dps */
    }
    return 1000.0 / mdps[fs];
}

static inline int16_t bes_sat16(double v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lrint(v);
}

static inline unsigned lsm_wtm(BesImu *m)
{
    return m->regs[LSM_FIFO_CTRL1] | ((m->regs[LSM_FIFO_CTRL2] & 1) << 8);
}

static inline void lsm_fifo_push(BesImu *m, uint8_t tag, const int16_t v[3])
{
    unsigned mode = m->regs[LSM_FIFO_CTRL4] & 7;
    if (!mode) {
        return;
    }
    if (m->fifo_count == BES_IMU_FIFO_WORDS) {
        m->fifo_overrun = true;
        if (mode == 1) {
            return;                       /* FIFO mode stops when full */
        }
        m->fifo_head = (m->fifo_head + 1) % BES_IMU_FIFO_WORDS;
        m->fifo_count--;
        m->fifo_read_pos = 0;
    }
    uint8_t *w = m->fifo[(m->fifo_head + m->fifo_count) % BES_IMU_FIFO_WORDS];
    uint8_t t = (tag << 3) | ((m->tag_counter & 3) << 1);
    t |= __builtin_parity(t);
    w[0] = t;
    for (int i = 0; i < 3; i++) {
        w[1 + i * 2] = v[i] & 0xff;
        w[2 + i * 2] = (uint16_t)v[i] >> 8;
    }
    m->fifo_count++;
}

static inline bool lsm_irq(BesImu *m)
{
    uint8_t c = m->regs[LSM_INT1_CTRL];
    unsigned wtm = lsm_wtm(m);
    bool level = ((c & 0x08) && wtm && m->fifo_count >= wtm) ||
                 ((c & 0x10) && m->fifo_overrun) ||
                 ((c & 0x20) && m->fifo_count == BES_IMU_FIFO_WORDS) ||
                 ((c & 0x01) && m->xl_ready) || ((c & 0x02) && m->gy_ready);
    return (m->regs[LSM_CTRL3_C] & 0x20) ? !level : level;  /* H_LACTIVE */
}

static inline uint8_t lsm_read(BesSensorHub *h, uint8_t reg)
{
    BesImu *m = &h->imu;
    int64_t now = h->host.now_ns(h->host.opaque);
    /* FUNC_CFG_ACCESS (0x01) maps the embedded-function (bit 7) or sensor
     * hub (bit 6) page over 0x02..0x7f; their engines are not used here. */
    if (reg > 1 && (m->regs[1] & 0xc0)) {
        return m->bank[(m->regs[1] & 0x80) ? 0 : 1][reg];
    }
    switch (reg) {
    case LSM_STATUS:
        return (m->xl_ready ? 1 : 0) | (m->gy_ready ? 2 : 0) | 4;
    case 0x20: return 0x00;              /* OUT_TEMP: 25 C */
    case 0x21: return 0x00;
    case 0x22 ... 0x27: {
        int16_t v = m->gyro_raw[(reg - 0x22) / 2];
        if (reg == 0x27) { m->gy_ready = false; }
        return reg & 1 ? (uint16_t)v >> 8 : v & 0xff;
    }
    case 0x28 ... 0x2d: {
        int16_t v = m->accel_raw[(reg - 0x28) / 2];
        if (reg == 0x2d) { m->xl_ready = false; }
        return reg & 1 ? (uint16_t)v >> 8 : v & 0xff;
    }
    case LSM_FIFO_STATUS1:
        return m->fifo_count & 0xff;
    case LSM_FIFO_STATUS2: {
        unsigned wtm = lsm_wtm(m);
        return ((m->fifo_count >> 8) & 3) |
               (wtm && m->fifo_count >= wtm ? 0x80 : 0) |
               (m->fifo_overrun ? 0x40 : 0) |
               (m->fifo_count == BES_IMU_FIFO_WORDS ? 0x20 : 0);
    }
    case LSM_TIMESTAMP0 ... LSM_TIMESTAMP0 + 3: {
        uint32_t ts = (uint32_t)((now - m->ts_base_ns) / 25000);
        return ts >> ((reg - LSM_TIMESTAMP0) * 8);
    }
    case LSM_FIFO_TAG ... LSM_FIFO_TAG + 6: {
        if (!m->fifo_count) {
            return 0;
        }
        uint8_t v = m->fifo[m->fifo_head][reg - LSM_FIFO_TAG];
        if (reg == LSM_FIFO_TAG + 6) {
            m->fifo_head = (m->fifo_head + 1) % BES_IMU_FIFO_WORDS;
            m->fifo_count--;
            m->fifo_overrun = false;
        }
        return v;
    }
    default:
        return m->regs[reg];
    }
}

static inline void bes_imu_update(BesSensorHub *h);

static inline void lsm_write(BesSensorHub *h, uint8_t reg, uint8_t value)
{
    BesImu *m = &h->imu;
    if (reg > 1 && (m->regs[1] & 0xc0)) {
        m->bank[(m->regs[1] & 0x80) ? 0 : 1][reg] = value;
        return;
    }
    switch (reg) {
    case LSM_WHO_AM_I:
    case LSM_STATUS:
    case LSM_FIFO_STATUS1: case LSM_FIFO_STATUS2:
        return;                            /* read-only */
    case LSM_CTRL3_C:
        if (value & 0x81) {                /* BOOT / SW_RESET self-clear */
            lsm_reset(m);
            m->regs[LSM_CTRL3_C] = (value & ~0x81) | 0x04;
            return;
        }
        break;
    case LSM_FIFO_CTRL4:
        if (!(value & 7)) {                /* bypass mode empties the FIFO */
            m->fifo_head = m->fifo_count = 0;
            m->fifo_overrun = false;
        }
        break;
    case LSM_TIMESTAMP0 + 2:               /* TIMESTAMP2 0xAA resets the counter */
        if (value == 0xaa) {
            m->ts_base_ns = h->host.now_ns(h->host.opaque);
        }
        return;
    }
    m->regs[reg] = value;
    if (reg == LSM_CTRL1_XL || reg == LSM_CTRL2_G || reg == LSM_FIFO_CTRL3 ||
        reg == LSM_FIFO_CTRL4) {
        m->next_sample_ns = 0;
    }
}

/* Produce every sample due up to now and reschedule. Samples are generated at
 * the highest active rate; FIFO batching follows each sensor's BDR. */
static inline void lsm_update(BesSensorHub *h)
{
    BesImu *m = &h->imu;
    int64_t now = h->host.now_ns(h->host.opaque);
    double xl = lsm_odr_hz(m->regs[LSM_CTRL1_XL] >> 4);
    double gy = lsm_odr_hz(m->regs[LSM_CTRL2_G] >> 4);
    double rate = xl > gy ? xl : gy;
    if (rate <= 0) {
        m->next_sample_ns = 0;
        return;
    }
    int64_t period = (int64_t)(1e9 / rate);
    if (!m->next_sample_ns || m->next_sample_ns > now + period) {
        m->next_sample_ns = now + period;
    }
    /* Never generate more than one FIFO's worth after a long pause. */
    if (now - m->next_sample_ns > period * BES_IMU_FIFO_WORDS) {
        m->next_sample_ns = now - period * BES_IMU_FIFO_WORDS;
    }
    double bdr_xl = lsm_odr_hz(m->regs[LSM_FIFO_CTRL3] & 15);
    double bdr_gy = lsm_odr_hz(m->regs[LSM_FIFO_CTRL3] >> 4);
    while (m->next_sample_ns <= now) {
        double a[3], g[3];
        bes_motion_sample(h, 1.0 / rate, a, g);
        bes_mount(m, a);
        bes_mount(m, g);
        m->samples++;
        unsigned n = (unsigned)m->samples;
        if (xl > 0) {
            for (int i = 0; i < 3; i++) {
                m->accel_raw[i] = bes_sat16(a[i] * lsm_accel_lsb_per_g(m));
            }
            m->xl_ready = true;
            if (bdr_xl > 0 && (n % (unsigned)fmax(1, lrint(rate / bdr_xl))) == 0) {
                lsm_fifo_push(m, 0x02, m->accel_raw);
            }
        }
        if (gy > 0) {
            for (int i = 0; i < 3; i++) {
                m->gyro_raw[i] = bes_sat16(g[i] * lsm_gyro_lsb_per_dps(m));
            }
            m->gy_ready = true;
            if (bdr_gy > 0 && (n % (unsigned)fmax(1, lrint(rate / bdr_gy))) == 0) {
                lsm_fifo_push(m, 0x01, m->gyro_raw);
            }
        }
        m->tag_counter++;
        m->next_sample_ns += period;
    }
    h->host.schedule(h->host.opaque, m->next_sample_ns);
}

/* ---- ICM-42607-P -------------------------------------------------------- */
/* Bank 0 registers used by the band's driver; sensor data and FIFO are big
 * endian. FIFO records are 16 bytes: header 0x68 (accel, gyro, timestamp),
 * accel and gyro words, temperature, timestamp.
 * MREG1 FIFO_CONFIG5 (0x01) selects which sensors enter the FIFO. */
#define ICM_WHO_AM_I       0x75
#define ICM_PWR_MGMT0      0x1f
#define ICM_GYRO_CONFIG0   0x20
#define ICM_ACCEL_CONFIG0  0x21
#define ICM_FIFO_CONFIG1   0x28
#define ICM_FIFO_CONFIG2   0x29
#define ICM_FIFO_CONFIG3   0x2a
#define ICM_INT_SOURCE0    0x2b
#define ICM_INT_STATUS_DRDY 0x39
#define ICM_INT_STATUS     0x3a
#define ICM_FIFO_COUNTH    0x3d
#define ICM_FIFO_COUNTL    0x3e
#define ICM_FIFO_DATA      0x3f
#define ICM_INTF_CONFIG0   0x35

static inline double icm_odr_hz(unsigned code)
{
    static const double hz[16] = {0, 0, 0, 0, 0, 1600, 800, 400, 200, 100, 50, 25,
                                  12.5, 6.25, 3.125, 1.5625};
    return hz[code & 15];
}

static inline void icm_reset(BesImu *m)
{
    memset(m->regs, 0, sizeof(m->regs));
    memset(m->mreg, 0, sizeof(m->mreg));
    m->regs[0x00] = 0x08;                 /* MCLK_RDY */
    m->regs[ICM_WHO_AM_I] = 0x60;
    m->regs[ICM_GYRO_CONFIG0] = 0x06;
    m->regs[ICM_ACCEL_CONFIG0] = 0x06;
    m->regs[ICM_FIFO_CONFIG1] = 0x01;     /* bypass */
    m->regs[ICM_INTF_CONFIG0] = 0x30;     /* big-endian count and data */
    m->mreg[1][0x01] = 0x20;              /* FIFO_CONFIG5: WM_GT_TH */
    m->bfifo_head = m->bfifo_count = 0;
    m->next_sample_ns = 0;
    m->xl_ready = m->gy_ready = false;
}

static inline bool icm_accel_on(BesImu *m) { return (m->regs[ICM_PWR_MGMT0] & 3) >= 2; }
static inline bool icm_gyro_on(BesImu *m) { return ((m->regs[ICM_PWR_MGMT0] >> 2) & 3) == 3; }

static inline unsigned icm_packet_size(BesImu *m)
{
    uint8_t c5 = m->mreg[1][0x01];
    /* The band's driver parses 16-byte records (header, accel, gyro, temp,
     * timestamp) whatever subset is enabled; unused fields stay zero. */
    return (c5 & 3) ? 16 : 0;
}

static inline unsigned icm_wtm(BesImu *m)
{
    return m->regs[ICM_FIFO_CONFIG2] | ((m->regs[ICM_FIFO_CONFIG3] & 0x0f) << 8);
}

/* FIFO level in records (FIFO_COUNT_FORMAT, INTF_CONFIG0 bit 6) or bytes. */
static inline unsigned icm_fifo_level(BesImu *m)
{
    unsigned size = icm_packet_size(m);
    if ((m->regs[ICM_INTF_CONFIG0] & 0x40) && size) {
        return m->bfifo_count / size;
    }
    return m->bfifo_count;
}

static inline bool icm_wtm_reached(BesImu *m)
{
    unsigned wtm = icm_wtm(m);
    return wtm && icm_fifo_level(m) >= wtm;
}

static inline void icm_push(BesImu *m, uint8_t b)
{
    if (m->bfifo_count == sizeof(m->bfifo)) {
        m->bfifo_head = (m->bfifo_head + 1) % sizeof(m->bfifo);
        m->bfifo_count--;
    }
    m->bfifo[(m->bfifo_head + m->bfifo_count) % sizeof(m->bfifo)] = b;
    m->bfifo_count++;
}

/* INTF_CONFIG0 bit 4 (SENSOR_DATA_ENDIAN): 1 = big endian. */
static inline void icm_push16(BesImu *m, int16_t v)
{
    bool big = m->regs[ICM_INTF_CONFIG0] & 0x10;
    icm_push(m, big ? (uint16_t)v >> 8 : v & 0xff);
    icm_push(m, big ? v & 0xff : (uint16_t)v >> 8);
}

static inline bool icm_irq(BesImu *m)
{
    if (!(m->regs[0x06] & 0x04)) {       /* pulse mode: idle level only */
        return !(m->regs[0x06] & 0x01);
    }
    uint8_t src = m->regs[ICM_INT_SOURCE0];
    bool level = ((src & 0x04) && (m->regs[ICM_INT_STATUS] & 0x04)) ||
                 ((src & 0x02) && (m->regs[ICM_INT_STATUS] & 0x02)) ||
                 ((src & 0x08) && (m->regs[ICM_INT_STATUS_DRDY] & 0x01));
    bool active_high = m->regs[0x06] & 0x01;   /* INT_CONFIG INT1_POLARITY */
    return active_high ? level : !level;
}

static inline void icm_update(BesSensorHub *h)
{
    BesImu *m = &h->imu;
    int64_t now = h->host.now_ns(h->host.opaque);
    double xl = icm_accel_on(m) ? icm_odr_hz(m->regs[ICM_ACCEL_CONFIG0]) : 0;
    double gy = icm_gyro_on(m) ? icm_odr_hz(m->regs[ICM_GYRO_CONFIG0]) : 0;
    double rate = xl > gy ? xl : gy;
    if (rate <= 0) {
        m->next_sample_ns = 0;
        return;
    }
    int64_t period = (int64_t)(1e9 / rate);
    if (!m->next_sample_ns || m->next_sample_ns > now + period) {
        m->next_sample_ns = now + period;
    }
    if (now - m->next_sample_ns > period * 128) {
        m->next_sample_ns = now - period * 128;
    }
    static const double g_lsb[4] = {2048, 4096, 8192, 16384};
    static const double dps_lsb[4] = {16.4, 32.8, 65.5, 131};
    while (m->next_sample_ns <= now) {
        double a[3], g[3];
        bes_motion_sample(h, 1.0 / rate, a, g);
        bes_mount(m, a);
        bes_mount(m, g);
        m->samples++;
        double ga = g_lsb[(m->regs[ICM_ACCEL_CONFIG0] >> 5) & 3];
        double gg = dps_lsb[(m->regs[ICM_GYRO_CONFIG0] >> 5) & 3];
        for (int i = 0; i < 3; i++) {
            m->accel_raw[i] = xl > 0 ? bes_sat16(a[i] * ga) : 0;
            m->gyro_raw[i] = gy > 0 ? bes_sat16(g[i] * gg) : 0;
        }
        m->xl_ready = true;
        m->regs[ICM_INT_STATUS_DRDY] |= 1;
        unsigned size = icm_packet_size(m);
        if (!(m->regs[ICM_FIFO_CONFIG1] & 1) && size) {
            bool fa = m->mreg[1][0x01] & 1, fg = m->mreg[1][0x01] & 2;
            if ((m->regs[ICM_FIFO_CONFIG1] & 2) && m->bfifo_count + size > sizeof(m->bfifo)) {
                m->regs[ICM_INT_STATUS] |= 0x02;      /* stop-on-full */
            } else {
                uint16_t ts = (uint16_t)(now / 16000);
                /* Always the 16-byte accel+gyro record: the driver advances
                 * by the header's fields and reads 16 bytes per record, so a
                 * disabled sensor's words are sent as zero. */
                icm_push(m, 0x68);
                for (int i = 0; i < 3; i++) { icm_push16(m, fa ? m->accel_raw[i] : 0); }
                for (int i = 0; i < 3; i++) { icm_push16(m, fg ? m->gyro_raw[i] : 0); }
                icm_push(m, 0);                       /* temperature: 25 C */
                icm_push16(m, ts);
            }
            if (icm_wtm_reached(m)) {
                m->regs[ICM_INT_STATUS] |= 0x04;
                /* INT1 pulse mode (INT_CONFIG bit 2 clear): one pulse when
                 * the level reaches the threshold and again for every
                 * further threshold's worth left unread. */
                unsigned wtm = icm_wtm(m), level = icm_fifo_level(m);
                if (!(m->regs[0x06] & 0x04) && (m->regs[ICM_INT_SOURCE0] & 0x04) &&
                    (level - wtm) % wtm == 0 && m->port.irq_pin >= 0) {
                    bool high = m->regs[0x06] & 0x01;
                    h->host.set_input(h->host.opaque, m->port.irq_pin, high);
                    h->host.set_input(h->host.opaque, m->port.irq_pin, !high);
                }
            }
        }
        m->next_sample_ns += period;
    }
    h->host.schedule(h->host.opaque, m->next_sample_ns);
}

static inline uint8_t icm_read(BesSensorHub *h, uint8_t reg)
{
    BesImu *m = &h->imu;
    switch (reg) {
    case 0x09: return 0;                  /* TEMP_DATA: 25 C */
    case 0x0a: return 0;
    case 0x0b ... 0x16: {
        unsigned i = (reg - 0x0b) / 2;
        int16_t v = i < 3 ? m->accel_raw[i] : m->gyro_raw[i - 3];
        bool first = !((reg - 0x0b) & 1), big = m->regs[ICM_INTF_CONFIG0] & 0x10;
        return first == big ? (uint16_t)v >> 8 : v & 0xff;
    }
    case ICM_INT_STATUS_DRDY: {
        uint8_t v = m->regs[reg];
        m->regs[reg] = 0;                 /* clear on read */
        return v;
    }
    case ICM_INT_STATUS: {
        /* Clear on read, but the threshold status reflects the FIFO level
         * at the time of the read. */
        uint8_t v = m->regs[reg] | (icm_wtm_reached(m) ? 0x04 : 0);
        m->regs[reg] = 0;
        return v;
    }
    case ICM_FIFO_COUNTH: case ICM_FIFO_COUNTL: {
        unsigned level = icm_fifo_level(m);
        bool little = !(m->regs[ICM_INTF_CONFIG0] & 0x20);
        bool high = (reg == ICM_FIFO_COUNTH) != little;
        return high ? level >> 8 : level & 0xff;
    }
    case ICM_FIFO_DATA:
        if (!m->bfifo_count) {
            return 0xff;
        }
        {
            uint8_t v = m->bfifo[m->bfifo_head];
            m->bfifo_head = (m->bfifo_head + 1) % sizeof(m->bfifo);
            m->bfifo_count--;
            return v;
        }
    case 0x7e: {                          /* M_R: MREG read */
        unsigned bank = m->regs[0x7c] == 0x00 ? 1 : m->regs[0x7c] == 0x28 ? 2 :
                        m->regs[0x7c] == 0x50 ? 3 : 0;
        return bank ? m->mreg[bank][m->regs[0x7d]] : 0;
    }
    default:
        return m->regs[reg];
    }
}

static inline void icm_write(BesSensorHub *h, uint8_t reg, uint8_t value)
{
    BesImu *m = &h->imu;
    switch (reg) {
    case ICM_WHO_AM_I: case 0x00:
    case ICM_INT_STATUS_DRDY: case ICM_INT_STATUS:
    case ICM_FIFO_COUNTH: case ICM_FIFO_COUNTL: case ICM_FIFO_DATA:
        return;
    case 0x02:                            /* SIGNAL_PATH_RESET */
        if (value & 0x10) {
            icm_reset(m);
            return;
        }
        if (value & 0x04) {
            m->bfifo_head = m->bfifo_count = 0;
            m->regs[ICM_INT_STATUS] = 0;
        }
        return;
    case 0x7b: {                          /* M_W: MREG write */
        unsigned bank = m->regs[0x79] == 0x00 ? 1 : m->regs[0x79] == 0x28 ? 2 :
                        m->regs[0x79] == 0x50 ? 3 : 0;
        if (bank) {
            m->mreg[bank][m->regs[0x7a]] = value;
        }
        return;
    }
    }
    m->regs[reg] = value;
    if (reg == ICM_FIFO_CONFIG1 && (value & 1)) {
        m->bfifo_head = m->bfifo_count = 0;
    }
    if (reg == ICM_PWR_MGMT0 || reg == ICM_ACCEL_CONFIG0 || reg == ICM_GYRO_CONFIG0) {
        m->next_sample_ns = 0;
    }
}

/* ---- PPG front end ------------------------------------------------------ */

/* TYHX HX3695, as driven by the band's hx3695 driver:
 *  0x00 chip id 0x65; 0x1b bit 4 runs the sampler, bit 7 enables the FIFO
 *  (clearing it drops the FIFO), bit 0 stores one ambient-subtracted word
 *  per channel (tag = channel; Band 9 / Band 10 drivers write 0x91), else
 *  the ambient and LED phase words of each channel (tags 2ch and 2ch+1;
 *  the Band 10 Pro driver writes 0x90 and rebuilds
 *  16 * (LED - ambient + 13000 * offset));
 *  0x38 = 1 selects a per-conversion interrupt instead of the FIFO;
 *  0x1c/0x1d FIFO watermark in words; 0x20/0x21[0] FIFO level in words;
 *  0x22 FIFO data port, 3 bytes per word, little endian: bits 0..20 signed
 *  sample, bits 21..23 channel; 0x26..0x29 channel 0..3 config, phase bits
 *  0x88 enable the channel; 0x2a..0x2c sample period in 1/30400 s ticks
 *  (the driver writes 32000/Hz*19/20); 0x43 selects the 0x44..0x4b slot bank.
 * The interrupt output pulses high (the driver configures a rising edge).
 * Channel 0 is the green pulse channel the heart-rate algorithm reads;
 * channel 1 is the reflectance the wear detection reads. */
static inline void ppg_reset(BesPpg *p)
{
    memset(p->regs, 0, sizeof(p->regs));
    memset(p->slot, 0, sizeof(p->slot));
    memset(p->page, 0, sizeof(p->page));
    p->fifo_head = p->fifo_count = p->read_byte = 0;
    p->next_sample_ns = 0;
    p->irq_sent = false;
    if (p->kind == BES_PPG_HX3695) {
        p->regs[0x00] = 0x65;
    }
}

static inline unsigned ppg_wtm(BesPpg *p)
{
    return p->regs[0x1c] | ((p->regs[0x1d] & 1) << 8);
}

/* Converted words enter the FIFO while it is enabled (0x1b bit 7) and the
 * interrupt is not in per-conversion data-ready mode (0x38 = 1, used by LED
 * calibration, which reads the conversion registers instead). */
static inline bool ppg_fifo_on(BesPpg *p)
{
    return (p->regs[0x1b] & 0x80) && p->regs[0x38] != 1;
}

static inline void ppg_pulse(BesSensorHub *h)
{
    int pin = h->ppg.port.irq_pin;
    if (pin >= 0) {
        h->host.set_input(h->host.opaque, pin, true);
        h->host.set_input(h->host.opaque, pin, false);
    }
}

/* Photodiode signal of channel ch in ADC counts above ambient: reflectance
 * times the channel's LED current code (slot 2ch+1 register 0x45). On a
 * wrist a channel whose LED-phase slot drives the green LED (register 0x44
 * bit 1: Band 9 0x42, Band 10 0x43) carries a pulse at heart_rate, one
 * driving only the infrared LED (bit 0) a weak pulse. */
static inline double ppg_photo(BesSensorHub *h, unsigned ch)
{
    BesPpg *p = &h->ppg;
    static const double reflect[4] = {3000, 2500, 1800, 2200};
    bool worn = h->health_enabled && h->heart_rate;
    double current = p->slot[(2 * ch + 1) & 7][1];
    if (!worn) {
        return current * 15 + bes_noise(h) * 200;      /* light leak only */
    }
    double v = reflect[ch] * current;
    unsigned led = p->slot[(2 * ch + 1) & 7][0] & 3;
    double q = p->beat_phase - floor(p->beat_phase);
    if (led & 2) {
        double pulse = exp(-pow((q - 0.15) / 0.07, 2)) +
                       0.35 * exp(-pow((q - 0.45) / 0.09, 2));
        v *= 1 - 0.012 * pulse + 0.003 * sin(2 * M_PI * p->t / 9.0);
    } else if (led & 1) {
        v *= 1 - 0.004 * exp(-pow((q - 0.15) / 0.07, 2));
    }
    return v + bes_noise(h) * 60;
}

/* Photo signal after the channel's offset DAC (slot register 0x48), in
 * conversion-register counts: 1250 per offset code, which is how the LED
 * calibration steers the LED-phase register into its 450k..600k window. */
static inline double ppg_after_offset(BesSensorHub *h, unsigned ch, double photo)
{
    return photo - 1250.0 * h->ppg.slot[(2 * ch + 1) & 7][4];
}

/* FIFO words carry the same signal at 5x scale, so the driver's
 * reconstruction 16 * (word + 6250 * offset) is independent of the offset. */
static inline int32_t ppg_value(BesSensorHub *h, unsigned ch, double photo)
{
    return (int32_t)(5 * ppg_after_offset(h, ch, photo));
}

/* Phase words: the ambient phase reads a small dark level, the LED phase
 * that level plus the signal at the scale the raw-phase driver expects. */
#define BES_PPG_AMBIENT_WORD 200
static inline int32_t ppg_led_word(BesSensorHub *h, unsigned ch, double photo)
{
    return BES_PPG_AMBIENT_WORD + (int32_t)(10.4 * ppg_after_offset(h, ch, photo));
}

static inline void ppg_put24(uint8_t *r, uint32_t v)
{
    r[0] = v; r[1] = v >> 8; r[2] = v >> 16;
}

static inline void ppg_push(BesPpg *p, unsigned tag, int32_t v)
{
    if (p->fifo_count == BES_PPG_FIFO_WORDS) {
        return;                           /* full: newest samples are lost */
    }
    if (v > 0xfffff) { v = 0xfffff; }
    if (v < -0x100000) { v = -0x100000; }
    p->fifo[(p->fifo_head + p->fifo_count) % BES_PPG_FIFO_WORDS] =
        ((uint32_t)v & 0x1fffff) | (tag << 21);
    p->fifo_count++;
}

static inline void hx7_update(BesSensorHub *h);
static inline void ppg_update(BesSensorHub *h)
{
    BesPpg *p = &h->ppg;
    if (p->kind == BES_PPG_HX3697) {
        hx7_update(h);
        return;
    }
    if (p->kind != BES_PPG_HX3695) {
        return;
    }
    unsigned ticks = p->regs[0x2a] | p->regs[0x2b] << 8 | p->regs[0x2c] << 16;
    if (!(p->regs[0x1b] & 0x10) || ticks < 30) {
        p->next_sample_ns = 0;
        return;
    }
    int64_t now = h->host.now_ns(h->host.opaque);
    int64_t period = (int64_t)ticks * 1000000000LL / (p->clock_hz ? p->clock_hz : 30400);
    if (!p->next_sample_ns || p->next_sample_ns > now + period) {
        p->next_sample_ns = now + period;
    }
    if (now - p->next_sample_ns > period * BES_PPG_FIFO_WORDS) {
        p->next_sample_ns = now - period * BES_PPG_FIFO_WORDS;
    }
    while (p->next_sample_ns <= now) {
        double dt = period / 1e9;
        p->t += dt;
        p->beat_phase += h->heart_rate / 60.0 * dt;
        p->channels = 0;
        for (unsigned ch = 0; ch < 4; ch++) {
            double photo = ppg_photo(h, ch);
            int32_t v = ppg_value(h, ch, photo);
            /* Latest conversion registers: ambient phase, then LED phase,
             * offset binary around 0x80000. */
            double led = 0x80000 + 2000 + ppg_after_offset(h, ch, photo);
            led = led < 0 ? 0 : led;
            ppg_put24(&p->regs[0x03 + 6 * ch], 0x80000 + 2000);
            ppg_put24(&p->regs[0x06 + 6 * ch], led > 0xffffff ? 0xffffff : (uint32_t)led);
            if (ppg_fifo_on(p) && (p->regs[0x26 + ch] & 0x88) == 0x88) {
                if (p->regs[0x1b] & 1) {
                    ppg_push(p, ch, v);
                } else {
                    ppg_push(p, 2 * ch, BES_PPG_AMBIENT_WORD);
                    ppg_push(p, 2 * ch + 1, ppg_led_word(h, ch, photo));
                }
                p->channels++;
            }
        }
        p->samples++;
        /* INT pulses: every conversion in data-ready mode (0x38 = 1, used
         * by LED calibration), else once the FIFO level reaches the
         * watermark, again after each FIFO read. */
        unsigned wtm = ppg_wtm(p);
        if (p->regs[0x38] == 1) {
            ppg_pulse(h);
        } else if (ppg_fifo_on(p) && wtm && p->fifo_count >= wtm && !p->irq_sent) {
            p->irq_sent = true;           /* re-armed by the next FIFO read */
            ppg_pulse(h);
        }
        p->next_sample_ns += period;
    }
    h->host.schedule(h->host.opaque, p->next_sample_ns);
}

static inline uint8_t hx7_read(BesSensorHub *h, uint8_t reg);
static inline void hx7_write(BesSensorHub *h, uint8_t reg, uint8_t value);

static inline uint8_t ppg_read(BesSensorHub *h, uint8_t reg)
{
    if (h->ppg.kind == BES_PPG_HX3697) {
        return hx7_read(h, reg);
    }
    BesPpg *p = &h->ppg;
    uint8_t v;
    ppg_update(h);
    if (reg >= 0x44 && reg <= 0x4b) {
        v = p->slot[p->regs[0x43] & 7][reg - 0x44];
    } else if (reg == 0x20) {
        v = p->fifo_count & 0xff;
    } else if (reg == 0x21) {
        v = (p->fifo_count >> 8) & 1;
    } else if (reg == 0x22) {
        v = 0;
        p->irq_sent = false;
        if (p->fifo_count) {
            v = p->fifo[p->fifo_head] >> (8 * p->read_byte);
            if (++p->read_byte == 3) {
                p->read_byte = 0;
                p->fifo_head = (p->fifo_head + 1) % BES_PPG_FIFO_WORDS;
                p->fifo_count--;
            }
        }
    } else {
        v = p->regs[reg];
    }
    return v;
}

static inline void ppg_write(BesSensorHub *h, uint8_t reg, uint8_t value)
{
    if (h->ppg.kind == BES_PPG_HX3697) {
        hx7_write(h, reg, value);
        return;
    }
    BesPpg *p = &h->ppg;
    ppg_update(h);
    if (reg >= 0x44 && reg <= 0x4b) {
        p->slot[p->regs[0x43] & 7][reg - 0x44] = value;
    } else if (reg == 0x1b) {
        if (!(value & 0x80)) {
            p->fifo_head = p->fifo_count = p->read_byte = 0;
            p->irq_sent = false;
        }
        p->regs[reg] = value;
        p->next_sample_ns = 0;
    } else if (reg && !(reg >= 0x03 && reg <= 0x1a) && reg != 0x20 && reg != 0x21 &&
               reg != 0x22) {
        p->regs[reg] = value;
    }
    ppg_update(h);
}

/* Address auto-increment, except on the FIFO data port. */
static inline uint8_t ppg_next(BesPpg *p, uint8_t reg)
{
    if (p->kind == BES_PPG_HX3697) {
        return reg == 0x6a ? reg : reg + 1;
    }
    return reg == 0x22 ? reg : reg + 1;
}

/* ---- HX3697 ------------------------------------------------------------- */

/* TYHX HX3697, as driven by the Band 11 hx3697 driver (Q66 4.200.078) over
 * SPI with the TYHX write flag (address bit 7):
 *  0x00 selects the register page for every other address (pages 0..2);
 *  page 0 0x01/0x02 read the chip id 0x69/0x19;
 *  page 0 0x34 | (0x35 & 3) << 8 enables receive slots: slot k is channel
 *  k & 7, its second receiver when k >= 8 (Q66 heart rate: 0x343 = channels
 *  0 and 1 with both receivers plus channel 6, the wear infrared channel);
 *  page 0 0x5a..0x5c sample period in ticks of a 32.192 kHz clock (the
 *  driver writes 32000 / rate * 1.006);
 *  page 0 0x68 | (0x69 & 3) << 8 FIFO level in words, 0x6a FIFO data port
 *  (no auto-increment): 3 bytes little endian, bits 0..19 signed sample,
 *  bits 20..23 the slot. The first word after a FIFO reset (page 1 0x08
 *  bit 7 cleared) is a pipeline word the driver discards;
 *  page 1 0x0b | (0x0c & 3) << 8 FIFO watermark in words; the interrupt
 *  pulses once the level reaches it and page 0 0x70 reads (and clears)
 *  bit 0 for that FIFO interrupt;
 *  page 1 0x10 + 2 * channel LED current code (init 80), 0x3a + slot offset
 *  DAC code; page 0 0x04 + 6 * channel / 0x07 + 6 * channel the latest LED
 *  and ambient conversions, read by the driver's LED tuning.
 * The driver's gain control keeps a slot between 0 and 50000 counts by
 * stepping the offset DAC 10 / 5 / 2 codes for errors above 230000 /
 * 100000 / 50000 counts, so the signal here is reflectance times LED
 * current minus 20000 counts per offset code. The first receivers of the
 * green channels 0 and 1 carry the pulse, the other slots a weak one (the
 * heart-rate algorithm locks on the pulse only this way). */
#define BES_HX7_CLOCK_HZ 32192

static inline unsigned hx7_slots(BesPpg *p)
{
    return p->page[0][0x34] | (p->page[0][0x35] & 3) << 8;
}

static inline int32_t hx7_value(BesSensorHub *h, unsigned slot)
{
    BesPpg *p = &h->ppg;
    unsigned ch = slot & 7;
    double current = p->page[1][0x10 + 2 * ch];
    double q = p->beat_phase - floor(p->beat_phase);
    double v;
    if (!(h->health_enabled && h->heart_rate)) {
        v = current * 15;                         /* light leak only */
    } else if (slot <= 1) {
        double pulse = exp(-pow((q - 0.15) / 0.07, 2)) +
                       0.35 * exp(-pow((q - 0.45) / 0.09, 2));
        v = 1200 * current * (1 - 0.012 * pulse + 0.003 * sin(2 * M_PI * p->t / 9.0));
    } else {
        v = (ch == 6 ? 20000 : 1200) * current *
            (1 - 0.004 * exp(-pow((q - 0.15) / 0.07, 2)));
    }
    v += bes_noise(h) * 60 - 20000.0 * p->page[1][0x3a + slot];
    return (int32_t)MAX(-0x80000, MIN(0x7ffff, v));
}

static inline void hx7_update(BesSensorHub *h)
{
    BesPpg *p = &h->ppg;
    unsigned slots = hx7_slots(p);
    unsigned ticks = p->page[0][0x5a] | p->page[0][0x5b] << 8 | p->page[0][0x5c] << 16;
    if (!slots || ticks < 30) {
        p->next_sample_ns = 0;
        return;
    }
    int64_t now = h->host.now_ns(h->host.opaque);
    int64_t period = (int64_t)ticks * 1000000000LL /
                     (p->clock_hz ? p->clock_hz : BES_HX7_CLOCK_HZ);
    if (!p->next_sample_ns || p->next_sample_ns > now + period) {
        p->next_sample_ns = now + period;
    }
    if (now - p->next_sample_ns > period * BES_PPG_FIFO_WORDS) {
        p->next_sample_ns = now - period * BES_PPG_FIFO_WORDS;
    }
    while (p->next_sample_ns <= now) {
        double dt = period / 1e9;
        p->t += dt;
        p->beat_phase += h->heart_rate / 60.0 * dt;
        for (unsigned slot = 0; slot < 10; slot++) {
            if (!(slots & (1u << slot))) {
                continue;
            }
            int32_t v = hx7_value(h, slot);
            if (p->fifo_count < BES_PPG_FIFO_WORDS) {
                p->fifo[(p->fifo_head + p->fifo_count++) % BES_PPG_FIFO_WORDS] =
                    ((uint32_t)v & 0xfffff) | (slot << 20);
            }
            if (slot < 8) {
                ppg_put24(&p->page[0][0x04 + 6 * slot], (uint32_t)v & 0xffffff);
                ppg_put24(&p->page[0][0x07 + 6 * slot], BES_PPG_AMBIENT_WORD);
            }
        }
        p->samples++;
        unsigned wtm = p->page[1][0x0b] | (p->page[1][0x0c] & 3) << 8;
        if (wtm && p->fifo_count >= wtm && !p->irq_sent) {
            p->irq_sent = true;               /* re-armed by a status or FIFO read */
            p->page[0][0x70] |= 1;
            ppg_pulse(h);
        }
        p->next_sample_ns += period;
    }
    h->host.schedule(h->host.opaque, p->next_sample_ns);
}

static inline uint8_t hx7_read(BesSensorHub *h, uint8_t reg)
{
    BesPpg *p = &h->ppg;
    unsigned page = p->page[0][0] & 3;
    hx7_update(h);
    if (reg == 0) {
        return p->page[0][0];
    }
    if (page != 0) {
        return p->page[page][reg];
    }
    switch (reg) {
    case 0x01: return 0x69;
    case 0x02: return 0x19;
    case 0x68: return p->fifo_count & 0xff;
    case 0x69: return (p->fifo_count >> 8) & 3;
    case 0x70: {
        uint8_t v = p->page[0][0x70];
        p->page[0][0x70] = 0;
        p->irq_sent = false;
        return v;
    }
    case 0x6a: {
        uint8_t v = 0;
        p->irq_sent = false;
        if (p->read_byte < 3) {
            p->read_byte++;                   /* pipeline word after a reset */
        } else if (p->fifo_count) {
            v = p->fifo[p->fifo_head] >> (8 * (p->read_byte - 3));
            if (++p->read_byte == 6) {
                p->read_byte = 3;
                p->fifo_head = (p->fifo_head + 1) % BES_PPG_FIFO_WORDS;
                p->fifo_count--;
            }
        }
        return v;
    }
    default:
        return p->page[0][reg];
    }
}

static inline void hx7_write(BesSensorHub *h, uint8_t reg, uint8_t value)
{
    BesPpg *p = &h->ppg;
    unsigned page = p->page[0][0] & 3;
    hx7_update(h);
    if (reg == 0) {
        p->page[0][0] = value;
        return;
    }
    if (page == 0 && (reg == 0x01 || reg == 0x02 || reg == 0x68 || reg == 0x69 ||
                      reg == 0x6a || reg == 0x70)) {
        return;                               /* read-only */
    }
    if (page == 1 && reg == 0x08 && !(value & 0x80)) {
        p->fifo_head = p->fifo_count = p->read_byte = 0;
        p->irq_sent = false;
    }
    p->page[page][reg] = value;
    if (page == 0 && (reg == 0x34 || reg == 0x35 || (reg >= 0x5a && reg <= 0x5c))) {
        p->next_sample_ns = 0;
    }
    hx7_update(h);
}

/* ---- dispatch ---------------------------------------------------------- */

static inline void bes_imu_irq_update(BesSensorHub *h)
{
    BesImu *m = &h->imu;
    bool level = false;
    if (m->kind == BES_IMU_LSM6DSO) {
        level = lsm_irq(m);
    } else if (m->kind == BES_IMU_ICM42607) {
        level = icm_irq(m);
    }
    if (m->port.irq_pin >= 0 && level != m->irq_level) {
        m->irq_level = level;
        h->host.set_input(h->host.opaque, m->port.irq_pin, level);
    }
}

static inline void bes_imu_update(BesSensorHub *h)
{
    if (h->imu.kind == BES_IMU_LSM6DSO) {
        lsm_update(h);
    } else if (h->imu.kind == BES_IMU_ICM42607) {
        icm_update(h);
    }
    bes_imu_irq_update(h);
}

static inline uint8_t bes_imu_read(BesSensorHub *h, uint8_t reg)
{
    bes_imu_update(h);
    uint8_t v = h->imu.kind == BES_IMU_LSM6DSO ? lsm_read(h, reg) :
                h->imu.kind == BES_IMU_ICM42607 ? icm_read(h, reg) : 0xff;
    bes_imu_irq_update(h);
    return v;
}

static inline void bes_imu_write(BesSensorHub *h, uint8_t reg, uint8_t value)
{
    bes_imu_update(h);
    if (h->imu.kind == BES_IMU_LSM6DSO) {
        lsm_write(h, reg, value);
    } else if (h->imu.kind == BES_IMU_ICM42607) {
        icm_write(h, reg, value);
    }
    bes_imu_update(h);
}

/* LSM6DSO register auto-increment; the FIFO output window wraps 0x7e->0x78. */
static inline uint8_t bes_imu_next(BesImu *m, uint8_t reg)
{
    if (m->kind == BES_IMU_LSM6DSO) {
        if (reg == LSM_FIFO_TAG + 6) {
            return LSM_FIFO_TAG;
        }
        return (m->regs[LSM_CTRL3_C] & 0x04) ? reg + 1 : reg;
    }
    if (m->kind == BES_IMU_ICM42607 && (reg == ICM_FIFO_DATA || reg == 0x7e)) {
        return reg;                       /* FIFO and MREG ports do not advance */
    }
    return reg + 1;
}

/* Register access through a port: device 0 = IMU, 1 = PPG. */
static inline uint8_t bes_port_read(BesSensorHub *h, BesSensorPort *port)
{
    uint8_t v = port == &h->imu.port ? bes_imu_read(h, port->address) :
                ppg_read(h, port->address);
    port->address = port == &h->imu.port ? bes_imu_next(&h->imu, port->address)
                                         : ppg_next(&h->ppg, port->address);
    return v;
}

static inline void bes_port_write(BesSensorHub *h, BesSensorPort *port, uint8_t value)
{
    if (port == &h->imu.port) {
        bes_imu_write(h, port->address, value);
        port->address = bes_imu_next(&h->imu, port->address);
    } else {
        ppg_write(h, port->address, value);
        port->address = ppg_next(&h->ppg, port->address);
    }
}

static inline BesSensorPort *bes_port(BesSensorHub *h, int i)
{
    return i ? (h->ppg.kind ? &h->ppg.port : NULL) : (h->imu.kind ? &h->imu.port : NULL);
}

static inline bool bes_sensors_on_spi(BesSensorHub *h, unsigned spi)
{
    BesSensorBusKind bus = spi ? BES_BUS_SPI1 : BES_BUS_SPI0;
    return (h->imu.kind && h->imu.port.bus == bus) ||
           (h->ppg.kind && h->ppg.port.bus == bus);
}

/* Chip-select edges from GPIO writes. */
static inline void bes_sensors_pins(BesSensorHub *h)
{
    for (int i = 0; i < 2; i++) {
        BesSensorPort *port = bes_port(h, i);
        if (port && (port->bus == BES_BUS_SPI0 || port->bus == BES_BUS_SPI1)) {
            bool sel = h->host.cs_low(h->host.opaque, port->cs_pin);
            if (sel != port->selected) {
                port->selected = sel;
                port->byte_index = 0;
            }
        }
    }
}

/* One SPI0 byte. Returns true when a modeled device is selected. The first
 * byte is the register address; its bit 7 is the read flag for the IMUs and
 * the write flag for the TYHX PPG parts. */
static inline bool bes_sensors_spi(BesSensorHub *h, unsigned spi, uint8_t tx, uint8_t *rx)
{
    bes_sensors_pins(h);
    for (int i = 0; i < 2; i++) {
        BesSensorPort *port = bes_port(h, i);
        if (!port || port->bus != (spi ? BES_BUS_SPI1 : BES_BUS_SPI0) || !port->selected) {
            continue;
        }
        if (port->byte_index++ == 0) {
            port->reading = i ? !(tx & 0x80) : !!(tx & 0x80);
            port->address = tx & 0x7f;
            *rx = 0;
        } else if (port->reading) {
            *rx = bes_port_read(h, port);
        } else {
            bes_port_write(h, port, tx);
            *rx = 0;
        }
        return true;
    }
    return false;
}

/* I2C register devices: first written byte is the register pointer. */
static inline BesSensorPort *bes_sensors_i2c_port(BesSensorHub *h, unsigned controller,
                                                  unsigned address)
{
    for (int i = 0; i < 2; i++) {
        BesSensorPort *port = bes_port(h, i);
        if (port && port->bus == BES_BUS_I2C && port->i2c_controller == controller &&
            port->i2c_address == address) {
            return port;
        }
    }
    return NULL;
}

static inline void bes_sensors_i2c_write(BesSensorHub *h, BesSensorPort *port, uint8_t value)
{
    if (port->byte_index++ == 0) {
        port->address = value;
    } else {
        bes_port_write(h, port, value);
    }
}

static inline uint8_t bes_sensors_i2c_read(BesSensorHub *h, BesSensorPort *port)
{
    port->byte_index = 1;
    return bes_port_read(h, port);
}

static inline void bes_sensors_i2c_stop(BesSensorHub *h, BesSensorPort *port)
{
    port->byte_index = 0;
}

static inline void bes_sensors_timer(BesSensorHub *h)
{
    bes_imu_update(h);
    ppg_update(h);
}

static inline void bes_sensors_reset(BesSensorHub *h)
{
    if (h->imu.kind == BES_IMU_LSM6DSO) {
        lsm_reset(&h->imu);
    } else if (h->imu.kind == BES_IMU_ICM42607) {
        icm_reset(&h->imu);
    }
    ppg_reset(&h->ppg);
    h->imu.port.selected = h->ppg.port.selected = false;
    h->imu.port.byte_index = h->ppg.port.byte_index = 0;
    h->imu.irq_level = false;
    h->walk_phase = 0;
    h->step_rate = h->step_gain = 1;
    h->noise = 0x1503;
}

/* Idle interrupt levels after a board reset. */
static inline void bes_sensors_reset_pins(BesSensorHub *h)
{
    for (int i = 0; i < 2; i++) {
        BesSensorPort *port = bes_port(h, i);
        if (port && port->irq_pin >= 0) {
            h->host.set_input(h->host.opaque, port->irq_pin, false);
        }
    }
}

/* Mount "+y+x-z": chip X reads body Y, chip Y body X, chip Z minus body Z
 * (no commas: the value travels inside QEMU's -M option list). The body
 * frame is the band on a wrist, display up (+Z away from the skin). */
static inline bool bes_parse_mount(const char *a, int8_t mount[3])
{
    unsigned used = 0;
    for (int i = 0; i < 3; i++) {
        int sign = 1;
        if (*a == '-' || *a == '+') {
            sign = *a++ == '-' ? -1 : 1;
        }
        if (*a < 'x' || *a > 'z' || (used & (1u << (*a - 'x')))) {
            return false;
        }
        used |= 1u << (*a - 'x');
        mount[i] = sign * (*a++ - 'x' + 1);
    }
    return !*a;
}

static inline bool bes_parse_port(gchar **f, BesSensorPort *port)
{
    char *end;
    long a = strtol(f[3], &end, 0);
    if (*end) {
        return false;
    }
    long irq = strtol(f[4], &end, 0);
    if (*end || irq < -1 || irq >= 96) {
        return false;
    }
    port->irq_pin = irq;
    if (!strcmp(f[2], "spi0") || !strcmp(f[2], "spi1")) {
        port->bus = f[2][3] == '1' ? BES_BUS_SPI1 : BES_BUS_SPI0;
        port->cs_pin = a;
        return a >= 0 && a < 96;
    }
    if (!strncmp(f[2], "i2c", 3)) {
        long c = strtol(f[2] + 3, &end, 0);
        port->bus = BES_BUS_I2C;
        port->i2c_controller = c;
        port->i2c_address = a;
        return !*end && c >= 0 && c < 8 && a > 0 && a < 0x80;
    }
    return false;
}

/* Entries "imu:<lsm6dso|icm42607>:<bus>:<cs-pin|address>:<irq-pin|-1>[:mount]" and
 * "ppg:<hx3695|hx3697>:<bus>:...", bus = spi0, spi1 or i2cN, separated by ';'. */
static inline bool bes_sensors_configure(BesSensorHub *h, const char *spec, char **error)
{
    BesImu imu = {.kind = BES_IMU_NONE, .port = {.irq_pin = -1, .cs_pin = -1}};
    BesPpg ppg = {.kind = BES_PPG_NONE, .port = {.irq_pin = -1, .cs_pin = -1}};
    gchar **entries = g_strsplit(spec, ";", 0);
    bool ok = true;
    for (gchar **e = entries; ok && *e; e++) {
        if (!**e) {
            continue;
        }
        gchar **f = g_strsplit(*e, ":", 0);
        unsigned n = g_strv_length(f);
        if (n != 5 && n != 6) {
            ok = false;
        } else if (!strcmp(f[0], "imu")) {
            imu.kind = !strcmp(f[1], "lsm6dso") ? BES_IMU_LSM6DSO :
                       !strcmp(f[1], "icm42607") ? BES_IMU_ICM42607 : BES_IMU_NONE;
            ok = imu.kind && bes_parse_port(f, &imu.port);
            if (ok && n == 6) {
                ok = bes_parse_mount(f[5], imu.mount);
            }
        } else if (!strcmp(f[0], "ppg")) {
            ppg.kind = !strcmp(f[1], "hx3695") ? BES_PPG_HX3695 :
                       !strcmp(f[1], "hx3697") ? BES_PPG_HX3697 : BES_PPG_NONE;
            ok = ppg.kind && bes_parse_port(f, &ppg.port);
            if (ok && n == 6) {
                /* Tick clock the board's driver programs the sample period
                 * against (Band 9: 30400 Hz, Band 10 Pro: 32000 Hz). */
                char *end;
                long hz = strtol(f[5], &end, 10);
                ok = !*end && hz >= 1000 && hz <= 1000000;
                ppg.clock_hz = hz;
            }
        } else {
            ok = false;
        }
        g_strfreev(f);
    }
    g_strfreev(entries);
    if (!ok) {
        *error = g_strdup("sensor-board entries must be <imu|ppg>:<chip>:<spi0|spi1|i2cN>:"
                          "<cs-pin|address>:<irq-pin|-1>[:+x+y+z imu mount | :ppg tick Hz], separated by ';'");
        return false;
    }
    h->imu = imu;
    h->ppg = ppg;
    bes_sensors_reset(h);
    return true;
}

static inline void bes_describe_port(GString *out, const char *what, const char *chip,
                                     BesSensorPort *port)
{
    if (out->len) {
        g_string_append_c(out, ';');
    }
    if (port->bus == BES_BUS_SPI0 || port->bus == BES_BUS_SPI1) {
        g_string_append_printf(out, "%s:%s:spi%d:%d:%d", what, chip,
                               port->bus == BES_BUS_SPI1, port->cs_pin, port->irq_pin);
    } else {
        g_string_append_printf(out, "%s:%s:i2c%u:0x%02x:%d", what, chip, port->i2c_controller,
                               port->i2c_address, port->irq_pin);
    }
}

static inline char *bes_sensors_describe(BesSensorHub *h)
{
    GString *out = g_string_new("");
    if (h->imu.kind) {
        bes_describe_port(out, "imu", h->imu.kind == BES_IMU_LSM6DSO ? "lsm6dso" : "icm42607",
                          &h->imu.port);
        if (h->imu.mount[0]) {
            g_string_append_c(out, ':');
            for (int i = 0; i < 3; i++) {
                g_string_append_printf(out, "%c%c", h->imu.mount[i] < 0 ? '-' : '+',
                                       'x' + abs(h->imu.mount[i]) - 1);
            }
        }
    }
    if (h->ppg.kind) {
        bes_describe_port(out, "ppg", h->ppg.kind == BES_PPG_HX3697 ? "hx3697" : "hx3695",
                          &h->ppg.port);
        if (h->ppg.clock_hz) {
            g_string_append_printf(out, ":%u", h->ppg.clock_hz);
        }
    }
    return g_string_free(out, false);
}

#endif
