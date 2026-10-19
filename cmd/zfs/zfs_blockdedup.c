/* SPDX-License-Identifier: CDDL-1.0 */
/* Background, verified sharing of ordinary native ZFS blocks. */
#ifdef BLOCKDEDUP_TEST_HEADER
#include BLOCKDEDUP_TEST_HEADER
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <libzfs.h>
#include <libzfs_core.h>
#include <libnvpair.h>
#include "zfs_util.h"
#endif
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

#define BD_READ_SIZE (256U << 10)
#define BD_MAX_BLOCK (16U << 20)

static volatile sig_atomic_t bd_stop;

struct bd_file {
	struct stat identity;
	char dataset[ZFS_MAX_DATASET_NAME_LEN];
	char path[PATH_MAX];
};

struct bd_item {
	uint64_t device, block, length, file, offset;
	char hash[65];
};

struct bd_seen {
	uint64_t device, object, occupied;
};

struct bd_catalog {
	FILE *files, *blocks, *sorted, *seen;
	char directory[PATH_MAX];
	uint64_t capacity, count, block_count, candidates, shared, already;
	uint64_t skipped, deferred;
	boolean_t dry_run;
};

static boolean_t
bd_deferred(int error)
{
	return (error == EBUSY || error == EAGAIN || error == ESTALE ||
	    error == ENOENT);
}

static void
bd_signal(int signo)
{
	(void) signo;
	bd_stop = 1;
}

static void
bd_identity(nvlist_t *args, const struct stat *st, boolean_t source)
{
	const char *object = source ? "source_object" : "object";
	const char *generation = source ? "source_generation" : "generation";
	const char *size = source ? "source_size" : "size";
	fnvlist_add_uint64(args, object, st->st_ino);
	fnvlist_add_uint64(args, size, st->st_size);
#ifdef __FreeBSD__
	fnvlist_add_uint64(args, generation, st->st_gen);
	fnvlist_add_uint64(args, source ? "source_mtime_sec" : "mtime_sec",
	    st->st_mtim.tv_sec);
	fnvlist_add_uint64(args, source ? "source_mtime_nsec" : "mtime_nsec",
	    st->st_mtim.tv_nsec);
#else
	fnvlist_add_uint64(args, generation, 0);
#endif
}

static boolean_t
bd_same_identity(const struct stat *a, const struct stat *b)
{
	return (a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
	    a->st_size == b->st_size && a->st_mtime == b->st_mtime
#ifdef __FreeBSD__
	    && a->st_gen == b->st_gen &&
	    a->st_mtim.tv_nsec == b->st_mtim.tv_nsec
#endif
	    );
}

/* All catalog data and sort spills reside in the selected temporary area. */
static FILE *
bd_tempfile(struct bd_catalog *c)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof (path), "%s/catalog.XXXXXX", c->directory) >=
	    (int)sizeof (path)) {
		errno = ENAMETOOLONG;
		return (NULL);
	}
	int fd = mkstemp(path);
	if (fd < 0)
		return (NULL);
	(void) unlink(path);
	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);
	FILE *fp = fdopen(fd, "w+");
	if (fp == NULL) {
		int error = errno;
		(void) close(fd);
		errno = error;
	}
	return (fp);
}

static int
bd_init(struct bd_catalog *c, const char *tmpdir, boolean_t dry_run)
{
	memset(c, 0, sizeof (*c));
	c->dry_run = dry_run;
	if (snprintf(c->directory, sizeof (c->directory),
	    "%s/zfs-blockdedup.XXXXXX", tmpdir) >= (int)sizeof (c->directory))
		return (ENAMETOOLONG);
	if (mkdtemp(c->directory) == NULL) {
		c->directory[0] = '\0';
		return (errno);
	}
	c->files = bd_tempfile(c);
	c->blocks = bd_tempfile(c);
	c->seen = bd_tempfile(c);
	if (c->files == NULL || c->blocks == NULL || c->seen == NULL)
		return (errno);
	c->capacity = 1024;
	return (ftruncate(fileno(c->seen), c->capacity *
	    sizeof (struct bd_seen)) == 0 ? 0 : errno);
}

static void
bd_fini(struct bd_catalog *c)
{
	FILE *files[] = { c->files, c->blocks, c->sorted, c->seen };
	for (size_t i = 0; i < sizeof (files) / sizeof (files[0]); i++)
		if (files[i] != NULL)
			(void) fclose(files[i]);
	if (c->directory[0] != '\0') {
		DIR *dir = opendir(c->directory);
		if (dir != NULL) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL)
				if (strcmp(entry->d_name, ".") != 0 &&
				    strcmp(entry->d_name, "..") != 0)
					(void) unlinkat(dirfd(dir), entry->d_name, 0);
			(void) closedir(dir);
		}
		(void) rmdir(c->directory);
	}
}

