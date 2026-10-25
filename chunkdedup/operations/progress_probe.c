/* Read-only observation of the existing userspace administrative API. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <sys/types.h>
/* Only pointers cross this observer; avoid importing either ZFS header ABI. */
typedef struct nvlist nvlist_t;
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int (*call_fn)(const char *, nvlist_t *, nvlist_t **);
typedef int (*string_fn)(nvlist_t *, const char *, char **);
typedef int (*uint64_fn)(nvlist_t *, const char *, uint64_t *);
static call_fn real_call;
static string_fn lookup_string;
static uint64_fn lookup_uint64;
static pid_t owner;
static atomic_uint_fast64_t scanned_bytes, scanned_files;
static atomic_int sharing;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static pthread_mutex_t writer_lock = PTHREAD_MUTEX_INITIALIZER;
static char output[PATH_MAX];
static struct timespec last_publish;
static int published;

static void
publish(int force)
{
    struct timespec now;
    char temporary[PATH_MAX];
    int fd, length;
    FILE *file;
    if (owner == 0 || owner != getpid() || output[0] == '\0')
        return;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;
    if (!force && published && (now.tv_sec < last_publish.tv_sec + 1 ||
        (now.tv_sec == last_publish.tv_sec + 1 &&
        now.tv_nsec < last_publish.tv_nsec)))
        return;
    length = snprintf(temporary, sizeof (temporary), "%s.tmp.%ld", output,
        (long)owner);
    if (length < 0 || (size_t)length >= sizeof (temporary))
        return;
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    file = fdopen(fd, "w");
    if (file == NULL) {
        (void)close(fd);
        (void)unlink(temporary);
        return;
    }
    int failed = fprintf(file,
        "scanned_bytes\t%" PRIu64 "\nscanned_files\t%" PRIu64
        "\nphase\t%s\nupdated_epoch\t%lld\n",
        (uint64_t)atomic_load_explicit(&scanned_bytes, memory_order_relaxed),
        (uint64_t)atomic_load_explicit(&scanned_files, memory_order_relaxed),
        atomic_load_explicit(&sharing, memory_order_relaxed) ? "sharing" :
        "scanning", (long long)time(NULL)) < 0;
    if (fclose(file) != 0)
        failed = 1;
    if (failed || rename(temporary, output) != 0) {
        (void)unlink(temporary);
        return;
    }
    last_publish = now;
    published = 1;
}

static void
final_publish(void)
{
    int saved = errno;
    (void)pthread_mutex_lock(&writer_lock);
    publish(1);
    (void)pthread_mutex_unlock(&writer_lock);
    errno = saved;
}

static void
initialize(void)
{
    void *symbol;
    const char *path;
    owner = getpid();
    symbol = dlsym(RTLD_NEXT, "lzc_chunkdedup");
    _Static_assert(sizeof (symbol) == sizeof (real_call), "POSIX function pointer");
    memcpy(&real_call, &symbol, sizeof (real_call));
    symbol = dlsym(RTLD_NEXT, "nvlist_lookup_string");
    memcpy(&lookup_string, &symbol, sizeof (lookup_string));
    symbol = dlsym(RTLD_NEXT, "nvlist_lookup_uint64");
    memcpy(&lookup_uint64, &symbol, sizeof (lookup_uint64));
    path = getenv("CD_PROGRESS_PATH");
    if (path != NULL && strlen(path) < sizeof (output))
        (void)snprintf(output, sizeof (output), "%s", path);
    (void)atexit(final_publish);
}

int
lzc_chunkdedup(const char *dataset, nvlist_t *args, nvlist_t **result)
{
    int incoming_errno = errno, error, saved_errno;
    char *operation = NULL;
    uint64_t length = 0, offset;
    int is_read = 0, initial_inspect = 0, is_share = 0;
    (void)pthread_once(&initialized, initialize);
    if (owner == getpid() && lookup_string != NULL && lookup_uint64 != NULL &&
        lookup_string(args, "op", &operation) == 0 && operation != NULL) {
        is_read = strcmp(operation, "read") == 0 &&
            lookup_uint64(args, "length", &length) == 0;
        initial_inspect = strcmp(operation, "inspect") == 0 &&
            lookup_uint64(args, "offset", &offset) != 0;
        is_share = strcmp(operation, "share-blocks") == 0;
    }
    errno = incoming_errno;
    if (real_call == NULL) {
        errno = ENOSYS;
        return (ENOSYS);
    }
    error = real_call(dataset, args, result);
    saved_errno = errno;
    if (owner == getpid()) {
        if (error == 0) {
            if (is_read)
                (void)atomic_fetch_add_explicit(&scanned_bytes, length,
                    memory_order_relaxed);
            if (initial_inspect)
                (void)atomic_fetch_add_explicit(&scanned_files, 1,
                    memory_order_relaxed);
        }
        if (is_share)
            atomic_store_explicit(&sharing, 1, memory_order_relaxed);
        if (pthread_mutex_trylock(&writer_lock) == 0) {
            publish(0);
            (void)pthread_mutex_unlock(&writer_lock);
        }
    }
    errno = saved_errno;
    return (error);
}
