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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "chunkdedup_chunker.h"
#include "chunkdedup_hybrid.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#include "zfs_util.h"

static volatile sig_atomic_t cd_stop;

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
 * union/find forest live in a private temporary file; sort spills catalogs to
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

struct cd_catalog {
	FILE *files, *blocks, *chunks, *sorted_blocks, *sorted_chunks, *seen;
	char directory[PATH_MAX];
	uint64_t count, processed, skipped;
	uint64_t seen_capacity;
	enum cd_method method;
	boolean_t watch;
};

struct cd_catalog_item {
	uint64_t dataset, length, file, offset, bytes, object;
	char hash[65];
};

static boolean_t cd_deferred(int);

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

static int
cd_catalog_chunk(void *argument, const struct chunkdedup_chunk *chunk,
    const uint8_t *bytes)
{
	struct cd_catalog_chunker *arg = argument;
	struct cd_catalog_file *file = arg->file;
	(void) bytes;
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
    const char *path, uint64_t age)
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
	if (fd < 0)
		return (errno);
	int error = fstat(fd, &file.identity) != 0 ? errno : 0;
	nvlist_t *info = NULL;
	if (error != 0)
		goto done;
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
	if ((error = cd_catalog_seen(c, &file.identity, B_FALSE,
	    &duplicate)) != 0)
		goto done;
	if (duplicate) {
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
	    error == 0 && !cd_stop; offset += file.block_size) {
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
	if (error == 0 && !cd_stop)
		error = chunkdedup_chunker_finish(chunker);
	free(buffer);
	chunkdedup_chunker_destroy(chunker);
	if (error != 0 || cd_stop)
		goto done;
	if (native_bytes > allocated) {
		file.uncertain = B_TRUE;
		file.native_base = 0;
	} else {
		file.native_base = allocated - native_bytes;
	}
append:
	if ((error = cd_catalog_seen(c, &file.identity, B_TRUE,
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
	if (error == EBUSY || error == ESTALE || error == EAGAIN) {
		(void) fprintf(stderr, "%s: deferred (%s)\n", path,
		    strerror(error));
		c->skipped++;
		return (0);
	}
	return (error);
}

static int
cd_catalog_connect(struct cd_catalog *c, FILE *items)
{
	struct cd_catalog_item first = { 0 }, item;
	boolean_t have = B_FALSE;
	int error;
	while ((error = cd_catalog_next(items, &item)) == 0) {
		if (!have || !cd_catalog_same(&first, &item)) {
			first = item;
			have = B_TRUE;
		} else if ((error = cd_catalog_join(c, first.file,
		    item.file)) != 0) {
			return (error);
		}
	}
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
	int error;
	while ((error = cd_catalog_next(items, &item)) == 0) {
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
	if (error != ENOENT)
		return (error);
	if (have && (error = cd_catalog_charge(c, first.file,
	    chunks && existing ? 0 : cost, chunks)) != 0)
		return (error);
	return (fseeko(items, 0, SEEK_SET) == 0 ? 0 : errno);
}

static int
cd_catalog_select(struct cd_catalog *c)
{
	int error = cd_hybrid_sort(c->blocks, &c->sorted_blocks,
	    c->directory);
	if (error == 0)
		error = cd_hybrid_sort(c->chunks, &c->sorted_chunks,
		    c->directory);
	if (error == 0)
		error = cd_catalog_connect(c, c->sorted_chunks);
	if (error == 0)
		error = cd_catalog_connect(c, c->sorted_blocks);
	for (uint64_t i = 0; error == 0 && i < c->count; i++) {
		struct cd_catalog_file file, group;
		uint64_t root;
		if ((error = cd_catalog_record(c, i, &file, B_FALSE)) != 0 ||
		    (error = cd_catalog_root(c, i, &root)) != 0 ||
		    (error = cd_catalog_record(c, root, &group, B_FALSE)) != 0)
			break;
		group.uncertain |= file.uncertain;
		if ((error = cd_hybrid_add(&group.native_estimate,
		    file.native_base)) == 0)
			error = cd_hybrid_add(&group.chunk_estimate,
			    file.chunk_base);
		if (error == 0)
			error = cd_catalog_record(c, root, &group, B_TRUE);
	}
	if (error == 0)
		error = cd_catalog_cost(c, c->sorted_blocks, B_FALSE);
	if (error == 0)
		error = cd_catalog_cost(c, c->sorted_chunks, B_TRUE);
	for (uint64_t i = 0; error == 0 && i < c->count; i++) {
		struct cd_catalog_file file, group;
		uint64_t root;
		if ((error = cd_catalog_record(c, i, &file, B_FALSE)) != 0 ||
		    (error = cd_catalog_root(c, i, &root)) != 0 ||
		    (error = cd_catalog_record(c, root, &group, B_FALSE)) != 0)
			break;
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
		error = cd_catalog_record(c, i, &file, B_TRUE);
	}
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

static int
cd_catalog_share(struct cd_catalog *c)
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

static int
cd_catalog_apply(struct cd_catalog *c, uint64_t age)
{
	int error = cd_catalog_select(c);
	if (error == 0)
		error = cd_catalog_share(c);
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
	c->seen_capacity = 1024;
	if (c->files == NULL || c->blocks == NULL || c->chunks == NULL ||
	    c->seen == NULL)
		return (errno);
	return (ftruncate(fileno(c->seen), c->seen_capacity *
	    sizeof (struct cd_catalog_seen)) == 0 ? 0 : errno);
}

static void
cd_catalog_fini(struct cd_catalog *c)
{
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
    char **argv, boolean_t watch, enum cd_method method, boolean_t methods)
{
	uint64_t processed = 0, skipped = 0;
	int ret = 0;
	boolean_t optimize = strcmp(op, "optimize") == 0;
	struct cd_catalog catalog = { 0 };
	if (optimize) {
		int error = cd_catalog_init(&catalog, method, watch);
		if (error != 0) {
			(void) fprintf(stderr, "catalog: %s\n", strerror(error));
			cd_catalog_fini(&catalog);
			return (1);
		}
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
					e = cd_catalog_analyze(&catalog, dataset,
					    entry->fts_path, age);
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
				}
			} else if (entry->fts_info == FTS_ERR ||
			    entry->fts_info == FTS_DNR ||
			    entry->fts_info == FTS_NS) {
				error = entry->fts_errno;
			}
		}
		if (fts_close(tree) != 0 && error == 0)
			error = errno;
target_done:
		if (error != 0) {
			(void) fprintf(stderr, "%s: %s\n", argv[i],
			    strerror(error));
			ret = 1;
		}
		zfs_close(zhp);
	}
	if (optimize) {
		/* Discovery failures never leave a partially selected pass installed. */
		if (ret == 0 && !cd_stop) {
			int error = cd_catalog_apply(&catalog, age);
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
    enum cd_method method)
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
		    method, B_FALSE);
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
		    "<dataset|path>...\n"
		    "       zfs chunkdedup watch [--interval seconds] "
		    "[--min-age days] [--method auto|block|chunk] <dataset>...\n");
		return (2);
	}
	const char *op = argv[1];
	boolean_t watch = strcmp(op, "watch") == 0;
	if (!watch && strcmp(op, "optimize") && strcmp(op, "status") &&
	    strcmp(op, "verify") && strcmp(op, "unoptimize"))
		return (2);
	boolean_t recursive = B_FALSE;
	boolean_t methods = B_FALSE;
	enum cd_method method = CD_METHOD_AUTO;
	uint64_t age = 3, interval = 3600;
	const struct option options[] = {
		{ "min-age", required_argument, NULL, 'a' },
		{ "interval", required_argument, NULL, 'i' },
		{ "recursive", no_argument, NULL, 'r' },
		{ "method", required_argument, NULL, 'm' },
		{ "methods", no_argument, NULL, 'M' },
		{ NULL, 0, NULL, 0 }
	};
	int c;
	optind = 2;
	while ((c = getopt_long(argc, argv, "+ra:i:m:M", options, NULL)) != -1) {
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
		} else if (c == 'M') {
			if (strcmp(op, "status") != 0)
				return (2);
			methods = B_TRUE;
		} else if (c == 'a' || c == 'i') {
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
			} else {
				if (!watch || value == 0 || value > INT_MAX)
					return (2);
				interval = value;
			}
		} else {
			return (2);
		}
	}
	if (optind == argc)
		return (2);
	if (watch)
		return (cd_watch(age, interval, argc - optind, argv + optind,
		    method));
	return (cd_pass(op, recursive, age, argc - optind, argv + optind,
	    B_FALSE, method, methods));
}
