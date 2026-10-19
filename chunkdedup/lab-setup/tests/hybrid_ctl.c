/* SPDX-License-Identifier: MIT
 * Bounded guest-only caller for verified native block sharing and inspection.
 * It never enables fault injection and opens only files in the named lab dataset.
 */
#define _WITH_GETLINE
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libzfs_core.h>
#include <sys/nvpair.h>

static void fail(const char *why) { fprintf(stderr, "%s\n", why); exit(1); }
static void die(const char *why) { perror(why); exit(1); }
static uint64_t number(const char *text)
{
    char *end;
    errno = 0;
    uint64_t value = strtoull(text, &end, 10);
    if (errno || *end || !*text || *text == '-') fail("invalid bounded offset");
    return value;
}
static void guard(const char *dataset)
{
    char hostname[256];
    if (gethostname(hostname, sizeof (hostname))) die("hostname");
    if (strcmp(hostname, "zfs-chunkdedup-lab") || geteuid())
        fail("only the disposable root guest may use this helper");
    if (strncmp(dataset, "cdwf-", 5) || !strchr(dataset, '/') ||
        strchr(dataset, '@') || strchr(dataset, '#') || strstr(dataset, ".."))
        fail("only live current-run file-vdev datasets are admitted");
}
static int open_file(const char *dataset, const char *path, struct stat *st)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) die("realpath");
    if (strncmp(resolved, "/mnt/cdwf-", 10)) fail("file is outside lab run");
    int fd = open(resolved, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, st)) die("open/fstat");
    struct statfs fs;
    if (fstatfs(fd, &fs)) die("fstatfs");
    if (strcmp(fs.f_fstypename, "zfs") || strcmp(fs.f_mntfromname, dataset) ||
        !S_ISREG(st->st_mode) || st->st_size <= 0 || st->st_size > (64U << 20))
        fail("target is not a bounded regular file in the named dataset");
    return fd;
}
static void identity(nvlist_t *in, const char *prefix, const struct stat *st)
{
    char key[64];
#define FIELD(name, value) do { \
    if (snprintf(key, sizeof (key), "%s%s", prefix, name) >= (int)sizeof (key)) \
        fail("identity key overflow"); \
    fnvlist_add_uint64(in, key, (uint64_t)(value)); \
} while (0)
    FIELD("object", st->st_ino);
    FIELD("generation", st->st_gen);
    FIELD("size", st->st_size);
    FIELD("mtime_sec", st->st_mtim.tv_sec);
    FIELD("mtime_nsec", st->st_mtim.tv_nsec);
