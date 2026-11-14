/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SD_O62M_MEMORY_BLOCK_V1_H
#define HW_SD_O62M_MEMORY_BLOCK_V1_H
#include "system/block-backend.h"
#include "qapi/error.h"

/* A real BlockBackend over sparse private RAM. Every accepted write copies
 * into owned RAM pages. Capacity is uint64_t and supports >=1GiB media without
 * allocating every absent zero page. The process must serialize these host APIs
 * and keep the BlockBackend reference alive; ordinary block I/O uses its lock. */
typedef struct O62MMemoryBlockObservationV1 {
    uint64_t capacity, allocated_pages;
    uint64_t read_calls, write_calls, read_bytes, write_bytes;
    uint64_t failed_reads, failed_writes;
    bool initialization_sealed;
    bool terminal_wiped;
} O62MMemoryBlockObservationV1;

BlockBackend *o62m_memory_block_create_v1(const char *node_name,
                                         uint64_t capacity, Error **errp);
/* Host initialization only, before seal. No file opening or image discovery.
 * Caller supplies immutable original/public bytes already read under its own
 * authorization. This performs actual BlockBackend I/O into private RAM. */
bool o62m_memory_block_seed_v1(BlockBackend *blk, uint64_t offset,
                               const void *bytes, size_t length, Error **errp);
bool o62m_memory_block_seal_v1(BlockBackend *blk, Error **errp);
/* Optional owned session file: only sectors changed after seal are written on
 * successful close. The owner keeps runtime_failed alive through backend close
 * and publishes the session file only after completed=1 and clean process exit. */
bool o62m_memory_block_set_writeback_v1(BlockBackend *blk,const char *path,
                                          const bool *runtime_failed,Error **errp);
/* Serialized shutdown only, after stopping producers: lock, volatile-wipe and
 * free every private page, then terminally reject I/O/seed/fault/length/flush.
 * Idempotent. Observation remains available and contains no payload. */
bool o62m_memory_block_wipe_v1(BlockBackend *blk, Error **errp);
bool o62m_memory_block_observe_v1(BlockBackend *blk,
                                  O62MMemoryBlockObservationV1 *out,
                                  Error **errp);
/* Public-fixture component tests only. One actual following matching I/O copies
 * exactly min(prefix_bytes,request_length) then returns -EIO. This deliberately
 * preserves real uncertain partial effects; it never rolls data back. */
bool o62m_memory_block_test_fault_v1(BlockBackend *blk, bool write,
                                     uint64_t prefix_bytes, Error **errp);
#endif
