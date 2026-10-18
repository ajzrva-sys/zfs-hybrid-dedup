/* SPDX-License-Identifier: MIT
 * Guest-only caller for default-disabled ZFS_DEBUG fault test operations.
 * Tokens are bounded XDR nvlists on the guest UFS boot filesystem.
 */
#define _WITH_GETLINE
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libzfs_core.h>
#include <sys/nvpair.h>

#define TOKEN_LIMIT (16U << 10)
static void fail(const char *why) { fprintf(stderr, "%s\n", why); exit(1); }
static void die(const char *why) { perror(why); exit(1); }

static void guard(const char *dataset)
{
    char hostname[256];
    if (gethostname(hostname, sizeof (hostname)) != 0) die("hostname");
    if (strcmp(hostname, "zfs-chunkdedup-lab") || geteuid() != 0)
        fail("only the disposable root guest may use this helper");
    if (strncmp(dataset, "cdlab/", 6) || strchr(dataset, '@') ||
        strchr(dataset, '#')) fail("only a live cdlab dataset is allowed");
}

static void token_guard(const char *path)
{
    char parent[PATH_MAX], resolved[PATH_MAX];
    if (strlen(path) >= sizeof (parent)) fail("token path too long");
    strcpy(parent, path);
    char *end = strrchr(parent, '/');
    if (end == NULL) fail("token requires an absolute path");
    *end = 0;
    if (realpath(parent, resolved) == NULL) die("token realpath");
    if (strcmp(resolved, "/root/lab/tests/work"))
        fail("token must live in /root/lab/tests/work");
    struct statfs fs;
    if (statfs(resolved, &fs) != 0) die("token statfs");
    if (strcmp(fs.f_fstypename, "ufs")) fail("token must be saved on UFS");
}

static nvlist_t *read_token(const char *path)
{
    token_guard(path);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) die("token open");
    struct stat st;
    if (fstat(fd, &st) != 0) die("token fstat");
    if (!S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_size <= 0 ||
        st.st_size > TOKEN_LIMIT) fail("invalid token file");
    char *bytes = malloc((size_t)st.st_size);
    if (bytes == NULL) die("token malloc");
    size_t offset = 0;
    while (offset < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + offset, (size_t)st.st_size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die("token read");
        offset += (size_t)n;
    }
    close(fd);
    nvlist_t *token;
    int error = nvlist_unpack(bytes, offset, &token, 0);
    free(bytes);
    if (error != 0) { errno = error; die("token unpack"); }
    return token;
}

static void write_token(const char *path, nvlist_t *token)
{
    token_guard(path);
    char *bytes = NULL;
    size_t size = 0;
    int error = nvlist_pack(token, &bytes, &size, NV_ENCODE_XDR, 0);
    if (error != 0) { errno = error; die("token pack"); }
    if (size > TOKEN_LIMIT) fail("oversized token");
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) die("token create");
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = write(fd, bytes + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die("token write");
        offset += (size_t)n;
    }
    if (fsync(fd) != 0) die("token fsync");
    close(fd);
    free(bytes);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 6)
        fail("debug_ctl collision DATASET | inject/restore DATASET FILE KIND TOKEN");
    guard(argv[2]);
    if (libzfs_core_init() != 0) die("libzfs_core_init");
    nvlist_t *in = fnvlist_alloc(), *out = NULL;
    int fd = -1;
    if (argc == 3) {
        if (strcmp(argv[1], "collision")) fail("invalid collision command");
        fnvlist_add_string(in, "op", "debug-collision-selftest");
    } else {
        int restore = strcmp(argv[1], "restore") == 0;
        if (!restore && strcmp(argv[1], "inject")) fail("invalid fault command");
        if (strcmp(argv[4], "recipe") && strcmp(argv[4], "refcount"))
            fail("invalid fault kind");
        char resolved[PATH_MAX];
        if (realpath(argv[3], resolved) == NULL) die("file realpath");
        if (strncmp(resolved, "/mnt/cdlab/", 11)) fail("file outside lab");
        fd = open(resolved, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) die("file open");
        struct stat st;
        if (fstat(fd, &st) != 0) die("file fstat");
        if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (64U << 20))
            fail("fault target must be a bounded regular file");
        token_guard(argv[5]);
        fnvlist_add_string(in, "op", restore ? "debug-restore" : "debug-inject");
        fnvlist_add_string(in, "kind", argv[4]);
        fnvlist_add_uint64(in, "object", st.st_ino);
        fnvlist_add_uint64(in, "generation", st.st_gen);
        fnvlist_add_uint64(in, "size", (uint64_t)st.st_size);
        if (restore) {
            nvlist_t *token = read_token(argv[5]);
            fnvlist_add_nvlist(in, "token", token);
            fnvlist_free(token);
        } else if (access(argv[5], F_OK) == 0) {
            fail("refusing to replace an existing restoration token");
        }
    }
    int error = lzc_chunkdedup(argv[2], in, &out);
    fnvlist_free(in);
    if (fd >= 0) close(fd);
    if (error != 0) { errno = error; die("chunkdedup debug operation"); }
    if (argc == 3) {
        if (!fnvlist_lookup_boolean_value(out, "different_bytes_rejected") ||
            !fnvlist_lookup_boolean_value(out, "matching_bytes_accepted") ||
            !fnvlist_lookup_boolean_value(out, "different_identifier_rejected"))
            fail("candidate comparator self-test failed");
        puts("COLLISION-COMPARATOR-PASS");
    } else if (strcmp(argv[1], "inject") == 0) {
        write_token(argv[5], fnvlist_lookup_nvlist(out, "token"));
        puts("FAULT-INJECTED");
    } else {
        puts("FAULT-RESTORED");
    }
    fnvlist_free(out);
    libzfs_core_fini();
    return 0;
}
