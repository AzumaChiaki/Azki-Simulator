/* Build the local UI preferences consumed by OpenVela's unqlite kvdb backend.
 * No account identifiers or authentication material are generated here.
 * The upstream common.c/direct.c API includes the terminating NUL in BOTH
 * key and string value lengths; omitting it produces different database keys.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "unqlite.h"

static const struct { const char *key, *value; } preferences[] = {
    {"persist.setup_wizard", "1"},
    {"persist.locale", "zh_CN"},
    {"persist.screen.auto_screen_timeout", "0"},
    {"persist.screen.dimmer_timeout", "2147483647"},
};

static int verify(const char *path)
{
    unqlite *db = NULL;
    unqlite_kv_cursor *cursor = NULL;
    unsigned count = 0;
    int status = 1;
    if (unqlite_open(&db, path, UNQLITE_OPEN_READONLY) != UNQLITE_OK)
        goto done;
    for (unsigned i = 0; i < sizeof(preferences) / sizeof(*preferences); i++) {
        char value[128] = {0};
        unqlite_int64 size = sizeof(value);
        const char *key = preferences[i].key, *expected = preferences[i].value;
        if (unqlite_kv_fetch(db, key, strlen(key) + 1, value, &size) != UNQLITE_OK ||
            size != (unqlite_int64)strlen(expected) + 1 ||
            memcmp(value, expected, (size_t)size))
            goto done;
        /* A common seed bug is accidentally using a non-NUL-terminated key. */
        size = sizeof(value);
        if (unqlite_kv_fetch(db, key, strlen(key), value, &size) != UNQLITE_NOTFOUND)
            goto done;
    }
    if (unqlite_kv_cursor_init(db, &cursor) != UNQLITE_OK ||
        unqlite_kv_cursor_first_entry(cursor) != UNQLITE_OK)
        goto done;
    while (unqlite_kv_cursor_valid_entry(cursor)) {
        if (++count > sizeof(preferences) / sizeof(*preferences))
            goto done;
        unqlite_kv_cursor_next_entry(cursor);
    }
    if (count != sizeof(preferences) / sizeof(*preferences))
        goto done;
    status = 0;
done:
    if (cursor) unqlite_kv_cursor_release(db, cursor);
    if (db && unqlite_close(db) != UNQLITE_OK) status = 1;
    return status;
}

int main(int argc, char **argv)
{
    unqlite *db = NULL;
    int status = 1;
    if (argc == 3 && !strcmp(argv[1], "--verify")) {
        if (verify(argv[2])) { fprintf(stderr, "UI database verification failed\n"); return 1; }
        puts("Verified four exact NUL-terminated UI preference records");
        return 0;
    }
    if (argc != 2) { fprintf(stderr, "Usage: %s [--verify] persist.db\n", argv[0]); return 2; }
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("Create new database"); return 1; }
    close(fd);
    if (unqlite_open(&db, argv[1], UNQLITE_OPEN_CREATE | UNQLITE_OPEN_OMIT_JOURNALING) != UNQLITE_OK)
        goto done;
    for (unsigned i = 0; i < sizeof(preferences) / sizeof(*preferences); i++) {
        const char *key = preferences[i].key, *value = preferences[i].value;
        if (unqlite_kv_store(db, key, strlen(key) + 1, value, strlen(value) + 1) != UNQLITE_OK)
            goto done;
    }
    if (unqlite_commit(db) != UNQLITE_OK) goto done;
    status = 0;
done:
    if (db && unqlite_close(db) != UNQLITE_OK) status = 1;
    if (!status) status = verify(argv[1]);
    if (status) { unlink(argv[1]); fprintf(stderr, "UI database creation or cold-reopen verification failed\n"); }
    else puts("Verified four exact NUL-terminated UI preference records after cold reopen");
    return status;
}
