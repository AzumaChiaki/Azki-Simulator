/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/error-report.h"
#include <glib/gstdio.h>
#include "block/block.h"
#include "block/block_int.h"
#include "block/graph-lock.h"
#include "system/block-backend.h"
#include "hw/sd/o62m-memory-block-v1.h"

#define O62M_MEMORY_PAGE_V1 4096u
#define O62M_MEMORY_SEED_CHUNK_V1 (1u << 20)
typedef struct O62MMemoryBlockStateV1 {
    QemuMutex lock;
    GHashTable *pages;
    GHashTable *dirty;
    FILE *writeback;
    const bool *runtime_failed;
    uint64_t capacity;
    uint64_t read_calls, write_calls, read_bytes, write_bytes;
    uint64_t failed_reads, failed_writes;
    bool sealed, wiped, fault_read, fault_write;
    uint64_t fault_read_prefix, fault_write_prefix;
} O62MMemoryBlockStateV1;

static void o62m_memory_page_free_v1(void *opaque)
{
    volatile uint8_t *p = opaque;
    for (size_t i = 0; i < O62M_MEMORY_PAGE_V1; i++) {
        p[i] = 0;
    }
    g_free(opaque);
}

static int o62m_memory_open_v1(BlockDriverState *bs, QDict *options,
                                int flags, Error **errp)
{
    O62MMemoryBlockStateV1 *s = bs->opaque;
    int64_t length = qdict_get_try_int(options, "capacity", 0);
    (void)flags;
    if (length <= 0 || (length & 511)) {
        error_setg(errp, "Private RAM medium requires a positive 512-byte capacity");
        return -EINVAL;
    }
    qdict_del(options, "capacity");
    qemu_mutex_init(&s->lock);
    s->pages = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                     o62m_memory_page_free_v1);
    s->capacity = length;
    bs->supported_write_flags = BDRV_REQ_FUA;
    return 0;
}

static void o62m_memory_writeback_close_v1(O62MMemoryBlockStateV1 *s)
{
    if(!s->writeback)return;
    bool ok=!s->wiped && !(s->runtime_failed && *s->runtime_failed) &&
            !s->failed_reads && !s->failed_writes;
    uint64_t sectors=0;
    if(ok) {
        GHashTableIter it;gpointer key,value;
        g_hash_table_iter_init(&it,s->dirty);
        while(ok && g_hash_table_iter_next(&it,&key,&value)) {
            uint64_t index=*(uint64_t *)key;
            uint8_t *page=g_hash_table_lookup(s->pages,key);
            unsigned mask=GPOINTER_TO_UINT(value);
            for(unsigned bit=0;ok && bit<8;bit++)if(mask&(1u<<bit)) {
                uint64_t offset=index*O62M_MEMORY_PAGE_V1+bit*512;
                ok=page && fseeko(s->writeback,offset,SEEK_SET)==0 &&
                   fwrite(page+bit*512,1,512,s->writeback)==512;
                if(ok)++sectors;
            }
        }
        if(ok)ok=fflush(s->writeback)==0 && fsync(fileno(s->writeback))==0;
    }
    if(fclose(s->writeback)!=0)ok=false;
    s->writeback=NULL;
    error_report("XRING_EMMC_WRITEBACK completed=%u dirty-sectors=%" PRIu64,
                 ok,sectors);
}

static void o62m_memory_close_v1(BlockDriverState *bs)
{
    O62MMemoryBlockStateV1 *s = bs->opaque;
    o62m_memory_writeback_close_v1(s);
    if(s->dirty){g_hash_table_destroy(s->dirty);s->dirty=NULL;}
    if (s->pages) {
        g_hash_table_destroy(s->pages);
        s->pages = NULL;
        qemu_mutex_destroy(&s->lock);
    }
}

static int64_t coroutine_fn o62m_memory_length_v1(BlockDriverState *bs)
{
    O62MMemoryBlockStateV1 *s = bs->opaque;
    int64_t length;
    qemu_mutex_lock(&s->lock);
    length = s->wiped ? -EIO : (int64_t)s->capacity;
    qemu_mutex_unlock(&s->lock);
    return length;
}

