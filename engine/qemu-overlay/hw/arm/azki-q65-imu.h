/* Optional Q65 LSM6DSO raw-input model. SPDX-License-Identifier: GPL-2.0-or-later
 * Firmware-supported fallback, not an assertion about the fitted board BOM.
 * Register contract: ST DS12140 Rev3 and AN5192 sections4.4,4.7,9.
 * No gait, steps, pulse or derived health result is generated here.
 * The caller serializes this state and supplies virtual nanoseconds.
 */
#ifndef AZKI_Q65_IMU_H
#define AZKI_Q65_IMU_H
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define Q65_IMU_FIFO_WORDS 512
typedef struct {
    uint8_t value, observation; /* Byte fragment: 1 output pair, 2 FIFO record. */
    uint64_t version;
    uint64_t observation_id;
    uint8_t part;
    uint8_t axis; /* 0..5; FIFO observations contain three axes from this base. */
    int16_t raw[3];
} Q65ImuByte;
typedef struct {
    uint8_t bytes[7];
    uint64_t version, observation_id;
} Q65ImuRecord;
typedef struct {
    bool enabled, selected, command, reading;
    uint8_t address, regs[128], bank[2][128];
    int16_t input[6]; /* accel XYZ, gyro XYZ, signed register counts */
    uint64_t input_version;
    int16_t output[6];
    uint64_t output_version[6];
    uint64_t observation_serial, pair_id[6], observed_id[7];
    uint8_t observed_mask[7];
    uint8_t pair_read[6]; /* BDU protects an initiated LSB/MSB pair */
    bool ready[2]; /* gyro, accel */
    int64_t next_ns[2], batch_ns[2], epoch_ns, fifo_epoch_ns, now_ns;
    int64_t reset_deadline_ns, boot_deadline_ns;
    uint32_t timestamp_ticks;
    uint64_t samples[2], fifo_pops, fifo_read_count, output_read_count;
    uint64_t last_read_version, dropped, reset_count, unsupported_count;
    uint64_t axis_read_count[6], axis_read_version[6];
    int16_t axis_read_raw[6];
    Q65ImuRecord fifo[Q65_IMU_FIFO_WORDS];
    unsigned head, count;
    uint8_t tag_counter;
    bool overrun, overrun_latched, fifo_stopped;
} Q65Imu;

static inline int64_t q65_imu_period(unsigned code)
{
    static const int64_t ns[16] = {
        0,80000000,38461538,19230769,9615385,4807692,2403846,
        1200480,600240,300120,149993,625000000,0,0,0,0
    };
    return ns[code & 15];
}

static inline int64_t q65_imu_batch_period(unsigned code, unsigned sensor)
{ return !sensor && code == 11 ? 153846154 : q65_imu_period(code); }

static inline unsigned q65_imu_watermark(const Q65Imu *m)
{ return m->regs[7] | (m->regs[8] & 1) << 8; }

static inline unsigned q65_imu_capacity(const Q65Imu *m)
{
    unsigned w = q65_imu_watermark(m);
    return (m->regs[8] & 0x80) && w ? w : Q65_IMU_FIFO_WORDS;
}

static inline bool q65_imu_full(const Q65Imu *m)
{ return m->count >= q65_imu_capacity(m) - 1; }

/* The optional raw model does not implement embedded motion algorithms,
 * auxiliary sensors, analog filters or compression. Preserve register
 * storage for diagnostics, but never silently count these as implemented. */
static inline bool q65_imu_storage_only(unsigned reg)
{
    return !(reg == 0 || reg == 1 || (reg >= 7 && reg <= 0x0a) ||
             (reg >= 0x0d && reg <= 0x13) || reg == 0x18 || reg == 0x19 ||
             reg == 0x1e || (reg >= 0x22 && reg <= 0x2d) ||
             reg == 0x3a || reg == 0x3b || (reg >= 0x40 && reg <= 0x43) ||
             reg == 0x62 || (reg >= 0x78 && reg <= 0x7e));
}

