/* SPDX-License-Identifier: MIT
 * Current CLI scan/merge functions and real temporary catalogs. Only file
 * analysis and the existing durability retry transport are test doubles.
 */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "chunkdedup_parallel.h"

typedef unsigned int uint_t;
typedef int boolean_t;
#define B_FALSE 0
#define B_TRUE 1
#define ZFS_MAX_DATASET_NAME_LEN 256
#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))
enum cd_method { CD_METHOD_AUTO, CD_METHOD_BLOCK, CD_METHOD_CHUNK };
static volatile sig_atomic_t cd_stop;
#include SCAN_TYPES_SOURCE

static int cd_catalog_analyze(struct cd_catalog *, const char *, const char *,
    uint64_t, const struct stat *, boolean_t);
static int cd_catalog_enqueue(struct cd_catalog *, const struct cd_catalog_file *, uint64_t);
static int cd_catalog_retry(struct cd_catalog *, uint64_t);

static size_t
test_strlcpy(char *destination, const char *source, size_t capacity)
{
    size_t length = strlen(source);
    if (capacity != 0) {
        size_t copied = length < capacity - 1 ? length : capacity - 1;
        memcpy(destination, source, copied);
        destination[copied] = '\0';
    }
    return (length);
}
#define strlcpy test_strlcpy
#include SCAN_FUNCTIONS_SOURCE

static pthread_mutex_t analysis_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t analysis_changed = PTHREAD_COND_INITIALIZER;
static unsigned analyses, completed, active, retry_calls;
static int hold_released;

static int
analysis_done(const char *path, int error)
{
    assert(pthread_mutex_lock(&analysis_lock) == 0);
    completed++;
    active--;
    if (strcmp(path, "release") == 0)
        hold_released = 1;
    assert(pthread_cond_broadcast(&analysis_changed) == 0);
    assert(pthread_mutex_unlock(&analysis_lock) == 0);
    return (error);
}

static int
cd_catalog_analyze(struct cd_catalog *c, const char *dataset, const char *path,
    uint64_t age, const struct stat *identity, boolean_t retryable)
{
    (void)age;
    assert(c->staging && !retryable && c->count == 0 && identity != NULL);
    assert(pthread_mutex_lock(&analysis_lock) == 0);
    analyses++;
    active++;
    assert(pthread_cond_broadcast(&analysis_changed) == 0);
    while ((strcmp(path, "slow") == 0 && completed < 7) ||
        (strcmp(path, "hold") == 0 && !hold_released))
        assert(pthread_cond_wait(&analysis_changed, &analysis_lock) == 0);
    assert(pthread_mutex_unlock(&analysis_lock) == 0);
    if (strcmp(path, "cancel") == 0) {
        while (!atomic_load(c->cancel))
            (void) sched_yield();
        return (analysis_done(path, ECANCELED));
    }
    if (strcmp(path, "skip") == 0) {
        c->skipped++;
        return (analysis_done(path, 0));
    }
    if (strcmp(path, "stale") == 0)
        return (analysis_done(path, ESTALE));
    uint8_t hash[32];
    memset(hash, (int)identity->st_ino, sizeof (hash));
    assert(cd_catalog_append(c->blocks, identity->st_dev, hash, 4096, 0,
        identity->st_ino * 4096, 4096, 128 << 10) == 0);
    assert(cd_catalog_append(c->chunks, identity->st_dev, hash, 4096, 0,
        identity->st_ino * 4096, 8192, 0) == 0);
    /* An unsuccessful worker's partial rows must never reach the catalog. */
    if (strcmp(path, "dirty") == 0)
        return (analysis_done(path, EAGAIN));
    struct cd_catalog_file file = { .identity = *identity,
        .dataset_id = identity->st_dev, .parent = 0 };
    assert(strlcpy(file.dataset, dataset, sizeof (file.dataset)) < sizeof (file.dataset));
    assert(strlcpy(file.path, path, sizeof (file.path)) < sizeof (file.path));
    assert(fwrite(&file, sizeof (file), 1, c->files) == 1);
    c->count = 1;
    return (analysis_done(path, 0));
}

