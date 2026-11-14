/* Explicit virtual AP read view: one const existing OTP data owner, a separate
 * AP gate initially zero/locked. This is not proof of physical AP/TEE aliases,
 * silicon reset values, OTP contents, programming or status semantics.
 */
#ifndef O62M_AP_EFUSE_READ_VIEW_V1_H
#define O62M_AP_EFUSE_READ_VIEW_V1_H

#include "otp-read-v1.h"
#include <stddef.h>

#define O62M_AP_EFUSE_READ_MAGIC_V1 UINT32_C(0x4d474352)
#define O62M_AP_EFUSE_KEY_ADDRESS_V1 UINT64_C(0x4a300030)
#define O62M_AP_EFUSE_DATA_FIRST_V1 UINT64_C(0x4a301000)
#define O62M_AP_EFUSE_DATA_LAST_V1 UINT64_C(0x4a3011fc)
#define O62M_AP_EFUSE_BONDING_ADDRESS_V1 UINT64_C(0x4a301190)

typedef struct O62MAPEfuseReadViewV1 {
    uint32_t key;
    bool initialized, unlocked;
    uint64_t key_reads, key_writes, data_reads;
} O62MAPEfuseReadViewV1;

static inline bool o62m_ap_efuse_read_view_overlap_v1(
    const void *first, size_t first_size, const void *second, size_t second_size)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;
    return a <= b ? b - a < first_size : a - b < second_size;
}

static inline bool o62m_ap_efuse_read_view_init_v1(O62MAPEfuseReadViewV1 *view)
{
    if (!view) {
        return false;
    }
    memset(view, 0, sizeof(*view));
    view->initialized = true;
    return true;
}

/* Exact original AP PCs and words only. The caller supplies the existing
 * initialized external data owner; its TEE gate and all 128 words stay intact.
 * A rejected transaction leaves view, owner and caller value unchanged.
 */
static inline bool o62m_ap_efuse_read_view_access_v1(
    O62MAPEfuseReadViewV1 *view, const O62MOtpRead *owner,
    uint64_t address, unsigned size, bool write, uint64_t pc, uint64_t *value)
{
    O62MAPEfuseReadViewV1 next;
    uint64_t result;
    if (!view || !owner || !value || !view->initialized || !owner->initialized ||
        size != 4 || (address & UINT64_C(3)) ||
        o62m_ap_efuse_read_view_overlap_v1(value, sizeof(*value),
                                         view, sizeof(*view)) ||
        o62m_ap_efuse_read_view_overlap_v1(value, sizeof(*value),
                                         owner, sizeof(*owner)) ||
        o62m_ap_efuse_read_view_overlap_v1(view, sizeof(*view),
                                         owner, sizeof(*owner))) {
        return false;
    }
    next = *view;
    if (address == O62M_AP_EFUSE_KEY_ADDRESS_V1) {
        if (write) {
            if (pc != UINT64_C(0x60e0cad2) ||
                (*value != O62M_AP_EFUSE_READ_MAGIC_V1 && *value != 0) ||
                next.key_writes == UINT64_MAX) {
                return false;
            }
            next.key = (uint32_t)*value;
            next.unlocked = next.key == O62M_AP_EFUSE_READ_MAGIC_V1;
            next.key_writes++;
            *view = next;
            return true;
        }
        if (pc != UINT64_C(0x60e0cace) || next.key_reads == UINT64_MAX) {
            return false;
        }
        result = next.key;
        next.key_reads++;
    } else {
        if (write || !next.unlocked ||
            next.key != O62M_AP_EFUSE_READ_MAGIC_V1 ||
            address < O62M_AP_EFUSE_DATA_FIRST_V1 ||
            address > O62M_AP_EFUSE_DATA_LAST_V1 ||
            !((pc == UINT64_C(0x60e0c91c)) ||
              (pc == UINT64_C(0x60c5f10e) &&
               address == O62M_AP_EFUSE_BONDING_ADDRESS_V1)) ||
            next.data_reads == UINT64_MAX) {
            return false;
        }
        result = owner->word[(unsigned)
            ((address - O62M_AP_EFUSE_DATA_FIRST_V1) / UINT64_C(4))];
        next.data_reads++;
    }
    *view = next;
    *value = result;
    return true;
}

#endif
