/* SPDX-License-Identifier: GPL-2.0-or-later
 * Compact, private NOR transaction working copy. Never an AP image.
 * The host owns publication and rollback of this disposable working copy.
 */
#ifndef BES_NOR_STORE_H
#define BES_NOR_STORE_H
#include "bes-nand-store.h"

#define BES_NOR_OFFSET 0xfb8000u
#define BES_NOR_BYTES  0x48000u

typedef struct BesNorLayout {
    uint32_t flash_bytes, ap_offset, tail_offset, factory_offset;
    uint32_t config_offset, config_bytes;
} BesNorLayout;

/* Exact geometries of the 14 cataloged N66/N67/O66/P67/Q66 profiles.
 * The compact format is tail first, then a separately bounded config extent.
 * Q66 remains the original headerless 288 KiB tail, byte for byte. N67 is
 * 16 KiB tail + 16 KiB config; the intervening AP is never in the store.
 */
static bool bes_nor_layout_init(BesNorLayout *layout, uint32_t flash_bytes,
                                uint32_t ap_offset, uint32_t tail_offset,
                                uint32_t factory_offset,
                                uint32_t config_offset, uint32_t config_bytes)
{
    static const BesNorLayout known[] = {
        { 0x800000, 0x100000, 0x7f8000, 0x7fe000, 0, 0 }, /* N66 */
        { 0x800000, 0x080000, 0x7fc000, 0x7fe000, 0x7c000, 0x4000 }, /* N67 */
        { 0x800000, 0x0a0000, 0x7f8000, 0x7fe000, 0, 0 }, /* O66 */
        { 0x1000000, 0x0c0000, 0xfc0000, 0xfc0000, 0, 0 }, /* P67 */
        /* P67 .043 shared AP ends at 0xde8190. BES userdata starts at the
         * next dual-chip erase boundary; mode/nv remain at 0xfc0000. */
        { 0x1000000, 0x0c0000, 0xdea000, 0xfc0000, 0, 0 },
        { 0x1000000, 0x0c0000, BES_NOR_OFFSET, 0xfc0000, 0, 0 }, /* Q66 */
        { 0x1000000, 0x0b0000, 0xfc0000, 0xfe0000, 0, 0 }, /* Q65 nv/nvflash0/misc */
        /* P65 BEST1502P: AP-derived NV tail starts at 0xcc0000; factory
         * data spans 0x100 x 512-byte sectors at 0xfe0000. No config extent. */
        { 0x1000000, 0x0c0000, 0xcc0000, 0xfe0000, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        const BesNorLayout *k = &known[i];
        if (flash_bytes == k->flash_bytes && ap_offset == k->ap_offset &&
            tail_offset == k->tail_offset && factory_offset == k->factory_offset &&
            config_offset == k->config_offset && config_bytes == k->config_bytes) {
            *layout = *k;
            return true;
        }
    }
    *layout = (BesNorLayout){ 0 };
    errno = EINVAL;
    return false;
}

static uint32_t bes_nor_layout_bytes(const BesNorLayout *layout)
{
    return layout->flash_bytes - layout->tail_offset + layout->config_bytes;
}

/* A command must fit wholly in one extent, never across the AP/config gap. */
static bool bes_nor_layout_translate(const BesNorLayout *layout,
                                    uint32_t address, unsigned count,
                                    off_t *offset)
{
    uint64_t end = (uint64_t)address + count;
    if (!count) {
        return false;
    }
    if (address >= layout->tail_offset && end <= layout->flash_bytes) {
        *offset = address - layout->tail_offset;
        return true;
    }
    if (layout->config_bytes && address >= layout->config_offset &&
        end <= (uint64_t)layout->config_offset + layout->config_bytes) {
        *offset = layout->flash_bytes - layout->tail_offset +
            address - layout->config_offset;
        return true;
    }
    return false;
}

typedef struct BesNorStore {
    int fd;
    bool enabled, failed;
    BesNorLayout layout;
} BesNorStore;

static void bes_nor_store_close(BesNorStore *s)
{
    if (s->enabled) {
        close(s->fd);
    }
    s->fd = -1;
    s->enabled = false;
}

static bool bes_nor_store_valid(int fd, const BesNorLayout *layout)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || (st.st_mode & 07777) != 0600 ||
        st.st_size != bes_nor_layout_bytes(layout)) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static bool bes_nor_store_read_extent(int fd, uint8_t *bytes, size_t count,
                                     off_t offset)
{
    while (count) {
        ssize_t n = pread(fd, bytes, count, offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (!n) {
                errno = EIO;
            }
            return false;
        }
        bytes += n;
        count -= n;
        offset += n;
    }
    return true;
}

static bool bes_nor_store_open(BesNorStore *s, const char *path,
                               const BesNorLayout *layout, uint8_t *flash)
{
    *s = (BesNorStore){ .fd = -1 };
    if (!bes_nor_layout_init(&s->layout, layout->flash_bytes, layout->ap_offset,
                            layout->tail_offset, layout->factory_offset,
                            layout->config_offset, layout->config_bytes)) {
        return false;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    uint32_t tail_bytes = layout->flash_bytes - layout->tail_offset;
    if (!bes_nor_store_valid(fd, layout) || flock(fd, LOCK_EX | LOCK_NB) ||
        !bes_nor_store_read_extent(fd, flash + layout->tail_offset, tail_bytes, 0) ||
        (layout->config_bytes &&
         !bes_nor_store_read_extent(fd, flash + layout->config_offset,
                                   layout->config_bytes, tail_bytes))) {
        int saved = errno;
        close(fd);
        errno = saved;
        return false;
    }
    s->fd = fd;
    s->enabled = true;
    return true;
}

static bool bes_nor_store_flush(BesNorStore *s, uint32_t address,
                                unsigned count, const uint8_t *bytes)
{
    if (!s->enabled) {
        return !s->failed;
    }
    off_t offset;
    if (s->failed || !bes_nor_layout_translate(&s->layout, address, count, &offset) ||
        !bes_nor_store_valid(s->fd, &s->layout)) {
        errno = EINVAL;
        s->failed = true;
        return false;
    }
    if (!bes_store_write(s->fd, bytes, count, offset) ||
        BES_STORE_FSYNC(s->fd)) {
        s->failed = true;
        return false;
    }
    return true;
}
#endif
