/* SPDX-License-Identifier: MIT
 * Actual userspace share driver/core. Only descriptors, nvlist/ioctl transport,
 * failure injection and the clock boundary are doubles; catalogs and the
 * bounded pthread pool are real. No ZFS, host, VM or network operation occurs.
 */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int fixture_thread_create(pthread_t *, const pthread_attr_t *,
    void *(*)(void *), void *);
#define pthread_create fixture_thread_create
#include "chunkdedup_parallel.h"
#undef pthread_create
#include "chunkdedup_hybrid.h"

typedef unsigned int uint_t;
typedef int boolean_t;
#define B_FALSE 0
#define B_TRUE 1
#define ZFS_MAX_DATASET_NAME_LEN 256
#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
static atomic_int cd_stop;
#include "types.inc"
#include "catalog.inc"

enum { FILES = 128, DESCRIPTORS = 256, BASE_FD = 10000 };
static struct cd_catalog_file model[FILES];
static int descriptors[DESCRIPTORS];
static unsigned opened[FILES], open_total, close_total, peak_open;
static unsigned calls, active_calls, peak_calls, finished_calls;
static unsigned record_calls, fail_record_at, fail_alloc_at, alloc_calls;
static unsigned thread_creations, fail_thread_at, outstanding_allocations;
static size_t largest_allocation;
static int reject_large_allocation, native_large_mode, native_resource_abort;
static void *allocations[1024];
static int open_error[FILES], ioctl_error[FILES], stale_file = -1;
static int pool_error, enabled = 1, enabled_error;
static int hold_calls, release_calls, stop_on_call;
static unsigned barrier_calls;
static uint64_t expected_source_offset;
static int verify_source_offset;
static unsigned progress_lines, threshold_lines, timed_waits, release_on_progress;
static int controlled_wait;
static atomic_long clock_advance;
static char output_log[65536], error_log[65536];
static size_t output_used, error_used;
static pthread_t coordinator;
static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_changed = PTHREAD_COND_INITIALIZER;

static uint64_t fixture_free_memory(void)
{
#ifdef __FreeBSD__
    union { uint32_t small; uint64_t large; } pages = {0};
    unsigned int page_size = 0;
    size_t size = sizeof (pages);
    if (sysctlbyname("vm.stats.vm.v_free_count", &pages, &size, NULL, 0) != 0)
        return 0;
    uint64_t count = size == sizeof (pages.small) ? pages.small : pages.large;
    size = sizeof (page_size);
    if (sysctlbyname("hw.pagesize", &page_size, &size, NULL, 0) != 0 ||
        !page_size || count > UINT64_MAX / page_size)
        return 0;
    return count * page_size;
#else
    return 0;
#endif
}

static int fixture_thread_create(pthread_t *thread, const pthread_attr_t *attributes,
    void *(*callback)(void *), void *argument)
{
    thread_creations++;
    if (fail_thread_at && thread_creations == fail_thread_at) return EAGAIN;
    return pthread_create(thread, attributes, callback, argument);
}

typedef struct nvlist {
    uint64_t source_object, object, source_offset, offset, length;
} nvlist_t;
static nvlist_t *fnvlist_alloc(void) { return calloc(1, sizeof (nvlist_t)); }
static void fnvlist_free(nvlist_t *value) { free(value); }
static void nvlist_free(nvlist_t *value) { free(value); }
static void fnvlist_add_string(nvlist_t *list, const char *key, const char *value)
{ (void)list; assert(!strcmp(key, "op") && !strcmp(value, "share-blocks")); }
static void fnvlist_add_uint64(nvlist_t *list, const char *key, uint64_t value)
{
    if (!strcmp(key, "source_object")) list->source_object = value;
    else if (!strcmp(key, "object")) list->object = value;
    else if (!strcmp(key, "source_offset")) list->source_offset = value;
    else if (!strcmp(key, "offset")) list->offset = value;
    else if (!strcmp(key, "length")) list->length = value;
}

static unsigned file_index(const char *path)
{
    unsigned id;
    assert(sscanf(path, "file/%u", &id) == 1 && id < FILES);
    return id;
}

static int fixture_open(const char *path, int flags, ...)
{
    (void)flags;
    unsigned id = file_index(path);
    assert(!pthread_mutex_lock(&fixture_lock));
    if (open_error[id]) {
        errno = open_error[id];
        assert(!pthread_mutex_unlock(&fixture_lock));
        return -1;
    }
    for (unsigned i = 0; i < FILES; i++)
        if (model[i].identity.st_dev == model[id].identity.st_dev &&
            model[i].identity.st_ino == model[id].identity.st_ino)
            assert(opened[i] == 0); /* All-file identity exclusion. */
    unsigned slot;
    for (slot = 0; slot < DESCRIPTORS && descriptors[slot] != -1; slot++) {}
    assert(slot < DESCRIPTORS);
    descriptors[slot] = (int)id;
    opened[id]++;
    open_total++;
    unsigned live = open_total - close_total;
    if (live > peak_open) peak_open = live;
    assert(!pthread_mutex_unlock(&fixture_lock));
    return BASE_FD + (int)slot;
}

