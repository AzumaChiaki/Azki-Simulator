/* O62M exploratory DW UART TX subset. SPDX-License-Identifier: GPL-2.0-only
 * Queue capacity and live input clock are explicit environment parameters,
 * not recovered O62M silicon configuration. RX/modem/break remain unsupported.
 * Caller MUST advance virtual time before each access, then schedule a timer
 * at next_deadline(). Only advance invokes emit, after a complete wire frame.
 */
#ifndef AZKI_O62M_UART_TX_MODEL_H
#define AZKI_O62M_UART_TX_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#define O62M_UART_TX_MAX_DEPTH 256
#define O62M_UART_TX_NO_DEADLINE INT64_MAX

typedef void (*O62MUartTxEmit)(void *opaque, uint8_t byte, int64_t completed_ns);
typedef struct O62MUartTx {
    uint8_t queue[O62M_UART_TX_MAX_DEPTH], shift;
    unsigned depth, head, count, stet;
    uint32_t input_hz;
    uint16_t divisor;
    uint8_t fraction;
    bool configured, running, fifo, ptime, ier_thre, active, thre_latch;
    int64_t last_ns, frame_floor_ns, paused_ns, deadline_ns;
    uint64_t frame_fraction, frame_numerator, accepted, emitted, discarded;
} O62MUartTx;

static inline bool o62m_uart_tx_init(O62MUartTx *s, unsigned explicit_depth)
{
    if (!explicit_depth || explicit_depth > O62M_UART_TX_MAX_DEPTH ||
        (explicit_depth & (explicit_depth - 1))) { return false; }
    memset(s, 0, sizeof(*s));
    s->depth = explicit_depth;
    s->deadline_ns = O62M_UART_TX_NO_DEADLINE;
    s->thre_latch = true;
    return true;
}

static inline unsigned o62m_uart_tx_capacity(const O62MUartTx *s)
{
    return s->fifo ? s->depth : 1;
}

static inline bool o62m_uart_tx_configure(O62MUartTx *s, uint32_t input_hz,
                                         uint16_t divisor, uint8_t fraction,
                                         uint8_t lcr, bool fifo)
{
    /* Only the observed 8N1 format. DLAB is not a wire format bit. */
    if (!input_hz || !divisor || fraction > 15 || (lcr & 0x7f) != 3) {
        return false;
    }
    if ((s->active || s->count) && (!s->configured ||
        s->input_hz != input_hz || s->divisor != divisor ||
        s->fraction != fraction || s->fifo != fifo)) { return false; }
    s->input_hz = input_hz;
    s->divisor = divisor;
    s->fraction = fraction;
    s->fifo = fifo;
    s->frame_numerator = UINT64_C(10000000000) * (16u * divisor + fraction);
    s->configured = true;
    return true;
}

static inline bool o62m_uart_tx_schedule_frame(O62MUartTx *s)
{
    uint64_t total = s->frame_numerator + s->frame_fraction;
    uint64_t ns = total / s->input_hz;
    uint64_t fraction = total % s->input_hz;
    if (ns > INT64_MAX || s->frame_floor_ns > INT64_MAX - (int64_t)ns - 1) {
        return false;
    }
    s->frame_floor_ns += ns;
    s->frame_fraction = fraction;
    s->deadline_ns = s->frame_floor_ns + (fraction != 0);
    return true;
}

static inline bool o62m_uart_tx_start(O62MUartTx *s, bool new_burst)
{
    if (s->active || !s->count || !s->running || !s->configured) { return true; }
    if (new_burst) {
        s->frame_floor_ns = s->last_ns;
        s->frame_fraction = 0;
    }
    if (!o62m_uart_tx_schedule_frame(s)) { return false; }
    s->shift = s->queue[s->head];
    s->head = (s->head + 1) % s->depth;
    s->count--;
    s->active = true;
    if (!s->count) { s->thre_latch = true; }
    return true;
}