/* A disk-backed identity set prevents aliases/hardlinks entering twice. */
static int
bd_seen_slot(FILE *fp, uint64_t capacity, uint64_t device, uint64_t object,
    boolean_t insert, boolean_t *found)
{
	uint64_t hash = (object ^ (device * UINT64_C(0x9e3779b97f4a7c15))) *
	    UINT64_C(0xbf58476d1ce4e5b9);
	for (uint64_t i = 0; i < capacity; i++) {
		uint64_t slot = (hash + i) & (capacity - 1);
		if (fseeko(fp, slot * sizeof (struct bd_seen), SEEK_SET) != 0)
			return (errno);
		struct bd_seen seen;
		if (fread(&seen, sizeof (seen), 1, fp) != 1)
			return (EIO);
		if (seen.occupied && seen.device == device &&
		    seen.object == object) {
			*found = B_TRUE;
			return (0);
		}
		if (!seen.occupied) {
			*found = B_FALSE;
			if (insert) {
				seen = (struct bd_seen){ device, object, 1 };
				if (fseeko(fp, slot * sizeof (seen), SEEK_SET) != 0 ||
				    fwrite(&seen, sizeof (seen), 1, fp) != 1)
					return (EIO);
			}
			return (0);
		}
	}
	return (EOVERFLOW);
}

static int
bd_seen(struct bd_catalog *c, const struct stat *st, boolean_t insert,
    boolean_t *found)
{
	if (insert && c->count >= c->capacity / 2) {
		if (c->capacity > INT64_MAX / 2 / sizeof (struct bd_seen))
			return (EOVERFLOW);
		uint64_t capacity = c->capacity * 2;
		FILE *larger = bd_tempfile(c);
		if (larger == NULL)
			return (errno);
		int error = 0;
		if (ftruncate(fileno(larger), capacity * sizeof (struct bd_seen))
		    != 0 || fseeko(c->seen, 0, SEEK_SET) != 0)
			error = errno;
		for (uint64_t i = 0; i < c->capacity && error == 0; i++) {
			struct bd_seen seen;
			if (fread(&seen, sizeof (seen), 1, c->seen) != 1) {
				error = EIO;
				break;
			}
			if (seen.occupied) {
				boolean_t duplicate;
				error = bd_seen_slot(larger, capacity, seen.device,
				    seen.object, B_TRUE, &duplicate);
			}
		}
		if (error != 0) {
			(void) fclose(larger);
			return (error);
		}
		(void) fclose(c->seen);
		c->seen = larger;
		c->capacity = capacity;
	}
	return (bd_seen_slot(c->seen, c->capacity, st->st_dev, st->st_ino,
	    insert, found));
}

static int
bd_inspect(const struct bd_file *file, boolean_t range, uint64_t offset,
    nvlist_t **result)
{
	nvlist_t *args = fnvlist_alloc();
	fnvlist_add_string(args, "op", "inspect");
	bd_identity(args, &file->identity, B_FALSE);
	if (range)
		fnvlist_add_uint64(args, "offset", offset);
	int error = lzc_blockdedup(file->dataset, args, result);
	fnvlist_free(args);
	return (error);
}

static int
bd_read(const struct bd_file *file, uint64_t offset, uint64_t length,
    uint8_t *buffer)
{
	nvlist_t *args = fnvlist_alloc(), *result = NULL;
	fnvlist_add_string(args, "op", "read");
	bd_identity(args, &file->identity, B_FALSE);
	fnvlist_add_uint64(args, "offset", offset);
	fnvlist_add_uint64(args, "length", length);
	int error = lzc_blockdedup(file->dataset, args, &result);
	fnvlist_free(args);
	uint8_t *bytes;
	uint_t count;
	if (error == 0) {
		if (result == NULL || nvlist_lookup_uint8_array(result, "bytes",
		    &bytes, &count) != 0 || count != length)
			error = EPROTO;
		else
			memcpy(buffer, bytes, count);
	}
	nvlist_free(result);
	return (error);
}