static int fixture_fstat(int fd, struct stat *identity)
{
    assert(!pthread_mutex_lock(&fixture_lock));
    assert(fd >= BASE_FD && fd < BASE_FD + DESCRIPTORS);
    int id = descriptors[fd - BASE_FD];
    assert(id >= 0);
    *identity = model[id].identity;
    if (id == stale_file) identity->st_mtime++;
    errno = EDOM; /* Actual open/identity check returns ESTALE, not errno. */
    assert(!pthread_mutex_unlock(&fixture_lock));
    return 0;
}

static int fixture_close(int fd)
{
    assert(!pthread_mutex_lock(&fixture_lock));
    assert(fd >= BASE_FD && fd < BASE_FD + DESCRIPTORS);
    int id = descriptors[fd - BASE_FD];
    assert(id >= 0 && opened[id] == 1);
    opened[id]--;
    descriptors[fd - BASE_FD] = -1;
    close_total++;
    errno = ERANGE;
    assert(!pthread_cond_broadcast(&fixture_changed));
    assert(!pthread_mutex_unlock(&fixture_lock));
    return 0;
}

static int lzc_chunkdedup(const char *dataset, nvlist_t *list, nvlist_t **out)
{
    (void)dataset; (void)out;
    assert(list->object >= 100 && list->object < 100 + FILES);
    unsigned id = (unsigned)(list->object - 100);
    assert(!pthread_mutex_lock(&fixture_lock));
    calls++; active_calls++;
    if (active_calls > peak_calls) peak_calls = active_calls;
    assert(list->length > 0);
    if (verify_source_offset) assert(list->source_offset == expected_source_offset);
    assert(!pthread_cond_broadcast(&fixture_changed));
    if (barrier_calls) {
        while (calls < barrier_calls)
            assert(!pthread_cond_wait(&fixture_changed, &fixture_lock));
    }
    while (hold_calls && !release_calls)
        assert(!pthread_cond_wait(&fixture_changed, &fixture_lock));
    if (stop_on_call) atomic_store(&cd_stop, 1);
    active_calls--; finished_calls++;
    int error = ioctl_error[id];
    assert(!pthread_cond_broadcast(&fixture_changed));
    assert(!pthread_mutex_unlock(&fixture_lock));
    errno = ENOTTY;
    return error;
}

static int cd_enabled(const char *dataset, boolean_t *value)
{
    (void)dataset;
    assert(pthread_equal(pthread_self(), coordinator));
    *value = enabled;
    return enabled_error;
}

#define open fixture_open
#define fstat fixture_fstat
#define close fixture_close
#include "transport.inc"
#undef open
#undef fstat
#undef close

static int fixture_record(struct cd_catalog *catalog, uint64_t id,
    struct cd_catalog_file *file, boolean_t write)
{
    assert(pthread_equal(pthread_self(), coordinator));
    record_calls++;
    if (native_large_mode && record_calls % 65536 == 0 &&
        fixture_free_memory() < (UINT64_C(8) << 30)) {
        native_resource_abort = 1;
        return ENOMEM;
    }
    if (fail_record_at && record_calls == fail_record_at) return EIO;
    return cd_catalog_record(catalog, id, file, write);
}

static void *fixture_malloc(size_t size)
{
    alloc_calls++;
    if (size > largest_allocation) largest_allocation = size;
    if (reject_large_allocation && size > (16U << 20)) return NULL;
    if (fail_alloc_at && alloc_calls == fail_alloc_at) return NULL;
    void *pointer = malloc(size);
    if (pointer) {
        unsigned slot;
        for (slot = 0; slot < ARRAY_SIZE(allocations) && allocations[slot]; slot++) {}
        assert(slot < ARRAY_SIZE(allocations));
        allocations[slot] = pointer; outstanding_allocations++;
    }
    return pointer;
}
static void *fixture_calloc(size_t count, size_t size)
{
    assert(count == 0 || size <= SIZE_MAX / count);
    void *pointer = fixture_malloc(count * size);
    if (pointer) memset(pointer, 0, count * size);
    return pointer;
}
static void fixture_free(void *pointer)
{
    if (!pointer) return;
    unsigned slot;
    for (slot = 0; slot < ARRAY_SIZE(allocations) && allocations[slot] != pointer; slot++) {}
    assert(slot < ARRAY_SIZE(allocations) && outstanding_allocations > 0);
    allocations[slot] = NULL; outstanding_allocations--;
    free(pointer);
}
static int fixture_pool_init(struct cd_parallel *pool, unsigned threads,
    int (*callback)(void *))
{
    if (pool_error) return pool_error;
    return cd_parallel_init_unordered(pool, threads, callback);
}