static int coroutine_fn o62m_memory_io_v1(BlockDriverState *bs,
                                           int64_t offset, int64_t bytes,
                                           QEMUIOVector *qiov, bool write)
{
    O62MMemoryBlockStateV1 *s = bs->opaque;
    uint64_t length, done = 0;
    bool fail;
    if (offset < 0 || bytes < 0 || (uint64_t)offset > s->capacity ||
        (uint64_t)bytes > s->capacity - (uint64_t)offset ||
        (uint64_t)bytes > qiov->size) {
        return -EIO;
    }
    qemu_mutex_lock(&s->lock);
    if (s->wiped) {
        qemu_mutex_unlock(&s->lock);
        return -EIO;
    }
    if ((write ? s->write_calls : s->read_calls) == UINT64_MAX ||
        (write ? s->write_bytes : s->read_bytes) > UINT64_MAX - bytes ||
        (write ? s->failed_writes : s->failed_reads) == UINT64_MAX) {
        qemu_mutex_unlock(&s->lock);
        return -EOVERFLOW;
    }
    fail = write ? s->fault_write : s->fault_read;
    length = fail ? MIN((uint64_t)bytes,
                        write ? s->fault_write_prefix : s->fault_read_prefix) : bytes;
    if (write) {
        s->write_calls++;
        s->fault_write = false;
    } else {
        s->read_calls++;
        s->fault_read = false;
    }
    while (done < length) {
        uint64_t address = (uint64_t)offset + done;
        uint64_t index = address / O62M_MEMORY_PAGE_V1;
        size_t in_page = address % O62M_MEMORY_PAGE_V1;
        size_t step = MIN(length - done, O62M_MEMORY_PAGE_V1 - in_page);
        uint8_t *page = g_hash_table_lookup(s->pages, &index);
        size_t copied;
        if (write) {
            if (!page) {
                uint64_t *key = g_new(uint64_t, 1);
                *key = index;
                page = g_malloc0(O62M_MEMORY_PAGE_V1);
                g_hash_table_insert(s->pages, key, page);
            }
            copied = qemu_iovec_to_buf(qiov, done, page + in_page, step);
            if(s->sealed && s->dirty && copied) {
                unsigned mask=GPOINTER_TO_UINT(g_hash_table_lookup(s->dirty,&index));
                for(unsigned bit=in_page/512;bit<=(in_page+copied-1)/512;bit++)mask|=1u<<bit;
                uint64_t *key=g_new(uint64_t,1);*key=index;
                g_hash_table_replace(s->dirty,key,GUINT_TO_POINTER(mask));
            }
        } else if (page) {
            copied = qemu_iovec_from_buf(qiov, done, page + in_page, step);
        } else {
            copied = qemu_iovec_memset(qiov, done, 0, step);
        }
        if (copied != step) {
            /* Earlier copied bytes are real effects; never roll them back. */
            if (write) {
                s->write_bytes += done + copied;
                s->failed_writes++;
            } else {
                s->read_bytes += done + copied;
                s->failed_reads++;
            }
            qemu_mutex_unlock(&s->lock);
            return -EIO;
        }
        done += step;
    }
    if (write) {
        s->write_bytes += done;
        s->failed_writes += fail;
    } else {
        s->read_bytes += done;
        s->failed_reads += fail;
    }
    qemu_mutex_unlock(&s->lock);
    return fail ? -EIO : 0;
}

static int coroutine_fn o62m_memory_preadv_v1(BlockDriverState *bs,
                                               int64_t off, int64_t bytes,
                                               QEMUIOVector *qiov,
                                               BdrvRequestFlags flags)
{
    (void)flags;
    return o62m_memory_io_v1(bs, off, bytes, qiov, false);
}