static int
cd_catalog_enqueue(struct cd_catalog *c, const struct cd_catalog_file *file,
    uint64_t age)
{
    (void)age;
    assert(c->retry_count < CD_SYNC_BATCH_FILES);
    struct cd_retry_file *retry = &c->retry[c->retry_count++];
    retry->identity = file->identity;
    assert(strlcpy(retry->dataset, file->dataset, sizeof (retry->dataset)) < sizeof (retry->dataset));
    assert(strlcpy(retry->path, file->path, sizeof (retry->path)) < sizeof (retry->path));
    return (0);
}

static int
cd_catalog_retry(struct cd_catalog *c, uint64_t age)
{
    (void)age;
    retry_calls++;
    for (uint_t i = 0; i < c->retry_count; i++) {
        boolean_t found;
        assert(cd_catalog_seen(c, &c->retry[i].identity, B_TRUE, &found) == 0);
        assert(!found);
    }
    c->retry_count = 0;
    return (0);
}

static struct stat
identity(unsigned inode)
{
    return ((struct stat){ .st_dev = 99, .st_ino = inode,
        .st_mode = S_IFREG | 0600, .st_size = 4096 });
}

static void
check_rows(FILE *stream, struct cd_catalog *catalog, uint64_t bytes,
    uint64_t object)
{
    assert(fseeko(stream, 0, SEEK_SET) == 0);
    for (unsigned i = 0; i < catalog->count; i++) {
        struct cd_catalog_item item;
        struct cd_catalog_file file;
        assert(cd_catalog_next(stream, &item) == 0);
        assert(cd_catalog_record(catalog, i, &file, B_FALSE) == 0);
        assert(item.file == i && item.dataset == 99 && item.length == 4096);
        assert(item.dataset == file.dataset_id && file.parent == item.file);
        assert(item.offset == file.identity.st_ino * 4096);
        assert(item.bytes == bytes && item.object == object);
        char hash[65];
        for (unsigned j = 0; j < 32; j++)
            (void) snprintf(hash + 2 * j, 3, "%02x",
                (unsigned)(uint8_t)file.identity.st_ino);
        assert(strcmp(item.hash, hash) == 0);
    }
    struct cd_catalog_item end;
    assert(cd_catalog_next(stream, &end) == ENOENT);
}