static int fixture_printf(const char *format, ...)
{
    assert(pthread_equal(pthread_self(), coordinator));
    va_list arguments; va_start(arguments, format);
    assert(!pthread_mutex_lock(&fixture_lock));
    int count = vsnprintf(output_log + output_used, sizeof (output_log) - output_used,
        format, arguments);
    assert(count >= 0 && (size_t)count < sizeof (output_log) - output_used);
    output_used += (size_t)count;
    progress_lines++;
    if (calls >= 1000) threshold_lines++;
    if (release_on_progress && active_calls == release_on_progress && progress_lines > 1) {
        release_calls = 1;
        assert(!pthread_cond_broadcast(&fixture_changed));
    }
    assert(!pthread_mutex_unlock(&fixture_lock));
    va_end(arguments);
    return count;
}
static int fixture_fprintf(FILE *stream, const char *format, ...)
{
    assert(stream == stderr && pthread_equal(pthread_self(), coordinator));
    va_list arguments; va_start(arguments, format);
    int count = vsnprintf(error_log + error_used, sizeof (error_log) - error_used,
        format, arguments);
    assert(count >= 0 && (size_t)count < sizeof (error_log) - error_used);
    error_used += (size_t)count; va_end(arguments);
    return count;
}
static int fixture_clock_gettime(clockid_t clock, struct timespec *value)
{
    int error = clock_gettime(clock, value);
    value->tv_sec += atomic_load(&clock_advance);
    return error;
}
static int fixture_timedwait(pthread_cond_t *condition, pthread_mutex_t *lock,
    const struct timespec *deadline)
{
    timed_waits++;
    if (controlled_wait) {
        /* Release the notification mutex exactly as a real wait does, while
         * the controlled clock reaches the next deadline without a sleep. */
        assert(!pthread_mutex_unlock(lock));
        (void)sched_yield();
        assert(!pthread_mutex_lock(&fixture_lock));
        if (!release_on_progress || active_calls == release_on_progress)
            atomic_fetch_add(&clock_advance, 31);
        assert(!pthread_mutex_unlock(&fixture_lock));
        assert(!pthread_mutex_lock(lock));
        return ETIMEDOUT;
    }
#ifdef __APPLE__
    return pthread_cond_timedwait_relative_np(condition, lock, deadline);
#else
    return pthread_cond_timedwait(condition, lock, deadline);
#endif
}

#define close fixture_close
#define cd_catalog_record fixture_record
#define malloc fixture_malloc
#define calloc fixture_calloc
#define free fixture_free
#define cd_parallel_init_unordered fixture_pool_init
#define printf fixture_printf
#define fprintf fixture_fprintf
#define clock_gettime fixture_clock_gettime
#define pthread_cond_timedwait fixture_timedwait
#define pthread_cond_timedwait_relative_np fixture_timedwait
#include "share.inc"
#undef close
#undef cd_catalog_record
#undef malloc
#undef calloc
#undef free
#undef cd_parallel_init_unordered
#undef printf
#undef fprintf
#undef clock_gettime
#undef pthread_cond_timedwait
#undef pthread_cond_timedwait_relative_np

static void reset(void)
{
    assert(active_calls == 0 && open_total == close_total);
    assert(outstanding_allocations == 0);
    memset(model, 0, sizeof (model));
    memset(opened, 0, sizeof (opened));
    memset(open_error, 0, sizeof (open_error));
    memset(ioctl_error, 0, sizeof (ioctl_error));
    for (unsigned i = 0; i < DESCRIPTORS; i++) descriptors[i] = -1;
    calls = active_calls = peak_calls = finished_calls = 0;
    open_total = close_total = peak_open = 0;
    record_calls = fail_record_at = fail_alloc_at = alloc_calls = 0;
    thread_creations = fail_thread_at = 0;
    largest_allocation = 0;
    reject_large_allocation = native_large_mode = native_resource_abort = 0;
    stale_file = -1;
    pool_error = enabled_error = hold_calls = release_calls = stop_on_call = 0;
    enabled = 1;
    barrier_calls = 0;
    verify_source_offset = 0;
    progress_lines = threshold_lines = timed_waits = release_on_progress = 0;
    controlled_wait = 0; atomic_store(&clock_advance, 0);
    output_log[0] = error_log[0] = '\0'; output_used = error_used = 0;
    atomic_store(&cd_stop, 0);
    coordinator = pthread_self();
}