static int
bd_hash(const struct bd_file *file, uint64_t offset, uint64_t length,
    uint8_t *buffer, char hash[65])
{
	int error = 0;
	uint8_t digest[32];
#ifdef __APPLE__
	CC_SHA256_CTX ctx;
	CC_SHA256_Init(&ctx);
#else
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (ctx == NULL)
		return (ENOMEM);
	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1)
		error = EIO;
#endif
	for (uint64_t part = 0; error == 0 && part < length; ) {
		if (bd_stop) {
			error = ECANCELED;
			break;
		}
		uint64_t count = MIN(length - part, BD_READ_SIZE);
		error = bd_read(file, offset + part, count, buffer);
		if (error != 0)
			break;
#ifdef __APPLE__
		CC_SHA256_Update(&ctx, buffer, (CC_LONG)count);
#else
		if (EVP_DigestUpdate(ctx, buffer, count) != 1)
			error = EIO;
#endif
		part += count;
	}
#ifdef __APPLE__
	CC_SHA256_Final(digest, &ctx);
#else
	unsigned int size;
	if (error == 0 && (EVP_DigestFinal_ex(ctx, digest, &size) != 1 ||
	    size != sizeof (digest)))
		error = EIO;
	EVP_MD_CTX_free(ctx);
#endif
	if (error == 0)
		for (size_t i = 0; i < sizeof (digest); i++)
			(void) snprintf(hash + i * 2, 3, "%02x", digest[i]);
	return (error);
}

static int
bd_analyze(struct bd_catalog *c, const char *dataset, const char *path,
    uint64_t age)
{
	struct bd_file file;
	if (strlcpy(file.path, path, sizeof (file.path)) >= sizeof (file.path) ||
	    strlcpy(file.dataset, dataset, sizeof (file.dataset)) >=
	    sizeof (file.dataset))
		return (ENAMETOOLONG);
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return (errno);
	int error = fstat(fd, &file.identity) != 0 ? errno : 0;
	nvlist_t *info = NULL;
	uint8_t *buffer = NULL;
	off_t start = ftello(c->blocks);
	uint64_t blocks = 0;
	if (start < 0 && error == 0)
		error = errno;
	if (error != 0)
		goto out;
	time_t now = time(NULL);
	if (now == (time_t)-1) {
		error = EIO;
		goto out;
	}
	if (!S_ISREG(file.identity.st_mode) || file.identity.st_size == 0 ||
	    now < file.identity.st_mtime ||
	    difftime(now, file.identity.st_mtime) < (double)age * 86400) {
		c->skipped++;
		goto out;
	}
	boolean_t duplicate;
	if ((error = bd_seen(c, &file.identity, B_FALSE, &duplicate)) != 0)
		goto out;
	if (duplicate) {
		c->skipped++;
		goto out;
	}
	if ((error = bd_inspect(&file, B_FALSE, 0, &info)) != 0)
		goto out;
	uint64_t block;
	boolean_t eligible;
	if (info == NULL || nvlist_lookup_uint64(info, "block_size", &block)
	    != 0 || nvlist_lookup_boolean_value(info, "eligible", &eligible)
	    != 0 || block == 0 || block > BD_MAX_BLOCK) {
		error = EPROTO;
		goto out;
	}
	if (!eligible) {
		c->skipped++;
		goto out;
	}
	buffer = malloc(BD_READ_SIZE);
	if (buffer == NULL) {
		error = ENOMEM;
		goto out;
	}
	for (uint64_t offset = 0; offset < (uint64_t)file.identity.st_size &&
	    error == 0 && !bd_stop; offset += block) {
		nvlist_t *range = NULL;
		error = bd_inspect(&file, B_TRUE, offset, &range);
		uint64_t length;
		boolean_t hole, embedded;
		if (error == 0 && (range == NULL ||
		    nvlist_lookup_uint64(range, "block_length", &length) != 0 ||
		    nvlist_lookup_boolean_value(range, "block_hole", &hole) != 0 ||
		    nvlist_lookup_boolean_value(range, "block_embedded", &embedded)
		    != 0 || length != MIN(block,
		    (uint64_t)file.identity.st_size - offset)))
			error = EPROTO;
		nvlist_free(range);
		if (error != 0)
			break;
		if (hole || embedded)
			continue;
		char hash[65];
		if ((error = bd_hash(&file, offset, length, buffer, hash)) != 0)
			break;
		if (fprintf(c->blocks, "%020" PRIu64 " %020" PRIu64
		    " %020" PRIu64 " %s %020" PRIu64 " %020" PRIu64 "\n",
		    (uint64_t)file.identity.st_dev, block, length, hash, c->count,
		    offset) < 0)
			error = EIO;
		blocks++;
	}
	/* A file racing discovery contributes no partially hashed candidates. */
	struct stat final;
	if (error == 0 && (fstat(fd, &final) != 0 ||
	    !bd_same_identity(&file.identity, &final)))
		error = ESTALE;
	if (error == 0 && !bd_stop) {
		if ((error = bd_seen(c, &file.identity, B_TRUE, &duplicate)) == 0 &&
		    (fseeko(c->files, 0, SEEK_END) != 0 ||
		    fwrite(&file, sizeof (file), 1, c->files) != 1))
			error = EIO;
		if (error == 0) {
			c->count++;
			c->block_count += blocks;
		}
	}
out:
	free(buffer);
	nvlist_free(info);
	(void) close(fd);
	if ((error != 0 || bd_stop) && start >= 0) {
		if (fflush(c->blocks) != 0 || ftruncate(fileno(c->blocks), start)
		    != 0 || fseeko(c->blocks, start, SEEK_SET) != 0)
			return (EIO);
	}
	if (bd_deferred(error)) {
		(void) fprintf(stderr, "%s: deferred (%s)\n", path, strerror(error));
		c->deferred++;
		return (0);
	}
	return (error);
}

