/* SPDX-License-Identifier: CDDL-1.0 */
/* Native, bounded, post-process chunk deduplication worker. */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/zfs_chunkdedup.h>
#include <libzfs.h>
#include <libzfs_core.h>
#include <libnvpair.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <fts.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "chunkdedup_chunker.h"
#include "chunkdedup_hybrid.h"
#include "chunkdedup_parallel.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#include "zfs_util.h"

/* A signal may arrive on any scan thread. Keep the handler lock-free. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "lock-free signal flag required");
static atomic_int cd_stop;

static void
cd_signal(int signal)
{
	(void) signal;
	cd_stop = 1;
}

/* Reopen for a fresh property view between background conversion batches. */
static int
cd_enabled(const char *dataset, boolean_t *enabled)
{
	zfs_handle_t *zhp = zfs_open(g_zfs, dataset, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (ENOENT);
	uint64_t value;
	int error = zfs_prop_get_numeric(zhp, ZFS_PROP_CHUNKDEDUP, &value,
	    NULL, NULL, 0);
	zfs_close(zhp);
	if (error != 0)
		return (EIO);
	*enabled = value != 0;
	return (0);
}

struct cd_worker {
	const char *dataset;
	const char *path;
	struct stat initial;
	int fd;
	boolean_t watch;
	uint64_t position;
	uint64_t batches;
	uint64_t offsets[ZFS_CHUNKDEDUP_MAX_CHUNKS];
	uint64_t lengths[ZFS_CHUNKDEDUP_MAX_CHUNKS];
	uint8_t hashes[ZFS_CHUNKDEDUP_MAX_CHUNKS * 32];
	uint_t count;
};

static void
cd_identity(nvlist_t *args, const struct stat *st)
{
	fnvlist_add_uint64(args, "object", st->st_ino);
#ifdef __FreeBSD__
	fnvlist_add_uint64(args, "generation", st->st_gen);
	fnvlist_add_uint64(args, "mtime_sec", st->st_mtim.tv_sec);
	fnvlist_add_uint64(args, "mtime_nsec", st->st_mtim.tv_nsec);
#else
	fnvlist_add_uint64(args, "generation", 0);
#endif
	fnvlist_add_uint64(args, "size", st->st_size);
}

static int
cd_submit(struct cd_worker *w)
{
	if (cd_stop)
		return (ECANCELED);
	if (w->position >= (uint64_t)w->initial.st_size || w->count == 0)
		return (EPROTO);
	uint64_t remaining = (uint64_t)w->initial.st_size - w->position;
	uint64_t length = MIN(remaining, ZFS_CHUNKDEDUP_MAX_RANGE);
	uint64_t covered = w->offsets[0];
	if (covered > w->position)
		return (EPROTO);
	for (uint_t i = 0; i < w->count; i++) {
		if (w->offsets[i] != covered || w->lengths[i] == 0 ||
		    w->lengths[i] > UINT64_MAX - covered)
			return (EPROTO);
		covered += w->lengths[i];
	}
	if (covered < w->position + length)
		return (EPROTO);
	if (w->watch) {
		boolean_t enabled;
		int error = cd_enabled(w->dataset, &enabled);
		if (error != 0)
			return (error);
		if (!enabled)
			return (ECANCELED);
	}
	nvlist_t *args = fnvlist_alloc();
	fnvlist_add_string(args, "op", "optimize");
	cd_identity(args, &w->initial);
	fnvlist_add_uint64(args, "offset", w->position);
	fnvlist_add_uint64(args, "length", length);
	fnvlist_add_uint64_array(args, "chunk_offsets", w->offsets, w->count);
	fnvlist_add_uint64_array(args, "chunk_lengths", w->lengths, w->count);
	fnvlist_add_uint8_array(args, "chunk_hashes", w->hashes, w->count * 32);
	int error = lzc_chunkdedup(w->dataset, args, NULL);
	fnvlist_free(args);
	if (error != 0 && error != EALREADY)
		return (error);
	w->position += length;
	w->batches++;
	uint_t keep = 0;
	for (uint_t i = 0; i < w->count; i++) {
		if (w->offsets[i] + w->lengths[i] <= w->position)
			continue;
		w->offsets[keep] = w->offsets[i];
		w->lengths[keep] = w->lengths[i];
		memmove(w->hashes + keep * 32, w->hashes + i * 32, 32);
		keep++;
	}
	w->count = keep;
	return (0);
}

static int
cd_found(void *arg, const struct chunkdedup_chunk *chunk,
    const uint8_t *bytes)
{
	struct cd_worker *w = arg;
	(void) bytes;
	if (cd_stop)
		return (ECANCELED);
	if (w->count == ZFS_CHUNKDEDUP_MAX_CHUNKS)
		return (EOVERFLOW);
	w->offsets[w->count] = chunk->offset;
	w->lengths[w->count] = chunk->length;
	memcpy(w->hashes + w->count * 32, chunk->hash, 32);
	w->count++;
	while (w->position < (uint64_t)w->initial.st_size) {
		uint64_t end = MIN((uint64_t)w->initial.st_size,
		    w->position + ZFS_CHUNKDEDUP_MAX_RANGE);
		if (chunk->offset + chunk->length < end)
			break;
		int error = cd_submit(w);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
cd_file(const char *dataset, const char *path, const char *op,
    uint64_t age_days, uint64_t *processed, uint64_t *skipped,
    boolean_t watch, const struct stat *expected)
{
	if (cd_stop)
		return (0);
	struct cd_worker w = { .dataset = dataset, .path = path, .fd = -1,
	    .watch = watch };
	w.fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (w.fd < 0)
		return (errno);
	int error = 0;
	if (fstat(w.fd, &w.initial) != 0) {
		error = errno;
		goto done;
	}
	if (!S_ISREG(w.initial.st_mode)) {
		error = EINVAL;
		goto done;
	}
	if (expected != NULL && (w.initial.st_dev != expected->st_dev ||
	    w.initial.st_ino != expected->st_ino ||
	    w.initial.st_size != expected->st_size ||
	    w.initial.st_mtime != expected->st_mtime
#ifdef __FreeBSD__
	    || w.initial.st_gen != expected->st_gen ||
	    w.initial.st_mtim.tv_nsec != expected->st_mtim.tv_nsec
#endif
	    )) {
		error = ESTALE;
		goto done;
	}
	if (strcmp(op, "optimize") != 0) {
		nvlist_t *args = fnvlist_alloc();
		fnvlist_add_string(args, "op", op);
		cd_identity(args, &w.initial);
		error = lzc_chunkdedup(dataset, args, NULL);
		fnvlist_free(args);
		if (error == 0)
			(*processed)++;
		goto done;
	}
	time_t now = time(NULL);
	if (now == (time_t)-1) {
		error = EIO;
		goto done;
	}
	if (w.initial.st_size == 0 ||
	    now < w.initial.st_mtime ||
	    difftime(now, w.initial.st_mtime) < (double)age_days * 86400) {
		(*skipped)++;
		goto done;
	}
	struct chunkdedup_chunker *chunker = NULL;
	error = chunkdedup_chunker_create(&chunker, cd_found, &w);
	if (error != 0)
		goto done;
	uint64_t source_offset = 0;
	while (source_offset < (uint64_t)w.initial.st_size) {
		if (cd_stop) {
			error = ECANCELED;
			break;
		}
		/* This logical-read ioctl preserves the source atime. */
		uint64_t length = MIN((uint64_t)w.initial.st_size -
		    source_offset, 256U << 10);
		nvlist_t *args = fnvlist_alloc(), *result = NULL;
		fnvlist_add_string(args, "op", "read");
		cd_identity(args, &w.initial);
		fnvlist_add_uint64(args, "offset", source_offset);
		fnvlist_add_uint64(args, "length", length);
		error = lzc_chunkdedup(dataset, args, &result);
		fnvlist_free(args);
		if (error != 0) {
			nvlist_free(result);
			if (error == EINTR && !cd_stop)
				continue;
			break;
		}
		uint8_t *bytes;
		uint_t count;
		if (result == NULL || nvlist_lookup_uint8_array(result,
		    "bytes", &bytes, &count) != 0 || count != length) {
			error = EPROTO;
		} else {
			error = chunkdedup_chunker_push(chunker, bytes, count);
		}
		nvlist_free(result);
		if (error != 0)
			break;
		source_offset += length;
	}
	if (error == 0)
		error = chunkdedup_chunker_finish(chunker);
	if (error == 0 && w.position != (uint64_t)w.initial.st_size)
		error = EPROTO;
	chunkdedup_chunker_destroy(chunker);
	if (error == ECANCELED && watch) {
		(*skipped)++;
		error = 0;
	} else if (error == EBUSY || error == ESTALE || error == EAGAIN) {
		(void) fprintf(stderr, "%s: deferred (%s)\n", path,
		    strerror(error));
		(*skipped)++;
		error = 0;
	} else if (error == 0) {
		(*processed)++;
		(void) printf("%s: optimized %" PRIu64 " batches\n", path,
		    w.batches);
	}
done:
	(void) close(w.fd);
	return (error);
}

/*
 * A pass is analyzed completely before any conversion. File metadata and the
 * file records live in a private temporary file; compact group state is
 * kept in bounded memory during selection (disk fallback on allocation failure); sort spills catalogs to
 * a private directory with a 4 MiB sort buffer. No content index survives a
 * pass. Connecting both CDC and native matches prevents charging a seed to
 * one policy group while silently reusing it in another.
 */
struct cd_catalog_file {
	char dataset[ZFS_MAX_DATASET_NAME_LEN];
	char path[PATH_MAX];
	struct stat identity;
	uint64_t parent, dataset_id, block_size, native_base, chunk_base;
	uint64_t native_estimate, chunk_estimate;
	uint64_t ashift, parity, columns, copies;
	boolean_t chunked, uncertain, deferred;
	enum cd_method method;
};

#define	CD_SYNC_BATCH_FILES	256

struct cd_retry_file {
	char dataset[ZFS_MAX_DATASET_NAME_LEN];
	char path[PATH_MAX];
	struct stat identity;
};

struct cd_catalog_group {
	uint64_t parent, native_estimate, chunk_estimate;
	boolean_t uncertain;
};

#ifndef CD_CATALOG_GROUP_LIMIT
#define CD_CATALOG_GROUP_LIMIT (UINT64_C(256) << 20)
#endif

struct cd_catalog {
	FILE *files, *blocks, *chunks, *sorted_blocks, *sorted_chunks, *seen;
	struct cd_catalog_group *groups;
	FILE *report;
	const char *report_path;
	char report_temporary[PATH_MAX];
	char directory[PATH_MAX];
	uint64_t count, processed, skipped;
	uint64_t seen_capacity;
	enum cd_method method;
	boolean_t watch;
	/* Worker-private spools; only the coordinator owns seen/retry/IDs. */
	boolean_t staging;
	atomic_int *cancel;
	struct cd_retry_file *retry;
	uint_t retry_count;
};

struct cd_catalog_item {
	uint64_t dataset, length, file, offset, bytes, object;
	char hash[65];
};

static boolean_t cd_deferred(int);

static boolean_t
cd_catalog_stopped(const struct cd_catalog *c)
{
	return (cd_stop || (c->cancel != NULL && atomic_load(c->cancel)));
}

struct cd_catalog_seen {
	uint64_t device, object, occupied;
};

static int
cd_catalog_seen_slot(FILE *table, uint64_t capacity, uint64_t device,
    uint64_t object, boolean_t insert, boolean_t *found)
{
	uint64_t hash = (object ^ (device * UINT64_C(0x9e3779b97f4a7c15))) *
	    UINT64_C(0xbf58476d1ce4e5b9);
	for (uint64_t n = 0; n < capacity; n++) {
		uint64_t slot = (hash + n) & (capacity - 1);
		if (slot > INT64_MAX / sizeof (struct cd_catalog_seen) ||
		    fseeko(table, slot * sizeof (struct cd_catalog_seen),
		    SEEK_SET) != 0)
			return (EOVERFLOW);
		struct cd_catalog_seen entry;
		if (fread(&entry, sizeof (entry), 1, table) != 1)
			return (EIO);
		if (entry.occupied && entry.device == device &&
		    entry.object == object) {
			*found = B_TRUE;
			return (0);
		}
		if (!entry.occupied) {
			*found = B_FALSE;
			if (!insert)
				return (0);
			entry = (struct cd_catalog_seen){ device, object, 1 };
			if (fseeko(table, slot * sizeof (entry), SEEK_SET) != 0 ||
			    fwrite(&entry, sizeof (entry), 1, table) != 1)
				return (EIO);
			return (0);
		}
	}
	return (EOVERFLOW);
}

static int
cd_catalog_seen(struct cd_catalog *c, const struct stat *identity,
    boolean_t insert, boolean_t *found)
{
	if (insert && c->count >= c->seen_capacity / 2) {
		uint64_t capacity = c->seen_capacity * 2;
		if (capacity < c->seen_capacity ||
		    capacity > INT64_MAX / sizeof (struct cd_catalog_seen))
			return (EOVERFLOW);
		FILE *larger = tmpfile();
		if (larger == NULL)
			return (errno);
		int error = 0;
		if (ftruncate(fileno(larger), capacity *
		    sizeof (struct cd_catalog_seen)) != 0 ||
		    fseeko(c->seen, 0, SEEK_SET) != 0)
			error = errno;
		for (uint64_t i = 0; error == 0 && i < c->seen_capacity; i++) {
			struct cd_catalog_seen entry;
			if (fread(&entry, sizeof (entry), 1, c->seen) != 1) {
				error = EIO;
				break;
			}
			if (entry.occupied) {
				boolean_t duplicate;
				error = cd_catalog_seen_slot(larger, capacity,
				    entry.device, entry.object, B_TRUE, &duplicate);
			}
		}
		if (error != 0) {
			(void) fclose(larger);
			return (error);
		}
		(void) fclose(c->seen);
		c->seen = larger;
		c->seen_capacity = capacity;
	}
	return (cd_catalog_seen_slot(c->seen, c->seen_capacity,
	    (uint64_t)identity->st_dev, identity->st_ino, insert, found));
}

static int
cd_catalog_record(struct cd_catalog *c, uint64_t id,
    struct cd_catalog_file *file, boolean_t write)
{
	if (id >= c->count || id > INT64_MAX / sizeof (*file))
		return (EOVERFLOW);
	if (fseeko(c->files, (off_t)(id * sizeof (*file)), SEEK_SET) != 0)
		return (errno);
	if (write) {
		if (fwrite(file, sizeof (*file), 1, c->files) != 1)
			return (errno != 0 ? errno : EIO);
	} else if (fread(file, sizeof (*file), 1, c->files) != 1) {
		return (errno != 0 ? errno : EIO);
	}
	return (0);
}

static int
cd_catalog_root(struct cd_catalog *c, uint64_t id, uint64_t *root)
{
	struct cd_catalog_file file;
	uint64_t start = id;
	if (c->groups != NULL) {
		for (uint64_t steps = 0; steps <= c->count; steps++) {
			if (id >= c->count)
				return (EOVERFLOW);
			uint64_t parent = c->groups[id].parent;
			if (parent == id) {
				*root = id;
				while (start != id) {
					uint64_t next = c->groups[start].parent;
					c->groups[start].parent = id;
					start = next;
				}
				return (0);
			}
			id = parent;
		}
		return (EPROTO);
	}
	for (uint64_t steps = 0; steps <= c->count; steps++) {
		int error = cd_catalog_record(c, id, &file, B_FALSE);
		if (error != 0)
			return (error);
		if (file.parent == id) {
			*root = id;
			if (start != id && cd_catalog_record(c, start,
			    &file, B_FALSE) == 0) {
				file.parent = id;
				return (cd_catalog_record(c, start, &file,
				    B_TRUE));
			}
			return (0);
		}
		id = file.parent;
	}
	return (EPROTO);
}

static int
cd_catalog_join(struct cd_catalog *c, uint64_t a, uint64_t b)
{
	uint64_t ar, br;
	int error = cd_catalog_root(c, a, &ar);
	if (error == 0)
		error = cd_catalog_root(c, b, &br);
	if (error != 0 || ar == br)
		return (error);
	struct cd_catalog_file file;
	uint64_t parent = MIN(ar, br), child = MAX(ar, br);
	if (c->groups != NULL) {
		c->groups[child].parent = parent;
		return (0);
	}
	if ((error = cd_catalog_record(c, child, &file, B_FALSE)) == 0) {
		file.parent = parent;
		error = cd_catalog_record(c, child, &file, B_TRUE);
	}
	return (error);
}

static int
cd_catalog_next(FILE *fp, struct cd_catalog_item *item)
{
	char line[256];
	if (fgets(line, sizeof (line), fp) == NULL)
		return (ferror(fp) ? EIO : ENOENT);
	if (strchr(line, '\n') == NULL || sscanf(line,
	    "%" SCNu64 " %64s %" SCNu64 " %" SCNu64 " %" SCNu64
	    " %" SCNu64 " %" SCNu64, &item->dataset, item->hash,
	    &item->length, &item->file, &item->offset, &item->bytes,
	    &item->object) != 7 || strlen(item->hash) != 64)
		return (EPROTO);
	return (0);
}

static boolean_t
cd_catalog_same(const struct cd_catalog_item *a,
    const struct cd_catalog_item *b)
{
	return (a->dataset == b->dataset && a->length == b->length &&
	    strcmp(a->hash, b->hash) == 0);
}

static int
cd_catalog_append(FILE *fp, uint64_t dataset, const uint8_t *hash,
    uint64_t length, uint64_t file, uint64_t offset, uint64_t bytes,
    uint64_t object)
{
	char hex[65];
	for (size_t i = 0; i < 32; i++)
		(void) snprintf(hex + i * 2, 3, "%02x", hash[i]);
	return (fprintf(fp, "%020" PRIu64 " %s %020" PRIu64 " %020"
	    PRIu64 " %020" PRIu64 " %020" PRIu64 " %020" PRIu64 "\n",
	    dataset, hex, length, file, offset, bytes, object) < 0 ? EIO : 0);
}

static int
cd_inspect(const char *dataset, const struct stat *identity,
    boolean_t range, uint64_t offset, nvlist_t **result)
{
	nvlist_t *args = fnvlist_alloc();
	fnvlist_add_string(args, "op", "inspect");
	cd_identity(args, identity);
	if (range)
		fnvlist_add_uint64(args, "offset", offset);
	int error = lzc_chunkdedup(dataset, args, result);
	fnvlist_free(args);
	return (error);
}

static int
cd_catalog_read(const struct cd_catalog_file *file, uint64_t offset,
    uint64_t length, uint8_t *buffer)
{
	nvlist_t *args = fnvlist_alloc(), *result = NULL;
	fnvlist_add_string(args, "op", "read");
	cd_identity(args, &file->identity);
	fnvlist_add_uint64(args, "offset", offset);
	fnvlist_add_uint64(args, "length", length);
	int error = lzc_chunkdedup(file->dataset, args, &result);
	fnvlist_free(args);
	uint8_t *bytes;
	uint_t count;
	if (error == 0) {
		if (result == NULL || nvlist_lookup_uint8_array(result,
		    "bytes", &bytes, &count) != 0 || count != length)
			error = EPROTO;
		else
			memcpy(buffer, bytes, count);
	}
	nvlist_free(result);
	return (error);
}

struct cd_catalog_chunker {
	struct cd_catalog *catalog;
	struct cd_catalog_file *file;
	uint64_t id;
};

static int cd_catalog_retry(struct cd_catalog *, uint64_t);

static int
cd_catalog_enqueue(struct cd_catalog *c, const struct cd_catalog_file *file,
    uint64_t age)
{
	if (cd_stop)
		return (ECANCELED);
	if (c->retry_count != 0 && (c->retry_count == CD_SYNC_BATCH_FILES ||
	    strcmp(c->retry[0].dataset, file->dataset) != 0)) {
		int error = cd_catalog_retry(c, age);
		if (error != 0)
			return (error);
	}
	if (cd_stop)
		return (ECANCELED);
	struct cd_retry_file *retry = &c->retry[c->retry_count++];
	(void) strlcpy(retry->dataset, file->dataset, sizeof (retry->dataset));
	(void) strlcpy(retry->path, file->path, sizeof (retry->path));
	retry->identity = file->identity;
	return (0);
}

static int
cd_catalog_chunk(void *argument, const struct chunkdedup_chunk *chunk,
    const uint8_t *bytes)
{
	struct cd_catalog_chunker *arg = argument;
	struct cd_catalog_file *file = arg->file;
	(void) bytes;
	if (cd_catalog_stopped(arg->catalog))
		return (ECANCELED);
	uint64_t allocation = 0;
	int error = cd_hybrid_alloc(chunk->length, file->ashift,
	    file->parity, file->columns, file->copies, &allocation);
	if (error != 0) {
		file->uncertain = B_TRUE;
		allocation = UINT64_MAX / 4;
	}
	/* Dnode plus hash/ref ZAP space; intentionally uncompressed bounds. */
	uint64_t metadata;
	if (cd_hybrid_alloc(UINT64_C(3) << MAX(12,
	    MIN(file->ashift, 24)), file->ashift, file->parity,
	    file->columns, file->copies, &metadata) != 0) {
		file->uncertain = B_TRUE;
		metadata = UINT64_C(3) << 24;
	}
	if (cd_hybrid_add(&allocation, metadata) != 0 ||
	    cd_hybrid_add(&file->chunk_base, 64) != 0)
		return (EOVERFLOW);
	nvlist_t *args = fnvlist_alloc(), *result = NULL;
	uint64_t length = chunk->length, *objects, *asizes;
	uint_t nobjects, nasizes;
	fnvlist_add_string(args, "op", "chunk-lookup");
	fnvlist_add_uint64_array(args, "chunk_lengths", &length, 1);
	fnvlist_add_uint8_array(args, "chunk_hashes",
	    (uint8_t *)chunk->hash, 32);
	error = lzc_chunkdedup(file->dataset, args, &result);
	fnvlist_free(args);
	uint64_t object = 0;
	if (error == 0 && result != NULL &&
	    nvlist_lookup_uint64_array(result, "chunk_objects", &objects,
	    &nobjects) == 0 && nobjects == 1 &&
	    nvlist_lookup_uint64_array(result, "chunk_asizes", &asizes,
	    &nasizes) == 0 && nasizes == 1) {
		object = objects[0];
	} else {
		file->uncertain = B_TRUE;
	}
	nvlist_free(result);
	return (cd_catalog_append(arg->catalog->chunks, file->dataset_id,
	    chunk->hash, chunk->length, arg->id, chunk->offset,
	    allocation, object));
}

static int
cd_catalog_analyze(struct cd_catalog *c, const char *dataset,
    const char *path, uint64_t age, const struct stat *expected,
    boolean_t retryable)
{
	off_t block_start = ftello(c->blocks), chunk_start = ftello(c->chunks);
	if (block_start < 0 || chunk_start < 0)
		return (errno);
	boolean_t appended = B_FALSE;
	struct cd_catalog_file file = { .parent = c->count,
	    .method = c->method };
	if (strlcpy(file.dataset, dataset, sizeof (file.dataset)) >=
	    sizeof (file.dataset) || strlcpy(file.path, path,
	    sizeof (file.path)) >= sizeof (file.path))
		return (ENAMETOOLONG);
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0 && expected != NULL &&
	    (errno == ENOENT || errno == ELOOP || errno == ESTALE)) {
		c->skipped++;
		return (0);
	}
	if (fd < 0)
		return (errno);
	int error = fstat(fd, &file.identity) != 0 ? errno : 0;
	nvlist_t *info = NULL;
	if (error != 0)
		goto done;
	if (expected != NULL && (file.identity.st_dev != expected->st_dev ||
	    file.identity.st_ino != expected->st_ino ||
	    file.identity.st_size != expected->st_size ||
	    file.identity.st_mtime != expected->st_mtime
#ifdef __FreeBSD__
	    || file.identity.st_gen != expected->st_gen ||
	    file.identity.st_mtim.tv_nsec != expected->st_mtim.tv_nsec
#endif
	    )) {
		error = ESTALE;
		goto done;
	}
	time_t now = time(NULL);
	if (now == (time_t)-1) {
		error = EIO;
		goto done;
	}
	if (!S_ISREG(file.identity.st_mode) || file.identity.st_size == 0 ||
	    now < file.identity.st_mtime ||
	    difftime(now, file.identity.st_mtime) < (double)age * 86400) {
		c->skipped++;
		goto done;
	}
	/* Mounted datasets have distinct device ids; hard links share one inode. */
	file.dataset_id = (uint64_t)file.identity.st_dev;
	boolean_t duplicate;
	if (!c->staging && (error = cd_catalog_seen(c, &file.identity, B_FALSE,
	    &duplicate)) != 0)
		goto done;
	if (!c->staging && duplicate) {
		c->skipped++;
		goto done;
	}
	if ((error = cd_inspect(dataset, &file.identity, B_FALSE, 0,
	    &info)) != 0)
		goto done;
	boolean_t eligible, supported;
	uint64_t allocated;
	if (nvlist_lookup_uint64(info, "block_size", &file.block_size) != 0 ||
	    nvlist_lookup_uint64(info, "allocated_bytes", &allocated) != 0 ||
	    nvlist_lookup_boolean_value(info, "chunked", &file.chunked) != 0 ||
	    nvlist_lookup_boolean_value(info, "eligible", &eligible) != 0 ||
	    nvlist_lookup_boolean_value(info, "geometry_supported", &supported)
	    != 0 || nvlist_lookup_uint64(info, "ashift", &file.ashift) != 0 ||
	    nvlist_lookup_uint64(info, "parity", &file.parity) != 0 ||
	    nvlist_lookup_uint64(info, "data_columns", &file.columns) != 0 ||
	    nvlist_lookup_uint64(info, "copies", &file.copies) != 0 ||
	    file.block_size == 0 || file.block_size > (16U << 20)) {
		error = EPROTO;
		goto done;
	}
	if (!eligible) {
		c->skipped++;
		goto done;
	}
	file.uncertain = !supported;
	if (file.chunked) {
		/* Resume CDC, never silently hydrate to satisfy block mode. */
		if (c->method == CD_METHOD_BLOCK) {
			c->skipped++;
			(void) printf("%s: deferred (existing chunked file; "
			    "unoptimize first)\n", path);
			goto done;
		}
		file.method = CD_METHOD_CHUNK;
		goto append;
	}
	uint64_t records = (uint64_t)file.identity.st_size / file.block_size +
	    ((uint64_t)file.identity.st_size % file.block_size != 0);
	if (file.block_size > ZFS_CHUNKDEDUP_MAX_RECORD ||
	    records >= ZFS_CHUNKDEDUP_MAX_ENTRIES) {
		/* The existing CDC format has a bounded record map. */
		if (c->method == CD_METHOD_CHUNK) {
			error = ENOTSUP;
			goto done;
		}
		file.uncertain = B_TRUE;
	}
	if (file.ashift > 24 || records > UINT64_MAX /
	    (UINT64_C(1) << MIN(file.ashift, 24))) {
		error = EOVERFLOW;
		goto done;
	}
	/* File map plus one conservatively allocated recipe record per record. */
	if (records > UINT64_MAX / 2 - 3 ||
	    (records * 2 + 3) > UINT64_MAX /
	    (UINT64_C(1) << MAX(12, MIN(file.ashift, 24))) ||
	    cd_hybrid_alloc((records * 2 + 3) << MAX(12,
	    MIN(file.ashift, 24)), file.ashift, file.parity,
	    file.columns, file.copies, &file.chunk_base) != 0) {
		file.uncertain = B_TRUE;
		file.chunk_base = UINT64_MAX / 4;
	}
	struct cd_catalog_chunker argument = { c, &file, c->count };
	struct chunkdedup_chunker *chunker = NULL;
	if ((error = chunkdedup_chunker_create(&chunker, cd_catalog_chunk,
	    &argument)) != 0)
		goto done;
	uint8_t *buffer = malloc(256U << 10);
	if (buffer == NULL) {
		chunkdedup_chunker_destroy(chunker);
		error = ENOMEM;
		goto done;
	}
	uint64_t native_bytes = 0;
	for (uint64_t offset = 0; offset < (uint64_t)file.identity.st_size &&
	    error == 0 && !cd_catalog_stopped(c); offset += file.block_size) {
		nvlist_t *block = NULL;
		error = cd_inspect(dataset, &file.identity, B_TRUE, offset,
		    &block);
		uint64_t length, asize;
		boolean_t hole, embedded, shared;
		if (error == 0 && (nvlist_lookup_uint64(block,
		    "block_length", &length) != 0 ||
		    nvlist_lookup_uint64(block, "block_asize", &asize) != 0 ||
		    nvlist_lookup_boolean_value(block, "block_hole", &hole) != 0 ||
		    nvlist_lookup_boolean_value(block, "block_embedded", &embedded)
		    != 0 || nvlist_lookup_boolean_value(block, "block_shared",
		    &shared) != 0 || length == 0 || length > file.block_size ||
		    length > (uint64_t)file.identity.st_size - offset))
			error = EPROTO;
		nvlist_free(block);
		if (error != 0)
			break;
		file.uncertain |= shared || embedded;
		if ((error = cd_hybrid_add(&native_bytes, asize)) != 0)
			break;
#ifdef __APPLE__
		CC_SHA512_CTX digest;
		CC_SHA512_Init(&digest);
#else
		EVP_MD_CTX *digest = EVP_MD_CTX_new();
		if (digest == NULL || EVP_DigestInit_ex(digest, EVP_sha512(),
		    NULL) != 1) {
			EVP_MD_CTX_free(digest);
			error = ENOMEM;
			break;
		}
#endif
		for (uint64_t part = 0; part < length && error == 0;
		    part += MIN(length - part, 256U << 10)) {
			uint64_t count = MIN(length - part, 256U << 10);
			error = cd_catalog_read(&file, offset + part, count, buffer);
			if (error != 0)
				break;
#ifdef __APPLE__
			CC_SHA512_Update(&digest, buffer, (CC_LONG)count);
#else
			if (EVP_DigestUpdate(digest, buffer, count) != 1) {
				error = EIO;
				break;
			}
#endif
			error = chunkdedup_chunker_push(chunker, buffer, count);
		}
		uint8_t hash[64];
#ifdef __APPLE__
		CC_SHA512_Final(hash, &digest);
#else
		unsigned int digest_length;
		if (EVP_DigestFinal_ex(digest, hash, &digest_length) != 1 ||
		    digest_length != sizeof (hash))
			error = EIO;
		EVP_MD_CTX_free(digest);
#endif
		if (error == 0 && !hole && !embedded)
			error = cd_catalog_append(c->blocks, file.dataset_id,
			    hash, length, c->count, offset, asize,
			    file.block_size);
	}
	if (error == 0 && !cd_catalog_stopped(c))
		error = chunkdedup_chunker_finish(chunker);
	free(buffer);
	chunkdedup_chunker_destroy(chunker);
	if (error != 0 || cd_catalog_stopped(c))
		goto done;
	if (native_bytes > allocated) {
		file.uncertain = B_TRUE;
		file.native_base = 0;
	} else {
		file.native_base = allocated - native_bytes;
	}
append:
	if (!c->staging && (error = cd_catalog_seen(c, &file.identity, B_TRUE,
	    &duplicate)) != 0)
		goto done;
	if (fseeko(c->files, 0, SEEK_END) != 0 ||
	    fwrite(&file, sizeof (file), 1, c->files) != 1) {
		error = errno != 0 ? errno : EIO;
		goto done;
	}
	c->count++;
	appended = B_TRUE;
done:
	nvlist_free(info);
	(void) close(fd);
	/* A changing file must not leave records pointing at the next file id. */
	if (!appended) {
		FILE *streams[] = { c->blocks, c->chunks };
		off_t starts[] = { block_start, chunk_start };
		for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
			if (fflush(streams[i]) != 0 ||
			    ftruncate(fileno(streams[i]), starts[i]) != 0 ||
			    fseeko(streams[i], starts[i], SEEK_SET) != 0) {
				if (error == 0 || cd_deferred(error))
					error = errno != 0 ? errno : EIO;
			}
		}
	}
	if (c->staging && cd_deferred(error))
		return (error);
	if (error == EAGAIN && retryable)
		return (cd_catalog_enqueue(c, &file, age));
	if (error == EBUSY || error == ESTALE || error == EAGAIN ||
	    (error == ENOENT && expected != NULL)) {
		(void) fprintf(stderr, "%s: deferred (%s)\n", path,
		    strerror(error));
		c->skipped++;
		return (0);
	}
	return (error);
}

/* One pool barrier for a bounded set of dirty candidates, then one retry.
 * Saved identities prevent a changed or replaced file joining the old pass.
 */
static int
cd_catalog_retry(struct cd_catalog *c, uint64_t age)
{
	uint_t count = c->retry_count;
	if (count == 0 || cd_stop)
		return (0);
	uint64_t objects[CD_SYNC_BATCH_FILES];
	for (uint_t i = 0; i < count; i++)
		objects[i] = c->retry[i].identity.st_ino;
	nvlist_t *args = fnvlist_alloc(), *result = NULL;
	fnvlist_add_string(args, "op", "sync-batch");
	fnvlist_add_uint64_array(args, "objects", objects, count);
	int error = lzc_chunkdedup(c->retry[0].dataset, args, &result);
	nvlist_free(result);
	fnvlist_free(args);
	/* A vanished/busy hint need not invalidate other discovery results.
	 * Retrying without its barrier remains safe: dirty BPs defer again.
	 */
	if (error != 0 && error != ENOENT && !cd_deferred(error))
		return (error);
	c->retry_count = 0;
	for (uint_t i = 0; i < count && !cd_stop; i++) {
		struct cd_retry_file *retry = &c->retry[i];
		error = cd_catalog_analyze(c, retry->dataset, retry->path, age,
		    &retry->identity, B_FALSE);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
cd_catalog_connect(struct cd_catalog *c, FILE *items)
{
	struct cd_catalog_item first = { 0 }, item;
	boolean_t have = B_FALSE;
	int error = ENOENT;
	while (!cd_stop && (error = cd_catalog_next(items, &item)) == 0) {
		if (!have || !cd_catalog_same(&first, &item)) {
			first = item;
			have = B_TRUE;
		} else if ((error = cd_catalog_join(c, first.file,
		    item.file)) != 0) {
			return (error);
		}
	}
	if (cd_stop)
		return (ECANCELED);
	if (error != ENOENT)
		return (error);
	return (fseeko(items, 0, SEEK_SET) == 0 ? 0 : errno);
}

static int
cd_catalog_charge(struct cd_catalog *c, uint64_t id, uint64_t bytes,
    boolean_t chunks)
{
	uint64_t root;
	int error = cd_catalog_root(c, id, &root);
	struct cd_catalog_file file;
	if (c->groups != NULL) {
		if (error != 0)
			return (error);
		return (cd_hybrid_add(chunks ? &c->groups[root].chunk_estimate :
		    &c->groups[root].native_estimate, bytes));
	}
	if (error == 0)
		error = cd_catalog_record(c, root, &file, B_FALSE);
	if (error == 0)
		error = cd_hybrid_add(chunks ? &file.chunk_estimate :
		    &file.native_estimate, bytes);
	if (error == 0)
		error = cd_catalog_record(c, root, &file, B_TRUE);
	return (error);
}

static int
cd_catalog_cost(struct cd_catalog *c, FILE *items, boolean_t chunks)
{
	struct cd_catalog_item first = { 0 }, item;
	boolean_t have = B_FALSE, existing = B_FALSE;
	uint64_t cost = 0;
	int error = ENOENT;
	while (!cd_stop && (error = cd_catalog_next(items, &item)) == 0) {
		if (!have || !cd_catalog_same(&first, &item)) {
			if (have && (error = cd_catalog_charge(c, first.file,
			    chunks && existing ? 0 : cost, chunks)) != 0)
				return (error);
			first = item;
			cost = item.bytes;
			existing = chunks && item.object != 0;
			have = B_TRUE;
		} else {
			/* Optimistic native baseline, conservative CDC allocation. */
			cost = chunks ? MAX(cost, item.bytes) :
			    MIN(cost, item.bytes);
			existing |= chunks && item.object != 0;
		}
	}
	if (cd_stop)
		return (ECANCELED);
	if (error != ENOENT)
		return (error);
	if (have && (error = cd_catalog_charge(c, first.file,
	    chunks && existing ? 0 : cost, chunks)) != 0)
		return (error);
	return (fseeko(items, 0, SEEK_SET) == 0 ? 0 : errno);
}

/* Reports are completed before sharing and survive temporary catalog cleanup. */
static int
cd_catalog_report_init(struct cd_catalog *c, const char *path)
{
	if (path == NULL)
		return (0);
	int length = snprintf(c->report_temporary, sizeof (c->report_temporary),
	    "%s.partial.XXXXXX", path);
	if (length < 0 || (size_t)length >= sizeof (c->report_temporary)) {
		c->report_temporary[0] = '\0';
		return (ENAMETOOLONG);
	}
	struct stat existing;
	if (lstat(path, &existing) == 0) {
		c->report_temporary[0] = '\0';
		return (EEXIST);
	}
	if (errno != ENOENT) {
		int error = errno;
		c->report_temporary[0] = '\0';
		return (error);
	}
	int fd = mkstemp(c->report_temporary);
	if (fd < 0) {
		c->report_temporary[0] = '\0';
		return (errno);
	}
	c->report = fdopen(fd, "w");
	if (c->report == NULL) {
		int error = errno;
		(void) close(fd);
		(void) unlink(c->report_temporary);
		c->report_temporary[0] = '\0';
		return (error);
	}
	c->report_path = path;
	(void) fprintf(c->report, "{\"type\":\"metadata\",\"schema\":1,"
	    "\"estimates_only\":true,\"scope\":\"sharing-group\","
	    "\"path_encoding\":\"utf-8-surrogateescape\",\"created_epoch\":%" PRId64 "}\n", (int64_t)time(NULL));
	return (ferror(c->report) ? EIO : 0);
}

/* Preserve UTF-8 names and escape undecodable filename bytes losslessly. */
static void
cd_catalog_report_string(FILE *fp, const char *value)
{
	(void) fputc('"', fp);
	const unsigned char *p = (const unsigned char *)value;
	while (*p) {
		unsigned int n = *p >= 0xc2 && *p <= 0xdf ? 2 :
		    *p >= 0xe0 && *p <= 0xef ? 3 :
		    *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
		boolean_t valid = n != 0;
		for (unsigned int i = 1; valid && i < n; i++)
			valid = (p[i] & 0xc0) == 0x80;
		if (valid && ((*p == 0xe0 && p[1] < 0xa0) ||
		    (*p == 0xed && p[1] >= 0xa0) ||
		    (*p == 0xf0 && p[1] < 0x90) ||
		    (*p == 0xf4 && p[1] >= 0x90)))
			valid = B_FALSE;
		if (valid) {
			(void) fwrite(p, 1, n, fp);
			p += n;
			continue;
		}
		if (*p >= 0x80)
			(void) fprintf(fp, "\\udc%02x", (unsigned int)*p);
		else if (*p < 0x20)
			(void) fprintf(fp, "\\u%04x", (unsigned int)*p);
		else {
			if (*p == '"' || *p == '\\')
				(void) fputc('\\', fp);
			(void) fputc(*p, fp);
		}
		p++;
	}
	(void) fputc('"', fp);
}

static int
cd_catalog_report_file(struct cd_catalog *c, uint64_t id, uint64_t root,
    const struct cd_catalog_file *file, const struct cd_catalog_file *group)
{
	FILE *fp = c->report;
	if (fp == NULL)
		return (0);
	if (id == root) {
		uint64_t savings = group->native_estimate > group->chunk_estimate ?
		    group->native_estimate - group->chunk_estimate : 0;
		enum cd_method recommended = cd_hybrid_choose(group->native_estimate,
		    group->chunk_estimate, group->uncertain);
		(void) fprintf(fp, "{\"type\":\"group\",\"group_id\":%" PRIu64
		    ",\"native_estimate\":%" PRIu64 ",\"chunk_estimate\":%" PRIu64
		    ",\"chunk_savings_estimate\":%" PRIu64
		    ",\"uncertain\":%s,\"recommended_method\":\"%s\",\"dataset\":",
		    root, group->native_estimate, group->chunk_estimate, savings,
		    group->uncertain ? "true" : "false",
		    recommended == CD_METHOD_CHUNK ? "chunk" : "block");
		cd_catalog_report_string(fp, file->dataset);
		(void) fputs("}\n", fp);
	}
	(void) fprintf(fp, "{\"type\":\"file\",\"file_id\":%" PRIu64
	    ",\"group_id\":%" PRIu64 ",\"dataset\":", id, root);
	cd_catalog_report_string(fp, file->dataset);
	(void) fputs(",\"path\":", fp);
	cd_catalog_report_string(fp, file->path);
	(void) fprintf(fp, ",\"object\":%" PRIu64 ",\"size\":%" PRIu64
	    ",\"mtime_sec\":%" PRId64 ",\"existing_chunked\":%s"
	    ",\"selected_method\":\"%s\"}\n", (uint64_t)file->identity.st_ino,
	    (uint64_t)file->identity.st_size, (int64_t)file->identity.st_mtime,
	    file->chunked ? "true" : "false",
	    file->method == CD_METHOD_CHUNK ? "chunk" : "block");
	return (ferror(fp) ? EIO : 0);
}

static int
cd_catalog_report_finish(struct cd_catalog *c)
{
	if (c->report == NULL)
		return (0);
	int error = 0;
	if (fflush(c->report) != 0 || fsync(fileno(c->report)) != 0)
		error = errno != 0 ? errno : EIO;
	if (fclose(c->report) != 0 && error == 0)
		error = errno != 0 ? errno : EIO;
	c->report = NULL;
	if (error == 0 && link(c->report_temporary, c->report_path) != 0)
		error = errno;
	if (error == 0 && unlink(c->report_temporary) != 0)
		error = errno;
	if (error == 0) {
		c->report_temporary[0] = '\0';
		(void) printf("catalog: report=%s\n", c->report_path);
		(void) fflush(stdout);
	}
	return (error);
}

static int
cd_catalog_select(struct cd_catalog *c, unsigned int threads)
{
	time_t started = time(NULL);
	(void) printf("catalog: sorting=native workers=%u\n", threads);
	(void) fflush(stdout);
	int error = cd_hybrid_sort_parallel(c->blocks, &c->sorted_blocks,
	    c->directory, threads, &cd_stop);
	if (error == 0) {
		(void) printf("catalog: sorted=native seconds=%jd\n",
		    (intmax_t)(time(NULL) - started));
		(void) printf("catalog: sorting=chunk workers=%u\n", threads);
		(void) fflush(stdout);
		started = time(NULL);
		error = cd_hybrid_sort_parallel(c->chunks, &c->sorted_chunks,
		    c->directory, threads, &cd_stop);
		if (error == 0) {
			(void) printf("catalog: sorted=chunk seconds=%jd\n",
			    (intmax_t)(time(NULL) - started));
			(void) fflush(stdout);
		}
	}
	if (error == 0 && c->count <= CD_CATALOG_GROUP_LIMIT / sizeof (*c->groups))
		c->groups = calloc((size_t)c->count, sizeof (*c->groups));
	if (error == 0) {
		(void) printf("catalog: files=%" PRIu64 " group_index=%s bytes=%zu\n",
		    c->count, c->groups != NULL ? "memory" : "disk",
		    c->groups != NULL ? (size_t)c->count * sizeof (*c->groups) : 0);
		(void) fflush(stdout);
	}
	if (c->groups != NULL) {
		for (uint64_t i = 0; error == 0 && i < c->count && !cd_stop; i++) {
			struct cd_catalog_file file;
			error = cd_catalog_record(c, i, &file, B_FALSE);
			if (error == 0)
				c->groups[i] = (struct cd_catalog_group) {
				    file.parent, file.native_estimate, file.chunk_estimate,
				    file.uncertain };
		}
	}
	if (error == 0)
		error = cd_catalog_connect(c, c->sorted_chunks);
	if (error == 0)
		error = cd_catalog_connect(c, c->sorted_blocks);
	for (uint64_t i = 0; error == 0 && i < c->count && !cd_stop; i++) {
		struct cd_catalog_file file, group;
		uint64_t root;
		if ((error = cd_catalog_record(c, i, &file, B_FALSE)) != 0 ||
		    (error = cd_catalog_root(c, i, &root)) != 0)
			break;
		if (c->groups != NULL) {
			group = (struct cd_catalog_file) {
			    .native_estimate = c->groups[root].native_estimate,
			    .chunk_estimate = c->groups[root].chunk_estimate,
			    .uncertain = c->groups[root].uncertain };
		} else if ((error = cd_catalog_record(c, root, &group, B_FALSE)) != 0) {
			break;
		}
		group.uncertain |= file.uncertain;
		if ((error = cd_hybrid_add(&group.native_estimate,
		    file.native_base)) == 0)
			error = cd_hybrid_add(&group.chunk_estimate,
			    file.chunk_base);
		if (error == 0 && c->groups != NULL) {
			c->groups[root].native_estimate = group.native_estimate;
			c->groups[root].chunk_estimate = group.chunk_estimate;
			c->groups[root].uncertain = group.uncertain;
		} else if (error == 0) {
			error = cd_catalog_record(c, root, &group, B_TRUE);
		}
	}
	if (error == 0)
		error = cd_catalog_cost(c, c->sorted_blocks, B_FALSE);
	if (error == 0)
		error = cd_catalog_cost(c, c->sorted_chunks, B_TRUE);
	for (uint64_t i = 0; error == 0 && i < c->count && !cd_stop; i++) {
		struct cd_catalog_file file, group;
		uint64_t root;
		if ((error = cd_catalog_record(c, i, &file, B_FALSE)) != 0 ||
		    (error = cd_catalog_root(c, i, &root)) != 0)
			break;
		if (c->groups != NULL) {
			group = (struct cd_catalog_file) {
			    .native_estimate = c->groups[root].native_estimate,
			    .chunk_estimate = c->groups[root].chunk_estimate,
			    .uncertain = c->groups[root].uncertain };
		} else if ((error = cd_catalog_record(c, root, &group, B_FALSE)) != 0) {
			break;
		}
		const char *reason = "forced";
		if (file.chunked) {
			file.method = CD_METHOD_CHUNK;
			reason = "existing-chunked";
		} else if (c->method == CD_METHOD_AUTO) {
			file.method = cd_hybrid_choose(group.native_estimate,
			    group.chunk_estimate, group.uncertain);
			reason = group.uncertain ? "uncertain-estimate" :
			    (file.method == CD_METHOD_CHUNK ? "chunk-savings" :
			    "native-preferred");
		}
		(void) printf("%s: method=%s native_estimate=%" PRIu64
		    " chunk_estimate=%" PRIu64 " reason=%s "
		    "estimate_scope=sharing-group\n", file.path,
		    file.method == CD_METHOD_CHUNK ? "chunk" : "block",
		    group.native_estimate, group.chunk_estimate, reason);
		if (c->groups != NULL) {
			file.parent = root;
			file.native_estimate = c->groups[i].native_estimate;
			file.chunk_estimate = c->groups[i].chunk_estimate;
			file.uncertain = c->groups[i].uncertain;
		}
		error = cd_catalog_record(c, i, &file, B_TRUE);
		if (error == 0)
			error = cd_catalog_report_file(c, i, root, &file, &group);
	}
	if (cd_stop && error == 0)
		error = ECANCELED;
	if (error == 0)
		error = cd_catalog_report_finish(c);
	free(c->groups);
	c->groups = NULL;
	return (error);
}

static int
cd_catalog_open_file(const struct cd_catalog_file *file, int *fd)
{
	*fd = open(file->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (*fd < 0)
		return (errno);
	struct stat current;
	if (fstat(*fd, &current) != 0)
		return (errno);
	if (current.st_dev != file->identity.st_dev ||
	    current.st_ino != file->identity.st_ino ||
	    current.st_size != file->identity.st_size ||
	    current.st_mtime != file->identity.st_mtime)
		return (ESTALE);
#ifdef __FreeBSD__
	if (current.st_gen != file->identity.st_gen ||
	    current.st_mtim.tv_nsec != file->identity.st_mtim.tv_nsec)
		return (ESTALE);
#endif
	return (0);
}

static boolean_t
cd_deferred(int error)
{
	return (error == EBUSY || error == EAGAIN || error == ESTALE);
}

/* SHARE_DRIVER_BEGIN */
static int
cd_catalog_share_serial(struct cd_catalog *c)
{
	struct cd_catalog_item canonical = { 0 }, item;
	struct cd_catalog_file source;
	boolean_t have = B_FALSE;
	int source_fd = -1, error;
	while (!cd_stop && (error = cd_catalog_next(c->sorted_blocks,
	    &item)) == 0) {
		struct cd_catalog_file destination;
		if ((error = cd_catalog_record(c, item.file, &destination,
		    B_FALSE)) != 0)
			break;
		if (destination.method != CD_METHOD_BLOCK)
			continue;
		if (!have || !cd_catalog_same(&canonical, &item)) {
			if (source_fd >= 0)
				(void) close(source_fd);
			source_fd = -1;
			have = B_FALSE;
			error = cd_catalog_open_file(&destination, &source_fd);
			if (cd_deferred(error)) {
				destination.deferred = B_TRUE;
				error = cd_catalog_record(c, item.file,
				    &destination, B_TRUE);
				if (error == 0)
					continue;
			}
			if (error != 0)
				break;
			canonical = item;
			source = destination;
			have = B_TRUE;
			continue;
		}
		/* A short tail may hash equally despite incompatible record sizes. */
		if (canonical.object != item.object ||
		    (canonical.file == item.file && canonical.offset == item.offset))
			continue;
		if (c->watch) {
			boolean_t enabled;
			if ((error = cd_enabled(destination.dataset, &enabled)) != 0)
				break;
			if (!enabled) {
				error = ECANCELED;
				break;
			}
		}
		int fd = -1;
		if (canonical.file != item.file)
			error = cd_catalog_open_file(&destination, &fd);
		else
			error = 0;
		if (error == 0) {
			nvlist_t *args = fnvlist_alloc(), *result = NULL;
			fnvlist_add_string(args, "op", "share-blocks");
			cd_identity(args, &destination.identity);
			fnvlist_add_uint64(args, "offset", item.offset);
			fnvlist_add_uint64(args, "length", item.length);
			fnvlist_add_uint64(args, "source_object",
			    source.identity.st_ino);
#ifdef __FreeBSD__
			fnvlist_add_uint64(args, "source_generation",
			    source.identity.st_gen);
			fnvlist_add_uint64(args, "source_mtime_sec",
			    source.identity.st_mtim.tv_sec);
			fnvlist_add_uint64(args, "source_mtime_nsec",
			    source.identity.st_mtim.tv_nsec);
#else
			fnvlist_add_uint64(args, "source_generation", 0);
#endif
			fnvlist_add_uint64(args, "source_size",
			    source.identity.st_size);
			fnvlist_add_uint64(args, "source_offset", canonical.offset);
			error = lzc_chunkdedup(destination.dataset, args, &result);
			fnvlist_free(args);
			nvlist_free(result);
		}
		if (fd >= 0)
			(void) close(fd);
		if (cd_deferred(error)) {
			destination.deferred = B_TRUE;
			(void) fprintf(stderr, "%s: deferred (%s)\n",
			    destination.path, strerror(error));
			error = cd_catalog_record(c, item.file, &destination, B_TRUE);
		} else if (error == EILSEQ) {
			/* Hashes discover candidates; only the kernel may prove equality. */
			(void) fprintf(stderr, "%s: unequal candidate left ordinary\n",
			    destination.path);
			error = 0;
		}
		if (error != 0)
			break;
	}
	if (source_fd >= 0)
		(void) close(source_fd);
	return (cd_stop || error == ENOENT ? 0 : error);
}

#ifndef CD_SHARE_MAX_ITEMS
#define	CD_SHARE_MAX_ITEMS	8192U
#endif
#ifndef CD_SHARE_SNAPSHOT_LIMIT
#define	CD_SHARE_SNAPSHOT_LIMIT	(UINT64_C(1) << 30)
#endif
#ifndef CD_SHARE_PROGRESS_SECONDS
#define	CD_SHARE_PROGRESS_SECONDS	30
#endif
#ifndef CD_SHARE_PROGRESS_ISSUED
#define	CD_SHARE_PROGRESS_ISSUED	1000U
#endif

struct cd_share_item {
	uint64_t file, offset, length, object;
};

enum cd_share_event_kind {
	CD_SHARE_SOURCE_DEFERRED, CD_SHARE_DEST_DEFERRED, CD_SHARE_UNEQUAL
};

struct cd_share_event {
	uint64_t file;
	enum cd_share_event_kind kind;
	int error;
};

struct cd_share_identity {
	dev_t device;
	ino_t inode;
};

struct cd_share_source {
	boolean_t have;
	struct cd_share_item canonical;
	int fd;
};

struct cd_share_state {
	const struct cd_catalog_file *files;
	uint64_t count;
	atomic_int first_error;
	atomic_uint_fast64_t groups, issued, deferred, unequal, reported_issued;
	pthread_mutex_t lock;
	pthread_cond_t completed;
	size_t pending;
	struct timespec reported_at;
};

struct cd_share_job {
	struct cd_share_state *state;
	struct cd_share_item *items;
	struct cd_share_event *events;
	struct cd_share_identity *identities;
	size_t count, event_count, identity_count;
	boolean_t active, complete;
};

static int
cd_share_error(struct cd_share_state *state, int error)
{
	int expected = 0;
	if (error != 0)
		(void) atomic_compare_exchange_strong(&state->first_error,
		    &expected, error);
	return (error);
}

static boolean_t
cd_share_stopped(struct cd_share_state *state)
{
	return (cd_stop || atomic_load(&state->first_error) != 0);
}

/* Only the coordinator calls this function or reads reported_at. */
static int
cd_share_progress(struct cd_share_state *state, boolean_t force)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return (cd_share_error(state, errno));
	uint64_t issued = atomic_load(&state->issued);
	if (!force && now.tv_sec - state->reported_at.tv_sec <
	    CD_SHARE_PROGRESS_SECONDS && issued -
	    atomic_load(&state->reported_issued) < CD_SHARE_PROGRESS_ISSUED)
		return (0);
	if (printf("share: groups=%" PRIu64 " issued=%" PRIu64
	    " deferred=%" PRIu64 " unequal=%" PRIu64 "\n",
	    (uint64_t)atomic_load(&state->groups), issued,
	    (uint64_t)atomic_load(&state->deferred),
	    (uint64_t)atomic_load(&state->unequal)) < 0 || fflush(stdout) != 0)
		return (cd_share_error(state, errno != 0 ? errno : EIO));
	state->reported_at = now;
	atomic_store(&state->reported_issued, issued);
	return (0);
}

static void
cd_share_event(struct cd_share_job *job, uint64_t file,
    enum cd_share_event_kind kind, int error)
{
	job->events[job->event_count++] = (struct cd_share_event) {
	    .file = file, .kind = kind, .error = error };
	if (kind == CD_SHARE_UNEQUAL)
		(void) atomic_fetch_add(&job->state->unequal, 1);
	else
		(void) atomic_fetch_add(&job->state->deferred, 1);
}

/* No catalog streams, property handles or output are touched by workers. */
static int
cd_share_process(struct cd_share_job *job, struct cd_share_source *source,
    boolean_t coordinator)
{
	struct cd_share_state *state = job->state;
	job->complete = B_FALSE;
	for (size_t i = 0; i < job->count; i++) {
		if (coordinator && cd_share_progress(state, B_FALSE) != 0)
			return (atomic_load(&state->first_error));
		if (cd_share_stopped(state))
			return (0);
		const struct cd_share_item *item = &job->items[i];
		struct cd_catalog_file destination = state->files[item->file];
		int error;
		if (!source->have) {
			if (cd_share_stopped(state))
				return (0);
			error = cd_catalog_open_file(&destination, &source->fd);
			if (error != 0) {
				if (!cd_deferred(error))
					(void) cd_share_error(state, error);
				if (source->fd >= 0)
					(void) close(source->fd);
				source->fd = -1;
				if (!cd_deferred(error))
					return (error);
				cd_share_event(job, item->file,
				    CD_SHARE_SOURCE_DEFERRED, error);
				continue;
			}
			source->canonical = *item;
			source->have = B_TRUE;
			continue;
		}
		const struct cd_share_item *canonical = &source->canonical;
		if (canonical->object != item->object ||
		    (canonical->file == item->file &&
		    canonical->offset == item->offset))
			continue;
		int fd = -1;
		if (cd_share_stopped(state))
			return (0);
		error = canonical->file != item->file ?
		    cd_catalog_open_file(&destination, &fd) : 0;
		if (error == 0 && !cd_share_stopped(state)) {
			struct cd_catalog_file original = state->files[canonical->file];
			nvlist_t *args = fnvlist_alloc(), *result = NULL;
			fnvlist_add_string(args, "op", "share-blocks");
			cd_identity(args, &destination.identity);
			fnvlist_add_uint64(args, "offset", item->offset);
			fnvlist_add_uint64(args, "length", item->length);
			fnvlist_add_uint64(args, "source_object",
			    original.identity.st_ino);
#ifdef __FreeBSD__
			fnvlist_add_uint64(args, "source_generation",
			    original.identity.st_gen);
			fnvlist_add_uint64(args, "source_mtime_sec",
			    original.identity.st_mtim.tv_sec);
			fnvlist_add_uint64(args, "source_mtime_nsec",
			    original.identity.st_mtim.tv_nsec);
#else
			fnvlist_add_uint64(args, "source_generation", 0);
#endif
			fnvlist_add_uint64(args, "source_size", original.identity.st_size);
			fnvlist_add_uint64(args, "source_offset", canonical->offset);
			if (coordinator)
				(void) cd_share_progress(state, B_FALSE);
			if (!cd_share_stopped(state)) {
				uint64_t issued = atomic_fetch_add(&state->issued, 1) + 1;
				if (issued - atomic_load(&state->reported_issued) >=
				    CD_SHARE_PROGRESS_ISSUED) {
					(void) pthread_mutex_lock(&state->lock);
					(void) pthread_cond_signal(&state->completed);
					(void) pthread_mutex_unlock(&state->lock);
				}
				error = lzc_chunkdedup(destination.dataset, args, &result);
				if (error != 0 && !cd_deferred(error) && error != EILSEQ)
					(void) cd_share_error(state, error);
			}
			fnvlist_free(args);
			nvlist_free(result);
			if (coordinator)
				(void) cd_share_progress(state, B_FALSE);
		}
		if (error != 0 && !cd_deferred(error) && error != EILSEQ)
			(void) cd_share_error(state, error);
		if (fd >= 0)
			(void) close(fd);
		if (cd_deferred(error))
			cd_share_event(job, item->file, CD_SHARE_DEST_DEFERRED, error);
		else if (error == EILSEQ)
			cd_share_event(job, item->file, CD_SHARE_UNEQUAL, error);
		else if (error != 0)
			return (error);
	}
	job->complete = B_TRUE;
	return (0);
}

static int
cd_share_work(void *argument)
{
	struct cd_share_job *job = argument;
	struct cd_share_source source = { .fd = -1 };
	int error = cd_share_process(job, &source, B_FALSE);
	if (job->complete && error == 0)
		(void) atomic_fetch_add(&job->state->groups, 1);
	if (source.fd >= 0)
		(void) close(source.fd);
	/* Publish only after every descriptor and ioctl belongs to the past. */
	(void) pthread_mutex_lock(&job->state->lock);
	job->state->pending++;
	(void) pthread_cond_signal(&job->state->completed);
	(void) pthread_mutex_unlock(&job->state->lock);
	return (error);
}

static void
cd_share_job_fini(struct cd_share_job *job)
{
	free(job->items);
	free(job->events);
	free(job->identities);
	memset(job, 0, sizeof (*job));
}

static void
cd_share_merge(struct cd_catalog *c, struct cd_share_job *job)
{
	for (size_t i = 0; i < job->event_count; i++) {
		const struct cd_share_event *event = &job->events[i];
		const char *path = job->state->files[event->file].path;
		if (event->kind == CD_SHARE_UNEQUAL) {
			(void) fprintf(stderr, "%s: unequal candidate left ordinary\n", path);
			continue;
		}
		if (event->kind == CD_SHARE_DEST_DEFERRED)
			(void) fprintf(stderr, "%s: deferred (%s)\n", path,
			    strerror(event->error));
		struct cd_catalog_file file;
		int error = cd_catalog_record(c, event->file, &file, B_FALSE);
		if (error == 0) {
			file.deferred = B_TRUE;
			error = cd_catalog_record(c, event->file, &file, B_TRUE);
		}
		(void) cd_share_error(job->state, error);
	}
	job->event_count = 0;
}

static void
cd_share_take(struct cd_catalog *c, struct cd_parallel *pool,
    struct cd_share_state *state)
{
	for (;;) {
		(void) cd_share_progress(state, B_FALSE);
		(void) pthread_mutex_lock(&state->lock);
		if (state->pending != 0) {
			(void) pthread_mutex_unlock(&state->lock);
			break;
		}
		/* A threshold signal may precede this lock acquisition. */
		if (atomic_load(&state->issued) -
		    atomic_load(&state->reported_issued) >= CD_SHARE_PROGRESS_ISSUED) {
			(void) pthread_mutex_unlock(&state->lock);
			continue;
		}
		struct timespec until = state->reported_at;
		until.tv_sec += CD_SHARE_PROGRESS_SECONDS;
#ifdef __APPLE__
		struct timespec now;
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
			(void) cd_share_error(state, errno);
			now = until;
		}
		until.tv_sec -= now.tv_sec;
		until.tv_nsec -= now.tv_nsec;
		if (until.tv_nsec < 0) {
			until.tv_sec--;
			until.tv_nsec += 1000000000L;
		}
		if (until.tv_sec < 0)
			until = (struct timespec) { 0 };
		int error = pthread_cond_timedwait_relative_np(&state->completed,
		    &state->lock, &until);
#else
		int error = pthread_cond_timedwait(&state->completed,
		    &state->lock, &until);
#endif
		(void) pthread_mutex_unlock(&state->lock);
		if (error != 0 && error != ETIMEDOUT)
			(void) cd_share_error(state, error);
	}
	void *argument;
	int result;
	int error = cd_parallel_take(pool, &argument, &result);
	if (error != 0) {
		(void) cd_share_error(state, error);
		return;
	}
	(void) pthread_mutex_lock(&state->lock);
	state->pending--;
	(void) pthread_mutex_unlock(&state->lock);
	struct cd_share_job *job = argument;
	(void) cd_share_error(state, result);
	cd_share_merge(c, job);
	cd_share_job_fini(job);
}

static int
cd_share_identity_compare(const void *left, const void *right)
{
	const struct cd_share_identity *a = left, *b = right;
	if (a->device != b->device)
		return (a->device < b->device ? -1 : 1);
	return (a->inode == b->inode ? 0 : a->inode < b->inode ? -1 : 1);
}

static boolean_t
cd_share_conflict(const struct cd_share_job *a, const struct cd_share_job *b)
{
	size_t i = 0, j = 0;
	while (i < a->identity_count && j < b->identity_count) {
		int order = cd_share_identity_compare(&a->identities[i], &b->identities[j]);
		if (order == 0)
			return (B_TRUE);
		if (order < 0)
			i++;
		else
			j++;
	}
	return (B_FALSE);
}

/* EOF is not a published failure. The current item is bounded lookahead. */
static int
cd_share_next(struct cd_catalog *c, struct cd_share_state *state,
    struct cd_catalog_item *item, boolean_t *have)
{
	int error = cd_share_progress(state, B_FALSE);
	if (error == 0)
		error = cd_catalog_next(c->sorted_blocks, item);
	*have = error == 0;
	if (error == ENOENT)
		return (0);
	if (error == 0 && item->file >= state->count)
		error = EINVAL;
	return (cd_share_error(state, error));
}

static int
cd_share_fill(struct cd_catalog *c, struct cd_share_job *job,
    const struct cd_catalog_item *key, struct cd_catalog_item *item,
    boolean_t *have)
{
	job->count = 0;
	while (*have && cd_catalog_same(key, item) &&
	    !cd_share_stopped(job->state)) {
		if (job->state->files[item->file].method == CD_METHOD_BLOCK) {
			if (job->count == CD_SHARE_MAX_ITEMS)
				break;
			job->items[job->count++] = (struct cd_share_item) {
			    item->file, item->offset, item->length, item->object };
		}
		int error = cd_share_next(c, job->state, item, have);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
cd_share_result(int error)
{
	return (cd_stop || error == ENOENT ? 0 : error);
}

static int
cd_catalog_share(struct cd_catalog *c, unsigned int threads)
{
	if (cd_stop)
		return (0);
	if (c->watch) {
		(void) printf("share: serial (watch)\n");
		(void) fflush(stdout);
		return (cd_catalog_share_serial(c));
	}
	if (fflush(c->files) != 0)
		return (cd_share_result(errno));
	if (c->count == 0)
		return (0);
	struct cd_catalog_file *files = NULL;
	if (c->count <= CD_SHARE_SNAPSHOT_LIMIT / sizeof (*files))
		files = malloc((size_t)c->count * sizeof (*files));
	if (files == NULL) {
		(void) printf("share: serial (file snapshot unavailable)\n");
		(void) fflush(stdout);
		return (cd_catalog_share_serial(c));
	}
	int error = 0;
	for (uint64_t i = 0; i < c->count && error == 0 && !cd_stop; i++)
		error = cd_catalog_record(c, i, &files[i], B_FALSE);
	if (error != 0 || cd_stop) {
		free(files);
		return (cd_share_result(error));
	}
	struct cd_share_state state = { .files = files, .count = c->count };
	atomic_init(&state.first_error, 0);
	atomic_init(&state.groups, 0);
	atomic_init(&state.issued, 0);
	atomic_init(&state.deferred, 0);
	atomic_init(&state.unequal, 0);
	atomic_init(&state.reported_issued, 0);
	if ((error = pthread_mutex_init(&state.lock, NULL)) != 0) {
		free(files);
		return (cd_share_result(error));
	}
	pthread_condattr_t attributes;
	error = pthread_condattr_init(&attributes);
	if (error == 0) {
#ifndef __APPLE__
		error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
		if (error == 0)
			error = pthread_cond_init(&state.completed, &attributes);
		(void) pthread_condattr_destroy(&attributes);
	}
	if (error != 0) {
		(void) pthread_mutex_destroy(&state.lock);
		free(files);
		return (cd_share_result(error));
	}
	struct cd_parallel pool;
	struct cd_share_job *jobs = NULL;
	boolean_t initialized = B_FALSE;
	error = cd_share_progress(&state, B_TRUE);
	if (error == 0)
		error = cd_parallel_init_unordered(&pool, threads, cd_share_work);
	if (error == 0) {
		initialized = B_TRUE;
		jobs = calloc(pool.capacity, sizeof (*jobs));
		if (jobs == NULL)
			error = ENOMEM;
	}
	(void) cd_share_error(&state, error);
	struct cd_catalog_item item;
	boolean_t have = B_FALSE;
	if (!cd_share_stopped(&state))
		(void) cd_share_next(c, &state, &item, &have);
	while (have && !cd_share_stopped(&state)) {
		while (cd_parallel_full(&pool) && !cd_share_stopped(&state))
			cd_share_take(c, &pool, &state);
		if (cd_share_stopped(&state))
			break;
		struct cd_share_job *job = NULL;
		for (size_t i = 0; i < pool.capacity; i++) {
			if (!jobs[i].active) {
				job = &jobs[i];
				break;
			}
		}
		job->state = &state;
		job->items = malloc(CD_SHARE_MAX_ITEMS * sizeof (*job->items));
		job->events = malloc(CD_SHARE_MAX_ITEMS * sizeof (*job->events));
		if (job->items == NULL || job->events == NULL) {
			(void) cd_share_error(&state, ENOMEM);
			break;
		}
		struct cd_catalog_item key = item;
		error = cd_share_fill(c, job, &key, &item, &have);
		if (error != 0 || cd_share_stopped(&state))
			break;
		if (job->count == 0) {
			cd_share_job_fini(job);
			continue;
		}
		if (have && cd_catalog_same(&key, &item)) {
			/* A giant run owns the coordinator after a complete barrier. */
			while (cd_parallel_count(&pool) != 0)
				cd_share_take(c, &pool, &state);
			struct cd_share_source source = { .fd = -1 };
			while (!cd_share_stopped(&state)) {
				error = cd_share_process(job, &source, B_TRUE);
				cd_share_merge(c, job);
				if (error != 0 || cd_share_stopped(&state) ||
				    !have || !cd_catalog_same(&key, &item))
					break;
				error = cd_share_fill(c, job, &key, &item, &have);
				if (error != 0)
					break;
			}
			if (job->complete && error == 0 &&
			    (!have || !cd_catalog_same(&key, &item)))
				(void) atomic_fetch_add(&state.groups, 1);
			if (source.fd >= 0)
				(void) close(source.fd);
			cd_share_job_fini(job);
			continue;
		}
		job->identities = malloc(job->count * sizeof (*job->identities));
		if (job->identities == NULL) {
			(void) cd_share_error(&state, ENOMEM);
			break;
		}
		for (size_t i = 0; i < job->count; i++) {
			const struct stat *identity = &files[job->items[i].file].identity;
			job->identities[i] = (struct cd_share_identity) {
			    identity->st_dev, identity->st_ino };
		}
		qsort(job->identities, job->count, sizeof (*job->identities),
		    cd_share_identity_compare);
		for (size_t i = 0; i < job->count; i++) {
			if (job->identity_count == 0 || cd_share_identity_compare(
			    &job->identities[i],
			    &job->identities[job->identity_count - 1]) != 0)
				job->identities[job->identity_count++] = job->identities[i];
		}
		for (;;) {
			boolean_t conflict = B_FALSE;
			for (size_t i = 0; i < pool.capacity; i++) {
				if (jobs[i].active && cd_share_conflict(job, &jobs[i])) {
					conflict = B_TRUE;
					break;
				}
			}
			if (!conflict || cd_share_stopped(&state))
				break;
			cd_share_take(c, &pool, &state);
		}
		if (cd_share_stopped(&state))
			break;
		error = cd_parallel_submit(&pool, job);
		if (error != 0) {
			(void) cd_share_error(&state, error);
			break;
		}
		job->active = B_TRUE;
	}
	/* Never abandon queued callbacks or their outcomes, even after an error. */
	if (initialized) {
		while (cd_parallel_count(&pool) != 0)
			cd_share_take(c, &pool, &state);
		cd_parallel_fini(&pool);
		if (jobs != NULL) {
			for (size_t i = 0; i < (size_t)threads * 2; i++)
				cd_share_job_fini(&jobs[i]);
		}
	}
	free(jobs);
	(void) cd_share_progress(&state, B_TRUE);
	error = atomic_load(&state.first_error);
	(void) pthread_cond_destroy(&state.completed);
	(void) pthread_mutex_destroy(&state.lock);
	free(files);
	return (cd_share_result(error));
}
/* SHARE_DRIVER_END */

static int
cd_catalog_apply(struct cd_catalog *c, uint64_t age, unsigned int threads)
{
	int error = cd_catalog_select(c, threads);
	if (error == 0)
		error = cd_catalog_share(c, threads);
	for (uint64_t i = 0; error == 0 && i < c->count && !cd_stop; i++) {
		struct cd_catalog_file file;
		if ((error = cd_catalog_record(c, i, &file, B_FALSE)) != 0)
			break;
		if (file.deferred) {
			c->skipped++;
			continue;
		}
		if (file.method == CD_METHOD_BLOCK) {
			c->processed++;
			continue;
		}
		int fd = -1;
		error = cd_catalog_open_file(&file, &fd);
		if (fd >= 0)
			(void) close(fd);
		if (cd_deferred(error)) {
			c->skipped++;
			error = 0;
			continue;
		}
		if (error == 0)
			error = cd_file(file.dataset, file.path, "optimize", age,
			    &c->processed, &c->skipped, c->watch, &file.identity);
		if (cd_deferred(error)) {
			c->skipped++;
			error = 0;
		}
	}
	return (error == ECANCELED && c->watch ? 0 : error);
}

static int
cd_catalog_init(struct cd_catalog *c, enum cd_method method, boolean_t watch)
{
	memset(c, 0, sizeof (*c));
	c->method = method;
	c->watch = watch;
	/* A private directory also bounds sort spill cleanup to this pass. */
	(void) strlcpy(c->directory, "/tmp/zfs-chunkdedup.XXXXXX",
	    sizeof (c->directory));
	if (mkdtemp(c->directory) == NULL)
		return (errno);
	c->files = tmpfile();
	c->blocks = tmpfile();
	c->chunks = tmpfile();
	c->seen = tmpfile();
	c->retry = calloc(CD_SYNC_BATCH_FILES, sizeof (*c->retry));
	c->seen_capacity = 1024;
	if (c->files == NULL || c->blocks == NULL || c->chunks == NULL ||
	    c->seen == NULL || c->retry == NULL)
		return (errno);
	return (ftruncate(fileno(c->seen), c->seen_capacity *
	    sizeof (struct cd_catalog_seen)) == 0 ? 0 : errno);
}

static void
cd_catalog_fini(struct cd_catalog *c)
{
	free(c->retry);
	free(c->groups);
	if (c->report != NULL)
		(void) fclose(c->report);
	if (c->report_temporary[0] != '\0')
		(void) unlink(c->report_temporary);
	FILE *streams[] = { c->files, c->blocks, c->chunks,
	    c->sorted_blocks, c->sorted_chunks, c->seen };
	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		if (streams[i] != NULL)
			(void) fclose(streams[i]);
	}
	if (c->directory[0] != '\0')
	{
		DIR *directory = opendir(c->directory);
		if (directory != NULL) {
			struct dirent *entry;
			while ((entry = readdir(directory)) != NULL) {
				if (strcmp(entry->d_name, ".") != 0 &&
				    strcmp(entry->d_name, "..") != 0)
					(void) unlinkat(dirfd(directory),
					    entry->d_name, 0);
			}
			(void) closedir(directory);
		}
		(void) rmdir(c->directory);
	}
}

/*
 * Workers never seek, truncate or assign IDs in the shared catalog. Each owns
 * three bounded-count disk spools; the coordinator merges any completed job
 * and assigns its shared IDs before selection and installation. A slow file
 * must not prevent finished slots from being reused by the other workers.
 */
struct cd_scan_job {
	struct cd_catalog local;
	struct cd_retry_file candidate;
	uint64_t age;
	boolean_t active;
};

struct cd_scan {
	struct cd_parallel parallel;
	struct cd_scan_job *jobs;
	struct cd_catalog *catalog;
	uint64_t age;
	atomic_int cancelled;
};

static int
cd_scan_work(void *argument)
{
	struct cd_scan_job *job = argument;
	if (cd_stop)
		return (ECANCELED);
	int error = cd_catalog_analyze(&job->local, job->candidate.dataset,
	    job->candidate.path, job->age, &job->candidate.identity, B_FALSE);
	return (cd_stop ? ECANCELED : error);
}

static int
cd_scan_copy(FILE *source, FILE *destination, uint64_t id)
{
	if (fseeko(source, 0, SEEK_SET) != 0)
		return (errno);
	struct cd_catalog_item item;
	int error;
	while ((error = cd_catalog_next(source, &item)) == 0 && !cd_stop) {
		if (item.file != 0)
			return (EPROTO);
		if (fprintf(destination, "%020" PRIu64 " %s %020" PRIu64
		    " %020" PRIu64 " %020" PRIu64 " %020" PRIu64
		    " %020" PRIu64 "\n", item.dataset, item.hash, item.length,
		    id, item.offset, item.bytes, item.object) < 0)
			return (EIO);
	}
	return (cd_stop ? ECANCELED : error == ENOENT ? 0 : error);
}

static int
cd_scan_take(struct cd_scan *scan)
{
	void *argument;
	int result;
	int error = cd_parallel_take(&scan->parallel, &argument, &result);
	if (error != 0)
		return (error);
	struct cd_scan_job *job = argument;
	job->active = B_FALSE;
	struct cd_catalog *c = scan->catalog, *local = &job->local;
	if (cd_stop)
		return (ECANCELED);
	if (result == EAGAIN) {
		struct cd_catalog_file file = { .identity = job->candidate.identity };
		(void) strlcpy(file.dataset, job->candidate.dataset,
		    sizeof (file.dataset));
		(void) strlcpy(file.path, job->candidate.path, sizeof (file.path));
		return (cd_catalog_enqueue(c, &file, scan->age));
	}
	if (cd_deferred(result)) {
		(void) fprintf(stderr, "%s: deferred (%s)\n",
		    job->candidate.path, strerror(result));
		c->skipped++;
		return (0);
	}
	if (result != 0) {
		(void) fprintf(stderr, "%s: %s\n", job->candidate.path,
		    strerror(result));
		return (result);
	}
	c->skipped += local->skipped;
	if (local->count == 0)
		return (0);
	if (local->count != 1)
		return (EPROTO);
	struct cd_catalog_file file;
	if ((error = cd_catalog_record(local, 0, &file, B_FALSE)) != 0)
		return (error);
	boolean_t duplicate;
	if ((error = cd_catalog_seen(c, &file.identity, B_FALSE,
	    &duplicate)) != 0)
		return (error);
	if (duplicate) {
		c->skipped++;
		return (0);
	}
	file.parent = c->count;
	if ((error = cd_scan_copy(local->blocks, c->blocks, c->count)) != 0 ||
	    (error = cd_scan_copy(local->chunks, c->chunks, c->count)) != 0 ||
	    (error = cd_catalog_seen(c, &file.identity, B_TRUE,
	    &duplicate)) != 0)
		return (error);
	if (fseeko(c->files, 0, SEEK_END) != 0 ||
	    fwrite(&file, sizeof (file), 1, c->files) != 1)
		return (errno != 0 ? errno : EIO);
	c->count++;
	return (0);
}

static int
cd_scan_drain(struct cd_scan *scan)
{
	while (cd_parallel_count(&scan->parallel) != 0 && !cd_stop) {
		int error = cd_scan_take(scan);
		if (error != 0)
			return (error);
	}
	return (cd_stop ? ECANCELED : 0);
}

static int
cd_scan_init(struct cd_scan *scan, struct cd_catalog *c, uint64_t age,
    unsigned int threads)
{
	memset(scan, 0, sizeof (*scan));
	scan->catalog = c;
	scan->age = age;
	atomic_init(&scan->cancelled, 0);
	int error = cd_parallel_init_unordered(&scan->parallel, threads,
	    cd_scan_work);
	if (error != 0)
		return (error);
	scan->jobs = calloc(scan->parallel.capacity, sizeof (*scan->jobs));
	if (scan->jobs == NULL) {
		cd_parallel_fini(&scan->parallel);
		return (ENOMEM);
	}
	return (0);
}

static void
cd_scan_fini(struct cd_scan *scan)
{
	unsigned int capacity = scan->parallel.capacity;
	/* Join before closing any spool or releasing any queued path/identity. */
	atomic_store(&scan->cancelled, 1);
	cd_parallel_fini(&scan->parallel);
	for (unsigned int i = 0; i < capacity; i++)
		cd_catalog_fini(&scan->jobs[i].local);
	free(scan->jobs);
}

static boolean_t
cd_scan_same(const struct stat *a, const struct stat *b)
{
	return (a->st_dev == b->st_dev && a->st_ino == b->st_ino);
}

static int
cd_scan_submit(struct cd_scan *scan, const char *dataset, const char *path,
    const struct stat *identity)
{
	struct cd_catalog *c = scan->catalog;
	/* Hard links must not create competing optimizer opens on one inode. */
	for (unsigned int i = 0; i < scan->parallel.capacity; i++) {
		if (!scan->jobs[i].active || !cd_scan_same(identity,
		    &scan->jobs[i].candidate.identity))
			continue;
		while (scan->jobs[i].active && !cd_stop) {
			int error = cd_scan_take(scan);
			if (error != 0)
				return (error);
		}
	}
	for (uint_t i = 0; i < c->retry_count; i++) {
		if (cd_scan_same(identity, &c->retry[i].identity)) {
			int error = cd_catalog_retry(c, scan->age);
			if (error != 0)
				return (error);
			break;
		}
	}
	if (cd_stop)
		return (ECANCELED);
	boolean_t duplicate;
	int error = cd_catalog_seen(c, identity, B_FALSE, &duplicate);
	if (error != 0)
		return (error);
	if (duplicate) {
		c->skipped++;
		return (0);
	}
	if (cd_parallel_full(&scan->parallel) &&
	    (error = cd_scan_take(scan)) != 0)
		return (error);
	struct cd_scan_job *job = NULL;
	for (unsigned int i = 0; i < scan->parallel.capacity; i++) {
		if (!scan->jobs[i].active) {
			job = &scan->jobs[i];
			break;
		}
	}
	if (job == NULL)
		return (EPROTO);
	struct cd_catalog *local = &job->local;
	local->staging = B_TRUE;
	local->cancel = &scan->cancelled;
	local->method = c->method;
	local->watch = c->watch;
	local->count = local->skipped = 0;
	FILE **streams[] = { &local->files, &local->blocks, &local->chunks };
	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		if (*streams[i] == NULL)
			*streams[i] = tmpfile();
		if (*streams[i] == NULL)
			return (errno);
		if (fflush(*streams[i]) != 0 ||
		    ftruncate(fileno(*streams[i]), 0) != 0 ||
		    fseeko(*streams[i], 0, SEEK_SET) != 0)
			return (errno != 0 ? errno : EIO);
	}
	if (strlcpy(job->candidate.dataset, dataset,
	    sizeof (job->candidate.dataset)) >= sizeof (job->candidate.dataset) ||
	    strlcpy(job->candidate.path, path, sizeof (job->candidate.path)) >=
	    sizeof (job->candidate.path))
		return (ENAMETOOLONG);
	job->candidate.identity = *identity;
	job->age = scan->age;
	job->active = B_TRUE;
	error = cd_parallel_submit(&scan->parallel, job);
	if (error != 0)
		job->active = B_FALSE;
	return (error);
}

static int
cd_status(const char *dataset)
{
	nvlist_t *args = fnvlist_alloc(), *result = NULL;
	fnvlist_add_string(args, "op", "status");
	int error = lzc_chunkdedup(dataset, args, &result);
	fnvlist_free(args);
	if (error == 0) {
		const char *fields[] = { "files", "records", "chunks",
		    "chunk_bytes", "logical_bytes", "references" };
		(void) printf("%s", dataset);
		for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
			uint64_t value;
			if (nvlist_lookup_uint64(result, fields[i],
			    &value) != 0) {
				error = EPROTO;
				break;
			}
			(void) printf(" %s=%" PRIu64, fields[i], value);
		}
		(void) putchar('\n');
	}
	nvlist_free(result);
	return (error);
}

static int
cd_method_status(const char *dataset, const char *path)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return (errno);
	struct stat identity;
	nvlist_t *result = NULL;
	int error = fstat(fd, &identity) != 0 ? errno : 0;
	if (error == 0)
		error = cd_inspect(dataset, &identity, B_FALSE, 0, &result);
	boolean_t chunked;
	if (error == 0 && nvlist_lookup_boolean_value(result, "chunked",
	    &chunked) != 0)
		error = EPROTO;
	if (error == 0)
		(void) printf("%s: representation=%s\n", path,
		    chunked ? "chunk" : "ordinary");
	nvlist_free(result);
	(void) close(fd);
	return (error);
}

