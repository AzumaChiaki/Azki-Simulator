/* SPDX-License-Identifier: GPL-2.0-or-later
 * Opt-in persistence for private instance NAND data and OOB files only.
 * Callers use a disposable transaction copy, never firmware/cache originals.
 * Failure is fatal to that run: a partial write must never be published.
 */
#ifndef BES_NAND_STORE_H
#define BES_NAND_STORE_H
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BES_STORE_PWRITE
#define BES_STORE_PWRITE pwrite
#endif
#ifndef BES_STORE_FSYNC
#define BES_STORE_FSYNC fsync
#endif

typedef struct BesNandStore {
    int data_fd, spare_fd;
    uint32_t first_page, pages;
    bool enabled, failed;
} BesNandStore;

static void bes_store_close(BesNandStore *s)
{
    if (s->enabled) {
        close(s->data_fd);
        close(s->spare_fd);
    }
    s->enabled = false;
}

static int bes_store_open_file(const char *path, uint64_t bytes)
{
    struct stat st;
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { return -1; }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || (st.st_mode & 077) ||
        (uint64_t)st.st_size != bytes) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    return fd;
}

static bool bes_store_open(BesNandStore *s, const char *data,
                           const char *spare, uint32_t offset, uint32_t bytes)
{
    *s = (BesNandStore){ .data_fd = -1, .spare_fd = -1 };
    if (!data || !spare || !bytes || ((offset | bytes) & (131072 - 1)) ||
        (uint64_t)offset + bytes > 512u * 1024 * 1024) {
        errno = EINVAL; return false;
    }
    s->data_fd = bes_store_open_file(data, bytes);
    if (s->data_fd < 0) { return false; }
    s->spare_fd = bes_store_open_file(spare, bytes / 2048 * 64);
    if (s->spare_fd < 0) { close(s->data_fd); s->data_fd = -1; return false; }
    s->first_page = offset / 2048;
    s->pages = bytes / 2048;
    s->enabled = true;
    return true;
}

static bool bes_store_read(int fd, uint8_t *bytes, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = pread(fd, bytes + done, length - done, done);
        if (count < 0 && errno == EINTR) { continue; }
        if (count <= 0) { return false; }
        done += count;
    }
    return true;
}

static bool bes_store_write(int fd, const uint8_t *bytes,
                            size_t length, uint64_t offset)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = BES_STORE_PWRITE(fd, bytes + done, length - done,
                                        offset + done);
        if (count < 0 && errno == EINTR) { continue; }
        if (count <= 0) { return false; }
        done += count;
    }
    return true;
}

/* Inputs point at the first affected page, not at a whole-chip backing. */
static bool bes_store_flush(BesNandStore *s, uint32_t page, uint32_t count,
                            const uint8_t *data, const uint8_t *spare)
{
    if (!s->enabled) { return !s->failed; }
    if (s->failed || !count || page < s->first_page ||
        (uint64_t)page + count > (uint64_t)s->first_page + s->pages) {
        errno = EINVAL; s->failed = true; return false;
    }
    uint64_t relative = page - s->first_page;
    if (!bes_store_write(s->data_fd, data, (size_t)count * 2048, relative * 2048) ||
        !bes_store_write(s->spare_fd, spare, (size_t)count * 64, relative * 64) ||
        BES_STORE_FSYNC(s->data_fd) || BES_STORE_FSYNC(s->spare_fd)) {
        s->failed = true;
        return false;
    }
    return true;
}
#endif