static void
merge_and_links(void)
{
    struct cd_catalog catalog;
    struct cd_scan scan;
    assert(cd_catalog_init(&catalog, CD_METHOD_AUTO, B_FALSE) == 0);
    assert(cd_scan_init(&scan, &catalog, 0, 4) == 0);
    const char *paths[] = { "slow", "skip", "dirty", "stale",
        "good4", "good5", "good6", "good7" };
    for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
        struct stat current = identity(i + 1);
        assert(cd_scan_submit(&scan, "pool/test", paths[i], &current) == 0);
    }
    assert(cd_scan_drain(&scan) == 0);
    assert(catalog.count == 5 && catalog.skipped == 2 && catalog.retry_count == 1);
    assert(catalog.retry[0].identity.st_ino == 3);
    assert(strcmp(catalog.retry[0].path, "dirty") == 0);
    const char *expected[] = { "slow", "good4", "good5", "good6", "good7" };
    const unsigned inodes[] = { 1, 5, 6, 7, 8 };
    unsigned char seen[ARRAY_SIZE(expected)] = { 0 };
    for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
        struct cd_catalog_file file;
        assert(cd_catalog_record(&catalog, i, &file, B_FALSE) == 0);
        unsigned match;
        for (match = 0; match < ARRAY_SIZE(expected); match++)
            if (strcmp(file.path, expected[match]) == 0)
                break;
        assert(match < ARRAY_SIZE(expected) && !seen[match]++);
        assert(file.parent == i && file.identity.st_ino == inodes[match]);
        assert(file.identity.st_dev == 99 && file.dataset_id == 99);
        assert(file.identity.st_size == 4096 && S_ISREG(file.identity.st_mode));
        assert(strcmp(file.dataset, "pool/test") == 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) assert(seen[i] == 1);
    check_rows(catalog.blocks, &catalog, 4096, 128 << 10);
    check_rows(catalog.chunks, &catalog, 8192, 0);
    unsigned before = analyses;
    struct stat same = identity(1);
    assert(cd_scan_submit(&scan, "pool/test", "seen-alias", &same) == 0);
    same = identity(3);
    assert(cd_scan_submit(&scan, "pool/test", "dirty-alias", &same) == 0);
    assert(analyses == before && retry_calls == 1 && catalog.retry_count == 0);
    assert(catalog.skipped == 4);
    /* A second name arriving while its first name scans must be drained first. */
    struct stat held = identity(77), release = identity(78);
    assert(cd_scan_submit(&scan, "pool/test", "hold", &held) == 0);
    assert(cd_scan_submit(&scan, "pool/test", "release", &release) == 0);
    assert(cd_scan_submit(&scan, "pool/test", "active-alias", &held) == 0);
    assert(cd_scan_drain(&scan) == 0);
    assert(analyses == before + 2 && catalog.count == 7 && catalog.skipped == 5);
    cd_scan_fini(&scan);
    assert(active == 0);
    cd_catalog_fini(&catalog);
    puts("PASS actual CLI merge assigns unique consistent global IDs and row IDs");
    puts("PASS skipped/stale workers and partial dirty rows do not enter the catalog");
    puts("PASS dirty results reach central retry queue; pending retry aliases flush once");
    puts("PASS active and completed hard links avoid a second optimizer analysis");
}

static void
copy_rejects_foreign_ids(void)
{
    FILE *source = tmpfile(), *destination = tmpfile();
    assert(source != NULL && destination != NULL);
    uint8_t hash[32] = { 0 };
    assert(cd_catalog_append(source, 1, hash, 4096, 8, 0, 4096, 0) == 0);
    assert(cd_scan_copy(source, destination, 99) == EPROTO);
    cd_stop = 1;
    assert(cd_scan_copy(source, destination, 99) == ECANCELED);
    cd_stop = 0;
    assert(fclose(source) == 0 && fclose(destination) == 0);
    puts("PASS current spool merge rejects nonlocal IDs and honors cancellation");
}

static void
cancel_before_spool_cleanup(void)
{
    struct cd_catalog catalog;
    struct cd_scan scan;
    assert(cd_catalog_init(&catalog, CD_METHOD_BLOCK, B_FALSE) == 0);
    assert(cd_scan_init(&scan, &catalog, 0, 1) == 0);
    unsigned before = analyses;
    struct stat current = identity(90);
    assert(cd_scan_submit(&scan, "pool/test", "cancel", &current) == 0);
    assert(pthread_mutex_lock(&analysis_lock) == 0);
    while (analyses == before)
        assert(pthread_cond_wait(&analysis_changed, &analysis_lock) == 0);
    assert(active == 1);
    assert(pthread_mutex_unlock(&analysis_lock) == 0);
    cd_scan_fini(&scan);
    assert(active == 0 && completed == analyses && catalog.count == 0);
    cd_catalog_fini(&catalog);
    puts("PASS actual CLI finalizer cancels and joins active analysis before spool cleanup");
}

int
main(void)
{
    (void) alarm(30);
    merge_and_links();
    copy_rejects_foreign_ids();
    cancel_before_spool_cleanup();
    assert(pthread_cond_destroy(&analysis_changed) == 0);
    assert(pthread_mutex_destroy(&analysis_lock) == 0);
    return (0);
}
