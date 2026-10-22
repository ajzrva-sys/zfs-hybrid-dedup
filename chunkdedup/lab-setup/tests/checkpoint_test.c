/* SPDX-License-Identifier: MIT
 * Actual checkpoint persistence and CLI lifecycle; only ZFS/scan transports are
 * doubled. The Python runner invokes fresh writer and resumer processes.
 */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "chunkdedup_checkpoint.h"
#include "chunkdedup_hybrid.h"
#include "chunkdedup_parallel.h"

static const char binding[] = "dataset=pool/a;guid=123;pool=456;abi=fixture-v1";
static const char *const payload[] = {
    "raw-file-records\ncommitted-prefix\n", "native-catalog\ncommitted-prefix\n",
    "chunk-catalog\ncommitted-prefix\n", "native-sorted\ncommitted-prefix\n",
    "chunks-sorted\ncommitted-prefix\n"
};

static void
closed_checkpoint(struct cd_checkpoint *checkpoint)
{
    int descriptors[7];
    for (unsigned i = 0; i < 5; i++)
        descriptors[i] = fileno(checkpoint->streams[i]);
    descriptors[5] = checkpoint->lockfd;
    descriptors[6] = checkpoint->dirfd;
    cd_checkpoint_close(checkpoint);
    for (unsigned i = 0; i < 7; i++) {
        errno = 0;
        assert(fcntl(descriptors[i], F_GETFD) == -1 && errno == EBADF);
    }
}

static void
check_payload(struct cd_checkpoint *checkpoint, unsigned mask)
{
    for (unsigned i = 0; i < 5; i++) {
        size_t length = i < 3 || (mask & (1U << (i - 3))) ? strlen(payload[i]) : 0;
        assert(ftello(checkpoint->streams[i]) == (off_t)length);
        rewind(checkpoint->streams[i]);
        char bytes[128] = { 0 };
        assert(fread(bytes, 1, sizeof (bytes), checkpoint->streams[i]) == length);
        assert(!memcmp(bytes, payload[i], length) && !ferror(checkpoint->streams[i]));
        assert(fseeko(checkpoint->streams[i], 0, SEEK_END) == 0);
    }
}

static void
create_checkpoint(const char *path, bool complete)
{
    struct cd_checkpoint checkpoint;
    assert(cd_checkpoint_open(&checkpoint, path, binding, sizeof (binding)) == 0);
    assert(!checkpoint.loaded && checkpoint.count == 0 && !checkpoint.complete);
    for (unsigned i = 0; i < 3; i++)
        assert(fputs(payload[i], checkpoint.streams[i]) >= 0);
    assert(cd_checkpoint_commit(&checkpoint, 7, 2, complete, 0) == 0);
    assert(checkpoint.count == 7 && checkpoint.skipped == 2 && checkpoint.complete == complete);
    closed_checkpoint(&checkpoint);
}

static void
verify_checkpoint(const char *path, bool complete, unsigned mask)
{
    struct cd_checkpoint checkpoint;
    assert(cd_checkpoint_open(&checkpoint, path, binding, sizeof (binding)) == 0);
    assert(checkpoint.loaded && checkpoint.count == 7 && checkpoint.skipped == 2);
    assert(checkpoint.complete == complete && checkpoint.sorted_mask == mask);
    check_payload(&checkpoint, mask);
    closed_checkpoint(&checkpoint);
}

static void
reject_checkpoint(const char *path, bool wrong_binding)
{
    struct cd_checkpoint checkpoint;
    const char *identity = wrong_binding ? "dataset=pool/b;guid=999" : binding;
    size_t length = wrong_binding ? strlen(identity) + 1 : sizeof (binding);
    int error = cd_checkpoint_open(&checkpoint, path, identity, length);
    assert(error != 0);
    cd_checkpoint_close(&checkpoint); /* Includes partially initialized opens. */
}

static void
sorted_caches(const char *path, unsigned mask)
{
    struct cd_checkpoint checkpoint;
    assert(cd_checkpoint_open(&checkpoint, path, binding, sizeof (binding)) == 0);
    assert(checkpoint.loaded && checkpoint.complete);
    for (unsigned i = 3; i < 5; i++)
        if (mask & (1U << (i - 3))) {
            assert(fseeko(checkpoint.streams[i], 0, SEEK_END) == 0);
            if (!(checkpoint.sorted_mask & (1U << (i - 3))))
                assert(fputs(payload[i], checkpoint.streams[i]) >= 0);
        }
    assert(cd_checkpoint_commit(&checkpoint, 7, 2, true, mask) == 0);
    closed_checkpoint(&checkpoint);
}