static inline void q65_imu_reset(Q65Imu *m, int64_t now)
{
    memset(m->regs, 0, sizeof(m->regs));
    memset(m->bank, 0, sizeof(m->bank));
    memset(m->output, 0, sizeof(m->output));
    memset(m->output_version, 0, sizeof(m->output_version));
    memset(m->pair_read, 0, sizeof(m->pair_read));
    memset(m->next_ns, 0, sizeof(m->next_ns));
    memset(m->batch_ns, 0, sizeof(m->batch_ns));
    m->regs[0x0f] = 0x6c; m->regs[0x12] = 4;
    m->ready[0] = m->ready[1] = false;
    m->head = m->count = m->tag_counter = 0;
    m->overrun = m->overrun_latched = false;
    m->fifo_stopped = false;
    m->epoch_ns = m->fifo_epoch_ns = m->now_ns = now;
    m->reset_deadline_ns = m->boot_deadline_ns = 0;
    m->timestamp_ticks = 0;
    m->reset_count++;
}

static inline bool q65_imu_input_set(Q65Imu *m, const char *text)
{
    static const char *const prefix[6] = {"accel=",",",",",";gyro=",",",","};
    int16_t v[6];
    for (unsigned i = 0; i < 6; i++) {
        size_t n = strlen(prefix[i]);
        if (strncmp(text, prefix[i], n)) { return false; }
        text += n;
        if (!(*text == '-' || (*text >= '0' && *text <= '9'))) { return false; }
        errno = 0;
        char *end;
        long x = strtol(text, &end, 10);
        if (end == text || errno || x < -32768 || x > 32767) { return false; }
        v[i] = x; text = end;
    }
    if (*text || m->input_version == UINT64_MAX) { return false; }
    memcpy(m->input, v, sizeof(v));
    m->input_version++;
    return true;
}

static inline void q65_imu_push(Q65Imu *m, unsigned sensor)
{
    unsigned mode = m->regs[0x0a] & 7;
    if (mode != 1 && mode != 6) { return; } /* FIFO or continuous */
    if (mode == 1 && m->fifo_stopped) { return; }
    if (m->count >= q65_imu_capacity(m)) {
        if (mode == 1) { m->fifo_stopped = true; return; }
        m->overrun = m->overrun_latched = true;
        m->head = (m->head + 1) % Q65_IMU_FIFO_WORDS;
        m->count--; m->dropped++;
    }
    Q65ImuRecord *r = &m->fifo[(m->head + m->count++) % Q65_IMU_FIFO_WORDS];
    uint8_t tag = ((sensor ? 2 : 1) << 3) | ((m->tag_counter & 3) << 1);
    r->bytes[0] = tag | __builtin_parity((unsigned)tag);
    for (unsigned i = 0; i < 3; i++) {
        uint16_t x = m->input[(sensor ? 0 : 3) + i];
        r->bytes[1 + 2 * i] = x; r->bytes[2 + 2 * i] = x >> 8;
    }
    r->version = m->input_version;
    r->observation_id = ++m->observation_serial;
    if (mode == 1 && m->count == q65_imu_capacity(m)) { m->fifo_stopped = true; }
}