static int
cd_block_status(const char *dataset)
{
	nvlist_t *args = fnvlist_alloc(), *result = NULL;
	fnvlist_add_string(args, "op", "block-status");
	int error = lzc_chunkdedup(dataset, args, &result);
	fnvlist_free(args);
	if (error == 0) {
		const char *pool;
		uint64_t used, saved, ratio;
		if (nvlist_lookup_string(result, "pool", &pool) != 0 ||
		    nvlist_lookup_uint64(result, "brt_used", &used) != 0 ||
		    nvlist_lookup_uint64(result, "brt_saved", &saved) != 0 ||
		    nvlist_lookup_uint64(result, "brt_ratio", &ratio) != 0)
			error = EPROTO;
		else
			(void) printf("pool_block_sharing: pool=%s "
			    "brt_used=%" PRIu64 " brt_saved=%" PRIu64
			    " brt_ratio=%" PRIu64
			    " scope=pool overhead=unreported\n", pool,
			    used, saved, ratio);
	}
	nvlist_free(result);
	return (error);
}

static int
cd_snapshot_path(zfs_handle_t *zhp, char *path, size_t size)
{
	char name[ZFS_MAX_DATASET_NAME_LEN];
	char *mountpoint = NULL;
	if (zfs_is_mounted(zhp, &mountpoint)) {
		int error = strlcpy(path, mountpoint, size) >= size ?
		    ENAMETOOLONG : 0;
		free(mountpoint);
		return (error);
	}
	if (strlcpy(name, zfs_get_name(zhp), sizeof (name)) >= sizeof (name))
		return (ENAMETOOLONG);
	char *snapshot = strchr(name, '@');
	if (snapshot == NULL)
		return (EINVAL);
	*snapshot++ = '\0';
	zfs_handle_t *head = zfs_open(g_zfs, name, ZFS_TYPE_FILESYSTEM);
	if (head == NULL)
		return (ENOENT);
	boolean_t mounted = zfs_is_mounted(head, &mountpoint);
	zfs_close(head);
	if (!mounted)
		return (ENOENT);
	int length = snprintf(path, size, "%s/.zfs/snapshot/%s", mountpoint,
	    snapshot);
	free(mountpoint);
	if (length < 0 || (size_t)length >= size)
		return (ENAMETOOLONG);
	/* Accessing the snapshot directory mounts its read-only objset. */
	struct stat st;
	if (stat(path, &st) != 0)
		return (errno);
	if (!S_ISDIR(st.st_mode))
		return (ENOTDIR);
	zfs_handle_t *resolved = zfs_path_to_zhandle(g_zfs, path,
	    ZFS_TYPE_FILESYSTEM | ZFS_TYPE_SNAPSHOT);
	if (resolved == NULL)
		return (ENOENT);
	int error = strcmp(zfs_get_name(resolved), zfs_get_name(zhp)) == 0 ?
	    0 : EXDEV;
	zfs_close(resolved);
	return (error);
}