static void
hold_checkpoint(const char *path)
{
    struct cd_checkpoint checkpoint;
    assert(cd_checkpoint_open(&checkpoint, path, binding, sizeof (binding)) == 0);
    puts("LOCKED");
    assert(fflush(stdout) == 0 && getchar() == 'x');
    closed_checkpoint(&checkpoint);
}

static void
uncommitted_tail(const char *path)
{
    struct cd_checkpoint checkpoint;
    assert(cd_checkpoint_open(&checkpoint, path, binding, sizeof (binding)) == 0);
    for (unsigned i = 0; i < 3; i++)
        assert(fputs("uncommitted cancelled worker tail\n", checkpoint.streams[i]) >= 0);
    closed_checkpoint(&checkpoint);
}

static void
report_receipt(const char *mode, const char *path, const char *report)
{
    struct cd_checkpoint checkpoint;
    assert(cd_checkpoint_open(&checkpoint, path, binding, sizeof (binding)) == 0);
    if (!strcmp(mode, "report-save")) {
        int fd = open(report, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        const char bytes[] = "{\"type\":\"metadata\",\"schema\":1}\n";
        assert(write(fd, bytes, sizeof (bytes) - 1) == (ssize_t)sizeof (bytes) - 1);
        assert(!fsync(fd) && !close(fd));
        assert(!cd_checkpoint_report_save(&checkpoint, report));
    } else {
        int error = cd_checkpoint_report_validate(&checkpoint, report);
        assert(!strcmp(mode, "report-reject") ? error != 0 : error == 0);
    }
    closed_checkpoint(&checkpoint);
}

typedef int boolean_t;
typedef unsigned int uint_t, zfs_type_t;
#define B_FALSE 0
#define B_TRUE 1
#define ZFS_MAX_DATASET_NAME_LEN 256
#define ZFS_MAXPROPLEN 1024
#define ARRAY_SIZE(value) (sizeof (value) / sizeof ((value)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
enum { ZFS_TYPE_FILESYSTEM = 1, ZFS_TYPE_SNAPSHOT = 2 };
enum { ZFS_PROP_GUID, ZFS_PROP_CHUNKDEDUP, ZFS_PROP_MOUNTED,
    ZFS_PROP_USEDSNAP, ZFS_PROP_MOUNTPOINT, ZPOOL_PROP_GUID };
typedef struct { const char *name; uint64_t guid; char mount[PATH_MAX]; } zfs_handle_t;
typedef struct { uint64_t guid; } zpool_handle_t;
static zfs_handle_t handle = { .name = "fixture/dataset", .guid = 123 };
static zpool_handle_t pool = { 456 };
static void *g_zfs;
static atomic_int cd_stop;
static atomic_uint analyses, source_reads, inspects, source_hashes;
static unsigned scan_starts, walks, applies, sorts;
static bool resume_complete, writer, partial_writer, partial_resume;
#include "checkpoint_types.inc"

static const char *zfs_get_name(zfs_handle_t *value) { return value->name; }
static zpool_handle_t *zfs_get_pool_handle(zfs_handle_t *value)
{ (void)value; return &pool; }
static uint64_t zpool_get_prop_int(zpool_handle_t *value, int property, void *source)
{ (void)property; (void)source; return value->guid; }
static int zfs_prop_get_numeric(zfs_handle_t *value, int property, uint64_t *number,
    void *source, char *buffer, size_t size)
{
    (void)source; (void)buffer; (void)size;
    *number = property == ZFS_PROP_GUID ? value->guid : property == ZFS_PROP_USEDSNAP ? 0 : 1;
    return 0;
}
static int zfs_prop_get(zfs_handle_t *value, int property, char *buffer, size_t size, ...)
{ (void)property; return strlcpy(buffer, value->mount, size) >= size ? 1 : 0; }
static zfs_handle_t *zfs_open(void *context, const char *name, zfs_type_t type)
{ (void)context; (void)name; (void)type; return &handle; }
static zfs_handle_t *zfs_path_to_zhandle(void *context, const char *path, zfs_type_t type)
{ (void)context; (void)path; (void)type; return &handle; }
static zfs_type_t zfs_get_type(zfs_handle_t *value) { (void)value; return ZFS_TYPE_FILESYSTEM; }
static zfs_type_t zfs_get_underlying_type(zfs_handle_t *value) { return zfs_get_type(value); }
static void zfs_close(zfs_handle_t *value) { (void)value; }
static int cd_snapshot_path(zfs_handle_t *value, char *path, size_t size)
{ (void)value; (void)path; (void)size; return ENOTSUP; }
static int cd_status(const char *dataset) { (void)dataset; return ENOTSUP; }
static int cd_block_status(const char *dataset) { (void)dataset; return ENOTSUP; }
static int cd_method_status(const char *dataset, const char *path)
{ (void)dataset; (void)path; inspects++; assert(!resume_complete); return ENOTSUP; }
static int cd_file(const char *dataset, const char *path, const char *op,
    uint64_t age, uint64_t *processed, uint64_t *skipped, boolean_t watch,
    const struct stat *expected)
{
    (void)dataset; (void)path; (void)op; (void)age; (void)processed;
    (void)skipped; (void)watch; (void)expected;
    source_reads++; source_hashes++; assert(!resume_complete); return ENOTSUP;
}
static int cd_catalog_retry(struct cd_catalog *catalog, uint64_t age)
{ (void)age; assert(catalog->retry_count == 0); return 0; }
static int cd_catalog_enqueue(struct cd_catalog *catalog, const struct cd_catalog_file *file,
    uint64_t age)
{ (void)catalog; (void)file; (void)age; assert(!"unexpected dirty transport"); return EIO; }
static int cd_catalog_analyze(struct cd_catalog *catalog, const char *dataset,
    const char *path, uint64_t age, const struct stat *expected, boolean_t retryable);
static int cd_catalog_apply(struct cd_catalog *catalog, uint64_t age, unsigned threads);

static int fixture_sort(FILE *input, FILE **output, const char *directory,
    unsigned threads, const atomic_int *stop)
{ sorts++; return cd_hybrid_sort_parallel(input, output, directory, threads, stop); }
static int fixture_sort_to(FILE *input, FILE *output, const char *directory,
    unsigned threads, const atomic_int *stop, int hold_fd)
{ sorts++; return cd_hybrid_sort_parallel_to(input, output, directory, threads, stop, hold_fd); }

#include "checkpoint_catalog.inc"
#define cd_hybrid_sort_parallel fixture_sort
#define cd_hybrid_sort_parallel_to fixture_sort_to
#include "checkpoint_hooks.inc"
#undef cd_hybrid_sort_parallel
#undef cd_hybrid_sort_parallel_to
static int fixture_checkpoint_save(struct cd_catalog *catalog, boolean_t complete)
{
    int error = cd_catalog_checkpoint_save(catalog, complete);
    if (!error && partial_writer && !complete && catalog->count == 1)
        cd_stop = 1;
    return error;
}
#define cd_catalog_checkpoint_save fixture_checkpoint_save
#include "checkpoint_scan.inc"
#undef cd_catalog_checkpoint_save

static int
cd_catalog_analyze(struct cd_catalog *catalog, const char *dataset,
    const char *path, uint64_t age, const struct stat *expected, boolean_t retryable)
{
    (void)age;
    analyses++; inspects++; source_reads++; source_hashes++;
    assert(!resume_complete && catalog->staging && !retryable);
    struct cd_catalog_file file = { .identity = *expected, .dataset_id = expected->st_dev,
        .block_size = 4096, .method = CD_METHOD_BLOCK };
    assert(strlcpy(file.path, path, sizeof (file.path)) < sizeof (file.path));
    assert(strlcpy(file.dataset, dataset, sizeof (file.dataset)) < sizeof (file.dataset));
    unsigned char hash[32] = { 1 };
    assert(!cd_catalog_append(catalog->blocks, file.dataset_id, hash, 4096, 0, 0, 4096, 4096));
    assert(!cd_catalog_append(catalog->chunks, file.dataset_id, hash, 4096, 0, 0, 8192, 0));
    assert(fwrite(&file, sizeof (file), 1, catalog->files) == 1);
    catalog->count = 1;
    return 0;
}

static int
fixture_scan_init(struct cd_scan *scan, struct cd_catalog *catalog,
    uint64_t age, unsigned threads)
{
    scan_starts++;
    assert(!resume_complete);
    return cd_scan_init(scan, catalog, age, threads);
}

static FTS *
fixture_fts_open(char *const *paths, int options, int (*compare)(const FTSENT **, const FTSENT **))
{
    walks++;
    assert(!resume_complete);
    return fts_open(paths, options, compare);
}

static int
cd_catalog_apply(struct cd_catalog *catalog, uint64_t age, unsigned threads)
{
    (void)age;
    applies++;
    assert(catalog->count == 2 && catalog->checkpoint->complete);
    assert(catalog->files != catalog->checkpoint->streams[0]);
    assert(!cd_catalog_sort(catalog, catalog->blocks, &catalog->sorted_blocks, threads, 0));
    assert(!cd_catalog_sort(catalog, catalog->chunks, &catalog->sorted_chunks, threads, 1));
    assert(catalog->checkpoint->sorted_mask == 3);
    for (unsigned which = 0; which < 2; which++) {
        FILE *stream = which ? catalog->sorted_chunks : catalog->sorted_blocks;
        assert(!fseeko(stream, 0, SEEK_SET));
        for (uint64_t i = 0; i < 2; i++) {
            struct cd_catalog_item item;
            assert(!cd_catalog_next(stream, &item) && item.file == i && item.length == 4096);
        }
        struct cd_catalog_item end;
        assert(cd_catalog_next(stream, &end) == ENOENT);
    }
    struct cd_catalog_file changed, raw;
    assert(!cd_catalog_record(catalog, 0, &changed, B_FALSE));
    changed.parent = 999;
    assert(!cd_catalog_record(catalog, 0, &changed, B_TRUE));
    rewind(catalog->checkpoint->streams[0]);
    assert(fread(&raw, sizeof (raw), 1, catalog->checkpoint->streams[0]) == 1 && raw.parent == 0);
    assert(!fseeko(catalog->checkpoint->streams[0], 0, SEEK_END));
    if (!resume_complete) {
        assert(catalog->report != NULL && sorts == 2);
        assert(!cd_catalog_report_finish(catalog));
        if (writer) {
            cd_stop = 1; /* Stop after durable complete scan, caches and report. */
            return ECANCELED;
        }
        return 0;
    }
    assert(catalog->report == NULL && sorts == 0);
    return 0;
}

#define cd_scan_init fixture_scan_init
#define fts_open fixture_fts_open
#include "checkpoint_pass.inc"
#undef cd_scan_init
#undef fts_open
static int cd_watch(uint64_t age, uint64_t interval, int argc, char **argv,
    enum cd_method method, unsigned threads)
{ (void)age; (void)interval; (void)argc; (void)argv; (void)method; (void)threads; return 2; }
#include "checkpoint_cli.inc"

static void
checkpoint_driver_test(int argc, char **argv)
{
    char checkpoint[PATH_MAX], report[PATH_MAX], target[PATH_MAX];
    assert(snprintf(checkpoint, sizeof (checkpoint), "%s/state", argv[2]) < (int)sizeof (checkpoint));
    assert(snprintf(report, sizeof (report), "%s/report.jsonl", argv[2]) < (int)sizeof (report));
    assert(snprintf(target, sizeof (target), "%s/input", argv[2]) < (int)sizeof (target));
    writer = !strcmp(argv[1], "driver-write");
    partial_writer = !strcmp(argv[1], "driver-partial-write");
    partial_resume = !strcmp(argv[1], "driver-partial-resume");
    resume_complete = !writer && !partial_writer && !partial_resume;
    if (writer || partial_writer) {
        assert(!mkdir(argv[2], 0700) && !mkdir(target, 0700));
        for (unsigned i = 0; i < 2; i++) {
            char path[PATH_MAX];
            assert(snprintf(path, sizeof (path), "%s/%u", target, i) < (int)sizeof (path));
            FILE *stream = fopen(path, "wx");
            assert(stream != NULL && !ftruncate(fileno(stream), 4096) && !fclose(stream));
        }
    }
    assert(realpath(target, handle.mount) != NULL);
    if (argc > 3 && !strcmp(argv[3], "wrong-dataset")) handle.name = "fixture/other";
    if (argc > 3 && !strcmp(argv[3], "wrong-guid")) handle.guid++;
    if (argc > 3 && !strcmp(argv[3], "wrong-pool")) pool.guid++;
    char *arguments[] = { "chunkdedup", "optimize", "--method", "block", "--threads", "40",
        "--min-age", "0", "--report", report, "--checkpoint", checkpoint, "-r", target, NULL };
    if (!strcmp(argv[1], "driver-invalid")) {
        assert(argc > 3);
        if (!strcmp(argv[3], "multiple")) {
            char *many[] = { "chunkdedup", "optimize", "--checkpoint", checkpoint, target, target, NULL };
            assert(zfs_do_chunkdedup(6, many) == 2);
        } else {
            char *bad[] = { "chunkdedup", argv[3], "--checkpoint", checkpoint, target, NULL };
            if (!strcmp(argv[3], "empty")) { bad[1] = "optimize"; bad[3] = ""; }
            assert(zfs_do_chunkdedup(5, bad) == 2);
        }
        assert(scan_starts == 0 && walks == 0 && analyses == 0 && applies == 0);
        return;
    }
    int result = zfs_do_chunkdedup(14, arguments);
    if (writer) {
        assert(result == 1 && analyses == 2 && scan_starts == 1 && walks == 1 && applies == 1);
    } else if (partial_writer) {
        assert(result == 1 && analyses >= 1 && analyses <= 2 && scan_starts == 1 && walks == 1);
        assert(applies == 0 && sorts == 0);
    } else if (partial_resume) {
        assert(result == 0 && analyses == 1 && inspects == 1 && source_reads == 1 && source_hashes == 1);
        assert(scan_starts == 1 && walks == 1 && applies == 1 && sorts == 2);
    } else {
        assert(analyses == 0 && inspects == 0 && source_reads == 0 && source_hashes == 0);
        assert(scan_starts == 0 && walks == 0 && sorts == 0);
        if (!strcmp(argv[1], "driver-reject")) assert(result != 0 && applies == 0);
        else assert(result == 0 && applies == 1);
    }
}

/* Observe production forks without replacing either actual sort exec path. */
static bool orphan_sort;
static unsigned orphan_forks;
static pid_t
checkpoint_sort_fork(void)
{
    unsigned number = orphan_sort ? ++orphan_forks : 0;
    pid_t child = fork();
    if (orphan_sort && number == 2 && child > 0) {
        assert(printf("MERGE %ld\n", (long)child) > 0);
        assert(!fflush(stdout));
    }
    return child;
}
#define fork checkpoint_sort_fork
#include "checkpoint_hybrid.inc"
#undef fork

static void
sort_orphan_owner(const char *path, int output_fd)
{
    struct cd_checkpoint checkpoint;
    assert(!cd_checkpoint_open(&checkpoint, path, binding, sizeof (binding)));
    assert(checkpoint.loaded && checkpoint.complete);
    assert(fcntl(checkpoint.lockfd, F_GETFD) & FD_CLOEXEC);
    assert(fputs("uncommitted sort tail\n", checkpoint.streams[3]) >= 0);
    assert(!fflush(checkpoint.streams[3]));
    FILE *input = tmpfile(), *destination = fdopen(output_fd, "w");
    assert(input && destination);
    /* More than a pipe buffer: the exec'd merge sort waits for our reader. */
    for (unsigned i = 0; i < 20000; i++)
        assert(fprintf(input, "%020u key %020u\n", i % 7, i % 13) > 0);
    orphan_sort = true;
    atomic_int stop = 0;
    (void) cd_hybrid_sort_parallel_to(input, destination, path, 1, &stop, checkpoint.lockfd);
    assert(!"orphan test owner should be killed while merge output is blocked");
}

int
main(int argc, char **argv)
{
    assert(argc >= 3);
    (void) alarm(30);
    struct cd_checkpoint zero = { 0 };
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFD);
    cd_checkpoint_close(&zero);
    assert(fcntl(STDIN_FILENO, F_GETFD) == stdin_flags);
    if (!strcmp(argv[1], "create"))
        create_checkpoint(argv[2], argc > 3 && !strcmp(argv[3], "complete"));
    else if (!strcmp(argv[1], "verify"))
        verify_checkpoint(argv[2], argc > 3 && !strcmp(argv[3], "complete"),
            argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 0);
    else if (!strcmp(argv[1], "reject"))
        reject_checkpoint(argv[2], argc > 3);
    else if (!strcmp(argv[1], "sorted"))
        sorted_caches(argv[2], (unsigned)strtoul(argv[3], NULL, 10));
    else if (!strcmp(argv[1], "hold"))
        hold_checkpoint(argv[2]);
    else if (!strcmp(argv[1], "locked")) {
        struct cd_checkpoint checkpoint;
        int error = cd_checkpoint_open(&checkpoint, argv[2], binding, sizeof (binding));
        assert(error == EWOULDBLOCK || error == EAGAIN);
        cd_checkpoint_close(&checkpoint);
    }
    else if (!strcmp(argv[1], "orphan-sort")) {
        assert(argc == 4);
        sort_orphan_owner(argv[2], atoi(argv[3]));
    }
    else if (!strcmp(argv[1], "cancel-tail"))
        uncommitted_tail(argv[2]);
    else if (!strncmp(argv[1], "report-", 7)) {
        assert(argc == 4);
        report_receipt(argv[1], argv[2], argv[3]);
    }
    else
        checkpoint_driver_test(argc, argv);
    return (0);
}