static inline void q65_imu_advance(Q65Imu *m, int64_t now)
{
    m->now_ns = now;
    if (!m->enabled) { return; }
    if (m->reset_deadline_ns) {
        if (now < m->reset_deadline_ns) { return; }
        m->reset_deadline_ns = 0; m->regs[0x12] &= ~1;
    }
    if (m->boot_deadline_ns && now >= m->boot_deadline_ns) {
        m->boot_deadline_ns = 0; m->regs[0x12] &= ~0x80;
    }
    int64_t period[2] = {q65_imu_period(m->regs[0x11] >> 4),
                         q65_imu_period(m->regs[0x10] >> 4)};
    if ((m->regs[0x11] >> 4) == 11 || (m->regs[0x13] & 0x40)) { period[0] = 0; }
    int64_t batch_due[2] = {0}, batch_step[2] = {0}, batch_end[2] = {0};
    for (unsigned k = 0; k < 2; k++) {
        if (!period[k]) { m->next_ns[k] = m->batch_ns[k] = 0; continue; }
        if (!m->next_ns[k]) { m->next_ns[k] = now + period[k]; }
        int64_t first = m->next_ns[k];
        if (first <= now) {
            uint64_t n = (now - first) / period[k] + 1;
            unsigned base = k ? 0 : 3;
            for (unsigned i = base; i < base + 3; i++) {
                if (!(m->regs[0x12] & 0x40) || !m->pair_read[i]) {
                    m->output[i] = m->input[i];
                    m->output_version[i] = m->input_version;
                    m->pair_read[i] = 0;
                }
            }
            m->ready[k] = true; m->samples[k] += n;
            m->next_ns[k] = first + n * period[k];
        }
        int64_t batch = q65_imu_batch_period(k ? m->regs[9] & 15 : m->regs[9] >> 4, k);
        if (!batch || ((m->regs[0x0a] & 7) != 1 && (m->regs[0x0a] & 7) != 6)) {
            m->batch_ns[k] = 0; continue;
        }
        /* BDR is its own clock. Do not round each batch to an ODR tick:
         * 26Hz ODR / 12.5Hz BDR must remain 12.5Hz, not 8.67Hz. */
        int64_t step = batch > period[k] ? batch : period[k];
        if (!m->batch_ns[k]) { m->batch_ns[k] = now + step; }
        int64_t due = m->batch_ns[k];
        if (due > now) { continue; }
        uint64_t records = (now - due) / step + 1;
        m->batch_ns[k] = due + records * step;
        /* Retain at least the entire capacity of each producer. Their merged
         * chronological suffix therefore contains the exact final FIFO.
         * Output ODR samples and FIFO batching are independently accounted. */
        unsigned capacity = q65_imu_capacity(m);
        if (records > capacity) {
            if ((m->regs[0x0a] & 7) == 6) {
                uint64_t skip = records - capacity;
                due += skip * step; m->dropped += skip;
                m->overrun = m->overrun_latched = true;
                m->observation_serial += skip;
            }
            records = capacity;
        }
        batch_due[k] = due; batch_step[k] = step; batch_end[k] = due + records * step;
    }
    for (;;) {
        unsigned k = !batch_due[0] || (batch_due[1] && batch_due[1] < batch_due[0]);
        int64_t due = batch_due[k];
        if (!due || due > now || due >= batch_end[k]) {
            batch_due[k] = 0;
            if (!batch_due[1-k]) { break; }
            continue;
        }
        int64_t own = q65_imu_batch_period(k ? m->regs[9] & 15 : m->regs[9] >> 4, k);
        int64_t other = q65_imu_batch_period(k ? m->regs[9] >> 4 : m->regs[9] & 15, 1-k);
        int64_t slot = other && other < own ? other : own;
        m->tag_counter = (due - m->fifo_epoch_ns) / slot;
        q65_imu_push(m, k);
        batch_due[k] += batch_step[k];
    }
}

static inline int64_t q65_imu_deadline(const Q65Imu *m)
{
    int64_t next = 0;
    int64_t values[] = {m->next_ns[0], m->next_ns[1], m->batch_ns[0], m->batch_ns[1],
                        m->boot_deadline_ns, m->reset_deadline_ns};
    for (unsigned i = 0; i < 6; i++) {
        if (values[i] && (!next || values[i] < next)) { next = values[i]; }
    }
    return next;
}

static inline bool q65_imu_irq(const Q65Imu *m)
{
    unsigned mask = m->regs[0x0d];
    /* INT2 bit2 is temperature DRDY, not INT1's BOOT signal. */
    if (m->regs[0x13] & 0x20) { mask |= m->regs[0x0e] & 0x3b; }
    unsigned w = q65_imu_watermark(m);
    bool high = ((mask & 1) && m->ready[1]) || ((mask & 2) && m->ready[0]) ||
        ((mask & 4) && m->boot_deadline_ns) ||
        ((mask & 8) && w && m->count >= w) || ((mask & 16) && m->overrun) ||
        ((mask & 32) && q65_imu_full(m));
    return m->enabled && ((m->regs[0x12] & 0x20) ? !high : high);
}