static int
cd_pass(const char *op, boolean_t recursive, uint64_t age, int argc,
    char **argv, boolean_t watch, enum cd_method method, boolean_t methods,
    unsigned int threads, const char *report_path)
{
	uint64_t processed = 0, skipped = 0;
	int ret = 0;
	boolean_t optimize = strcmp(op, "optimize") == 0;
	struct cd_catalog catalog = { 0 };
	struct cd_scan scan = { 0 };
	if (optimize) {
		int error = cd_catalog_init(&catalog, method, watch);
		if (error == 0)
			error = cd_catalog_report_init(&catalog, report_path);
		if (error != 0) {
			(void) fprintf(stderr, "catalog: %s\n", strerror(error));
			cd_catalog_fini(&catalog);
			return (1);
		}
		error = cd_scan_init(&scan, &catalog, age, threads);
		if (error != 0) {
			(void) fprintf(stderr, "scan workers: %s\n", strerror(error));
			cd_catalog_fini(&catalog);
			return (1);
		}
		(void) printf("optimize: scan_threads=%u max_inflight=%zu\n",
		    threads, scan.parallel.capacity);
		(void) fflush(stdout);
	}
	boolean_t readonly = strcmp(op, "status") == 0 ||
	    strcmp(op, "verify") == 0;
	zfs_type_t types = ZFS_TYPE_FILESYSTEM |
	    (readonly ? ZFS_TYPE_SNAPSHOT : 0);
	for (int i = 0; i < argc && !cd_stop; i++) {
		char mountpoint[ZFS_MAXPROPLEN], path[PATH_MAX];
		struct stat st;
		boolean_t dataset_target = watch || lstat(argv[i], &st) != 0;
		zfs_handle_t *zhp = dataset_target ?
		    zfs_open(g_zfs, argv[i], types) :
		    zfs_path_to_zhandle(g_zfs, argv[i], types);
		if (zhp == NULL) {
			ret = 1;
			continue;
		}
		const char *dataset = zfs_get_name(zhp);
		int error = 0;
		if (zfs_get_underlying_type(zhp) != ZFS_TYPE_FILESYSTEM) {
			error = ENOTSUP;
			goto target_done;
		}
		boolean_t snapshot = zfs_get_type(zhp) == ZFS_TYPE_SNAPSHOT;
		if (snapshot && dataset_target &&
		    (error = cd_snapshot_path(zhp, path, sizeof (path))) != 0)
			goto target_done;
		if (watch) {
			uint64_t enabled, mounted;
			if (zfs_prop_get_numeric(zhp, ZFS_PROP_CHUNKDEDUP,
			    &enabled, NULL, NULL, 0) != 0 ||
			    zfs_prop_get_numeric(zhp, ZFS_PROP_MOUNTED,
			    &mounted, NULL, NULL, 0) != 0) {
				error = EIO;
				goto target_done;
			}
			if (!enabled || !mounted)
				goto target_done;
		}
		if ((optimize || methods) && !snapshot) {
			uint64_t retained;
			if (zfs_prop_get_numeric(zhp, ZFS_PROP_USEDSNAP,
			    &retained, NULL, NULL, 0) != 0) {
				error = EIO;
				goto target_done;
			}
			(void) printf("%s: snapshot_retained_bytes=%" PRIu64
			    " scope=dataset\n", dataset, retained);
		}
		if (strcmp(op, "status") == 0) {
			error = cd_status(dataset);
			if (error != 0 || !methods)
				goto target_done;
			error = cd_block_status(dataset);
			if (error != 0)
				goto target_done;
		}
		if (dataset_target && !snapshot) {
			if (zfs_prop_get(zhp, ZFS_PROP_MOUNTPOINT, mountpoint,
			    sizeof (mountpoint), NULL, NULL, 0, B_TRUE) != 0 ||
			    mountpoint[0] != '/') {
				error = ENOENT;
				goto target_done;
			}
			(void) strlcpy(path, mountpoint, sizeof (path));
		} else if (!dataset_target &&
		    strlen(argv[i]) >= sizeof (path)) {
			error = ENAMETOOLONG;
			goto target_done;
		} else if (!dataset_target) {
			(void) strlcpy(path, argv[i], sizeof (path));
		}
		char *paths[] = { path, NULL };
		FTS *tree = fts_open(paths,
		    FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, NULL);
		if (tree == NULL) {
			error = errno;
			goto target_done;
		}
		FTSENT *entry;
		while (!cd_stop) {
			errno = 0;
			entry = fts_read(tree);
			if (entry == NULL) {
				if (errno != 0)
					error = errno;
				break;
			}
			if (entry->fts_info == FTS_D &&
			    (strcmp(entry->fts_name, ".zfs") == 0 ||
			    (entry->fts_level > 0 && !recursive &&
			    !dataset_target))) {
				(void) fts_set(tree, entry, FTS_SKIP);
			} else if (entry->fts_info == FTS_F) {
				int e;
				if (optimize)
					e = cd_scan_submit(&scan, dataset,
					    entry->fts_path, entry->fts_statp);
				else if (methods)
					e = cd_method_status(dataset,
					    entry->fts_path);
				else
					e = cd_file(dataset, entry->fts_path,
					    op, age, &processed, &skipped, watch, NULL);
				if (e != 0) {
					(void) fprintf(stderr, "%s: %s\n",
					    entry->fts_path, strerror(e));
					error = e;
					if (optimize)
						break;
				}
			} else if (entry->fts_info == FTS_ERR ||
			    entry->fts_info == FTS_DNR ||
			    entry->fts_info == FTS_NS) {
				error = entry->fts_errno;
			}
		}
		if (fts_close(tree) != 0 && error == 0)
			error = errno;
		if (optimize && error == 0)
			error = cd_scan_drain(&scan);
target_done:
		if (error != 0) {
			(void) fprintf(stderr, "%s: %s\n", argv[i],
			    strerror(error));
			ret = 1;
		}
		zfs_close(zhp);
		if (optimize && error != 0)
			break;
	}
	if (optimize) {
		cd_scan_fini(&scan);
		/* Discovery failures never leave a partially selected pass installed. */
		if (ret == 0 && !cd_stop) {
			int error = cd_catalog_retry(&catalog, age);
			if (error == 0 && !cd_stop)
				error = cd_catalog_apply(&catalog, age, threads);
			if (error != 0) {
				(void) fprintf(stderr, "optimize: %s\n",
				    strerror(error));
				ret = 1;
			}
		}
		processed = catalog.processed;
		skipped = catalog.skipped;
		cd_catalog_fini(&catalog);
	}
	if (strcmp(op, "status") != 0)
		(void) printf("%s: processed=%" PRIu64 " skipped=%" PRIu64 "\n",
		    watch ? "watch" : op, processed, skipped);
	(void) fflush(stdout);
	return (ret);
}