#undef FIELD
}
static uint64_t value(nvlist_t *out, const char *key)
{
    uint64_t v = 0;
    if (nvlist_lookup_uint64(out, key, &v)) fail("missing integer result");
    return v;
}
static const char *boolean(nvlist_t *out, const char *key)
{
    boolean_t v;
    if (nvlist_lookup_boolean_value(out, key, &v)) fail("missing boolean result");
    return v ? "true" : "false";
}
static int sync_objects(const char *dataset, uint64_t *objects, uint_t count)
{
    nvlist_t *in = fnvlist_alloc(), *out = NULL;
    fnvlist_add_string(in, "op", "sync-batch");
    fnvlist_add_uint64_array(in, "objects", objects, count);
    int error = lzc_chunkdedup(dataset, in, &out);
    fnvlist_free(in); fnvlist_free(out);
    return error;
}
int main(int argc, char **argv)
{
    if (argc < 4)
        fail("hybrid_ctl inspect|inspect-raw|inspect-stale DATASET FILE OFFSET | inspect-initial DATASET FILE | inspect-loop DATASET FILE COUNT | sync-batch DATASET FILE COUNT [zero-object|OTHER_FILE] | share DATASET SOURCE DEST SOURCE_OFFSET DEST_OFFSET LENGTH [stale-generation|stale-mtime|stale-source]");
    guard(argv[2]);
    int inspect = !strcmp(argv[1], "inspect"),
        raw = !strcmp(argv[1], "inspect-raw"),
        initial = !strcmp(argv[1], "inspect-initial"),
        stale = !strcmp(argv[1], "inspect-stale"),
        loop = !strcmp(argv[1], "inspect-loop"),
        batch = !strcmp(argv[1], "sync-batch");
    if (((inspect || raw || stale || loop) && argc != 5) ||
        (initial && argc != 4) || (batch && argc != 5 && argc != 6) ||
        (!(inspect || raw || initial || stale || loop || batch) &&
        (strcmp(argv[1], "share") || (argc != 8 && argc != 9))))
        fail("invalid operation");
    if (libzfs_core_init()) die("libzfs_core_init");
    nvlist_t *in = fnvlist_alloc(), *out = NULL;
    struct stat source, dest;
    int source_fd = open_file(argv[2], argv[3], &source), dest_fd = -1;
    if (batch) {
        uint64_t count = number(argv[4]), objects[257];
        if (count > 257) fail("test batch exceeds bounded rejection probe");
        uint64_t other = source.st_ino;
        if (argc == 6 && strcmp(argv[5], "zero-object")) {
            int fd = open_file(argv[2], argv[5], &dest);
            other = dest.st_ino; close(fd);
        }
        for (uint64_t i = 0; i < count; i++)
            objects[i] = argc == 6 && !strcmp(argv[5], "zero-object") ?
                0 : (i % 2 ? other : source.st_ino);
        int error = sync_objects(argv[2], objects, (uint_t)count);
        printf("{\"error\":%d}\n", error);
        fnvlist_free(in); close(source_fd); libzfs_core_fini(); return 0;
    }
    if (inspect || raw || initial || stale || loop) {
        fnvlist_add_string(in, "op", "inspect");
        if (stale) source.st_gen++;
        identity(in, "", &source);
        if (loop) {
            uint64_t count = number(argv[4]), changes = 0, previous = 0;
            if (count < 2 || count > 64) fail("invalid bounded inspect count");
            int error = 0;
            for (uint64_t i = 0; i < count; i++) {
                error = lzc_chunkdedup(argv[2], in, &out);
                if (error) break;
                uint64_t txg = value(out, "synced_txg");
                if (i && txg != previous) changes++;
                previous = txg; fnvlist_free(out); out = NULL;
            }
            printf("{\"error\":%d,\"count\":%" PRIu64
                ",\"txg_changes\":%" PRIu64 "}\n", error, count, changes);
            fnvlist_free(in); fnvlist_free(out); close(source_fd);
            libzfs_core_fini(); return 0;
        }
        if (inspect) {
            /* Only dirty range queries require the explicit shared barrier. */
            int initial_error = lzc_chunkdedup(argv[2], in, &out);
            fnvlist_free(out); out = NULL;
            if (initial_error) {
                printf("{\"error\":%d}\n", initial_error);
                fnvlist_free(in); close(source_fd); libzfs_core_fini(); return 0;
            }
        }
        if (!initial) fnvlist_add_uint64(in, "offset", number(argv[4]));
    } else {
        dest_fd = open_file(argv[2], argv[4], &dest);
        fnvlist_add_string(in, "op", "share-blocks");
        if (argc == 9) {
            if (!strcmp(argv[8], "stale-generation")) dest.st_gen++;
            else if (!strcmp(argv[8], "stale-mtime")) dest.st_mtim.tv_sec--;
            else if (!strcmp(argv[8], "stale-source")) source.st_gen++;
            else fail("invalid identity variant");
        }
        identity(in, "source_", &source); identity(in, "", &dest);
        fnvlist_add_uint64(in, "source_offset", number(argv[5]));
        fnvlist_add_uint64(in, "offset", number(argv[6]));
        fnvlist_add_uint64(in, "length", number(argv[7]));
    }
    int error = lzc_chunkdedup(argv[2], in, &out);
    if (error == EAGAIN && inspect) {
        uint64_t object = source.st_ino;
        fnvlist_free(out); out = NULL;
        error = sync_objects(argv[2], &object, 1);
        if (!error) error = lzc_chunkdedup(argv[2], in, &out);
    }
    fnvlist_free(in);
    close(source_fd); if (dest_fd >= 0) close(dest_fd);
    printf("{\"error\":%d", error);
    if (!error && (inspect || raw || initial || stale)) {
        printf(",\"block_size\":%" PRIu64 ",\"allocated_bytes\":%" PRIu64
            ",\"chunked\":%s,\"eligible\":%s,\"synced_txg\":%" PRIu64,
            value(out, "block_size"), value(out, "allocated_bytes"),
            boolean(out, "chunked"), boolean(out, "eligible"),
            value(out, "synced_txg"));
        if (!initial) printf(",\"block_length\":%" PRIu64
            ",\"block_asize\":%" PRIu64 ",\"block_hole\":%s,\"block_shared\":%s",
            value(out, "block_length"), value(out, "block_asize"),
            boolean(out, "block_hole"), boolean(out, "block_shared"));
    } else if (!error) {
        printf(",\"shared_bytes\":%" PRIu64 ",\"already_shared_bytes\":%" PRIu64
            ",\"compared_bytes\":%" PRIu64, value(out, "shared_bytes"),
            value(out, "already_shared_bytes"), value(out, "compared_bytes"));
    }
    puts("}");
    fnvlist_free(out); libzfs_core_fini();
    return 0;
}