static int
bd_sort(struct bd_catalog *c)
{
	if (fflush(c->blocks) != 0 || fseeko(c->blocks, 0, SEEK_SET) != 0)
		return (errno);
	c->sorted = bd_tempfile(c);
	if (c->sorted == NULL)
		return (errno);
	pid_t pid = fork();
	if (pid < 0)
		return (errno);
	if (pid == 0) {
		if (dup2(fileno(c->blocks), STDIN_FILENO) < 0 ||
		    dup2(fileno(c->sorted), STDOUT_FILENO) < 0 ||
		    setenv("LC_ALL", "C", 1) != 0 ||
		    setenv("TMPDIR", c->directory, 1) != 0)
			_exit(126);
		execl("/usr/bin/sort", "sort", "-S", "4M", (char *)NULL);
		_exit(127);
	}
	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return (errno);
		if (bd_stop)
			(void) kill(pid, SIGTERM);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return (bd_stop ? ECANCELED : EIO);
	return (fseeko(c->sorted, 0, SEEK_SET) == 0 ? 0 : errno);
}

static int
bd_next(FILE *fp, struct bd_item *item)
{
	char line[256];
	if (fgets(line, sizeof (line), fp) == NULL)
		return (ferror(fp) ? EIO : ENOENT);
	char extra;
	return (sscanf(line, "%" SCNu64 " %" SCNu64 " %" SCNu64
	    " %64[0123456789abcdef] %" SCNu64 " %" SCNu64 " %c",
	    &item->device, &item->block, &item->length, item->hash,
	    &item->file, &item->offset, &extra) == 6 &&
	    strlen(item->hash) == 64 ? 0 : EPROTO);
}

static boolean_t
bd_same_item(const struct bd_item *a, const struct bd_item *b)
{
	return (a->device == b->device && a->block == b->block &&
	    a->length == b->length && strcmp(a->hash, b->hash) == 0);
}

static int
bd_get_file(struct bd_catalog *c, uint64_t id, struct bd_file *file)
{
	if (id >= c->count || id > INT64_MAX / sizeof (*file))
		return (EPROTO);
	if (fseeko(c->files, id * sizeof (*file), SEEK_SET) != 0 ||
	    fread(file, sizeof (*file), 1, c->files) != 1)
		return (EIO);
	return (0);
}