static struct cd_catalog catalog(unsigned files)
{
    assert(files <= FILES);
    struct cd_catalog c = { .count = files, .method = CD_METHOD_BLOCK };
    c.files = tmpfile(); c.sorted_blocks = tmpfile();
    assert(c.files && c.sorted_blocks);
    for (unsigned i = 0; i < files; i++) {
        struct cd_catalog_file *file = &model[i];
        assert(snprintf(file->path, sizeof (file->path), "file/%u", i) > 0);
        strcpy(file->dataset, "fixture/test");
        file->identity = (struct stat){ .st_dev = 1, .st_ino = 100 + i,
            .st_mode = S_IFREG | 0600, .st_size = INT32_MAX, .st_mtime = 1234 };
        file->method = CD_METHOD_BLOCK;
        file->block_size = 4096;
        file->parent = i;
        assert(fwrite(file, sizeof (*file), 1, c.files) == 1);
    }
    return c;
}

static void item(struct cd_catalog *c, unsigned group, unsigned file,
    uint64_t offset, uint64_t object)
{
    assert(file < c->count);
    assert(fprintf(c->sorted_blocks,
        "%020u %064x %020u %020u %020" PRIu64 " %020u %020" PRIu64 "\n",
        1, group, 4096, file, offset, 4096, object) > 0);
}

static int run(struct cd_catalog *c, unsigned threads)
{
    assert(!fflush(c->files) && !fflush(c->sorted_blocks));
    rewind(c->sorted_blocks);
    int error = cd_catalog_share(c, threads);
    assert(active_calls == 0 && open_total == close_total);
    assert(outstanding_allocations == 0);
    for (unsigned i = 0; i < FILES; i++) assert(opened[i] == 0);
    return error;
}

static void finish_catalog(struct cd_catalog *c)
{ assert(!fclose(c->files) && !fclose(c->sorted_blocks)); }

static void independent_groups(void)
{
    reset();
    struct cd_catalog c = catalog(8);
    for (unsigned group = 0; group < 4; group++) {
        item(&c, group, group * 2, 4096, 4096);
        item(&c, group, group * 2 + 1, 8192, 4096);
    }
    barrier_calls = 4;
    assert(run(&c, 4) == 0 && calls == 4 && peak_calls == 4);
    finish_catalog(&c);
    puts("PASS actual share driver overlaps disjoint groups");
}

static void conflicting_groups(void)
{
    reset();
    struct cd_catalog c = catalog(4);
    /* Different sources, crossed source/destination, and an inode alias. */
    model[3].identity.st_ino = model[0].identity.st_ino;
    assert(!cd_catalog_record(&c, 3, &model[3], B_TRUE));
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    item(&c, 2, 2, 4096, 4096); item(&c, 2, 0, 8192, 4096);
    item(&c, 3, 1, 4096, 4096); item(&c, 3, 2, 8192, 4096);
    item(&c, 4, 3, 4096, 4096); item(&c, 4, 1, 8192, 4096);
    assert(run(&c, 4) == 0 && calls == 4 && peak_calls == 1);
    finish_catalog(&c);
    puts("PASS all-file reservations exclude source/destination and inode aliases");
}

static void chunk_boundaries(void)
{
    const unsigned sizes[] = {8191, 8192, 8193};
    for (unsigned test = 0; test < ARRAY_SIZE(sizes); test++) {
        reset();
        struct cd_catalog c = catalog(2);
        verify_source_offset = 1; expected_source_offset = 123 * 4096;
        item(&c, 1, 0, expected_source_offset, 4096);
        for (unsigned i = 1; i < sizes[test]; i++)
            item(&c, 1, 1, (uint64_t)i * 4096, 4096);
        assert(run(&c, 4) == 0 && calls == sizes[test] - 1);
        finish_catalog(&c);
    }
    puts("PASS 8191/8192/8193 items retain nonzero canonical source offsets");
}

