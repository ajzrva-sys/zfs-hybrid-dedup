/* SPDX-License-Identifier: MIT
 * Native-free contract checks for the actual production retry-queue functions.
 * Only the nvlist transport and the repeated file analysis are replaced.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SYNC_BATCH_QUEUE_SOURCE
#error Supply the extracted production functions in SYNC_BATCH_QUEUE_SOURCE
#endif

typedef unsigned int uint_t;
typedef int boolean_t;
#define B_FALSE 0
#define B_TRUE 1
#define ZFS_MAX_DATASET_NAME_LEN 256
#define TEST_BATCH_LIMIT 256
#define TEST_FILES 600

struct cd_catalog_file {
    char dataset[ZFS_MAX_DATASET_NAME_LEN];
    char path[PATH_MAX];
    struct stat identity;
};
struct cd_retry_file {
    char dataset[ZFS_MAX_DATASET_NAME_LEN];
    char path[PATH_MAX];
    struct stat identity;
};
struct cd_catalog {
    struct cd_retry_file *retry;
    uint_t retry_count;
    uint64_t skipped;
};
typedef struct {
    char op[24];
    uint64_t objects[TEST_BATCH_LIMIT];
    uint_t count;
    int reply;
} nvlist_t;
struct batch_call {
    char dataset[ZFS_MAX_DATASET_NAME_LEN];
    uint64_t objects[TEST_BATCH_LIMIT];
    uint_t count;
};
static struct cd_catalog_file expected[TEST_FILES];
static unsigned analyzed[TEST_FILES], barriers, analyses;
static unsigned output_supplied, replies_allocated, replies_freed;
static unsigned all_output_supplied, all_replies_allocated, all_replies_freed;
static unsigned missing_output;
static struct batch_call calls[8];
static int barrier_error, still_dirty;
static uint64_t expected_age = 7;
static volatile sig_atomic_t cd_stop;

static size_t test_strlcpy(char *dest, const char *source, size_t size)
{
    size_t length = strlen(source);
    assert(size > 0 && length < size);
    memcpy(dest, source, length + 1);
    return length;
}
#define strlcpy test_strlcpy
static nvlist_t *fnvlist_alloc(void)
{
    nvlist_t *nvl = calloc(1, sizeof (*nvl));
    assert(nvl != NULL);
    return nvl;
}
static void nvlist_free(nvlist_t *nvl)
{
    if (nvl != NULL && nvl->reply) {
        replies_freed++;
        all_replies_freed++;
        assert(replies_freed <= replies_allocated);
    }
    free(nvl);
}
static void fnvlist_free(nvlist_t *nvl) { nvlist_free(nvl); }
static void fnvlist_add_string(nvlist_t *nvl, const char *key,
    const char *text)
{
    assert(!strcmp(key, "op") && !strcmp(text, "sync-batch"));
    (void) test_strlcpy(nvl->op, text, sizeof (nvl->op));
}
static void fnvlist_add_uint64_array(nvlist_t *nvl, const char *key,
    const uint64_t *objects, uint_t count)
{
    assert(!strcmp(key, "objects") && count && count <= TEST_BATCH_LIMIT);
    nvl->count = count;
    memcpy(nvl->objects, objects, count * sizeof (*objects));
}
static int lzc_chunkdedup(const char *dataset, nvlist_t *args, nvlist_t **out)
{
    assert(!strcmp(args->op, "sync-batch"));
    /* The real ioctl needs a reply buffer for sync-batch's synced_txg result. */
    if (out == NULL) {
        missing_output++;
        return ENOMEM;
    }
    output_supplied++;
    all_output_supplied++;
    *out = NULL;
    assert(barriers < sizeof (calls) / sizeof (calls[0]));
    struct batch_call *call = &calls[barriers++];
    (void) test_strlcpy(call->dataset, dataset, sizeof (call->dataset));
    call->count = args->count;
    memcpy(call->objects, args->objects, args->count * sizeof (uint64_t));
    /* A batch may contain only the saved identities from its own dataset. */
    for (uint_t i = 0; i < call->count; i++) {
        assert(call->objects[i] > 0 && call->objects[i] < TEST_FILES);
        assert(!strcmp(dataset, expected[call->objects[i]].dataset));
    }
    if (barrier_error != 0) return barrier_error;
    *out = fnvlist_alloc();
    (*out)->reply = 1;
    replies_allocated++;
    all_replies_allocated++;
    return 0;
}

static int cd_catalog_retry(struct cd_catalog *, uint64_t);
static int cd_catalog_analyze(struct cd_catalog *, const char *, const char *,
    uint64_t, const struct stat *, boolean_t);
static boolean_t cd_deferred(int);

#include SYNC_BATCH_QUEUE_SOURCE

static int cd_catalog_analyze(struct cd_catalog *catalog, const char *dataset,
    const char *path, uint64_t age, const struct stat *identity,
    boolean_t retryable)
{
    assert(age == expected_age && !retryable);
    assert(replies_allocated == replies_freed);
    assert(identity != NULL && identity->st_ino > 0 &&
        identity->st_ino < TEST_FILES);
    const struct cd_catalog_file *file = &expected[identity->st_ino];
    assert(!strcmp(dataset, file->dataset) && !strcmp(path, file->path));
    assert(identity->st_dev == file->identity.st_dev &&
        identity->st_size == file->identity.st_size &&
        identity->st_mode == file->identity.st_mode &&
        identity->st_uid == file->identity.st_uid &&
        identity->st_gid == file->identity.st_gid &&
        identity->st_mtime == file->identity.st_mtime &&
        identity->st_ctime == file->identity.st_ctime);
    assert(++analyzed[identity->st_ino] == 1);
    analyses++;
    /* Match analyze's nonretryable EAGAIN disposition, without a filesystem. */
    if (still_dirty) catalog->skipped++;
    return 0;
}