static int
bd_open_file(const struct bd_file *file, int *fd)
{
	*fd = open(file->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (*fd < 0)
		return (errno);
	struct stat st;
	if (fstat(*fd, &st) != 0)
		return (errno);
	return (bd_same_identity(&file->identity, &st) ? 0 : ESTALE);
}

static int
bd_apply(struct bd_catalog *c)
{
	int error = bd_sort(c);
	if (error != 0)
		return (error);
	struct bd_item canonical = { 0 }, item;
	struct bd_file source;
	boolean_t have = B_FALSE;
	int source_fd = -1;
	while (!bd_stop && (error = bd_next(c->sorted, &item)) == 0) {
		struct bd_file destination;
		if ((error = bd_get_file(c, item.file, &destination)) != 0)
			break;
		if (!have || !bd_same_item(&canonical, &item)) {
			if (source_fd >= 0)
				(void) close(source_fd);
			source_fd = -1;
			have = B_FALSE;
			error = bd_open_file(&destination, &source_fd);
			if (bd_deferred(error)) {
				c->deferred++;
				continue;
			}
			if (error != 0)
				break;
			canonical = item;
			source = destination;
			have = B_TRUE;
			continue;
		}
		if (canonical.file == item.file && canonical.offset == item.offset)
			continue;
		c->candidates++;
		if (c->dry_run)
			continue;
		int fd = -1;
		error = canonical.file == item.file ? 0 :
		    bd_open_file(&destination, &fd);
		if (error == 0) {
			nvlist_t *args = fnvlist_alloc(), *result = NULL;
			fnvlist_add_string(args, "op", "share-blocks");
			bd_identity(args, &destination.identity, B_FALSE);
			bd_identity(args, &source.identity, B_TRUE);
			fnvlist_add_uint64(args, "offset", item.offset);
			fnvlist_add_uint64(args, "source_offset", canonical.offset);
			fnvlist_add_uint64(args, "length", item.length);
			error = lzc_blockdedup(destination.dataset, args, &result);
			if (error == 0) {
				uint64_t shared, already;
				if (result == NULL || nvlist_lookup_uint64(result,
				    "shared_bytes", &shared) != 0 ||
				    nvlist_lookup_uint64(result, "already_shared_bytes",
				    &already) != 0 || shared > item.length ||
				    already != item.length - shared)
					error = EPROTO;
				else if (shared != 0)
					c->shared++;
				else
					c->already++;
			}
			fnvlist_free(args);
			nvlist_free(result);
		}
		if (fd >= 0)
			(void) close(fd);
		if (bd_deferred(error)) {
			(void) fprintf(stderr, "%s: deferred (%s)\n",
			    destination.path, strerror(error));
			c->deferred++;
			/* The source may be stale; allow a fresh representative. */
			have = B_FALSE;
			error = 0;
		} else if (error == EILSEQ || error == ENOTSUP) {
			(void) fprintf(stderr, "%s: candidate left ordinary (%s)\n",
			    destination.path, strerror(error));
			c->skipped++;
			error = 0;
		}
		if (error != 0)
			break;
	}
	if (source_fd >= 0)
		(void) close(source_fd);
	return (error == ENOENT ? 0 : error);
}

static int
bd_pass(uint64_t age, const char *tmpdir, boolean_t recursive,
    boolean_t dry_run, int argc, char **argv)
{
	struct bd_catalog c;
	int error = bd_init(&c, tmpdir, dry_run);
	if (error != 0) {
		(void) fprintf(stderr, "blockdedup catalog: %s\n", strerror(error));
		bd_fini(&c);
		return (1);
	}
	int ret = 0;
	for (int i = 0; i < argc && !bd_stop; i++) {
		struct stat st;
		boolean_t dataset_target = lstat(argv[i], &st) != 0;
		zfs_handle_t *zhp = dataset_target ?
		    zfs_open(g_zfs, argv[i], ZFS_TYPE_FILESYSTEM) :
		    zfs_path_to_zhandle(g_zfs, argv[i], ZFS_TYPE_FILESYSTEM);
		if (zhp == NULL) {
			ret = 1;
			continue;
		}
		char path[PATH_MAX];
		char *mountpoint = NULL;
		error = 0;
		if (zfs_get_type(zhp) != ZFS_TYPE_FILESYSTEM ||
		    strchr(zfs_get_name(zhp), '@') != NULL) {
			error = ENOTSUP;
			goto target_done;
		}
		if (dataset_target) {
			if (!zfs_is_mounted(zhp, &mountpoint)) {
				error = ENOENT;
				goto target_done;
			}
			if (strlcpy(path, mountpoint, sizeof (path)) >= sizeof (path)) {
				error = ENAMETOOLONG;
				goto target_done;
			}
		} else if (strlcpy(path, argv[i], sizeof (path)) >= sizeof (path)) {
			error = ENAMETOOLONG;
			goto target_done;
		}
		char *paths[] = { path, NULL };
		FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV,
		    NULL);
		if (tree == NULL) {
			error = errno;
			goto target_done;
		}
		FTSENT *entry;
		while (!bd_stop) {
			errno = 0;
			entry = fts_read(tree);
			if (entry == NULL) {
				if (errno != 0)
					error = errno;
				break;
			}
			if (entry->fts_info == FTS_D &&
			    (strcmp(entry->fts_name, ".zfs") == 0 ||
			    (entry->fts_level > 0 && !recursive && !dataset_target))) {
				(void) fts_set(tree, entry, FTS_SKIP);
			} else if (entry->fts_info == FTS_F) {
				int e = bd_analyze(&c, zfs_get_name(zhp),
				    entry->fts_path, age);
				if (e != 0) {
					(void) fprintf(stderr, "%s: %s\n", entry->fts_path,
					    strerror(e));
					error = e;
				}
			} else if (entry->fts_info == FTS_ERR ||
			    entry->fts_info == FTS_DNR || entry->fts_info == FTS_NS) {
				error = entry->fts_errno;
			}
		}
		if (fts_close(tree) != 0 && error == 0)
			error = errno;
target_done:
		free(mountpoint);
		zfs_close(zhp);
		if (error != 0) {
			(void) fprintf(stderr, "%s: %s\n", argv[i], strerror(error));
			ret = 1;
		}
	}
	/* Discovery must finish successfully before installing any sharing. */
	if (ret == 0 && !bd_stop && (error = bd_apply(&c)) != 0) {
		(void) fprintf(stderr, "blockdedup optimize: %s\n", strerror(error));
		ret = 1;
	}
	(void) printf("blockdedup: files=%" PRIu64 " blocks=%" PRIu64
	    " candidates=%" PRIu64 " shared=%" PRIu64
	    " already_shared=%" PRIu64 " deferred=%" PRIu64
	    " skipped=%" PRIu64 " dry_run=%u\n", c.count, c.block_count,
	    c.candidates, c.shared, c.already, c.deferred, c.skipped,
	    dry_run ? 1U : 0U);
	(void) fflush(stdout);
	bd_fini(&c);
	return (ret);
}