static void deferred_and_filtered(void)
{
    reset();
    struct cd_catalog c = catalog(5);
    stale_file = 0;
    ioctl_error[2] = EAGAIN;
    ioctl_error[3] = EILSEQ;
    model[4].method = CD_METHOD_CHUNK;
    assert(!cd_catalog_record(&c, 4, &model[4], B_TRUE));
    item(&c, 1, 0, 4096, 4096); /* ESTALE while errno deliberately says EDOM. */
    item(&c, 1, 1, 8192, 4096); /* Canonical source. */
    item(&c, 1, 1, 8192, 4096); /* Same source block is excluded. */
    item(&c, 1, 2, 16384, 8192); /* Incompatible geometry is excluded. */
    item(&c, 1, 2, 16384, 4096);
    item(&c, 1, 3, 16384, 4096);
    item(&c, 1, 4, 16384, 4096); /* Selected CDC file is excluded. */
    assert(run(&c, 4) == 0 && calls == 2);
    struct cd_catalog_file file;
    assert(!cd_catalog_record(&c, 0, &file, B_FALSE) && file.deferred);
    assert(!cd_catalog_record(&c, 2, &file, B_FALSE) && file.deferred);
    assert(!cd_catalog_record(&c, 3, &file, B_FALSE) && !file.deferred);
    assert(!strstr(error_log, "file/0:"));
    char expected[256];
    assert(snprintf(expected, sizeof (expected), "file/2: deferred (%s)\n", strerror(EAGAIN)) > 0);
    assert(strstr(error_log, expected));
    assert(strstr(error_log, "file/3: unequal candidate left ordinary\n"));
    assert(strstr(output_log, "deferred=2 unequal=1"));
    finish_catalog(&c);

    reset(); c = catalog(1); open_error[0] = EBUSY;
    item(&c, 1, 0, 4096, 4096);
    assert(run(&c, 4) == 0 && calls == 0);
    assert(!cd_catalog_record(&c, 0, &file, B_FALSE) && file.deferred);
    assert(error_log[0] == '\0');
    finish_catalog(&c);

    reset(); c = catalog(3); open_error[0] = EBUSY;
    for (unsigned i = 0; i < 8192; i++) item(&c, 1, 0, i * 4096, 4096);
    item(&c, 1, 1, 123 * 4096, 4096);
    item(&c, 1, 2, 456 * 4096, 4096);
    verify_source_offset = 1; expected_source_offset = 123 * 4096;
    assert(run(&c, 4) == 0 && calls == 1);
    assert(!cd_catalog_record(&c, 0, &file, B_FALSE) && file.deferred);
    finish_catalog(&c);
    puts("PASS source deferrals, singleton side effects, geometry and cross-chunk source selection");
}

static void fatal_and_stop(void)
{
    reset();
    struct cd_catalog c = catalog(8);
    for (unsigned i = 0; i < 4; i++) {
        item(&c, i + 1, i * 2, 4096, 4096);
        item(&c, i + 1, i * 2 + 1, 8192, 4096);
    }
    ioctl_error[1] = EIO;
    assert(run(&c, 1) == EIO && calls == 1);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    ioctl_error[1] = ENOENT;
    assert(run(&c, 4) == ENOENT && calls == 1);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    ioctl_error[1] = EIO; stop_on_call = 1;
    assert(run(&c, 4) == 0 && calls == 1);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    atomic_store(&cd_stop, 1);
    assert(run(&c, 4) == 0 && calls == 0);
    finish_catalog(&c);
    puts("PASS fatal errors propagate, queued work stops and stop dominates");
}

static void operational_enoent(void)
{
    for (unsigned watch = 0; watch < 2; watch++) {
        for (unsigned failure = 0; failure < 3; failure++) {
            reset();
            struct cd_catalog c = catalog(8);
            c.watch = watch;
            for (unsigned group = 0; group < 4; group++) {
                item(&c, group, group * 2, 4096, 4096);
                item(&c, group, group * 2 + 1, 8192, 4096);
            }
            if (failure < 2) open_error[failure] = ENOENT;
            else ioctl_error[1] = ENOENT;
            assert(run(&c, 1) == ENOENT);
            assert(calls == (failure == 2 ? 1U : 0U));
            for (unsigned file = 0; file < 8; file++) {
                struct cd_catalog_file record;
                assert(!cd_catalog_record(&c, file, &record, B_FALSE));
                assert(!record.deferred);
            }
            finish_catalog(&c);
        }
        /* Genuine EOF remains success after all groups, including watch mode. */
        reset();
        struct cd_catalog c = catalog(8);
        c.watch = watch;
        for (unsigned group = 0; group < 4; group++) {
            item(&c, group, group * 2, 4096, 4096);
            item(&c, group, group * 2 + 1, 8192, 4096);
        }
        assert(run(&c, 4) == 0 && calls == 4);
        finish_catalog(&c);
        reset(); c = catalog(0); c.watch = watch;
        assert(run(&c, 4) == 0 && calls == 0);
        finish_catalog(&c);
        reset(); c = catalog(2); c.watch = watch;
        item(&c, 0, 0, 4096, 4096); item(&c, 0, 1, 8192, 4096);
        ioctl_error[1] = ENOENT; stop_on_call = 1;
        assert(run(&c, 4) == 0 && calls == 1);
        finish_catalog(&c);
    }
    /* All four native calls are in flight when ENOENT is returned; run()
     * verifies that every callback, descriptor and allocation is drained. */
    reset();
    struct cd_catalog c = catalog(8);
    for (unsigned group = 0; group < 4; group++) {
        item(&c, group, group * 2, 4096, 4096);
        item(&c, group, group * 2 + 1, 8192, 4096);
    }
    barrier_calls = 4; ioctl_error[1] = ENOENT;
    assert(run(&c, 4) == ENOENT && calls == 4 && peak_calls == 4);
    assert(finished_calls == calls);
    finish_catalog(&c);
    puts("PASS multi-group open/native ENOENT differs from EOF in parallel/watch modes; stop wins and concurrent work drains");
}

