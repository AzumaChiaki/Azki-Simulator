#ifndef HW_SD_O62M_SD_OBSERVER_V1_H
#define HW_SD_O62M_SD_OBSERVER_V1_H

/* Opt-in, synchronous receipts for this checkout's actual eMMC methods.
 * No card data is stored in receipts. No rollback or persistence promise is
 * inferred from local buffer acceptance. Serialize calls on the card owner
 * context; keep the same live card/session for the entire API lifetime.
 * All pointer arguments must be valid, and outputs must not overlap the card,
 * its private observer, or the token passed to finish. No migration is claimed.
 */
#include "hw/sd/sd.h"

#define O62M_SD_OBSERVER_IO_CAPACITY_V1 16u

typedef enum O62MSdObserverKindV1 {
    O62M_SD_OBSERVER_COMMAND_V1 = 1,
    O62M_SD_OBSERVER_READ_BYTE_V1,
    O62M_SD_OBSERVER_WRITE_BYTE_V1,
    O62M_SD_OBSERVER_RESET_V1,
} O62MSdObserverKindV1;

typedef enum O62MSdObserverResultV1 {
    O62M_SD_OBSERVER_OK_V1 = 0,
    O62M_SD_OBSERVER_BAD_ARGUMENT_V1,
    O62M_SD_OBSERVER_BAD_CARD_V1,
    O62M_SD_OBSERVER_DISABLED_V1,
    O62M_SD_OBSERVER_ALREADY_ENABLED_V1,
    O62M_SD_OBSERVER_SESSION_V1,
    O62M_SD_OBSERVER_ACTIVE_V1,
    O62M_SD_OBSERVER_INACTIVE_V1,
    O62M_SD_OBSERVER_STALE_TOKEN_V1,
    O62M_SD_OBSERVER_SERIAL_LIMIT_V1,
} O62MSdObserverResultV1;

enum {
    O62M_SD_OBSERVER_INCOMPLETE_V1       = 1u << 0,
    O62M_SD_OBSERVER_UNEXPECTED_CALL_V1  = 1u << 1,
    O62M_SD_OBSERVER_COUNTER_OVERFLOW_V1 = 1u << 2,
    O62M_SD_OBSERVER_IO_ERROR_V1        = 1u << 3,
    O62M_SD_OBSERVER_PARTIAL_UNKNOWN_V1 = 1u << 4,
    O62M_SD_OBSERVER_PAYLOAD_UNKNOWN_V1 = 1u << 5,
    O62M_SD_OBSERVER_EVENT_LIMIT_V1     = 1u << 6,
};

typedef struct O62MSdObserverTokenV1 {
    uintptr_t owner;
    uint64_t session;
    uint64_t serial;
    O62MSdObserverKindV1 kind;
} O62MSdObserverTokenV1;

typedef struct O62MSdObserverSnapshotV1 {
    uint64_t session;
    int32_t card_state;
    uint32_t card_status;
    uint8_t current_cmd;
    /* Finite public card mode/capability fields; never payload or command arg. */
    bool cmd21_tuning_enabled, in_transfer_state;
    uint8_t bus_width_config, hs_timing, ext_csd_revision, card_type;
    uint8_t active_part_config; /* actual ext_csd[179], not QOM default */
    uint8_t reset_default_part_config; /* diagnostic property only */
    bool inserted;
    bool readonly;
    bool sending_data;   /* state indication only, not success */
    bool receiving_data; /* state indication only, not success */
    uint64_t data_start;
    uint64_t data_size;
    uint32_t data_offset;
    uint32_t block_length;
    uint32_t multi_block_count;
    uint32_t known_read_buffer_bytes;
    uint64_t completed_resets; /* since observer enable only */
    uint64_t unscoped_calls;
    uint64_t host_io_errors;
    int32_t last_host_errno; /* last negative result,0 if none */
    bool counter_overflow;
} O62MSdObserverSnapshotV1;

typedef struct O62MSdObserverIoV1 {
    bool write;
    bool backend_called;
    bool full_success; /* actual synchronous blk_* returned0 */
    bool partial_unknown; /* failed called backend may have partial effects */
    uint64_t host_offset; /* actual offset after boot-partition adjustment */
    uint32_t length;
    int32_t result; /* actual blk_* result; -ENOMEDIUM if no backend called */
} O62MSdObserverIoV1;

typedef struct O62MSdObserverReceiptV1 {
    O62MSdObserverTokenV1 token;
    O62MSdObserverSnapshotV1 before;
    O62MSdObserverSnapshotV1 after;
    uint32_t flags;
    uint64_t actual_calls;
    uint64_t returned_calls;
    uint64_t actual_consumed_bytes; /* real load/store point, never ready */
    uint64_t read_bytes_consumed;
    uint64_t write_bytes_accepted; /* card buffer, not host persistence */
    uint64_t command_response_bytes;
    uint64_t host_read_success_bytes;
    uint64_t host_write_success_bytes;
    uint64_t host_io_events;
    uint32_t io_count;
    O62MSdObserverIoV1 io[O62M_SD_OBSERVER_IO_CAPACITY_V1];
} O62MSdObserverReceiptV1;

/* Enable once, after actual TYPE_EMMC realization and before operations.
 * Default private pointer is NULL; legacy cards retain original behavior.
 */
O62MSdObserverResultV1 o62m_sd_observer_enable_v1(DeviceState *card,
                                                uint64_t session);
O62MSdObserverResultV1 o62m_sd_observer_snapshot_v1(
    DeviceState *card, O62MSdObserverSnapshotV1 *out);
/* Begin -> exactly one real sdbus command/byte call or device_cold_reset(card)
 * -> finish. The caller must use the actual bus containing this exact card.
 * Finish OK means a receipt was delivered, not that the card operation
 * succeeded: check flags, actual/returned call counts, consumed count, actual
 * response/card status, and I/O events before committing controller progress.
 * A command may legitimately return no response; no response-size success
 * rule is invented here. RESET counts only after the actual reset returns.
 * Invalid API transactions leave observer/output unchanged. An accepted
 * finish closes the token even when its receipt reports an operation failure;
 * no external/card/buffer effects are rolled back.
 */
O62MSdObserverResultV1 o62m_sd_observer_begin_v1(
    DeviceState *card, uint64_t session, O62MSdObserverKindV1 kind,
    O62MSdObserverTokenV1 *out);
O62MSdObserverResultV1 o62m_sd_observer_finish_v1(
    DeviceState *card, const O62MSdObserverTokenV1 *token,
    O62MSdObserverReceiptV1 *out);
/* One controller DMA chunk, bounded by the existing card/block/descriptor
 * plan. Call byte_progress after each real byte; stop at its first false.
 * Finish always closes the scope and retains consumed bytes and I/O errors,
 * including a failed partial chunk. PIO/commands/reset keep the one-call API. */
O62MSdObserverResultV1 o62m_sd_observer_begin_bytes_v1(
    DeviceState *card, uint64_t session, bool write, uint32_t bytes,
    O62MSdObserverTokenV1 *out);
bool o62m_sd_observer_byte_progress_v1(DeviceState *card,
    const O62MSdObserverTokenV1 *token, uint32_t completed);
const char *o62m_sd_observer_reason_v1(O62MSdObserverResultV1 result);

#endif