static int
cd_watch(uint64_t age, uint64_t interval, int argc, char **argv,
    enum cd_method method, unsigned int threads)
{
	/* Reject paths and misspelled datasets before entering supervision. */
	for (int i = 0; i < argc; i++) {
		zfs_handle_t *zhp = zfs_open(g_zfs, argv[i],
		    ZFS_TYPE_FILESYSTEM);
		if (zhp == NULL)
			return (1);
		zfs_close(zhp);
	}
	struct sigaction action = { 0 }, old_int, old_term;
	action.sa_handler = cd_signal;
	(void) sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &old_int) != 0)
		return (1);
	if (sigaction(SIGTERM, &action, &old_term) != 0) {
		(void) sigaction(SIGINT, &old_int, NULL);
		return (1);
	}
	int ret = 0;
	(void) printf("watch: interval=%" PRIu64 " seconds min-age=%" PRIu64
	    " days\n", interval, age);
	(void) fflush(stdout);
	while (!cd_stop) {
		(void) cd_pass("optimize", B_TRUE, age, argc, argv, B_TRUE,
		    method, B_FALSE, threads, NULL);
		if (cd_stop)
			break;
		struct timespec delay = { .tv_sec = (time_t)interval };
		while (!cd_stop && nanosleep(&delay, &delay) != 0) {
			if (errno != EINTR) {
				(void) fprintf(stderr, "watch: sleep: %s\n",
				    strerror(errno));
				ret = 1;
				cd_stop = 1;
				break;
			}
		}
	}
	(void) sigaction(SIGINT, &old_int, NULL);
	(void) sigaction(SIGTERM, &old_term, NULL);
	(void) printf("watch: stopped\n");
	return (ret);
}