static void coordinator_failures(void)
{
    /* Every catalog-operation failure is injected once; cleanup must always
     * reclaim callbacks, file descriptors and reservations before returning. */
    reset();
    struct cd_catalog c = catalog(8);
    for (unsigned i = 0; i < 4; i++) {
        item(&c, i + 1, i * 2, 4096, 4096);
        item(&c, i + 1, i * 2 + 1, 8192, 4096);
    }
    ioctl_error[1] = EAGAIN;
    assert(run(&c, 4) == 0);
    unsigned records = record_calls;
    finish_catalog(&c);
    assert(records > 8);
    for (unsigned failure = 1; failure <= records; failure++) {
        reset(); c = catalog(8);
        for (unsigned i = 0; i < 4; i++) {
            item(&c, i + 1, i * 2, 4096, 4096);
            item(&c, i + 1, i * 2 + 1, 8192, 4096);
        }
        ioctl_error[1] = EAGAIN; fail_record_at = failure;
        assert(run(&c, 4) == EIO);
        finish_catalog(&c);
    }
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    assert(fputs("malformed catalog record\n", c.sorted_blocks) >= 0);
    assert(run(&c, 4) != 0);
    finish_catalog(&c);
    puts("PASS coordinator snapshot/merge/parse failures drain all descriptors and jobs");
}

static void fallback_and_initialization(void)
{
    reset();
    struct cd_catalog c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    c.watch = B_TRUE;
    assert(run(&c, 4) == 0 && calls == 1);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    c.watch = B_TRUE; enabled = 0;
    assert(run(&c, 4) == ECANCELED && calls == 0);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    fail_alloc_at = 1;
    assert(run(&c, 4) == 0 && calls == 1);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    c.count = UINT64_C(1) << 50;
    assert(run(&c, 4) == 0 && calls == 1);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    pool_error = EAGAIN;
    assert(run(&c, 4) == EAGAIN && calls == 0);
    finish_catalog(&c);
    reset(); c = catalog(2);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    fail_thread_at = 2;
    assert(run(&c, 4) == EAGAIN && calls == 0 && thread_creations == 2);
    finish_catalog(&c);
    reset(); c = catalog(0);
    assert(run(&c, 4) == 0 && thread_creations == 0 && alloc_calls == 0);
    finish_catalog(&c);
    puts("PASS watch/disabled, allocation/cap fallback and pool initialization failure");
}

static void allocation_failures(void)
{
    reset();
    struct cd_catalog c = catalog(8);
    for (unsigned i = 0; i < 4; i++) {
        item(&c, i + 1, i * 2, 4096, 4096);
        item(&c, i + 1, i * 2 + 1, 8192, 4096);
    }
    assert(run(&c, 4) == 0);
    unsigned count = alloc_calls;
    finish_catalog(&c);
    assert(count > 4);
    for (unsigned failure = 2; failure <= count; failure++) {
        reset(); c = catalog(8);
        for (unsigned i = 0; i < 4; i++) {
            item(&c, i + 1, i * 2, 4096, 4096);
            item(&c, i + 1, i * 2 + 1, 8192, 4096);
        }
        fail_alloc_at = failure;
        assert(run(&c, 4) == ENOMEM);
        finish_catalog(&c);
    }
    puts("PASS every driver allocation failure frees submitted and unsubmitted storage");
}

/* Exercise the default arithmetic without allocating multi-GiB buffers in the
 * normal strict/sanitized suite. The allocator records and refuses the request.
 */
static void snapshot_cap_boundaries(void)
{
    assert(CD_SHARE_SNAPSHOT_LIMIT == (UINT64_C(8) << 30));
    uint64_t last = CD_SHARE_SNAPSHOT_LIMIT / sizeof (struct cd_catalog_file);
    const uint64_t counts[] = {2569137, last, last + 1, UINT64_MAX};
    for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
        reset();
        struct cd_catalog c = catalog(2);
        c.count = counts[i];
        reject_large_allocation = 1;
        item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
        assert(run(&c, 4) == 0 && calls == 1 && thread_creations == 0);
        assert(strstr(output_log, "serial (file snapshot unavailable)"));
        if (counts[i] <= last) {
            assert(alloc_calls == 1);
            assert(largest_allocation == counts[i] * sizeof (struct cd_catalog_file));
        } else {
            assert(alloc_calls == 0 && largest_allocation == 0);
        }
        finish_catalog(&c);
    }
    puts("PASS default 8 GiB snapshot admission, exact count boundary and overflow-safe no-allocation fallback");
}