static inline Q65ImuByte q65_imu_read(Q65Imu *m, uint8_t reg)
{
    Q65ImuByte r = {0};
    if (reg > 1 && (m->regs[1] & 0xc0)) {
        m->unsupported_count++; /* Bank storage has no embedded-function engine. */
        r.value = m->bank[(m->regs[1] & 0x80) ? 0 : 1][reg]; return r;
    }
    if (reg >= 0x22 && reg <= 0x2d) {
        unsigned i = reg < 0x28 ? (reg - 0x22) / 2 + 3 : (reg - 0x28) / 2;
        r.value = (uint16_t)m->output[i] >> ((reg & 1) * 8);
        if (!m->pair_read[i]) { m->pair_id[i] = ++m->observation_serial; }
        r.observation = 1; r.version = m->output_version[i];
        r.axis = i; r.raw[0] = m->output[i]; r.part = reg & 1;
        r.observation_id = m->pair_id[i];
        m->pair_read[i] |= 1u << (reg & 1);
        if (m->pair_read[i] == 3) {
            m->pair_read[i] = 0;
        }
        if (reg == 0x27) { m->ready[0] = false; }
        if (reg == 0x2d) { m->ready[1] = false; }
        return r;
    }
    switch (reg) {
    case 0x1e: r.value = m->ready[1] | m->ready[0] << 1; break;
    case 0x3a: r.value = m->count; break;
    case 0x3b: {
        unsigned w = q65_imu_watermark(m);
        r.value = (m->count >> 8) | (m->overrun_latched ? 8 : 0) |
            (q65_imu_full(m) ? 32 : 0) |
            (m->overrun ? 64 : 0) | (w && m->count >= w ? 128 : 0);
        m->overrun_latched = false; break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43:
        r.value = (m->timestamp_ticks +
                   ((m->regs[0x19] & 0x20) ? (uint32_t)((m->now_ns - m->epoch_ns) / 25000) : 0))
                   >> ((reg - 0x40) * 8);
        break;
    case 0x78 ... 0x7e:
        if (m->count) {
            Q65ImuRecord *f = &m->fifo[m->head];
            r.value = f->bytes[reg - 0x78];
            r.observation = 2; r.version = f->version;
            r.observation_id = f->observation_id; r.part = reg - 0x78;
            r.axis = (f->bytes[0] >> 3) == 2 ? 0 : 3;
            for (unsigned i = 0; i < 3; i++) {
                r.raw[i] = (int16_t)(f->bytes[1 + 2 * i] |
                                    f->bytes[2 + 2 * i] << 8);
            }
            if (reg == 0x7e) {
                m->head = (m->head + 1) % Q65_IMU_FIFO_WORDS;
                m->count--; m->fifo_pops++; m->overrun = false;
            }
        }
        break;
    default:
        if (q65_imu_storage_only(reg)) { m->unsupported_count++; }
        r.value = m->regs[reg]; break;
    }
    return r;
}

static inline void q65_imu_write(Q65Imu *m, uint8_t reg, uint8_t value)
{
    if (reg > 1 && (m->regs[1] & 0xc0)) {
        m->bank[(m->regs[1] & 0x80) ? 0 : 1][reg] = value;
        m->unsupported_count++; /* Storage only; no embedded engine. */
        return;
    }
    if (reg == 0x0f || reg == 0x1e || (reg >= 0x20 && reg <= 0x2d) ||
        reg == 0x3a || reg == 0x3b || reg >= 0x78) { return; }
    if (reg == 0x12 && (value & 1)) {
        if (value & 0x80) { m->unsupported_count++; } /* Forbidden simultaneous flows. */
        q65_imu_reset(m, m->now_ns);
        m->regs[0x12] |= 1; m->reset_deadline_ns = m->now_ns + 50000;
        return;
    }
    if (reg == 0x12 && (value & 0x80)) {
        /* AN5192 section5: reload trims, preserve control registers, 10ms. */
        m->boot_deadline_ns = m->now_ns + 10000000;
    }
    if (reg == 0x19 && ((m->regs[reg] ^ value) & 0x20)) {
        if (m->regs[reg] & 0x20) {
            m->timestamp_ticks += (m->now_ns - m->epoch_ns) / 25000;
        }
        m->epoch_ns = m->now_ns;
    }
    if (reg == 0x42 && value == 0xaa) {
        m->epoch_ns = m->now_ns; m->timestamp_ticks = 0; return;
    }
    if (reg >= 0x40 && reg <= 0x43) { return; } /* Timestamp is otherwise read-only. */
    if (reg == 0x0a && !(value & 7)) {
        m->head = m->count = 0; m->overrun = m->overrun_latched = false;
        m->fifo_stopped = false;
        m->batch_ns[0] = m->batch_ns[1] = 0;
    }
    if ((reg == 0x0a && ((value & 7) != 0 && (value & 7) != 1 && (value & 7) != 6)) ||
        (reg == 8 && (value & 0x70)) || (reg == 0x0a && (value & 0xf0))) {
        m->unsupported_count++; /* Compression/trigger/temp/timestamp batching. */
    }
    if (q65_imu_storage_only(reg) ||
        (reg == 0x0d && (value & 0xc0)) || (reg == 0x0e && (value & 0xc4)) ||
        (reg == 0x12 && (value & 0x0a)) || /* BLE and three-wire SPI. */
        (reg == 0x13 && (value & ~0x64)) ||
        (reg == 0x18 && (value & ~2)) || (reg == 0x19 && (value & ~0x20))) {
        m->unsupported_count++;
    }
    if (m->regs[reg] != value &&
        (reg == 0x10 || reg == 0x11 || reg == 0x13)) {
        if (reg != 0x10) { m->next_ns[0] = m->batch_ns[0] = 0; }
        if (reg != 0x11) { m->next_ns[1] = m->batch_ns[1] = 0; }
    }
    if (reg == 9 && m->regs[reg] != value) {
        m->fifo_epoch_ns = m->now_ns; m->batch_ns[0] = m->batch_ns[1] = 0;
    }
    m->regs[reg] = value;
}

static inline void q65_imu_select(Q65Imu *m, bool selected)
{
    if (selected != m->selected) { m->command = false; }
    m->selected = selected;
}

static inline Q65ImuByte q65_imu_transfer(Q65Imu *m, uint8_t tx)
{
    Q65ImuByte r = {0xff, 0, 0};
    if (!m->enabled || !m->selected) { return r; }
    if (!m->command) {
        m->reading = !!(tx & 0x80); m->address = tx & 0x7f;
        m->command = true; return r;
    }
    if (m->reading) { r = q65_imu_read(m, m->address); }
    else { q65_imu_write(m, m->address, tx); }
    if (m->regs[0x12] & 4) {
        m->address = m->reading && m->address == 0x7e ? 0x78 : (m->address + 1) & 0x7f;
    }
    return r;
}

/* Called only when the guest drains a controller RX byte. All pair/record
 * fragments must match before counting consumption. Chip clocking, a dropped
 * RX byte and accepting QMP input are not observations. */
static inline void q65_imu_observe(Q65Imu *m, Q65ImuByte r)
{
    if (!r.observation) { return; }
    unsigned slot = r.observation == 2 ? 6 : r.axis;
    unsigned complete = r.observation == 2 ? 0x7f : 3;
    if (m->observed_id[slot] != r.observation_id) {
        m->observed_id[slot] = r.observation_id;
        m->observed_mask[slot] = 0;
    }
    if (m->observed_mask[slot] == complete) { return; }
    m->observed_mask[slot] |= 1u << r.part;
    if (m->observed_mask[slot] != complete) { return; }
    if (r.observation == 1) { m->output_read_count++; }
    if (r.observation == 2) { m->fifo_read_count++; }
    m->last_read_version = r.version;
    for (unsigned i = 0; i < (r.observation == 2 ? 3 : 1); i++) {
        m->axis_read_count[r.axis + i]++;
        m->axis_read_version[r.axis + i] = r.version;
        m->axis_read_raw[r.axis + i] = r.raw[i];
    }
}
#endif
