/* Read or update the firmware's persist.db properties inside an instance's
 * YAFFS2 (in-band tags) NAND data image, before the core opens that image.
 * GPL-2.0-only, linked against Aleph One YAFFS2 and the vendored UnQLite.
 *
 *   instance-props DATA SPARE get        print every property as one JSON object
 *   instance-props DATA SPARE set FILE   apply "S\tkey\tvalue" / "D\tkey" lines
 *
 * OpenVela's kvdb stores keys and string values with their terminating NUL.
 * The old checkpoint is read and invalidated on write but never rewritten, so
 * the firmware's next mount scans the modified partition.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "yaffsfs.h"
#include "yaffs_guts.h"
#include "yaffs_trace.h"
#include "unqlite.h"

#define BLOCK 131072
#define MAX_PROPERTIES 4096
#define MAX_VALUE 4096

unsigned yaffs_trace_mask;
static int last_error;
static unsigned char *storage, *dirty;
static size_t storage_size;
void yaffsfs_Lock(void) {}
void yaffsfs_Unlock(void) {}
void yaffsfs_OSInitialisation(void) {}
void yaffsfs_SetError(int err) { last_error = err; }
int yaffsfs_GetLastError(void) { return last_error; }
u32 yaffsfs_CurrentTime(void) { return 1700000000; }
void *yaffsfs_malloc(size_t size) { return malloc(size); }
void yaffsfs_free(void *p) { free(p); }
void yaffsfs_get_malloc_values(unsigned *a, unsigned *b) { if (a) *a = 0; if (b) *b = 0; }
int yaffsfs_CheckMemRegion(const void *p, size_t n, int wr) { return p ? 0 : -1; }
void yaffs_bug_fn(const char *file, int line) { fprintf(stderr, "YAFFS BUG %s:%d\n", file, line); abort(); }

static void check(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "instance-props: %s failed (YAFFS error %d, errno %d)\n", what, last_error, errno); exit(1); }
}
static int init(struct yaffs_dev *d) { return YAFFS_OK; }
static int bad(struct yaffs_dev *d, int block) { return block >= 0 && (size_t)block * BLOCK < storage_size ? YAFFS_OK : YAFFS_FAIL; }
static int mark_bad(struct yaffs_dev *d, int block) { return YAFFS_FAIL; }
static int erase(struct yaffs_dev *d, int block)
{
    if (bad(d, block) != YAFFS_OK) return YAFFS_FAIL;
    memset(storage + (size_t)block * BLOCK, 255, BLOCK);
    dirty[block] = 2; /* data and spare */
    return YAFFS_OK;
}
static int read_chunk(struct yaffs_dev *d, int chunk, u8 *data, int len, u8 *oob, int olen, enum yaffs_ecc_result *ecc)
{
    if (chunk < 0 || len < 0 || len > 2048 || olen || (size_t)chunk * 2048 + len > storage_size) return YAFFS_FAIL;
    if (data) memcpy(data, storage + (size_t)chunk * 2048, len);
    if (ecc) *ecc = YAFFS_ECC_RESULT_NO_ERROR;
    return YAFFS_OK;
}
static int write_chunk(struct yaffs_dev *d, int chunk, const u8 *data, int len, const u8 *oob, int olen)
{
    if (chunk < 0 || len < 0 || len > 2048 || olen || !data || (size_t)chunk * 2048 + len > storage_size) return YAFFS_FAIL;
    unsigned char *dest = storage + (size_t)chunk * 2048;
    for (int i = 0; i < len; i++) { if ((dest[i] & data[i]) != data[i]) return YAFFS_FAIL; dest[i] &= data[i]; }
    if (!dirty[chunk / 64]) dirty[chunk / 64] = 1;
    return YAFFS_OK;
}

