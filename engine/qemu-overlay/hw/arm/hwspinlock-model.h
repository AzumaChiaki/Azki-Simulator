/* Original TEE73e60194..73e6020a hardware lock/reset contract.
 * Explicit exploration: opaque config seed; owner token1 is abstract nonzero.
 * Only proven reset command pair, config low31 bits and locks0..30 accepted.
 */
#ifndef AZKI_O62M_HWSPINLOCK_MODEL_H
#define AZKI_O62M_HWSPINLOCK_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct O62MHwSpinlock {
    uint32_t config_seed, config, owned, external_owned;
    bool reset_asserted, reset_complete;
    int64_t asserted_ns, released_ns;
    uint64_t reset_count, acquisitions, conflicts, releases;
} O62MHwSpinlock;

static inline void o62m_hwspinlock_init(O62MHwSpinlock *s, uint32_t seed)
{
    memset(s,0,sizeof(*s));
    s->config_seed = s->config = seed;
}

/* Physical canonical addresses (root translates secure aliases once).
 * Names assert/release express the observed command sequence, not proven
 * electrical polarity. Unmatched release is outside the bounded contract. */
static inline bool o62m_hwspinlock_reset_write(O62MHwSpinlock *s,
                                              uint32_t address,
                                              uint32_t value,
                                              int64_t now_ns)
{
    if (value != 4 || now_ns < 0) { return false; }
    if (address == 0x4a00802c) {
        s->reset_asserted = true;
        s->reset_complete = false;
        s->owned = s->external_owned = 0;
        s->config = s->config_seed;
        s->asserted_ns = now_ns;
        s->reset_count++;
        return true;
    }
    if (address == 0x4a00402c && s->reset_asserted && now_ns >= s->asserted_ns) {
        s->reset_asserted = false;
        s->reset_complete = true;
        s->released_ns = now_ns;
        return true;
    }
    return false;
}

static inline bool o62m_hwspinlock_read(O62MHwSpinlock *s, uint32_t address,
                                      uint32_t *value)
{
    if (!s->reset_complete || s->reset_asserted) { return false; }
    if (address == 0x4a022004) {
        *value = s->config;
        return true;
    }
    if (address >= 0x4a020000 && address <= 0x4a021e00 && !(address & 0xff)) {
        uint32_t mask = 1u << ((address-0x4a020000) >> 8);
        if ((s->owned | s->external_owned) & mask) {
            *value = 1; /* Abstract occupied token; firmware tests only nonzero. */
            s->conflicts++;
        } else {
            s->owned |= mask;
            *value = 0;
            s->acquisitions++;
        }
        return true;
    }
    return false;
}

static inline bool o62m_hwspinlock_write(O62MHwSpinlock *s, uint32_t address,
                                       uint32_t value)
{
    if (!s->reset_complete || s->reset_asserted) { return false; }
    if (address == 0x4a022004) {
        if ((value ^ s->config) & 0x80000000u) { return false; }
        s->config = value;
        return true;
    }
    if (address >= 0x4a020000 && address <= 0x4a021e00 && !(address & 0xff)) {
        uint32_t mask = 1u << ((address-0x4a020000) >> 8);
        if (value != 0xaa55a55a || !(s->owned & mask) || (s->external_owned & mask)) {
            return false;
        }
        s->owned &= ~mask;
        s->releases++;
        return true;
    }
    return false;
}

/* Optional test input represents another actor already holding one lock;
 * no second real core is claimed. It cannot steal the emulated CPU's lock. */
static inline bool o62m_hwspinlock_external(O62MHwSpinlock *s, unsigned index,
                                          bool held)
{
    if (index > 30 || !s->reset_complete || s->reset_asserted) { return false; }
    uint32_t mask = 1u << index;
    if (s->owned & mask) { return false; }
    if (held) { s->external_owned |= mask; }
    else { s->external_owned &= ~mask; }
    return true;
}
#endif