int
zfs_do_blockdedup(int argc, char **argv)
{
	bd_stop = 0;
	if (argc < 3)
		return (2);
	boolean_t watch = strcmp(argv[1], "watch") == 0;
	if (!watch && strcmp(argv[1], "optimize") != 0)
		return (2);
	if (geteuid() != 0) {
		(void) fprintf(stderr, "blockdedup requires root\n");
		return (1);
	}
	boolean_t recursive = B_FALSE, dry_run = B_FALSE, once = B_FALSE;
	uint64_t age = 3, interval = 3600;
	const char *tmpdir = "/tmp";
	const struct option options[] = {
		{ "min-age", required_argument, NULL, 'a' },
		{ "interval", required_argument, NULL, 'i' },
		{ "tmpdir", required_argument, NULL, 't' },
		{ "recursive", no_argument, NULL, 'r' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "once", no_argument, NULL, '1' },
		{ NULL, 0, NULL, 0 }
	};
	optind = 2;
	int option;
	while ((option = getopt_long(argc, argv, "+rna:i:t:1", options, NULL))
	    != -1) {
		if (option == 'r')
			recursive = B_TRUE;
		else if (option == 'n')
			dry_run = B_TRUE;
		else if (option == 't')
			tmpdir = optarg;
		else if (option == '1') {
			if (!watch)
				return (2);
			once = B_TRUE;
		} else if (option == 'a' || option == 'i') {
			char *end;
			errno = 0;
			uint64_t value = strtoull(optarg, &end, 10);
			if (errno != 0 || *end != '\0' || optarg[0] < '0' ||
			    optarg[0] > '9')
				return (2);
			if (option == 'a') {
				if (value > UINT64_MAX / 86400)
					return (2);
				age = value;
			} else {
				if (!watch || value == 0 || value > INT_MAX)
					return (2);
				interval = value;
			}
		} else
			return (2);
	}
	if (optind == argc)
		return (2);
	struct sigaction action = { 0 }, old_int, old_term;
	action.sa_handler = bd_signal;
	(void) sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &old_int) != 0)
		return (1);
	if (sigaction(SIGTERM, &action, &old_term) != 0) {
		(void) sigaction(SIGINT, &old_int, NULL);
		return (1);
	}
	int ret;
	do {
		ret = bd_pass(age, tmpdir, recursive, dry_run, argc - optind,
		    argv + optind);
		/* A failed pass must be surfaced, never hidden by supervision. */
		if (!watch || once || bd_stop || ret != 0)
			break;
		struct timespec delay = { .tv_sec = (time_t)interval };
		while (!bd_stop && nanosleep(&delay, &delay) != 0) {
			if (errno != EINTR) {
				ret = 1;
				bd_stop = 1;
			}
		}
	} while (!bd_stop);
	(void) sigaction(SIGINT, &old_int, NULL);
	(void) sigaction(SIGTERM, &old_term, NULL);
	return (ret);
}