static int open_image(const char *path, size_t *size, int writing)
{
    int fd = open(path, (writing ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    check(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
          st.st_uid == geteuid() && !(st.st_mode & 077), "private image open");
    *size = (size_t)st.st_size;
    return fd;
}

static void mount_image(void)
{
    static struct yaffs_dev dev;
    memset(&dev, 0, sizeof(dev));
    dev.param.name = "/nand"; dev.param.inband_tags = 1; dev.param.total_bytes_per_chunk = 2048;
    dev.param.chunks_per_block = 64; dev.param.start_block = 0; dev.param.end_block = storage_size / BLOCK - 1;
    dev.param.n_reserved_blocks = 5; dev.param.n_caches = 10; dev.param.is_yaffs2 = 1; dev.param.use_nand_ecc = 1;
    dev.param.skip_checkpt_rd = 0; dev.param.skip_checkpt_wr = 1; dev.param.stored_endian = 1;
    dev.drv.drv_initialise_fn = init; dev.drv.drv_deinitialise_fn = init; dev.drv.drv_read_chunk_fn = read_chunk;
    dev.drv.drv_write_chunk_fn = write_chunk; dev.drv.drv_erase_fn = erase;
    dev.drv.drv_check_bad_fn = bad; dev.drv.drv_mark_bad_fn = mark_bad;
    yaffs_add_device(&dev);
    check(yaffs_mount("/nand") == 0, "mount");
}

/* Copy one YAFFS file to a host temporary file; returns 0 when absent. */
static int extract(const char *inside, const char *host)
{
    int in = yaffs_open(inside, 0, 0);
    if (in < 0) return 0;
    int out = open(host, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    check(out >= 0, "temporary file");
    unsigned char buf[65536];
    int n;
    while ((n = yaffs_read(in, buf, sizeof(buf))) > 0) check(write(out, buf, n) == n, "temporary write");
    check(n == 0 && yaffs_close(in) == 0 && close(out) == 0, "extract");
    return 1;
}

static void store(const char *host, const char *inside)
{
    int in = open(host, O_RDONLY | O_NOFOLLOW);
    check(in >= 0, "updated database open");
    /* O_CREAT|O_TRUNC|O_RDWR in the Linux values yportenv uses. */
    int out = yaffs_open(inside, 02 | 0100 | 01000, 0644);
    check(out >= 0, "database replace");
    unsigned char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) check(yaffs_write(out, buf, (unsigned)n) == n, "database write");
    check(n == 0 && close(in) == 0 && yaffs_close(out) == 0, "database store");
}

static void json_string(const unsigned char *s, size_t n)
{
    putchar('"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20 || c == 0x7f) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static int valid_key(const char *k)
{
    size_t n = strlen(k);
    if (!n || n > 128) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= 'A' && k[i] <= 'Z') || (k[i] >= '0' && k[i] <= '9') ||
              k[i] == '.' || k[i] == '_' || k[i] == '-')) return 0;
    return 1;
}

static int dump(unqlite *db)
{
    unqlite_kv_cursor *cursor;
    check(unqlite_kv_cursor_init(db, &cursor) == UNQLITE_OK, "cursor");
    int first = 1, count = 0;
    putchar('{');
    for (int rc = unqlite_kv_cursor_first_entry(cursor); rc == UNQLITE_OK && unqlite_kv_cursor_valid_entry(cursor);
         rc = unqlite_kv_cursor_next_entry(cursor)) {
        unsigned char key[256], value[MAX_VALUE];
        int klen = sizeof(key);
        unqlite_int64 vlen = sizeof(value);
        if (unqlite_kv_cursor_key(cursor, key, &klen) != UNQLITE_OK ||
            unqlite_kv_cursor_data(cursor, value, &vlen) != UNQLITE_OK) continue;
        /* Only NUL-terminated text pairs are firmware string properties. */
        if (klen < 2 || key[klen - 1] || memchr(key, 0, klen - 1) ||
            vlen < 1 || value[vlen - 1] || memchr(value, 0, vlen - 1)) continue;
        if (!first) putchar(',');
        first = 0;
        json_string(key, klen - 1); putchar(':'); json_string(value, vlen - 1);
        if (++count >= MAX_PROPERTIES) break;
    }
    puts("}");
    unqlite_kv_cursor_release(db, cursor);
    return 0;
}

static void apply(unqlite *db, const char *path)
{
    FILE *f = fopen(path, "r");
    check(f != NULL, "change file");
    char line[MAX_VALUE + 256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        check(n && line[n - 1] == '\n', "change line length");
        line[n - 1] = 0;
        check(++count <= MAX_PROPERTIES, "change count");
        char *key = line + 2, *value = NULL;
        check((line[0] == 'S' || line[0] == 'D') && line[1] == '\t', "change syntax");
        if (line[0] == 'S') {
            value = strchr(key, '\t');
            check(value != NULL, "change value");
            *value++ = 0;
        }
        check(valid_key(key), "property key");
        if (value) check(unqlite_kv_store(db, key, (int)strlen(key) + 1, value, (unqlite_int64)strlen(value) + 1) == UNQLITE_OK, "property store");
        else {
            int rc = unqlite_kv_delete(db, key, (int)strlen(key) + 1);
            check(rc == UNQLITE_OK || rc == UNQLITE_NOTFOUND, "property delete");
        }
    }
    check(!ferror(f) && fclose(f) == 0, "change read");
    check(unqlite_commit(db) == UNQLITE_OK, "commit");
}

int main(int argc, char **argv)
{
    int extracting = argc == 6 && !strcmp(argv[3], "extract");
    if (!(argc == 4 && !strcmp(argv[3], "get")) && !(argc == 5 && !strcmp(argv[3], "set")) && !extracting) {
        fprintf(stderr, "Usage: instance-props DATA SPARE get | set CHANGES | extract /nand/PATH OUTPUT\n");
        return 2;
    }
    int writing = argc == 5;
    if (extracting && (strncmp(argv[4], "/nand/", 6) || strstr(argv[4], ".."))) return 2;
    size_t spare_size;
    int data_fd = open_image(argv[1], &storage_size, writing);
    int spare_fd = open_image(argv[2], &spare_size, writing);
    check(storage_size >= 8u << 20 && storage_size <= 512u << 20 && storage_size % BLOCK == 0 &&
          spare_size == storage_size / 2048 * 64, "image geometry");
    storage = malloc(storage_size);
    dirty = calloc(storage_size / BLOCK, 1);
    check(storage && dirty, "allocation");
    check(pread(data_fd, storage, storage_size, 0) == (ssize_t)storage_size, "image read");
    mount_image();
    if (extracting) {
        int present = extract(argv[4], argv[5]);
        check(yaffs_unmount("/nand") == 0, "unmount");
        close(data_fd); close(spare_fd);
        return present ? 0 : 3;
    }

    /* The sandboxed runner grants only its own TMPDIR. */
    const char *tmp = getenv("TMPDIR");
    char dir[1024];
    check(snprintf(dir, sizeof(dir), "%s/instance-props-XXXXXX", tmp && *tmp ? tmp : "/tmp") < (int)sizeof(dir), "temporary path");
    check(mkdtemp(dir) != NULL, "temporary directory");
    char db_path[1100], journal_path[1100];
    snprintf(db_path, sizeof(db_path), "%s/persist.db", dir);
    snprintf(journal_path, sizeof(journal_path), "%s/persist.db_unqlite_journal", dir);
    int present = extract("/nand/persist.db", db_path);
    int journal = extract("/nand/persist.db_unqlite_journal", journal_path);
    int status = 0;
    if (!present && !writing) {
        puts("{}");
    } else {
        unqlite *db = NULL;
        check(unqlite_open(&db, db_path, writing ? UNQLITE_OPEN_CREATE : UNQLITE_OPEN_READONLY) == UNQLITE_OK, "database open");
        if (writing) apply(db, argv[4]);
        else status = dump(db);
        check(unqlite_close(db) == UNQLITE_OK, "database close");
    }
    if (writing) {
        store(db_path, "/nand/persist.db");
        if (journal) check(yaffs_unlink("/nand/persist.db_unqlite_journal") == 0, "stale journal removal");
    }
    check(yaffs_unmount("/nand") == 0, "unmount");
    unlink(db_path); unlink(journal_path); rmdir(dir);
    if (writing) {
        unsigned char erased[2048];
        memset(erased, 255, sizeof(erased));
        for (size_t b = 0; b < storage_size / BLOCK; b++) {
            if (!dirty[b]) continue;
            check(pwrite(data_fd, storage + b * BLOCK, BLOCK, (off_t)(b * BLOCK)) == BLOCK, "data write-back");
            if (dirty[b] == 2)
                for (unsigned i = 0; i < 2; i++)
                    check(pwrite(spare_fd, erased, 2048, (off_t)(b * 4096 + i * 2048)) == 2048, "spare erase");
        }
        check(fsync(data_fd) == 0 && fsync(spare_fd) == 0, "sync");
    }
    close(data_fd); close(spare_fd);
    return status;
}