static void reset(struct cd_catalog *catalog)
{
    assert(missing_output == 0 && output_supplied == barriers);
    assert(replies_allocated == replies_freed);
    free(catalog->retry);
    memset(catalog, 0, sizeof (*catalog));
    catalog->retry = calloc(TEST_BATCH_LIMIT, sizeof (*catalog->retry));
    assert(catalog->retry != NULL);
    memset(expected, 0, sizeof (expected));
    memset(analyzed, 0, sizeof (analyzed));
    memset(calls, 0, sizeof (calls));
    barriers = analyses = 0;
    output_supplied = replies_allocated = replies_freed = 0;
    barrier_error = still_dirty = cd_stop = 0;
}
static struct cd_catalog_file file(unsigned id, const char *dataset)
{
    assert(id && id < TEST_FILES);
    struct cd_catalog_file result = {0};
    (void) test_strlcpy(result.dataset, dataset, sizeof (result.dataset));
    assert(snprintf(result.path, sizeof (result.path), "/mock/%s/%u",
        dataset, id) > 0);
    result.identity.st_ino = id;
    result.identity.st_dev = 17;
    result.identity.st_size = 65536 + id;
    result.identity.st_mode = S_IFREG | 0640;
    result.identity.st_uid = 3;
    result.identity.st_gid = 4;
    result.identity.st_mtime = 1500000000 + id;
    result.identity.st_ctime = 1500000100 + id;
    expected[id] = result;
    return result;
}
static void enqueue(struct cd_catalog *catalog, unsigned id,
    const char *dataset)
{
    struct cd_catalog_file next = file(id, dataset);
    assert(cd_catalog_enqueue(catalog, &next, expected_age) == 0);
    /* Retries must use the saved identity, never this caller's later contents. */
    memset(&next, 0xff, sizeof (next));
}

int main(void)
{
    assert(CD_SYNC_BATCH_FILES == TEST_BATCH_LIMIT);
    struct cd_catalog catalog = {0};
    reset(&catalog);
    assert(cd_catalog_retry(&catalog, expected_age) == 0);
    assert(barriers == 0 && analyses == 0);
    puts("PASS empty queue has no durability barrier");

    for (unsigned i = 1; i <= 257; i++) enqueue(&catalog, i, "pool/a");
    assert(barriers == 1 && analyses == 256 && catalog.retry_count == 1);
    assert(calls[0].count == 256);
    for (unsigned i = 0; i < 256; i++) assert(calls[0].objects[i] == i + 1);
    assert(cd_catalog_retry(&catalog, expected_age) == 0);
    assert(barriers == 2 && analyses == 257 && catalog.retry_count == 0);
    assert(calls[1].count == 1 && calls[1].objects[0] == 257);
    assert(cd_catalog_retry(&catalog, expected_age) == 0 && barriers == 2);
    puts("PASS 257 candidates use 256+1 objects and exactly two barriers");

    reset(&catalog);
    enqueue(&catalog, 1, "pool/a"); enqueue(&catalog, 2, "pool/a");
    assert(barriers == 0 && analyses == 0);
    enqueue(&catalog, 3, "pool/b");
    assert(barriers == 1 && analyses == 2 && catalog.retry_count == 1);
    assert(!strcmp(calls[0].dataset, "pool/a") && calls[0].count == 2);
    assert(analyzed[1] == 1 && analyzed[2] == 1 && analyzed[3] == 0);
    assert(!strcmp(catalog.retry[0].dataset, "pool/b"));
    assert(cd_catalog_retry(&catalog, expected_age) == 0);
    assert(barriers == 2 && analyses == 3 && calls[1].count == 1);
    assert(!strcmp(calls[1].dataset, "pool/b"));
    puts("PASS dataset change flushes the prior dataset before queueing the next");

    reset(&catalog);
    for (unsigned i = 1; i <= 3; i++) enqueue(&catalog, i, "pool/a");
    still_dirty = 1;
    assert(cd_catalog_retry(&catalog, expected_age) == 0);
    assert(catalog.retry_count == 0 && catalog.skipped == 3);
    assert(barriers == 1 && analyses == 3);
    assert(cd_catalog_retry(&catalog, expected_age) == 0 && barriers == 1);
    puts("PASS copied identities and one nonretryable analysis per candidate");

    reset(&catalog);
    enqueue(&catalog, 1, "pool/a"); enqueue(&catalog, 2, "pool/a");
    barrier_error = EIO;
    assert(cd_catalog_retry(&catalog, expected_age) == EIO);
    assert(barriers == 1 && analyses == 0 && catalog.retry_count == 2);
    puts("PASS failed durability barrier propagates without retrying files");

    reset(&catalog);
    for (unsigned i = 1; i <= 256; i++) enqueue(&catalog, i, "pool/a");
    cd_stop = 1;
    assert(cd_catalog_retry(&catalog, expected_age) == 0);
    assert(barriers == 0 && analyses == 0 && catalog.retry_count == 256);
    struct cd_catalog_file next = file(257, "pool/a");
    assert(cd_catalog_enqueue(&catalog, &next, expected_age) == ECANCELED);
    assert(barriers == 0 && analyses == 0 && catalog.retry_count == 256);
    puts("PASS cancellation leaves a full queue bounded and performs no barrier");
    free(catalog.retry);
    assert(missing_output == 0 && all_output_supplied > 0);
    assert(all_replies_allocated > 0 &&
        all_replies_allocated == all_replies_freed);
    assert(all_output_supplied > all_replies_allocated);
    puts("PASS reply buffers supplied and freed; failed calls leave NULL replies");
    puts("PASS actual production enqueue/retry contract; native behavior is separate");
    return 0;
}