/* A separately compiled small cap proves the successful snapshot path at its
 * actual record boundary, alongside over-cap and malloc-failure fallback.
 */
static void small_snapshot_boundaries(void)
{
    assert(CD_SHARE_SNAPSHOT_LIMIT == UINT64_C(4) * sizeof (struct cd_catalog_file));
    for (unsigned count = 3; count <= 5; count++) {
        reset();
        struct cd_catalog c = catalog(count);
        item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
        assert(run(&c, 4) == 0 && calls == 1);
        if (count <= 4) {
            assert(thread_creations == 4 && record_calls == count);
            assert(!strstr(output_log, "serial"));
        } else {
            assert(thread_creations == 0 && alloc_calls == 0);
            assert(strstr(output_log, "serial (file snapshot unavailable)"));
        }
        finish_catalog(&c);
    }
    reset();
    struct cd_catalog c = catalog(4);
    item(&c, 1, 0, 4096, 4096); item(&c, 1, 1, 8192, 4096);
    fail_alloc_at = 1;
    assert(run(&c, 4) == 0 && calls == 1 && thread_creations == 0);
    assert(strstr(output_log, "serial (file snapshot unavailable)"));
    finish_catalog(&c);
    puts("PASS bounded actual snapshot copy below/at cap; above-cap and allocation failure retain serial behavior");
}

static int native_large_snapshot(void)
{
    uint64_t available = fixture_free_memory();
    if (available < (UINT64_C(16) << 30)) {
        fprintf(stderr, "native large snapshot requires FreeBSD and 16 GiB freshly free RAM\n");
        return 2;
    }
    reset();
    const uint64_t count = 2569137;
    const uint64_t bytes = count * sizeof (struct cd_catalog_file);
    assert(sizeof (struct cd_catalog_file) == 1608 && bytes == UINT64_C(4131172296));
    assert(CD_SHARE_SNAPSHOT_LIMIT == (UINT64_C(8) << 30) && bytes <= CD_SHARE_SNAPSHOT_LIMIT);
    struct cd_catalog c = catalog(0);
    c.count = count;
    assert(!ftruncate(fileno(c.files), (off_t)bytes));
    native_large_mode = 1;
    int result = run(&c, 40);
    unsigned copied = record_calls;
    size_t requested = largest_allocation;
    int guarded_abort = native_resource_abort;
    int admitted = !strstr(output_log, "serial") && thread_creations == 40;
    uint64_t free_after = fixture_free_memory();
    struct rusage usage;
    assert(!getrusage(RUSAGE_SELF, &usage));
    assert(calls == 0 && open_total == 0 && outstanding_allocations == 0);
    finish_catalog(&c);

    reset(); c = catalog(0);
    c.count = CD_SHARE_SNAPSHOT_LIMIT / sizeof (struct cd_catalog_file) + 1;
    reject_large_allocation = 1;
    int over_result = run(&c, 40);
    int over_no_alloc = over_result == 0 && alloc_calls == 0 && record_calls == 0 &&
        thread_creations == 0 && strstr(output_log, "serial (file snapshot unavailable)");
    finish_catalog(&c);
    int passed = result == 0 && admitted && copied == count && requested == bytes &&
        !guarded_abort && over_no_alloc;
    printf("{\"status\":\"%s\",\"scope\":\"sparse snapshot capacity and cleanup only; no share ioctls\","
        "\"record_size\":%zu,\"records\":%" PRIu64 ",\"snapshot_bytes\":%" PRIu64
        ",\"allocation_request\":%zu,\"records_read\":%u,\"free_before_bytes\":%" PRIu64
        ",\"free_after_bytes\":%" PRIu64 ",\"peak_rss_kib\":%ld,\"resource_guard_aborted\":%s,"
        "\"over_cap_no_allocation\":%s,\"share_ioctls\":0,\"cleanup_passed\":true}\n",
        passed ? "PASS" : "FAIL", sizeof (struct cd_catalog_file), count, bytes,
        requested, copied, available, free_after, usage.ru_maxrss,
        guarded_abort ? "true" : "false", over_no_alloc ? "true" : "false");
    return passed ? 0 : 1;
}

static void share_state_init(struct cd_share_state *state, unsigned count)
{
    *state = (struct cd_share_state){ .files = model, .count = count };
    atomic_init(&state->first_error, 0);
    atomic_init(&state->groups, 0); atomic_init(&state->issued, 0);
    atomic_init(&state->deferred, 0); atomic_init(&state->unequal, 0);
    atomic_init(&state->reported_issued, 0);
    assert(!pthread_mutex_init(&state->lock, NULL));
    assert(!pthread_cond_init(&state->completed, NULL));
    assert(!clock_gettime(CLOCK_MONOTONIC, &state->reported_at));
}