static int coroutine_fn o62m_memory_pwritev_v1(BlockDriverState *bs,
                                                int64_t off, int64_t bytes,
                                                QEMUIOVector *qiov,
                                                BdrvRequestFlags flags)
{
    (void)flags;
    return o62m_memory_io_v1(bs, off, bytes, qiov, true);
}

static int coroutine_fn o62m_memory_flush_v1(BlockDriverState *bs)
{
    O62MMemoryBlockStateV1 *s = bs->opaque;
    int result;
    /* Guest writes are complete in RAM; the owned session file is committed
     * only at normal close, after all producers have stopped. */
    qemu_mutex_lock(&s->lock);
    result = s->wiped ? -EIO : 0;
    qemu_mutex_unlock(&s->lock);
    return result;
}

static BlockDriver o62m_memory_driver_v1 = {
    .format_name = "o62m-private-memory-v1",
    .instance_size = sizeof(O62MMemoryBlockStateV1),
    .bdrv_open = o62m_memory_open_v1,
    .bdrv_close = o62m_memory_close_v1,
    .bdrv_co_getlength = o62m_memory_length_v1,
    .bdrv_co_preadv = o62m_memory_preadv_v1,
    .bdrv_co_pwritev = o62m_memory_pwritev_v1,
    .bdrv_co_flush_to_disk = o62m_memory_flush_v1,
};

static O62MMemoryBlockStateV1 *o62m_memory_state_v1(BlockBackend *blk,
                                                  Error **errp)
{
    BlockDriverState *bs;
    O62MMemoryBlockStateV1 *state = NULL;
    if (!blk) {
        error_setg(errp, "Private RAM medium requires a BlockBackend");
        return NULL;
    }
    bdrv_graph_rdlock_main_loop();
    bs = blk_bs(blk);
    if (bs && bs->drv == &o62m_memory_driver_v1) {
        state = bs->opaque;
    }
    bdrv_graph_rdunlock_main_loop();
    if (!state) {
        error_setg(errp, "BlockBackend is not the owned private RAM medium");
    }
    return state;
}

BlockBackend *o62m_memory_block_create_v1(const char *node_name,
                                         uint64_t capacity, Error **errp)
{
    BlockBackend *blk;
    BlockDriverState *bs;
    QDict *options;
    if (!node_name || !capacity || capacity > INT64_MAX || (capacity & 511)) {
        error_setg(errp, "Private RAM medium capacity/name invalid");
        return NULL;
    }
    options = qdict_new();
    qdict_put_int(options, "capacity", capacity);
    bs = bdrv_new_open_driver_opts(&o62m_memory_driver_v1, node_name, options,
                                    BDRV_O_RDWR, errp);
    if (!bs) {
        return NULL;
    }
    blk = blk_new(qemu_get_aio_context(), BLK_PERM_ALL, BLK_PERM_ALL);
    if (blk_insert_bs(blk, bs, errp) < 0) {
        bdrv_unref(bs);
        blk_unref(blk);
        return NULL;
    }
    bdrv_unref(bs);
    return blk;
}

bool o62m_memory_block_seed_v1(BlockBackend *blk, uint64_t offset,
                               const void *bytes, size_t length, Error **errp)
{
    O62MMemoryBlockStateV1 *s = o62m_memory_state_v1(blk, errp);
    size_t done = 0;
    bool sealed;
    if (!s) {
        return false;
    }
    if ((!bytes && length) || offset > s->capacity || length > s->capacity - offset) {
        error_setg(errp, "Private RAM initialization range invalid");
        return false;
    }
    qemu_mutex_lock(&s->lock);
    sealed = s->sealed || s->wiped;
    qemu_mutex_unlock(&s->lock);
    if (sealed) {
        error_setg(errp, "Private RAM host initialization already sealed");
        return false;
    }
    while (done < length) {
        size_t step = MIN(length - done, O62M_MEMORY_SEED_CHUNK_V1);
        if (blk_pwrite(blk, offset + done, step, (const uint8_t *)bytes + done, 0) < 0) {
            error_setg(errp, "Actual private RAM BlockBackend initialization failed");
            return false;
        }
        done += step;
    }
    return true;
}