int
zfs_do_chunkdedup(int argc, char **argv)
{
	cd_stop = 0;
	if (argc < 3) {
		(void) fprintf(stderr, "usage: zfs chunkdedup "
		    "<optimize|status|verify|unoptimize> [-r] [--min-age days] "
		    "[--method auto|block|chunk] [--methods] "
		    "[--threads 1..64] [--report PATH] "
		    "<dataset|path>...\n"
		    "       zfs chunkdedup watch [--interval seconds] "
		    "[--min-age days] [--method auto|block|chunk] "
		    "[--threads 1..64] <dataset>...\n");
		return (2);
	}
	const char *op = argv[1];
	boolean_t watch = strcmp(op, "watch") == 0;
	if (!watch && strcmp(op, "optimize") && strcmp(op, "status") &&
	    strcmp(op, "verify") && strcmp(op, "unoptimize"))
		return (2);
	boolean_t recursive = B_FALSE;
	boolean_t methods = B_FALSE;
	const char *report_path = NULL;
	enum cd_method method = CD_METHOD_AUTO;
	uint64_t age = 3, interval = 3600;
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned int threads = online > 0 ? MIN(online, 64) : 1;
	const struct option options[] = {
		{ "min-age", required_argument, NULL, 'a' },
		{ "interval", required_argument, NULL, 'i' },
		{ "recursive", no_argument, NULL, 'r' },
		{ "method", required_argument, NULL, 'm' },
		{ "methods", no_argument, NULL, 'M' },
		{ "threads", required_argument, NULL, 't' },
		{ "report", required_argument, NULL, 'R' },
		{ NULL, 0, NULL, 0 }
	};
	int c;
	optind = 2;
	while ((c = getopt_long(argc, argv, "+ra:i:m:Mt:R:", options, NULL)) != -1) {
		if (c == 'r') {
			if (watch)
				return (2);
			recursive = B_TRUE;
		} else if (c == 'm') {
			if (!watch && strcmp(op, "optimize") != 0)
				return (2);
			if (strcmp(optarg, "auto") == 0)
				method = CD_METHOD_AUTO;
			else if (strcmp(optarg, "block") == 0)
				method = CD_METHOD_BLOCK;
			else if (strcmp(optarg, "chunk") == 0)
				method = CD_METHOD_CHUNK;
			else
				return (2);
		} else if (c == 'R') {
			if (watch || strcmp(op, "optimize") != 0 || optarg[0] == '\0')
				return (2);
			report_path = optarg;
		} else if (c == 'M') {
			if (strcmp(op, "status") != 0)
				return (2);
			methods = B_TRUE;
		} else if (c == 'a' || c == 'i' || c == 't') {
			char *end;
			errno = 0;
			uint64_t value = strtoull(optarg, &end, 10);
			if (errno || *end || optarg[0] == '-' ||
			    optarg[0] == '\0')
				return (2);
			if (c == 'a') {
				if (value > UINT64_MAX / 86400)
					return (2);
				age = value;
			} else if (c == 'i') {
				if (!watch || value == 0 || value > INT_MAX)
					return (2);
				interval = value;
			} else {
				if ((!watch && strcmp(op, "optimize") != 0) ||
				    value == 0 || value > 64)
					return (2);
				threads = (unsigned int)value;
			}
		} else {
			return (2);
		}
	}
	if (optind == argc)
		return (2);
	if (watch)
		return (cd_watch(age, interval, argc - optind, argv + optind,
		    method, threads));
	struct sigaction action = { 0 }, old_int, old_term;
	boolean_t optimize = strcmp(op, "optimize") == 0;
	if (optimize) {
		action.sa_handler = cd_signal;
		(void) sigemptyset(&action.sa_mask);
		if (sigaction(SIGINT, &action, &old_int) != 0)
			return (1);
		if (sigaction(SIGTERM, &action, &old_term) != 0) {
			(void) sigaction(SIGINT, &old_int, NULL);
			return (1);
		}
	}
	int result = cd_pass(op, recursive, age, argc - optind, argv + optind,
	    B_FALSE, method, methods, threads, report_path);
	if (optimize) {
		(void) sigaction(SIGINT, &old_int, NULL);
		(void) sigaction(SIGTERM, &old_term, NULL);
		if (cd_stop)
			return (1);
	}
	return (result);
}