static inline bool o62m_uart_tx_advance(O62MUartTx *s, int64_t now_ns,
                                       O62MUartTxEmit emit, void *opaque)
{
    if (now_ns < s->last_ns || now_ns < 0) { return false; }
    while (s->running && s->active && s->deadline_ns <= now_ns) {
        int64_t completed = s->deadline_ns;
        uint8_t byte = s->shift;
        s->active = false;
        s->deadline_ns = O62M_UART_TX_NO_DEADLINE;
        s->emitted++;
        if (emit) { emit(opaque, byte, completed); }
        if (!o62m_uart_tx_start(s, false)) { return false; }
    }
    s->last_ns = now_ns;
    return true;
}

static inline bool o62m_uart_tx_set_running(O62MUartTx *s, bool running,
                                           int64_t now_ns)
{
    /* Explicit gate fixture; caller advance(now) first. Paused wire time does
     * not elapse while disabled. A pending shifter retains phase and byte. */
    if (now_ns != s->last_ns) { return false; }
    if (running == s->running) { return true; }
    if (!running) {
        s->paused_ns = now_ns;
        s->running = false;
        return true;
    }
    if (s->active) {
        int64_t paused = now_ns - s->paused_ns;
        if (s->deadline_ns > INT64_MAX - paused) { return false; }
        s->frame_floor_ns += paused;
        s->deadline_ns += paused;
    }
    s->running = true;
    return o62m_uart_tx_start(s, true);
}

static inline bool o62m_uart_tx_write(O62MUartTx *s, uint8_t byte, int64_t now_ns)
{
    if (now_ns != s->last_ns || !s->configured ||
        s->count >= o62m_uart_tx_capacity(s)) { return false; }
    s->queue[(s->head + s->count) % s->depth] = byte;
    s->count++;
    s->accepted++;
    s->thre_latch = false;
    return o62m_uart_tx_start(s, true);
}

static inline bool o62m_uart_tx_irq_config(O62MUartTx *s, bool ier_thre,
                                          bool ptime, unsigned stet)
{
    if (stet > 3) { return false; }
    if (ier_thre && !s->ier_thre && !s->count) { s->thre_latch = true; }
    s->ier_thre = ier_thre;
    s->ptime = ptime;
    s->stet = stet;
    return true;
}

static inline unsigned o62m_uart_tx_threshold(const O62MUartTx *s)
{
    unsigned threshold = s->stet == 0 ? 0 : s->stet == 1 ? 2 :
                         s->stet == 2 ? s->depth / 4 : s->depth / 2;
    return threshold < s->depth ? threshold : s->depth;
}

static inline bool o62m_uart_tx_irq(const O62MUartTx *s)
{
    if (!s->ier_thre) { return false; }
    return s->fifo && s->ptime ? s->count <= o62m_uart_tx_threshold(s) :
                                s->thre_latch;
}

static inline uint8_t o62m_uart_tx_iir(O62MUartTx *s)
{
    uint8_t id = o62m_uart_tx_irq(s) ? 2 : 1;
    if (id == 2 && !(s->fifo && s->ptime)) { s->thre_latch = false; }
    return (s->fifo ? 0xc0 : 0) | id;
}

/* Bits5/6 only; caller merges actual supported RX/error bits separately. */
static inline uint8_t o62m_uart_tx_lsr(const O62MUartTx *s)
{
    bool bit5 = s->fifo && s->ptime ? s->count == s->depth : s->count == 0;
    return (bit5 ? 0x20 : 0) | ((!s->active && !s->count) ? 0x40 : 0);
}

/* Bits0..2 only: BUSY, TFNF, TFE. */
static inline uint8_t o62m_uart_tx_usr(const O62MUartTx *s)
{
    return ((s->active || s->count) ? 1 : 0) |
           (s->count < o62m_uart_tx_capacity(s) ? 2 : 0) |
           (s->count == 0 ? 4 : 0);
}

static inline void o62m_uart_tx_fifo_reset(O62MUartTx *s)
{
    /* FCR.TFR/SRR.XFR affects queued bytes, not the current shift register. */
    s->discarded += s->count;
    s->head = s->count = 0;
    s->thre_latch = true;
}

static inline int64_t o62m_uart_tx_next_deadline(const O62MUartTx *s)
{
    return s->running && s->active ? s->deadline_ns : O62M_UART_TX_NO_DEADLINE;
}
#endif