bool o62m_memory_block_seal_v1(BlockBackend *blk, Error **errp)
{
    O62MMemoryBlockStateV1 *s = o62m_memory_state_v1(blk, errp);
    if (!s) {
        return false;
    }
    qemu_mutex_lock(&s->lock);
    if (s->wiped) {
        qemu_mutex_unlock(&s->lock);
        error_setg(errp, "Private RAM medium terminally wiped");
        return false;
    }
    s->sealed = true;
    qemu_mutex_unlock(&s->lock);
    return true;
}

bool o62m_memory_block_set_writeback_v1(BlockBackend *blk,const char *path,
                                          const bool *runtime_failed,Error **errp)
{
    O62MMemoryBlockStateV1 *s=o62m_memory_state_v1(blk,errp);
    if(!s)return false;
    if(!s->sealed || s->wiped || s->writeback || !path || !*path) {
        error_setg(errp,"Instance writeback requires a sealed live RAM medium");return false;
    }
    FILE *file=g_fopen(path,"r+b");struct stat st;
    if(!file || fstat(fileno(file),&st)<0 || !S_ISREG(st.st_mode) ||
       st.st_size<0 || (uint64_t)st.st_size!=s->capacity) {
        if(file)fclose(file);
        error_setg(errp,"Instance writeback requires an existing capacity-matched file");return false;
    }
    s->writeback=file;s->runtime_failed=runtime_failed;
    s->dirty=g_hash_table_new_full(g_int64_hash,g_int64_equal,g_free,NULL);
    return true;
}

bool o62m_memory_block_wipe_v1(BlockBackend *blk, Error **errp)
{
    O62MMemoryBlockStateV1 *s = o62m_memory_state_v1(blk, errp);
    if (!s) {
        return false;
    }
    qemu_mutex_lock(&s->lock);
    /* Every value destructor performs a volatile byte wipe before g_free. */
    g_hash_table_remove_all(s->pages);
    s->wiped = s->sealed = true;
    s->fault_read = s->fault_write = false;
    s->fault_read_prefix = s->fault_write_prefix = 0;
    qemu_mutex_unlock(&s->lock);
    return true;
}

bool o62m_memory_block_observe_v1(BlockBackend *blk,
                                  O62MMemoryBlockObservationV1 *out,
                                  Error **errp)
{
    O62MMemoryBlockStateV1 *s = o62m_memory_state_v1(blk, errp);
    if (!s || !out) {
        if (s) {
            error_setg(errp, "Private RAM observation requires output");
        }
        return false;
    }
    qemu_mutex_lock(&s->lock);
    O62MMemoryBlockObservationV1 value = {
        s->capacity, g_hash_table_size(s->pages), s->read_calls, s->write_calls,
        s->read_bytes, s->write_bytes, s->failed_reads, s->failed_writes,
        s->sealed, s->wiped
    };
    qemu_mutex_unlock(&s->lock);
    *out = value;
    return true;
}

bool o62m_memory_block_test_fault_v1(BlockBackend *blk, bool write,
                                     uint64_t prefix_bytes, Error **errp)
{
    O62MMemoryBlockStateV1 *s = o62m_memory_state_v1(blk, errp);
    if (!s) {
        return false;
    }
    qemu_mutex_lock(&s->lock);
    if (s->wiped) {
        qemu_mutex_unlock(&s->lock);
        error_setg(errp, "Private RAM medium terminally wiped");
        return false;
    }
    if ((write && s->fault_write) || (!write && s->fault_read)) {
        qemu_mutex_unlock(&s->lock);
        error_setg(errp, "Private RAM matching test fault already armed");
        return false;
    }
    if (write) {
        s->fault_write_prefix = prefix_bytes;
        s->fault_write = true;
    } else {
        s->fault_read_prefix = prefix_bytes;
        s->fault_read = true;
    }
    qemu_mutex_unlock(&s->lock);
    return true;
}