static void share_state_fini(struct cd_share_state *state)
{
    assert(state->pending == 0);
    assert(!pthread_cond_destroy(&state->completed));
    assert(!pthread_mutex_destroy(&state->lock));
}

static void first_error_and_notifications(void)
{
    reset();
    struct cd_share_state state;
    share_state_init(&state, 0);
    assert(cd_share_error(&state, ENOENT) == ENOENT);
    assert(cd_share_error(&state, EIO) == EIO);
    assert(atomic_load(&state.first_error) == ENOENT);
    assert(cd_share_result(atomic_load(&state.first_error)) == ENOENT);
    atomic_store(&state.first_error, EIO);
    atomic_store(&cd_stop, 1);
    assert(cd_share_result(atomic_load(&state.first_error)) == 0);
    atomic_store(&cd_stop, 0);
    struct cd_share_job skipped = { .state = &state };
    assert(cd_share_work(&skipped) == 0 && calls == 0 && state.pending == 1);
    state.pending = 0;
    share_state_fini(&state);

    reset();
    struct cd_catalog c = catalog(4);
    share_state_init(&state, 4);
    struct cd_parallel pool;
    assert(!cd_parallel_init_unordered(&pool, 2, cd_share_work));
    struct cd_share_job jobs[2] = {0};
    for (unsigned i = 0; i < ARRAY_SIZE(jobs); i++) {
        jobs[i] = (struct cd_share_job){ .state = &state, .count = 2, .active = B_TRUE };
        jobs[i].items = fixture_malloc(2 * sizeof (*jobs[i].items));
        jobs[i].events = fixture_malloc(2 * sizeof (*jobs[i].events));
        assert(jobs[i].items && jobs[i].events);
        jobs[i].items[0] = (struct cd_share_item){ i * 2, 4096, 4096, 4096 };
        jobs[i].items[1] = (struct cd_share_item){ i * 2 + 1, 8192, 4096, 4096 };
        ioctl_error[i * 2 + 1] = EAGAIN;
        assert(!cd_parallel_submit(&pool, &jobs[i]));
    }
    while (cd_parallel_count(&pool)) cd_share_take(&c, &pool, &state);
    assert(state.pending == 0 && atomic_load(&state.groups) == 2);
    assert(atomic_load(&state.issued) == 2 && atomic_load(&state.deferred) == 2);
    assert(outstanding_allocations == 0 && open_total == close_total);
    cd_parallel_fini(&pool);
    unsigned reports = progress_lines;
    atomic_store(&state.issued, 999); atomic_store(&state.reported_issued, 0);
    assert(!cd_share_progress(&state, B_FALSE) && progress_lines == reports);
    atomic_store(&state.issued, 1000);
    assert(!cd_share_progress(&state, B_FALSE) && progress_lines == reports + 1);
    assert(atomic_load(&state.reported_issued) == 1000);
    assert(!cd_share_progress(&state, B_FALSE) && progress_lines == reports + 1);
    share_state_fini(&state); finish_catalog(&c);
    puts("PASS first published errno wins; guarded callbacks notify; one token/result and outcome counters merge once");
}

static void timed_heartbeat(void)
{
    reset();
    struct cd_catalog c = catalog(8);
    for (unsigned i = 0; i < 4; i++) {
        item(&c, i + 1, i * 2, 4096, 4096);
        item(&c, i + 1, i * 2 + 1, 8192, 4096);
    }
    barrier_calls = 4; hold_calls = 1;
    release_on_progress = 4; controlled_wait = 1;
    assert(run(&c, 4) == 0 && calls == 4 && peak_calls == 4);
    assert(timed_waits > 0 && progress_lines >= 3 && release_calls);
    assert(strstr(output_log, "groups=0 issued=4 deferred=0 unequal=0"));
    assert(strstr(output_log, "groups=4 issued=4 deferred=0 unequal=0"));
    finish_catalog(&c);
    puts("PASS timed coordinator heartbeat advances while every worker ioctl is blocked");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--native-large-snapshot")) {
        (void) alarm(600);
        return native_large_snapshot();
    }
    (void) alarm(30);
    if (argc == 2 && !strcmp(argv[1], "--small-cap")) {
        small_snapshot_boundaries();
        return 0;
    }
    assert(argc == 1);
    independent_groups(); conflicting_groups(); chunk_boundaries();
    deferred_and_filtered(); operational_enoent(); fatal_and_stop(); coordinator_failures();
    fallback_and_initialization(); allocation_failures();
    snapshot_cap_boundaries();
    first_error_and_notifications(); timed_heartbeat();
    assert(!pthread_cond_destroy(&fixture_changed));
    assert(!pthread_mutex_destroy(&fixture_lock));
    return 0;
}
