// SPDX-License-Identifier: CDDL-1.0
/*
 * Experimental native post-process chunk storage.
 *
 * All metadata and immutable chunk objects belong to the file's objset.  DMU
 * copy-on-write therefore preserves complete recipes, indexes and reference
 * counts in snapshots and clones. References count recipe slices, not files.
 * The dataset mutex serializes metadata readers with object reclamation and is
 * held until transaction commit. The caller provides normal vnode/range locks.
 */
#include <sys/zfs_context.h>
#include <sys/zfs_chunkdedup.h>
#include <sys/zap.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/sha2.h>
#include <sys/nvpair.h>
#include <sys/dsl_dataset.h>
#include <sys/zfeature.h>
#include <sys/sa.h>
#if defined(__FreeBSD__) && defined(_KERNEL)
#include <sys/proc.h>
#endif
#if defined(__FreeBSD__) && defined(__amd64__) && defined(_KERNEL)
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>
#endif

#define	CD_ROOT_WORDS 4
#define	CD_REF_WORDS 6
#define	CD_COLLISIONS 32
#define	CD_RECIPE_WORDS (3 * ZFS_CHUNKDEDUP_MAX_CHUNKS)
#define	CD_SOURCE_MAX (ZFS_CHUNKDEDUP_MAX_RANGE + \
    2 * ZFS_CHUNKDEDUP_MAX_CHUNK)
#define	CD_STAGE_IDLE_MAX (256U << 10)

typedef struct cd_roots {
	uint64_t version;
	uint64_t maps;
	uint64_t index;
	uint64_t refs;
} cd_roots_t;

#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
/*
 * Only serialized, validated metadata supplies cached validity; no chunk dbuf
 * is retained. Generic staging capacity below has no valid cached contents.
 * The mutex protects the complete cache and prevents reclamation during copies.
 * All mutations, including DEBUG faults, invalidate before their first change;
 * receive/rollback resume invalidates even when the objset pointer is reused.
 * Forced verification/reclamation uses raw helpers and a fresh complete scan.
 * Internal reads in a mutating hydration TXG must neither use nor fill entries.
 * Limits cover allocation requests, not allocator padding or bookkeeping.
 */
#define	CD_CACHE_BYTES (1U << 20)
#define	CD_CACHE_ENTRIES 4096
#define	CD_CACHE_BUCKETS 1024
#define	CD_CACHE_FILE 1
#define	CD_CACHE_RECORD 2
#define	CD_ROOT_UNKNOWN 0
#define	CD_ROOT_ABSENT 1
#define	CD_ROOT_PRESENT 2

typedef struct cd_cache_entry {
	struct cd_cache_entry *hash_next;
	struct cd_cache_entry *prev;
	struct cd_cache_entry *next;
	uint64_t epoch;
	uint64_t object;
	uint64_t generation;
	uint64_t offset;
	uint64_t record_length;
	size_t allocation;
	uint_t kind;
	uint_t words;
	int error;
	/* File words, or recipe triples followed by one length per triple. */
	uint64_t data[];
} cd_cache_entry_t;

typedef struct zfs_chunkdedup_cache {
	objset_t *os;
	cd_roots_t roots;
	boolean_t valid;
	uint_t root_state;
	uint64_t epoch;
	size_t bytes;
	uint_t entries;
	cd_cache_entry_t *head;
	cd_cache_entry_t *tail;
	cd_cache_entry_t *buckets[CD_CACHE_BUCKETS];
	/* Generic scratch capacity, never a recipe or valid logical data cache. */
	void *stage_idle;
	size_t stage_capacity;
} zfs_chunkdedup_cache_t;

static uint64_t
cd_cache_generation(znode_t *zp)
{
#if defined(__linux__)
	/* Linux exposes the SA generation through its existing inode field. */
	return (ZTOI(zp)->i_generation);
#else
	return (zp->z_gen);
#endif
}

static uint_t
cd_cache_bucket(uint_t kind, uint64_t object, uint64_t generation,
    uint64_t offset, uint64_t record_length)
{
	uint64_t hash = object ^ (generation << 7) ^ (offset >> 9) ^
	    (record_length << 3) ^ kind;
	hash ^= hash >> 33;
	hash *= UINT64_C(0xff51afd7ed558ccd);
	hash ^= hash >> 33;
	return (hash & (CD_CACHE_BUCKETS - 1));
}

static void
cd_cache_unlink(zfs_chunkdedup_cache_t *cache, cd_cache_entry_t *entry)
{
	uint_t bucket = cd_cache_bucket(entry->kind, entry->object,
	    entry->generation, entry->offset, entry->record_length);
	cd_cache_entry_t **link = &cache->buckets[bucket];
	while (*link != entry) {
		ASSERT(*link != NULL);
		link = &(*link)->hash_next;
	}
	*link = entry->hash_next;
	if (entry->prev != NULL)
		entry->prev->next = entry->next;
	else
		cache->head = entry->next;
	if (entry->next != NULL)
		entry->next->prev = entry->prev;
	else
		cache->tail = entry->prev;
	ASSERT(cache->entries != 0 && cache->bytes >= entry->allocation);
	cache->entries--;
	cache->bytes -= entry->allocation;
	kmem_free(entry, entry->allocation);
}

static void
cd_cache_reset(zfs_chunkdedup_cache_t *cache)
{
	while (cache->tail != NULL)
		cd_cache_unlink(cache, cache->tail);
	ASSERT(cache->entries == 0 && cache->bytes == sizeof (*cache));
	cache->valid = B_FALSE;
	cache->root_state = CD_ROOT_UNKNOWN;
	/* Resetting frees every old entry before an epoch can be reused. */
	if (++cache->epoch == 0)
		cache->epoch = 1;
}

static zfs_chunkdedup_cache_t *
cd_cache_get(zfsvfs_t *zfsvfs)
{
	ASSERT(MUTEX_HELD(&zfsvfs->z_chunkdedup_lock));
	zfs_chunkdedup_cache_t *cache = zfsvfs->z_chunkdedup_cache;
	if (cache == NULL) {
		if (sizeof (*cache) > CD_CACHE_BYTES)
			return (NULL);
		cache = kmem_alloc(sizeof (*cache), KM_NOSLEEP);
		if (cache == NULL)
			return (NULL);
		memset(cache, 0, sizeof (*cache));
		cache->bytes = sizeof (*cache);
		cache->epoch = 1;
		cache->os = zfsvfs->z_os;
		zfsvfs->z_chunkdedup_cache = cache;
	} else if (cache->os != zfsvfs->z_os) {
		cd_cache_reset(cache);
		cache->os = zfsvfs->z_os;
	}
	return (cache);
}

static cd_cache_entry_t *
cd_cache_find(zfs_chunkdedup_cache_t *cache, uint_t kind, znode_t *zp,
    uint64_t offset, uint64_t record_length)
{
	uint64_t generation = cd_cache_generation(zp);
	uint_t bucket = cd_cache_bucket(kind, zp->z_id, generation, offset,
	    record_length);
	for (cd_cache_entry_t *entry = cache->buckets[bucket]; entry != NULL;
	    entry = entry->hash_next) {
		if (entry->epoch != cache->epoch || entry->kind != kind ||
		    entry->object != zp->z_id || entry->generation != generation ||
		    entry->offset != offset || entry->record_length != record_length)
			continue;
		if (entry != cache->head) {
			if (entry->prev != NULL)
				entry->prev->next = entry->next;
			if (entry->next != NULL)
				entry->next->prev = entry->prev;
			else
				cache->tail = entry->prev;
			entry->prev = NULL;
			entry->next = cache->head;
			cache->head->prev = entry;
			cache->head = entry;
		}
		return (entry);
	}
	return (NULL);
}

static void
cd_cache_store(zfsvfs_t *zfsvfs, uint_t kind, znode_t *zp,
    uint64_t offset, uint64_t record_length, int error,
    const uint64_t *data, uint_t words, const uint64_t *lengths)
{
	zfs_chunkdedup_cache_t *cache = cd_cache_get(zfsvfs);
	if (cache == NULL || (kind != CD_CACHE_FILE && kind != CD_CACHE_RECORD) ||
	    cache->root_state != CD_ROOT_PRESENT ||
	    (kind == CD_CACHE_FILE &&
	    (words != 2 || (error != 0 && error != ENOENT))) ||
	    (kind == CD_CACHE_RECORD && (!cache->valid || error != 0 ||
	    words == 0 || words > CD_RECIPE_WORDS || words % 3 != 0)))
		return;
	if (cd_cache_find(cache, kind, zp, offset, record_length) != NULL)
		return;
	size_t extra = kind == CD_CACHE_RECORD ? words / 3 : 0;
	size_t allocation = sizeof (cd_cache_entry_t) +
	    (words + extra) * sizeof (uint64_t);
	if (allocation > CD_CACHE_BYTES - sizeof (*cache))
		return;
	while (cache->entries >= CD_CACHE_ENTRIES ||
	    cache->bytes > CD_CACHE_BYTES - allocation)
		cd_cache_unlink(cache, cache->tail);
	cd_cache_entry_t *entry = kmem_alloc(allocation, KM_NOSLEEP);
	if (entry == NULL)
		return;
	memset(entry, 0, sizeof (*entry));
	entry->epoch = cache->epoch;
	entry->kind = kind;
	entry->object = zp->z_id;
	entry->generation = cd_cache_generation(zp);
	entry->offset = offset;
	entry->record_length = record_length;
	entry->allocation = allocation;
	entry->words = words;
	entry->error = error;
	if (error == 0)
		memcpy(entry->data, data, words * sizeof (uint64_t));
	if (extra != 0)
		memcpy(entry->data + words, lengths,
		    extra * sizeof (uint64_t));
	uint_t bucket = cd_cache_bucket(kind, entry->object, entry->generation,
	    offset, record_length);
	entry->hash_next = cache->buckets[bucket];
	cache->buckets[bucket] = entry;
	entry->next = cache->head;
	if (cache->head != NULL)
		cache->head->prev = entry;
	else
		cache->tail = entry;
	cache->head = entry;
	cache->entries++;
	cache->bytes += allocation;
}
#endif

static int cd_counts_verify(objset_t *, const cd_roots_t *);
static int cd_key_offset(const char *, uint64_t *);
static int cd_file_offsets(znode_t *, const cd_roots_t *, const uint64_t *,
    uint64_t **, uint_t *);

typedef struct cd_ref {
	uint64_t length;
	uint64_t count;
	uint64_t hash[4];
} cd_ref_t;

void
zfs_chunkdedup_lock(zfsvfs_t *zfsvfs)
{
	mutex_enter(&zfsvfs->z_chunkdedup_lock);
}

void
zfs_chunkdedup_unlock(zfsvfs_t *zfsvfs)
{
	mutex_exit(&zfsvfs->z_chunkdedup_lock);
}

static void
cd_cache_invalidate_locked(zfsvfs_t *zfsvfs)
{
	ASSERT(MUTEX_HELD(&zfsvfs->z_chunkdedup_lock));
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	if (zfsvfs->z_chunkdedup_cache != NULL)
		cd_cache_reset(zfsvfs->z_chunkdedup_cache);
#endif
}

void
zfs_chunkdedup_cache_invalidate(zfsvfs_t *zfsvfs)
{
	void *idle = NULL;
	size_t capacity = 0;
	zfs_chunkdedup_lock(zfsvfs);
	cd_cache_invalidate_locked(zfsvfs);
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	zfs_chunkdedup_cache_t *cache = zfsvfs->z_chunkdedup_cache;
	if (cache != NULL) {
		idle = cache->stage_idle;
		capacity = cache->stage_capacity;
		cache->stage_idle = NULL;
		cache->stage_capacity = 0;
	}
#endif
	zfs_chunkdedup_unlock(zfsvfs);
	if (idle != NULL)
		kmem_free(idle, capacity);
}

void
zfs_chunkdedup_cache_fini(zfsvfs_t *zfsvfs)
{
	void *idle = NULL;
	size_t capacity = 0;
	zfs_chunkdedup_lock(zfsvfs);
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	zfs_chunkdedup_cache_t *cache = zfsvfs->z_chunkdedup_cache;
	zfsvfs->z_chunkdedup_cache = NULL;
	if (cache != NULL) {
		idle = cache->stage_idle;
		capacity = cache->stage_capacity;
		cd_cache_reset(cache);
		kmem_free(cache, sizeof (*cache));
	}
#endif
	zfs_chunkdedup_unlock(zfsvfs);
	if (idle != NULL)
		kmem_free(idle, capacity);
}

/*
 * No reader waits for another loan. Slot operations alone hold the metadata
 * mutex; allocation/free and all destination copying stay outside it. Normal
 * metadata resets may leave idle capacity, since every read refills its bytes.
 * Teardown/resume explicitly drains it. The caller's existing teardown reader
 * prevents header destruction/reuse during this ownership token's lifetime.
 */
int
zfs_chunkdedup_stage_acquire(zfsvfs_t *zfsvfs, size_t size,
    zfs_chunkdedup_stage_t *stage)
{
	memset(stage, 0, sizeof (*stage));
	if (size == 0)
		return (0);
	if (size > ZFS_CHUNKDEDUP_MAX_RANGE)
		return (SET_ERROR(E2BIG));
	void *retired = NULL;
	size_t capacity = 0;
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	if (size <= CD_STAGE_IDLE_MAX) {
		zfs_chunkdedup_lock(zfsvfs);
		zfs_chunkdedup_cache_t *cache = zfsvfs->z_chunkdedup_cache;
		boolean_t changed = cache != NULL && cache->os != zfsvfs->z_os;
		cache = cd_cache_get(zfsvfs);
		if (cache != NULL) {
			if (!changed) {
				stage->owner = cache;
				stage->os = cache->os;
				stage->epoch = cache->epoch;
			}
			if (cache->stage_idle != NULL) {
				if (!changed && cache->stage_capacity >= size) {
					stage->buffer = cache->stage_idle;
					stage->allocation = cache->stage_capacity;
				} else {
					retired = cache->stage_idle;
					capacity = cache->stage_capacity;
				}
				cache->stage_idle = NULL;
				cache->stage_capacity = 0;
			}
		}
		zfs_chunkdedup_unlock(zfsvfs);
	}
#else
	(void) zfsvfs;
#endif
	/* Free insufficient/old-lifecycle capacity before a replacement request. */
	if (retired != NULL)
		kmem_free(retired, capacity);
	if (stage->buffer == NULL) {
		stage->buffer = kmem_alloc(size, KM_SLEEP);
		if (stage->buffer == NULL) {
			memset(stage, 0, sizeof (*stage));
			return (SET_ERROR(ENOMEM));
		}
		stage->allocation = size;
	}
	return (0);
}

void
zfs_chunkdedup_stage_release(zfsvfs_t *zfsvfs,
    zfs_chunkdedup_stage_t *stage, boolean_t complete)
{
	if (stage->buffer == NULL)
		return;
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	if (complete && stage->owner != NULL &&
	    stage->allocation <= CD_STAGE_IDLE_MAX) {
		zfs_chunkdedup_lock(zfsvfs);
		zfs_chunkdedup_cache_t *cache = zfsvfs->z_chunkdedup_cache;
		/* Compare before dereferencing: the token never owns the header. */
		if (cache == stage->owner && cache->os == zfsvfs->z_os &&
		    cache->os == stage->os && cache->epoch == stage->epoch &&
		    cache->stage_idle == NULL) {
			cache->stage_idle = stage->buffer;
			cache->stage_capacity = stage->allocation;
			stage->buffer = NULL;
		}
		zfs_chunkdedup_unlock(zfsvfs);
	}
#else
	(void) zfsvfs;
	(void) complete;
#endif
	if (stage->buffer != NULL)
		kmem_free(stage->buffer, stage->allocation);
	memset(stage, 0, sizeof (*stage));
}

/* Platforms/models without the cache-owning zfsvfs layout retain full scans. */
static int
cd_counts_check(zfsvfs_t *zfsvfs, const cd_roots_t *roots, boolean_t force)
{
	ASSERT(MUTEX_HELD(&zfsvfs->z_chunkdedup_lock));
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	zfs_chunkdedup_cache_t *cache = cd_cache_get(zfsvfs);
	if (!force && cache != NULL && cache->valid &&
	    cache->os == zfsvfs->z_os &&
	    memcmp(&cache->roots, roots, sizeof (*roots)) == 0)
		return (0);
	cd_cache_invalidate_locked(zfsvfs);
#else
	(void) force;
#endif
	int error = cd_counts_verify(zfsvfs->z_os, roots);
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	if (error == 0) {
		cache = cd_cache_get(zfsvfs);
		if (cache != NULL) {
			cache->roots = *roots;
			cache->root_state = CD_ROOT_PRESENT;
			cache->valid = B_TRUE;
		}
	}
#endif
	return (error);
}

static void
cd_file_key(char *key, uint64_t object)
{
	(void) snprintf(key, 64, "f:%016llx", (u_longlong_t)object);
}

static void
cd_record_key(char *key, uint64_t object, uint64_t offset)
{
	(void) snprintf(key, 64, "r:%016llx:%016llx",
	    (u_longlong_t)object, (u_longlong_t)offset);
}

static void
cd_ref_key(char *key, uint64_t object)
{
	(void) snprintf(key, 64, "o:%016llx", (u_longlong_t)object);
}

static void
cd_hash_words(const uint8_t *hash, uint64_t *words)
{
	for (uint_t i = 0; i < 4; i++) {
		words[i] = 0;
		for (uint_t j = 0; j < 8; j++)
			words[i] = (words[i] << 8) | hash[8 * i + j];
	}
}

static void
cd_hash_key(char *key, const uint64_t *hash, uint64_t length)
{
	(void) snprintf(key, 96,
	    "%016llx%016llx%016llx%016llx:%016llx",
	    (u_longlong_t)hash[0], (u_longlong_t)hash[1],
	    (u_longlong_t)hash[2], (u_longlong_t)hash[3],
	    (u_longlong_t)length);
}

static void
cd_hash(const void *data, size_t length, uint8_t *hash)
{
	SHA2_CTX ctx;
	uint8_t full[64];

	SHA2Init(SHA512, &ctx);
	SHA2Update(&ctx, data, length);
	SHA2Final(full, &ctx);
	memcpy(hash, full, ZFS_CHUNKDEDUP_HASH_SIZE);
}

static boolean_t
cd_candidate_matches(const cd_ref_t *expected, const cd_ref_t *candidate,
    const void *expected_bytes, const void *candidate_bytes)
{
	return (expected->length == candidate->length &&
	    memcmp(expected->hash, candidate->hash,
	    sizeof (expected->hash)) == 0 &&
	    memcmp(expected_bytes, candidate_bytes, expected->length) == 0);
}

static int
cd_caps(objset_t *os, const cd_roots_t *roots, uint64_t maps_add,
    uint64_t index_add, uint64_t refs_add)
{
	const uint64_t objects[3] = { roots->maps, roots->index, roots->refs };
	const uint64_t additions[3] = { maps_add, index_add, refs_add };
	for (uint_t i = 0; i < 3; i++) {
		uint64_t count;
		int error = zap_count(os, objects[i], &count);
		if (error != 0)
			return (error);
		if (count > ZFS_CHUNKDEDUP_MAX_ENTRIES ||
		    additions[i] > ZFS_CHUNKDEDUP_MAX_ENTRIES - count)
			return (SET_ERROR(E2BIG));
	}
	return (0);
}

static int
cd_roots_lookup(objset_t *os, cd_roots_t *roots)
{
	int error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_CHUNKDEDUP_MASTER,
	    sizeof (uint64_t), CD_ROOT_WORDS, roots);
	if (error == 0 && (roots->version != ZFS_CHUNKDEDUP_VERSION ||
	    roots->maps == 0 || roots->index == 0 || roots->refs == 0))
		return (SET_ERROR(EIO));
	if (error == 0)
		return (cd_caps(os, roots, 0, 0, 0));
	return (error);
}

/* Classifiers/normal reads only; forced and mutating paths use raw lookups. */
static int
cd_roots_read(zfsvfs_t *zfsvfs, cd_roots_t *roots)
{
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	zfs_chunkdedup_cache_t *cache = cd_cache_get(zfsvfs);
	if (cache != NULL && cache->root_state == CD_ROOT_ABSENT)
		return (SET_ERROR(ENOENT));
	if (cache != NULL && cache->root_state == CD_ROOT_PRESENT) {
		*roots = cache->roots;
		return (0);
	}
#endif
	int error = cd_roots_lookup(zfsvfs->z_os, roots);
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	if (cache != NULL) {
		if (error == 0) {
			cache->roots = *roots;
			cache->root_state = CD_ROOT_PRESENT;
		} else if (error == ENOENT) {
			cache->root_state = CD_ROOT_ABSENT;
		}
	}
#endif
	return (error);
}

/* Called only after source validation, under the dataset mutex. */
static int
cd_roots_create(objset_t *os, cd_roots_t *roots)
{
	dmu_tx_t *tx = dmu_tx_create(os);
	int error;

	dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_TRUE, ZFS_CHUNKDEDUP_MASTER);
	for (uint_t i = 0; i < 3; i++)
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_TRUE, NULL);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		return (error);
	}
	roots->version = ZFS_CHUNKDEDUP_VERSION;
	roots->maps = zap_create(os, DMU_OT_ZAP_OTHER, DMU_OT_NONE, 0, tx);
	roots->index = zap_create(os, DMU_OT_ZAP_OTHER, DMU_OT_NONE, 0, tx);
	roots->refs = zap_create(os, DMU_OT_ZAP_OTHER, DMU_OT_NONE, 0, tx);
	VERIFY0(zap_add(os, MASTER_NODE_OBJ, ZFS_CHUNKDEDUP_MASTER,
	    sizeof (uint64_t), CD_ROOT_WORDS, roots, tx));
	dmu_tx_commit(tx);
	return (0);
}

static int
cd_file_lookup(objset_t *os, const cd_roots_t *roots, uint64_t object,
    uint64_t *file)
{
	char key[64];
	cd_file_key(key, object);
	int error = zap_lookup(os, roots->maps, key, sizeof (uint64_t), 2,
	    file);
	if (error == 0 && (file[0] == 0 ||
	    file[0] > ZFS_CHUNKDEDUP_MAX_RECORD ||
	    file[0] % SPA_MINBLOCKSIZE != 0 ||
	    file[1] == 0 || file[1] > ZFS_CHUNKDEDUP_MAX_ENTRIES))
		return (SET_ERROR(EIO));
	return (error);
}

static int
cd_file_read(znode_t *zp, const cd_roots_t *roots, uint64_t *file)
{
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	zfs_chunkdedup_cache_t *cache = cd_cache_get(ZTOZSB(zp));
	if (cache != NULL && cache->root_state == CD_ROOT_PRESENT &&
	    memcmp(&cache->roots, roots, sizeof (*roots)) == 0) {
		cd_cache_entry_t *entry = cd_cache_find(cache, CD_CACHE_FILE,
		    zp, 0, 0);
		if (entry != NULL) {
			if (entry->error == 0)
				memcpy(file, entry->data, 2 * sizeof (uint64_t));
			return (entry->error);
		}
	}
#endif
	int error = cd_file_lookup(ZTOZSB(zp)->z_os, roots, zp->z_id, file);
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	cd_cache_store(ZTOZSB(zp), CD_CACHE_FILE, zp, 0, 0, error,
	    file, 2, NULL);
#endif
	return (error);
}

static int
cd_recipe_lookup(objset_t *os, const cd_roots_t *roots, uint64_t object,
    uint64_t offset, uint64_t *recipe, uint_t *nwords)
{
	char key[64];
	uint64_t integers, count;
	cd_record_key(key, object, offset);
	int error = zap_length(os, roots->maps, key, &integers, &count);
	if (error != 0)
		return (error);
	if (integers != sizeof (uint64_t) || count == 0 ||
	    count > CD_RECIPE_WORDS || count % 3 != 0)
		return (SET_ERROR(EIO));
	*nwords = count;
	return (zap_lookup(os, roots->maps, key, sizeof (uint64_t), count,
	    recipe));
}

static int
cd_ref_lookup(objset_t *os, const cd_roots_t *roots, uint64_t object,
    cd_ref_t *ref)
{
	char key[64];
	cd_ref_key(key, object);
	int error = zap_lookup(os, roots->refs, key, sizeof (uint64_t),
	    CD_REF_WORDS, ref);
	if (error != 0)
		return (error == ENOENT ? SET_ERROR(EIO) : error);
	if (ref->length == 0 || ref->length > ZFS_CHUNKDEDUP_MAX_CHUNK ||
	    ref->count == 0)
		return (SET_ERROR(EIO));
	return (0);
}

static int
cd_candidates(objset_t *os, const cd_roots_t *roots, const cd_ref_t *ref,
    uint64_t *objects, uint_t *count)
{
	char key[96];
	uint64_t integers, n;
	cd_hash_key(key, ref->hash, ref->length);
	int error = zap_length(os, roots->index, key, &integers, &n);
	if (error != 0)
		return (error);
	if (integers != sizeof (uint64_t) || n == 0 || n > CD_COLLISIONS)
		return (SET_ERROR(EIO));
	*count = n;
	error = zap_lookup(os, roots->index, key, sizeof (uint64_t), n,
	    objects);
	if (error != 0)
		return (error);
	for (uint_t i = 0; i < n; i++) {
		if (objects[i] == 0)
			return (SET_ERROR(EIO));
		for (uint_t j = 0; j < i; j++)
			if (objects[i] == objects[j])
				return (SET_ERROR(EIO));
	}
	return (0);
}

/* A corrupted recipe is rejected before looking up or freeing chunk data. */
static int
cd_recipe_validate_lengths(objset_t *os, const cd_roots_t *roots,
    const uint64_t *recipe, uint_t nwords, uint64_t record_length,
    uint64_t *lengths)
{
	uint64_t total = 0;
	for (uint_t i = 0; i < nwords; i += 3) {
		cd_ref_t ref;
		int error = cd_ref_lookup(os, roots, recipe[i], &ref);
		if (error != 0)
			return (error);
		if (recipe[i + 2] == 0 || recipe[i + 1] > ref.length ||
		    recipe[i + 2] > ref.length - recipe[i + 1] ||
		    total > record_length ||
		    recipe[i + 2] > record_length - total)
			return (SET_ERROR(EIO));
		if (lengths != NULL)
			lengths[i / 3] = ref.length;
		total += recipe[i + 2];
	}
	return (total == record_length ? 0 : SET_ERROR(EIO));
}

static int
cd_recipe_validate(objset_t *os, const cd_roots_t *roots,
    const uint64_t *recipe, uint_t nwords, uint64_t record_length)
{
	return (cd_recipe_validate_lengths(os, roots, recipe, nwords,
	    record_length, NULL));
}

/* No cache use/fill during intermediate hydration transactions. */
static int
cd_recipe_read(znode_t *zp, const cd_roots_t *roots, uint64_t start,
    uint64_t record_length, uint64_t *recipe, uint_t *words,
    uint64_t *chunk_lengths, boolean_t cache_allowed)
{
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	zfs_chunkdedup_cache_t *cache = cache_allowed ?
	    cd_cache_get(ZTOZSB(zp)) : NULL;
	if (cache != NULL && cache->valid &&
	    cache->root_state == CD_ROOT_PRESENT &&
	    memcmp(&cache->roots, roots, sizeof (*roots)) == 0) {
		cd_cache_entry_t *entry = cd_cache_find(cache, CD_CACHE_RECORD,
		    zp, start, record_length);
		if (entry != NULL) {
			*words = entry->words;
			memcpy(recipe, entry->data, *words * sizeof (uint64_t));
			memcpy(chunk_lengths, entry->data + *words,
			    (*words / 3) * sizeof (uint64_t));
			return (0);
		}
	}
#else
	(void) cache_allowed;
#endif
	int error = cd_recipe_lookup(ZTOZSB(zp)->z_os, roots, zp->z_id,
	    start, recipe, words);
	if (error == 0)
		error = cd_recipe_validate_lengths(ZTOZSB(zp)->z_os, roots,
		    recipe, *words, record_length, chunk_lengths);
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	if (cache_allowed && error == 0)
		cd_cache_store(ZTOZSB(zp), CD_CACHE_RECORD, zp, start,
		    record_length, 0, recipe, *words, chunk_lengths);
#endif
	return (error);
}

/* Only one cd_read_impl call owns this hold, under the metadata mutex. */
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
typedef struct cd_chunk_hold {
	dmu_buf_t *db;
	uint64_t object;
} cd_chunk_hold_t;

static const char cd_chunk_tag;
#else
typedef int cd_chunk_hold_t;
#endif

static void
cd_chunk_rele(cd_chunk_hold_t *hold)
{
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	if (hold->db != NULL) {
		dmu_buf_rele(hold->db, &cd_chunk_tag);
		hold->db = NULL;
		hold->object = 0;
	}
#else
	(void) hold;
#endif
}

/* Called under the metadata mutex, which also prevents chunk reclamation. */
static int
cd_chunk_read(zfsvfs_t *zfsvfs, uint64_t object, uint64_t chunk_length,
    uint64_t offset, size_t length, void *buffer, zfs_uio_t *uio,
    cd_chunk_hold_t *hold)
{
	ASSERT(MUTEX_HELD(&zfsvfs->z_chunkdedup_lock));
	if (offset > chunk_length || length > chunk_length - offset)
		return (SET_ERROR(EIO));
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	/* Immutable chunks occupy exactly one sector-aligned native block. */
	if (hold->db != NULL && hold->object != object)
		cd_chunk_rele(hold);
	if (hold->db == NULL) {
		dmu_buf_t *db = NULL;
		int error = dmu_buf_hold(zfsvfs->z_os, object, 0,
		    &cd_chunk_tag, &db,
		    DMU_READ_NO_PREFETCH | DMU_READ_LINEAR_ABD);
		if (error != 0)
			return (error);
		hold->db = db;
		hold->object = object;
	}
	dmu_buf_t *db = hold->db;
	if (db == NULL || db->db_object != object || db->db_offset != 0 ||
	    db->db_size < chunk_length ||
	    db->db_size > ZFS_CHUNKDEDUP_MAX_CHUNK || db->db_data == NULL)
		return (SET_ERROR(EIO));
	if (uio != NULL)
		return (zfs_uio_fault_move((uint8_t *)db->db_data + offset,
		    length, UIO_READ, uio));
	memcpy(buffer, (uint8_t *)db->db_data + offset, length);
	return (0);
#else
	(void) hold;
	ASSERT(uio == NULL);
	return (dmu_read(zfsvfs->z_os, object, offset, length, buffer,
	    DMU_READ_PREFETCH));
#endif
}

/* Native file portions retain their logical file offset in either sink. */
static int
cd_native_read(znode_t *zp, uint64_t offset, size_t length, void *buffer,
    zfs_uio_t *uio)
{
#if defined(__FreeBSD__) && defined(_KERNEL) && \
    defined(ZFS_CHUNKDEDUP_CACHE_SUPPORTED)
	if (uio != NULL) {
		ASSERT3U(zfs_uio_offset(uio), ==, offset);
		return (dmu_read_uio(ZTOZSB(zp)->z_os, zp->z_id, uio, length,
		    DMU_READ_PREFETCH));
	}
#else
	ASSERT(uio == NULL);
#endif
	return (dmu_read(ZTOZSB(zp)->z_os, zp->z_id, offset, length, buffer,
	    DMU_READ_PREFETCH));
}

/* Only the FreeBSD nofault caller may supply a UIO instead of kernel storage. */
static int
cd_read_sink(znode_t *zp, uint64_t offset, size_t length, void *buffer,
    zfs_uio_t *uio, boolean_t validate)
{
	/* Invalid internal ranges must never return partly initialized output. */
	if (offset > zp->z_size || length > zp->z_size - offset)
		return (SET_ERROR(EINVAL));
	if (length == 0)
		return (0);
	objset_t *os = ZTOZSB(zp)->z_os;
	cd_roots_t roots;
	uint64_t file[2];
	int error = validate ? cd_roots_read(ZTOZSB(zp), &roots) :
	    cd_roots_lookup(os, &roots);
	if (error == ENOENT) {
		if (zp->z_pflags & ZFS_CHUNKDEDUP)
			return (SET_ERROR(EIO));
		return (cd_native_read(zp, offset, length, buffer, uio));
	}
	if (error != 0)
		return (error);
	error = validate ? cd_file_read(zp, &roots, file) :
	    cd_file_lookup(os, &roots, zp->z_id, file);
	if (error == ENOENT) {
		if (zp->z_pflags & ZFS_CHUNKDEDUP)
			return (SET_ERROR(EIO));
		return (cd_native_read(zp, offset, length, buffer, uio));
	}
	if (error != 0)
		return (error);
	if (!(zp->z_pflags & ZFS_CHUNKDEDUP))
		return (SET_ERROR(EIO));
	/* An absent recipe may be ordinary only after current count validation. */
	if (validate && (error = cd_counts_check(ZTOZSB(zp), &roots,
	    B_FALSE)) != 0)
		return (error);
#ifdef ZFS_CHUNKDEDUP_CACHE_SUPPORTED
	/* The first consistency scan may have discarded an earlier file entry. */
	if (validate)
		cd_cache_store(ZTOZSB(zp), CD_CACHE_FILE, zp, 0, 0, 0,
		    file, 2, NULL);
#endif
	uint8_t *out = buffer;
	cd_chunk_hold_t hold = { 0 };
	while (length != 0) {
		uint64_t start = ((offset / file[0]) * file[0]);
		uint64_t within = offset - start;
		size_t take = MIN(length, file[0] - within);
		uint64_t recipe[CD_RECIPE_WORDS];
		uint64_t chunk_lengths[ZFS_CHUNKDEDUP_MAX_CHUNKS];
		uint_t words;
		if (start >= zp->z_size) {
			error = SET_ERROR(EIO);
			break;
		}
		uint64_t record_length = MIN(file[0], zp->z_size - start);
		error = cd_recipe_read(zp, &roots, start, record_length,
		    recipe, &words, chunk_lengths, validate);
		if (error == ENOENT) {
			cd_chunk_rele(&hold);
			error = cd_native_read(zp, offset, take, out, uio);
		} else if (error == 0) {
			uint64_t pos = 0, end = within + take;
			for (uint_t i = 0; i < words && pos < end; i += 3) {
				uint64_t next = pos + recipe[i + 2];
				if (next > within) {
					uint64_t begin = MAX(pos, within);
					uint64_t n = MIN(next, end) - begin;
					error = cd_chunk_read(ZTOZSB(zp), recipe[i],
					    chunk_lengths[i / 3],
					    recipe[i + 1] + begin - pos, n,
					    out != NULL ? out + begin - within : NULL,
					    uio, &hold);
					if (error != 0)
						break;
				}
				pos = next;
			}
		}
		if (error != 0)
			break;
		offset += take;
		if (out != NULL)
			out += take;
		length -= take;
	}
	cd_chunk_rele(&hold);
	return (error);
}

static int
cd_read_impl(znode_t *zp, uint64_t offset, size_t length, void *buffer,
    boolean_t validate)
{
	return (cd_read_sink(zp, offset, length, buffer, NULL, validate));
}

static int
cd_read_locked(znode_t *zp, uint64_t offset, size_t length, void *buffer)
{
	return (cd_read_impl(zp, offset, length, buffer, B_TRUE));
}

boolean_t
zfs_chunkdedup_active(znode_t *zp)
{
	/* A set marker always selects logical reads, which check mapping presence. */
	if (zp->z_pflags & ZFS_CHUNKDEDUP)
		return (B_TRUE);
	cd_roots_t roots;
	uint64_t file[2];
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	zfs_chunkdedup_lock(zfsvfs);
	int error = cd_roots_read(zfsvfs, &roots);
	if (error == 0)
		error = cd_file_read(zp, &roots, file);
	zfs_chunkdedup_unlock(zfsvfs);
	/* Fail closed: corruption must never select ordinary backing holes. */
	return ((zp->z_pflags & ZFS_CHUNKDEDUP) != 0 || error != ENOENT);
}

int
zfs_chunkdedup_read_buf(znode_t *zp, uint64_t offset, size_t length,
    void *buffer)
{
	if (offset > zp->z_size || length > zp->z_size - offset)
		return (SET_ERROR(EINVAL));
	if (length == 0)
		return (0);
	/*
	 * The caller holds the file's range lock (and teardown reader), so a
	 * native file cannot be converted while this read is in progress.
	 * Check both the marker and mapping under the metadata mutex, then
	 * release it before native I/O: unrelated scanner reads must not wait
	 * for this file's disk access. Missing/corrupt mappings still take the
	 * fail-closed logical path; chunk reads retain reclamation protection.
	 */
	if (!zfs_chunkdedup_active(zp))
		return (cd_native_read(zp, offset, length, buffer, NULL));

	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	zfs_chunkdedup_lock(zfsvfs);
	int error = cd_read_locked(zp, offset, length, buffer);
	zfs_chunkdedup_unlock(zfsvfs);
	return (error);
}

/*
 * This fast path is limited to the reviewed amd64 physical-page copy path.
 * Held destinations must also pass the explicit direct-map admission.
 * No user fault may recurse through a filesystem while this mutex is held.
 * The outer vn_io_fault wrapper retries EFAULT only after this VOP returns;
 * fault_move also consumes TDP_UIOHELD physical pages on the held retry.
 */
static boolean_t
cd_uio_nofault(zfs_uio_t *uio, size_t length)
{
#if defined(__FreeBSD__) && defined(__amd64__) && defined(_KERNEL) && \
    defined(ZFS_CHUNKDEDUP_CACHE_SUPPORTED)
	if (zfs_uio_rw(uio) != UIO_READ ||
	    zfs_uio_segflg(uio) != UIO_USERSPACE ||
	    zfs_uio_td(uio) != curthread ||
	    (uio->uio_extflg & UIO_DIRECT) != 0 ||
	    (curthread->td_pflags & TDP_NOFAULTING) == 0)
		return (B_FALSE);
	if ((curthread->td_pflags & TDP_UIOHELD) == 0)
		return (B_TRUE);

	/*
	 * The VFS owns this array until the current short UIO returns.
	 * Refuse the amd64 helper's waiting transient-KVA mapping path
	 * while metadata is held; staging uses it only after unlock.
	 */
	if (length > ZFS_CHUNKDEDUP_MAX_RANGE || zfs_uio_iovcnt(uio) != 1 ||
	    GET_UIO_STRUCT(uio)->uio_iov == NULL || curthread->td_ma == NULL ||
	    curthread->td_ma_cnt <= 0 ||
	    (size_t)curthread->td_ma_cnt > howmany(ZFS_CHUNKDEDUP_MAX_RANGE,
	    PAGE_SIZE) + 2 || zfs_uio_resid(uio) < 0 ||
	    length > (size_t)zfs_uio_resid(uio) ||
	    length > zfs_uio_iovlen(uio, 0))
		return (B_FALSE);
	size_t pages = howmany(((uintptr_t)zfs_uio_iovbase(uio, 0) &
	    PAGE_MASK) + length, PAGE_SIZE);
	if (pages > (size_t)curthread->td_ma_cnt)
		return (B_FALSE);
	for (int i = 0; i < curthread->td_ma_cnt; i++) {
		if (curthread->td_ma[i] == NULL ||
		    VM_PAGE_TO_PHYS(curthread->td_ma[i]) >= dmaplimit)
			return (B_FALSE);
	}
	return (B_TRUE);
#else
	(void) uio;
	(void) length;
	return (B_FALSE);
#endif
}

int
zfs_chunkdedup_read(znode_t *zp, zfs_uio_t *uio, size_t length)
{
	if (cd_uio_nofault(uio,
	    MIN(length, ZFS_CHUNKDEDUP_MAX_RANGE))) {
		zfsvfs_t *zfsvfs = ZTOZSB(zp);
		while (length != 0) {
			size_t take = MIN(length, ZFS_CHUNKDEDUP_MAX_RANGE);
			zfs_chunkdedup_lock(zfsvfs);
			int error = cd_read_sink(zp, zfs_uio_offset(uio), take,
			    NULL, uio, B_TRUE);
			zfs_chunkdedup_unlock(zfsvfs);
			if (error != 0)
				return (error);
			length -= take;
		}
		return (0);
	}
	size_t allocation = MIN(length, ZFS_CHUNKDEDUP_MAX_RANGE);
	if (allocation == 0)
		return (0);
	zfs_chunkdedup_stage_t stage;
	int error = zfs_chunkdedup_stage_acquire(ZTOZSB(zp), allocation, &stage);
	if (error != 0)
		return (error);
	void *buffer = stage.buffer;
	while (length != 0) {
		size_t take = MIN(length, allocation);
		error = zfs_chunkdedup_read_buf(zp, zfs_uio_offset(uio), take,
		    buffer);
		if (error != 0)
			break;
#if defined(__FreeBSD__) && defined(_KERNEL)
		error = zfs_uio_fault_move(buffer, take, UIO_READ, uio);
#else
		error = zfs_uiomove(buffer, take, UIO_READ, uio);
#endif
		if (error != 0)
			break;
		length -= take;
	}
	zfs_chunkdedup_stage_release(ZTOZSB(zp), &stage, error == 0);
	return (error);
}

static int
cd_validate_index(objset_t *os, const cd_roots_t *roots,
    uint64_t object, const cd_ref_t *ref)
{
	uint64_t candidates[CD_COLLISIONS];
	uint_t count;
	int error = cd_candidates(os, roots, ref, candidates, &count);
	if (error != 0)
		return (error == ENOENT ? SET_ERROR(EIO) : error);
	for (uint_t i = 0; i < count; i++)
		if (candidates[i] == object)
			return (0);
	return (SET_ERROR(EIO));
}

static int
cd_hold_hydrate(znode_t *zp, uint64_t offset,
    uint64_t length, dmu_tx_t *tx, boolean_t discard)
{
	objset_t *os = ZTOZSB(zp)->z_os;
	cd_roots_t roots;
	uint64_t file[2];
	int error;
	ASSERT(MUTEX_HELD(&ZTOZSB(zp)->z_chunkdedup_lock));
	if (length == 0 || offset >= zp->z_size)
		return (0);
	if (length > UINT64_MAX - offset)
		return (SET_ERROR(EINVAL));
	if (length > DMU_MAX_ACCESS)
		return (SET_ERROR(E2BIG));
	if ((error = cd_roots_lookup(os, &roots)) == ENOENT)
		return ((zp->z_pflags & ZFS_CHUNKDEDUP) ? SET_ERROR(EIO) : 0);
	if (error != 0)
		return (error);
	if ((error = cd_file_lookup(os, &roots, zp->z_id, file)) == ENOENT)
		return ((zp->z_pflags & ZFS_CHUNKDEDUP) ? SET_ERROR(EIO) : 0);
	if (error != 0)
		return (error);
	/* Validate persisted reference counts before any reclamation. */
	if ((error = cd_counts_check(ZTOZSB(zp), &roots, B_TRUE)) != 0)
		return (error);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	char hold_key[96];
	cd_file_key(hold_key, zp->z_id);
	dmu_tx_hold_zap(tx, roots.maps, B_FALSE, hold_key);
	uint64_t end = MIN(zp->z_size, offset + length);
	for (uint64_t start = ((offset / file[0]) * file[0]); start < end;
	    start += file[0]) {
		uint64_t recipe[CD_RECIPE_WORDS];
		uint_t words;
		error = cd_recipe_lookup(os, &roots, zp->z_id, start,
		    recipe, &words);
		if (error == ENOENT)
			continue;
		if (error != 0)
			return (error);
		cd_record_key(hold_key, zp->z_id, start);
		dmu_tx_hold_zap(tx, roots.maps, B_FALSE, hold_key);
		uint64_t n = MIN(file[0], zp->z_size - start);
		if ((error = cd_recipe_validate(os, &roots, recipe, words,
		    n)) != 0)
			return (error);
		for (uint_t i = 0; i < words; i += 3) {
			cd_ref_t ref;
			if ((error = cd_ref_lookup(os, &roots, recipe[i],
			    &ref)) != 0 || (error = cd_validate_index(os,
			    &roots, recipe[i], &ref)) != 0)
				return (error);
			cd_ref_key(hold_key, recipe[i]);
			dmu_tx_hold_zap(tx, roots.refs, B_FALSE, hold_key);
			cd_hash_key(hold_key, ref.hash, ref.length);
			dmu_tx_hold_zap(tx, roots.index, B_FALSE, hold_key);
			dmu_tx_hold_free(tx, recipe[i], 0, DMU_OBJECT_END);
		}
		if (!discard)
			dmu_tx_hold_write(tx, zp->z_id, start, n);
	}
	return (0);
}

int
zfs_chunkdedup_hold_hydrate(znode_t *zp, uint64_t offset,
    uint64_t length, dmu_tx_t *tx)
{
	return (cd_hold_hydrate(zp, offset, length, tx, B_FALSE));
}

static void
cd_release(objset_t *os, const cd_roots_t *roots, uint64_t object,
    dmu_tx_t *tx)
{
	cd_ref_t ref;
	char object_key[64], index_key[96];
	VERIFY0(cd_ref_lookup(os, roots, object, &ref));
	cd_ref_key(object_key, object);
	if (--ref.count != 0) {
		VERIFY0(zap_update(os, roots->refs, object_key,
		    sizeof (uint64_t), CD_REF_WORDS, &ref, tx));
		return;
	}
	uint64_t candidates[CD_COLLISIONS];
	uint_t count;
	VERIFY0(cd_candidates(os, roots, &ref, candidates, &count));
	uint_t index;
	for (index = 0; index < count; index++)
		if (candidates[index] == object)
			break;
	VERIFY3U(index, <, count);
	candidates[index] = candidates[--count];
	cd_hash_key(index_key, ref.hash, ref.length);
	if (count == 0)
		VERIFY0(zap_remove(os, roots->index, index_key, tx));
	else
		VERIFY0(zap_update(os, roots->index, index_key,
		    sizeof (uint64_t), count, candidates, tx));
	VERIFY0(zap_remove(os, roots->refs, object_key, tx));
	VERIFY0(dmu_object_free(os, object, tx));
}

static int
cd_hydrate(znode_t *zp, uint64_t offset, uint64_t length, dmu_tx_t *tx,
    boolean_t discard)
{
	objset_t *os = ZTOZSB(zp)->z_os;
	cd_roots_t roots;
	uint64_t file[2];
	int error;
	ASSERT(MUTEX_HELD(&ZTOZSB(zp)->z_chunkdedup_lock));
	if (length == 0 || offset >= zp->z_size)
		return (0);
	if (length > UINT64_MAX - offset)
		return (SET_ERROR(EINVAL));
	if (length > DMU_MAX_ACCESS)
		return (SET_ERROR(E2BIG));
	if ((error = cd_roots_lookup(os, &roots)) == ENOENT)
		return ((zp->z_pflags & ZFS_CHUNKDEDUP) ? SET_ERROR(EIO) : 0);
	if (error != 0)
		return (error);
	if ((error = cd_file_lookup(os, &roots, zp->z_id, file)) == ENOENT)
		return ((zp->z_pflags & ZFS_CHUNKDEDUP) ? SET_ERROR(EIO) : 0);
	if (error != 0)
		return (error);
	/* The hold phase checked counts; discard that result before mutations. */
	cd_cache_invalidate_locked(ZTOZSB(zp));
	uint64_t end = MIN(zp->z_size, offset + length);
	void *buffer = discard ? NULL : kmem_alloc(file[0], KM_SLEEP);
	for (uint64_t start = ((offset / file[0]) * file[0]); start < end;
	    start += file[0]) {
		uint64_t recipe[CD_RECIPE_WORDS];
		uint_t words;
		error = cd_recipe_lookup(os, &roots, zp->z_id, start,
		    recipe, &words);
		if (error == ENOENT) {
			error = 0;
			continue;
		}
		if (error != 0)
			break;
		uint64_t n = MIN(file[0], zp->z_size - start);
		if (!discard) {
			/* Counts were checked before this TXG. */
			error = cd_read_impl(zp, start, n, buffer, B_FALSE);
			if (error != 0)
				break;
			dmu_write(os, zp->z_id, start, n, buffer, tx, 0);
		}
		for (uint_t i = 0; i < words; i += 3)
			cd_release(os, &roots, recipe[i], tx);
		char key[64];
		cd_record_key(key, zp->z_id, start);
		VERIFY0(zap_remove(os, roots.maps, key, tx));
		VERIFY3U(file[1], >, 0);
		file[1]--;
	}
	char key[64];
	cd_file_key(key, zp->z_id);
	if (file[1] == 0) {
		VERIFY0(zap_remove(os, roots.maps, key, tx));
		zp->z_pflags &= ~ZFS_CHUNKDEDUP;
		VERIFY0(sa_update(zp->z_sa_hdl, SA_ZPL_FLAGS(ZTOZSB(zp)),
		    &zp->z_pflags, sizeof (zp->z_pflags), tx));
	} else
		VERIFY0(zap_update(os, roots.maps, key, sizeof (uint64_t),
		    2, file, tx));
	if (buffer != NULL)
		kmem_free(buffer, file[0]);
	return (error);
}

int
zfs_chunkdedup_hydrate(znode_t *zp, uint64_t offset, uint64_t length,
    dmu_tx_t *tx)
{
	return (cd_hydrate(zp, offset, length, tx, B_FALSE));
}

static int
cd_restore(znode_t *zp, boolean_t discard)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error = 0;
	cd_roots_t roots;
	uint64_t file[2];
	zfs_chunkdedup_lock(zfsvfs);
	error = cd_roots_lookup(zfsvfs->z_os, &roots);
	if (error == 0)
		error = cd_file_lookup(zfsvfs->z_os, &roots, zp->z_id, file);
	if (error != 0) {
		zfs_chunkdedup_unlock(zfsvfs);
		return (error == ENOENT && !(zp->z_pflags & ZFS_CHUNKDEDUP) ?
		    0 : (error == ENOENT ? SET_ERROR(EIO) : error));
	}
	if ((error = cd_counts_check(zfsvfs, &roots, B_TRUE)) != 0) {
		zfs_chunkdedup_unlock(zfsvfs);
		return (error);
	}
	uint64_t *offsets;
	uint_t n;
	error = cd_file_offsets(zp, &roots, file, &offsets, &n);
	if (error != 0) {
		zfs_chunkdedup_unlock(zfsvfs);
		return (error);
	}
	for (uint_t i = 0; i < n; ) {
		uint64_t start = offsets[i];
		uint64_t length = MIN(ZFS_CHUNKDEDUP_MAX_RANGE,
		    zp->z_size - start);
		dmu_tx_t *tx = dmu_tx_create(zfsvfs->z_os);
		if (discard)
			dmu_tx_mark_netfree(tx);
		error = cd_hold_hydrate(zp, start, length, tx, discard);
		if (error == 0)
			error = dmu_tx_assign(tx, DMU_TX_WAIT);
		if (error != 0) {
			dmu_tx_abort(tx);
			break;
		}
		error = cd_hydrate(zp, start, length, tx, discard);
		dmu_tx_commit(tx);
		if (error != 0)
			break;
		do {
			i++;
		} while (i < n && offsets[i] < start + length);
	}
	kmem_free(offsets, file[1] * sizeof (uint64_t));
	zfs_chunkdedup_unlock(zfsvfs);
	return (error);
}

int
zfs_chunkdedup_unoptimize(znode_t *zp)
{
	return (cd_restore(zp, B_FALSE));
}

int
zfs_chunkdedup_purge(znode_t *zp)
{
	return (cd_restore(zp, B_TRUE));
}

int
zfs_chunkdedup_optimize(znode_t *zp, uint64_t offset, uint64_t length,
    const zfs_chunkdedup_chunk_t *chunks, uint_t count)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	objset_t *os = zfsvfs->z_os;
	cd_roots_t roots;
	uint64_t file[2], objects[ZFS_CHUNKDEDUP_MAX_CHUNKS] = { 0 };
	uint_t aliases[ZFS_CHUNKDEDUP_MAX_CHUNKS];
	boolean_t needed[ZFS_CHUNKDEDUP_MAX_CHUNKS] = { B_FALSE };
	int error = 0;
	uint64_t block = zp->z_blksz;
	if (!spa_feature_is_enabled(dmu_objset_spa(os), SPA_FEATURE_CHUNKDEDUP))
		return (SET_ERROR(ENOTSUP));
	if (count == 0 || count > ZFS_CHUNKDEDUP_MAX_CHUNKS || length == 0 ||
	    length > ZFS_CHUNKDEDUP_MAX_RANGE || block == 0 ||
	    block % SPA_MINBLOCKSIZE != 0 ||
	    block > ZFS_CHUNKDEDUP_MAX_RECORD || offset >= zp->z_size ||
	    length > zp->z_size - offset || offset % block != 0 ||
	    ((offset + length) % block != 0 && offset + length != zp->z_size))
		return (SET_ERROR(EINVAL));
	/* DMU non-power-of-two block sizes require a single block. */
	if (!ISP2(block) && zp->z_size > block)
		return (SET_ERROR(EIO));
	uint64_t source = chunks[0].zc_offset, end = source;
	for (uint_t i = 0; i < count; i++) {
		if (chunks[i].zc_offset != end || chunks[i].zc_length == 0 ||
		    chunks[i].zc_length > ZFS_CHUNKDEDUP_MAX_CHUNK ||
		    end > zp->z_size || chunks[i].zc_length > zp->z_size - end)
			return (SET_ERROR(EINVAL));
		end += chunks[i].zc_length;
		aliases[i] = i;
	}
	for (uint_t i = 0; i < count; i++)
		if (chunks[i].zc_offset >= offset + length ||
		    chunks[i].zc_offset + chunks[i].zc_length <= offset)
			return (SET_ERROR(EINVAL));
	if (source > offset || end < offset + length ||
	    end - source > CD_SOURCE_MAX ||
	    offset - source >= ZFS_CHUNKDEDUP_MAX_CHUNK ||
	    end - (offset + length) >= ZFS_CHUNKDEDUP_MAX_CHUNK)
		return (SET_ERROR(EINVAL));
	size_t allocation = end - source;
	uint8_t *data = kmem_alloc(allocation, KM_SLEEP);
	uint8_t *compare = kmem_alloc(ZFS_CHUNKDEDUP_MAX_CHUNK, KM_SLEEP);
	uint64_t record_count = (length + block - 1) / block;
	uint64_t free_length = record_count * block;
	uint8_t *mapped = kmem_alloc(record_count, KM_SLEEP);
	memset(mapped, 0, record_count);
	zfs_chunkdedup_lock(zfsvfs);
	if ((error = cd_read_locked(zp, source, allocation, data)) != 0)
		goto out;
	for (uint_t i = 0; i < count; i++) {
		uint8_t hash[ZFS_CHUNKDEDUP_HASH_SIZE];
		cd_hash(data + chunks[i].zc_offset - source,
		    chunks[i].zc_length, hash);
		if (memcmp(hash, chunks[i].zc_hash, sizeof (hash)) != 0) {
			error = SET_ERROR(ESTALE);
			goto out;
		}
	}
	error = cd_roots_lookup(os, &roots);
	if (error == ENOENT) {
		cd_cache_invalidate_locked(zfsvfs);
		error = cd_roots_create(os, &roots);
	}
	if (error != 0)
		goto out;
	error = cd_file_lookup(os, &roots, zp->z_id, file);
	if (error == ENOENT) {
		file[0] = block;
		file[1] = 0;
	} else if (error != 0) {
		goto out;
	} else if (file[0] != block) {
		error = SET_ERROR(EIO);
		goto out;
	}
	uint64_t mapped_count = 0, sparse_count = 0;
	for (uint64_t start = offset; start < offset + length; start += block) {
		uint64_t recipe[CD_RECIPE_WORDS];
		uint_t words;
		error = cd_recipe_lookup(os, &roots, zp->z_id, start,
		    recipe, &words);
		if (error == 0) {
			mapped[(start - offset) / block] = 1;
			mapped_count++;
		} else if (error == ENOENT) {
			/* Physical holes must stay ordinary sparse records. */
			blkptr_t bp;
			size_t nbps = 1;
			error = dmu_read_l0_bps(os, zp->z_id, start, block,
			    &bp, &nbps);
			if (error != 0)
				goto out;
			if (nbps != 1) {
				error = SET_ERROR(EIO);
				goto out;
			}
			if (BP_IS_HOLE(&bp)) {
				mapped[(start - offset) / block] = 2;
				sparse_count++;
				continue;
			}
			uint64_t record_end = MIN(start + block, offset +
			    length);
			for (uint_t i = 0; i < count; i++)
				if (chunks[i].zc_offset < record_end &&
				    chunks[i].zc_offset +
				    chunks[i].zc_length > start)
					needed[i] = B_TRUE;
		} else {
			goto out;
		}
	}
	if (mapped_count + sparse_count == record_count) {
		error = mapped_count == record_count ? SET_ERROR(EALREADY) : 0;
		goto out;
	}
	/* A matching identifier is only a candidate; bytes must also match. */
	for (uint_t i = 0; i < count; i++) {
		if (!needed[i])
			continue;
		cd_ref_t ref = { .length = chunks[i].zc_length };
		cd_hash_words(chunks[i].zc_hash, ref.hash);
		uint64_t candidates[CD_COLLISIONS];
		uint_t n = 0;
		error = cd_candidates(os, &roots, &ref, candidates, &n);
		if (error != 0 && error != ENOENT)
			goto out;
		for (uint_t j = 0; j < n; j++) {
			cd_ref_t actual;
			if ((error = cd_ref_lookup(os, &roots, candidates[j],
			    &actual)) != 0)
				goto out;
			if (actual.length != ref.length ||
			    memcmp(actual.hash, ref.hash,
			    sizeof (ref.hash)) != 0) {
				error = SET_ERROR(EIO);
				goto out;
			}
			error = dmu_read(os, candidates[j], 0, ref.length,
			    compare, DMU_READ_PREFETCH);
			if (error != 0)
				goto out;
			if (cd_candidate_matches(&ref, &actual,
			    data + chunks[i].zc_offset - source, compare)) {
				objects[i] = candidates[j];
				break;
			}
		}
		if (objects[i] == 0) {
			uint_t additional = 1;
			for (uint_t j = 0; j < i; j++) {
				if (!needed[j] ||
				    chunks[j].zc_length !=
				    chunks[i].zc_length ||
				    memcmp(chunks[j].zc_hash, chunks[i].zc_hash,
				    ZFS_CHUNKDEDUP_HASH_SIZE) != 0)
					continue;
				if (memcmp(data + chunks[j].zc_offset - source,
				    data + chunks[i].zc_offset - source,
				    chunks[i].zc_length) == 0) {
					aliases[i] = aliases[j];
					objects[i] = objects[j];
					break;
				}
				if (objects[j] == 0 && aliases[j] == j)
					additional++;
			}
			if (aliases[i] == i && n + additional > CD_COLLISIONS) {
				error = SET_ERROR(EOVERFLOW);
				goto out;
			}
		}
	}
	if ((error = cd_counts_check(zfsvfs, &roots, B_TRUE)) != 0)
		goto out;
	uint64_t new_chunks = 0;
	for (uint_t i = 0; i < count; i++)
		if (needed[i] && objects[i] == 0 && aliases[i] == i)
			new_chunks++;
	uint64_t new_maps = record_count - mapped_count - sparse_count;
	if (file[1] == 0)
		new_maps++;
	if ((error = cd_caps(os, &roots, new_maps, new_chunks,
	    new_chunks)) != 0)
		goto out;
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	char hold_key[96];
	cd_file_key(hold_key, zp->z_id);
	dmu_tx_hold_zap(tx, roots.maps, B_TRUE, hold_key);
	for (uint64_t start = offset; start < offset + length; start += block) {
		if (mapped[(start - offset) / block])
			continue;
		cd_record_key(hold_key, zp->z_id, start);
		dmu_tx_hold_zap(tx, roots.maps, B_TRUE, hold_key);
	}
	for (uint_t i = 0; i < count; i++) {
		if (!needed[i])
			continue;
		uint64_t words[4];
		cd_hash_words(chunks[i].zc_hash, words);
		cd_hash_key(hold_key, words, chunks[i].zc_length);
		dmu_tx_hold_zap(tx, roots.index, B_TRUE, hold_key);
		dmu_tx_hold_zap(tx, roots.refs, B_TRUE, NULL);
	}
	dmu_tx_hold_free(tx, zp->z_id, offset, free_length);
	for (uint_t i = 0; i < count; i++) {
		if (needed[i] && objects[i] == 0 && aliases[i] == i)
			dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0,
			    chunks[i].zc_length);
	}
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		goto out;
	}
	cd_cache_invalidate_locked(zfsvfs);
	for (uint_t i = 0; i < count; i++) {
		if (!needed[i] || objects[i] != 0)
			continue;
		if (aliases[i] != i) {
			objects[i] = objects[aliases[i]];
			continue;
		}
		uint_t chunk_block = (chunks[i].zc_length +
		    SPA_MINBLOCKSIZE - 1) / SPA_MINBLOCKSIZE * SPA_MINBLOCKSIZE;
		objects[i] = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS,
		    chunk_block, DMU_OT_NONE, 0, tx);
		dmu_write(os, objects[i], 0, chunks[i].zc_length,
		    data + chunks[i].zc_offset - source, tx, 0);
		cd_ref_t ref = { .length = chunks[i].zc_length, .count = 0 };
		cd_hash_words(chunks[i].zc_hash, ref.hash);
		char key[96];
		uint64_t candidates[CD_COLLISIONS];
		uint_t n = 0;
		int lookup = cd_candidates(os, &roots, &ref, candidates, &n);
		VERIFY(lookup == 0 || lookup == ENOENT);
		VERIFY3U(n, <, CD_COLLISIONS);
		candidates[n++] = objects[i];
		cd_hash_key(key, ref.hash, ref.length);
		VERIFY0(zap_update(os, roots.index, key, sizeof (uint64_t),
		    n, candidates, tx));
		cd_ref_key(key, objects[i]);
		VERIFY0(zap_add(os, roots.refs, key, sizeof (uint64_t),
		    CD_REF_WORDS, &ref, tx));
	}
	for (uint64_t start = offset; start < offset + length; start += block) {
		if (mapped[(start - offset) / block])
			continue;
		uint64_t record_end = MIN(start + block, offset + length);
		uint64_t recipe[CD_RECIPE_WORDS];
		uint_t words = 0;
		for (uint_t i = 0; i < count; i++) {
			uint64_t chunk_end = chunks[i].zc_offset +
			    chunks[i].zc_length;
			if (chunks[i].zc_offset >= record_end || chunk_end <=
			    start)
				continue;
			uint64_t begin = MAX(start, chunks[i].zc_offset);
			recipe[words++] = objects[i];
			recipe[words++] = begin - chunks[i].zc_offset;
			recipe[words++] = MIN(record_end, chunk_end) - begin;
			cd_ref_t ref;
			char key[64];
			cd_ref_key(key, objects[i]);
			VERIFY0(zap_lookup(os, roots.refs, key,
			    sizeof (uint64_t),
			    CD_REF_WORDS, &ref));
			ref.count++;
			VERIFY0(zap_update(os, roots.refs, key,
			    sizeof (uint64_t),
			    CD_REF_WORDS, &ref, tx));
		}
		char key[64];
		cd_record_key(key, zp->z_id, start);
		VERIFY0(zap_add(os, roots.maps, key, sizeof (uint64_t),
		    words, recipe, tx));
		file[1]++;
	}
	char key[64];
	cd_file_key(key, zp->z_id);
	VERIFY0(zap_update(os, roots.maps, key, sizeof (uint64_t), 2, file,
	    tx));
	zp->z_pflags |= ZFS_CHUNKDEDUP;
	VERIFY0(sa_update(zp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs), &zp->z_pflags,
	    sizeof (zp->z_pflags), tx));
	/* Free complete records, including inaccessible bytes past EOF. */
	VERIFY0(dmu_free_range(os, zp->z_id, offset, free_length, tx));
	/* Dataset sync activates the feature in the backing removal TXG. */
	dsl_dataset_t *ds = dmu_objset_ds(os);
	mutex_enter(&ds->ds_lock);
	ds->ds_feature_activation[SPA_FEATURE_CHUNKDEDUP] = (void *)B_TRUE;
	mutex_exit(&ds->ds_lock);
	dmu_tx_commit(tx);
	error = 0;
out:
	zfs_chunkdedup_unlock(zfsvfs);
	kmem_free(mapped, record_count);
	kmem_free(compare, ZFS_CHUNKDEDUP_MAX_CHUNK);
	kmem_free(data, allocation);
	return (error);
}

#ifdef __FreeBSD__
int
zfs_chunkdedup_lookup(zfsvfs_t *zfsvfs,
    const zfs_chunkdedup_chunk_t *chunks, uint_t count, uint64_t *objects,
    uint64_t *asizes)
{
	if (count == 0 || count > ZFS_CHUNKDEDUP_MAX_CHUNKS)
		return (SET_ERROR(EINVAL));
	for (uint_t i = 0; i < count; i++)
		if (chunks[i].zc_length == 0 ||
		    chunks[i].zc_length > ZFS_CHUNKDEDUP_MAX_CHUNK)
			return (SET_ERROR(EINVAL));
	memset(objects, 0, count * sizeof (*objects));
	memset(asizes, 0, count * sizeof (*asizes));
	zfs_chunkdedup_lock(zfsvfs);
	cd_roots_t roots;
	objset_t *os = zfsvfs->z_os;
	int error = cd_roots_read(zfsvfs, &roots);
	if (error == ENOENT) {
		error = 0;
		goto out;
	}
	if (error != 0)
		goto out;
	for (uint_t i = 0; i < count; i++) {
		cd_ref_t wanted = { .length = chunks[i].zc_length };
		uint64_t candidates[CD_COLLISIONS];
		uint_t n = 0;
		cd_hash_words(chunks[i].zc_hash, wanted.hash);
		error = cd_candidates(os, &roots, &wanted, candidates, &n);
		if (error == ENOENT) {
			error = 0;
			continue;
		}
		if (error != 0)
			break;
		/* Candidates are estimates only; installation compares bytes. */
		cd_ref_t actual;
		error = cd_ref_lookup(os, &roots, candidates[0], &actual);
		if (error != 0)
			break;
		if (actual.length != wanted.length ||
		    memcmp(actual.hash, wanted.hash, sizeof (actual.hash)) != 0) {
			error = SET_ERROR(EIO);
			break;
		}
		dmu_object_info_t doi;
		error = dmu_object_info(os, candidates[0], &doi);
		if (error != 0)
			break;
		if (doi.doi_type != DMU_OT_PLAIN_FILE_CONTENTS ||
		    doi.doi_max_offset < actual.length) {
			error = SET_ERROR(EIO);
			break;
		}
		objects[i] = candidates[0];
		asizes[i] = doi.doi_physical_blocks_512 * SPA_MINBLOCKSIZE;
	}
out:
	zfs_chunkdedup_unlock(zfsvfs);
	return (error);
}
#endif

int
zfs_chunkdedup_verify(znode_t *zp)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	objset_t *os = zfsvfs->z_os;
	cd_roots_t roots;
	uint64_t file[2], records = 0;
	int error;
	zfs_chunkdedup_lock(zfsvfs);
	/* Even a failed fresh root lookup must discard previous classifications. */
	cd_cache_invalidate_locked(zfsvfs);
	if ((error = cd_roots_lookup(os, &roots)) == ENOENT)
		goto done;
	if (error != 0)
		goto out;
	if ((error = cd_counts_check(zfsvfs, &roots, B_TRUE)) != 0)
		goto out;
	if ((error = cd_file_lookup(os, &roots, zp->z_id, file)) == ENOENT)
		goto done;
	if (error != 0)
		goto out;
	if (!(zp->z_pflags & ZFS_CHUNKDEDUP)) {
		error = SET_ERROR(EIO);
		goto out;
	}
	uint64_t *offsets;
	uint_t n;
	if ((error = cd_file_offsets(zp, &roots, file, &offsets, &n)) != 0)
		goto out;
	void *buffer = kmem_alloc(ZFS_CHUNKDEDUP_MAX_CHUNK, KM_SLEEP);
	for (uint_t record = 0; record < n; record++) {
		uint64_t start = offsets[record];
		uint64_t recipe[CD_RECIPE_WORDS];
		uint_t words;
		error = cd_recipe_lookup(os, &roots, zp->z_id, start, recipe,
		    &words);
		if (error != 0)
			break;
		records++;
		error = cd_recipe_validate(os, &roots, recipe, words,
		    MIN(file[0], zp->z_size - start));
		if (error != 0)
			break;
		for (uint_t i = 0; i < words; i += 3) {
			cd_ref_t ref;
			if ((error = cd_ref_lookup(os, &roots, recipe[i],
			    &ref)) != 0 ||
			    (error = cd_validate_index(os, &roots, recipe[i],
			    &ref)) != 0 ||
			    (error = dmu_read(os, recipe[i], 0, ref.length,
			    buffer,
			    DMU_READ_PREFETCH)) != 0)
				break;
			uint8_t hash[ZFS_CHUNKDEDUP_HASH_SIZE];
			uint64_t words_hash[4];
			cd_hash(buffer, ref.length, hash);
			cd_hash_words(hash, words_hash);
			if (memcmp(words_hash, ref.hash,
			    sizeof (words_hash)) != 0) {
				error = SET_ERROR(EIO);
				break;
			}
		}
		if (error != 0)
			break;
	}
	kmem_free(buffer, ZFS_CHUNKDEDUP_MAX_CHUNK);
	kmem_free(offsets, file[1] * sizeof (uint64_t));
	if (error == ENOENT)
		error = 0;
	if (error == 0 && records != file[1])
		error = SET_ERROR(EIO);
	goto out;
done:
	error = (zp->z_pflags & ZFS_CHUNKDEDUP) ? SET_ERROR(EIO) : 0;
out:
	zfs_chunkdedup_unlock(zfsvfs);
	return (error);
}

int
zfs_chunkdedup_status(zfsvfs_t *zfsvfs, zfs_chunkdedup_stats_t *stats)
{
	objset_t *os = zfsvfs->z_os;
	cd_roots_t roots;
	zap_cursor_t cursor;
	zap_attribute_t *entry = zap_attribute_alloc();
	int error;
	memset(stats, 0, sizeof (*stats));
	zfs_chunkdedup_lock(zfsvfs);
	if ((error = cd_roots_lookup(os, &roots)) == ENOENT) {
		error = 0;
		goto out;
	}
	if (error != 0)
		goto out;
	uint64_t visits = 0;
	for (zap_cursor_init(&cursor, os, roots.maps);
	    (error = zap_cursor_retrieve(&cursor, entry)) == 0;
	    zap_cursor_advance(&cursor)) {
		if (++visits > ZFS_CHUNKDEDUP_MAX_ENTRIES) {
			error = SET_ERROR(E2BIG);
			break;
		}
		if (entry->za_name[0] == 'f') {
			stats->zcs_files++;
		} else if (entry->za_name[0] == 'r') {
			uint64_t recipe[CD_RECIPE_WORDS];
			if (entry->za_integer_length != sizeof (uint64_t) ||
			    entry->za_num_integers == 0 ||
			    entry->za_num_integers > CD_RECIPE_WORDS ||
			    entry->za_num_integers % 3 != 0) {
				error = SET_ERROR(EIO);
				break;
			}
			error = zap_lookup(os, roots.maps, entry->za_name,
			    sizeof (uint64_t), entry->za_num_integers, recipe);
			if (error != 0)
				break;
			stats->zcs_records++;
			for (uint_t i = 0; i < entry->za_num_integers; i += 3)
				stats->zcs_logical_bytes += recipe[i + 2];
		} else {
			error = SET_ERROR(EIO);
			break;
		}
	}
	zap_cursor_fini(&cursor);
	if (error != ENOENT)
		goto out;
	visits = 0;
	for (zap_cursor_init(&cursor, os, roots.refs);
	    (error = zap_cursor_retrieve(&cursor, entry)) == 0;
	    zap_cursor_advance(&cursor)) {
		if (++visits > ZFS_CHUNKDEDUP_MAX_ENTRIES) {
			error = SET_ERROR(E2BIG);
			break;
		}
		cd_ref_t ref;
		error = zap_lookup(os, roots.refs, entry->za_name,
		    sizeof (uint64_t), CD_REF_WORDS, &ref);
		if (error != 0 || ref.count == 0 || ref.length == 0 ||
		    ref.length > ZFS_CHUNKDEDUP_MAX_CHUNK) {
			error = error != 0 ? error : SET_ERROR(EIO);
			break;
		}
		stats->zcs_chunks++;
		stats->zcs_chunk_bytes += ref.length;
		stats->zcs_references += ref.count;
	}
	zap_cursor_fini(&cursor);
	if (error == ENOENT)
		error = 0;
out:
	zfs_chunkdedup_unlock(zfsvfs);
	zap_attribute_free(entry);
	return (error);
}

static int
cd_count_add(nvlist_t *counts, const char *key)
{
	uint64_t count = 0;
	int error = nvlist_lookup_uint64(counts, key, &count);
	if (error != 0 && error != ENOENT)
		return (error);
	if (count == UINT64_MAX)
		return (SET_ERROR(EIO));
	return (nvlist_add_uint64(counts, key, count + 1));
}

/*
 * Before freeing anything, prove reference counts against every live recipe.
 * Reads can reuse a successful result while the serialized metadata is intact.
 * Snapshots have independent COW maps/counts and are not live references here.
 */
static int
cd_counts_verify(objset_t *os, const cd_roots_t *roots)
{
	nvlist_t *counts;
	zap_cursor_t cursor;
	zap_attribute_t *entry = zap_attribute_alloc();
	int error = cd_caps(os, roots, 0, 0, 0);
	if (error == 0)
		error = nvlist_alloc(&counts, NV_UNIQUE_NAME, KM_SLEEP);
	if (error != 0) {
		zap_attribute_free(entry);
		return (error);
	}
	uint64_t visits = 0;
	for (zap_cursor_init(&cursor, os, roots->maps);
	    (error = zap_cursor_retrieve(&cursor, entry)) == 0;
	    zap_cursor_advance(&cursor)) {
		if (++visits > ZFS_CHUNKDEDUP_MAX_ENTRIES) {
			error = SET_ERROR(E2BIG);
			break;
		}
		if (entry->za_name[0] == 'f')
			continue;
		if (strlen(entry->za_name) != 35 || entry->za_name[0] != 'r' ||
		    entry->za_name[1] != ':' || entry->za_name[18] != ':' ||
		    entry->za_integer_length != sizeof (uint64_t) ||
		    entry->za_num_integers == 0 ||
		    entry->za_num_integers > CD_RECIPE_WORDS ||
		    entry->za_num_integers % 3 != 0) {
			error = SET_ERROR(EIO);
			break;
		}
		char key[64];
		memcpy(key, entry->za_name, 18);
		key[0] = 'f';
		key[18] = '\0';
		uint64_t file[2], recipe[CD_RECIPE_WORDS], total = 0;
		error = zap_lookup(os, roots->maps, key, sizeof (uint64_t), 2,
		    file);
		if (error != 0 || file[0] == 0 ||
		    file[0] % SPA_MINBLOCKSIZE != 0 ||
		    file[0] > ZFS_CHUNKDEDUP_MAX_RECORD || file[1] == 0) {
			error = error != 0 ? error : SET_ERROR(EIO);
			break;
		}
		if ((error = cd_count_add(counts, key)) != 0)
			break;
		error = zap_lookup(os, roots->maps, entry->za_name,
		    sizeof (uint64_t), entry->za_num_integers, recipe);
		if (error != 0)
			break;
		for (uint_t i = 0; i < entry->za_num_integers; i += 3) {
			cd_ref_t ref;
			error = cd_ref_lookup(os, roots, recipe[i], &ref);
			if (error != 0)
				break;
			if (recipe[i + 2] == 0 || recipe[i + 1] > ref.length ||
			    recipe[i + 2] > ref.length - recipe[i + 1] ||
			    total > file[0] || recipe[i + 2] >
			    file[0] - total) {
				error = SET_ERROR(EIO);
				break;
			}
			total += recipe[i + 2];
			cd_ref_key(key, recipe[i]);
			if ((error = cd_count_add(counts, key)) != 0)
				break;
		}
		if (error != 0)
			break;
	}
	zap_cursor_fini(&cursor);
	if (error != ENOENT)
		goto out;
	/* Check declared file counts against their records. */
	visits = 0;
	for (zap_cursor_init(&cursor, os, roots->maps);
	    (error = zap_cursor_retrieve(&cursor, entry)) == 0;
	    zap_cursor_advance(&cursor)) {
		if (++visits > ZFS_CHUNKDEDUP_MAX_ENTRIES) {
			error = SET_ERROR(E2BIG);
			break;
		}
		if (entry->za_name[0] != 'f')
			continue;
		uint64_t file[2], observed;
		error = zap_lookup(os, roots->maps, entry->za_name,
		    sizeof (uint64_t), 2, file);
		if (error == 0)
			error = nvlist_lookup_uint64(counts, entry->za_name,
			    &observed);
		if (error != 0 || observed != file[1]) {
			error = SET_ERROR(EIO);
			break;
		}
	}
	zap_cursor_fini(&cursor);
	if (error != ENOENT)
		goto out;
	visits = 0;
	for (zap_cursor_init(&cursor, os, roots->refs);
	    (error = zap_cursor_retrieve(&cursor, entry)) == 0;
	    zap_cursor_advance(&cursor)) {
		if (++visits > ZFS_CHUNKDEDUP_MAX_ENTRIES) {
			error = SET_ERROR(E2BIG);
			break;
		}
		cd_ref_t ref;
		uint64_t observed;
		error = zap_lookup(os, roots->refs, entry->za_name,
		    sizeof (uint64_t), CD_REF_WORDS, &ref);
		if (error == 0)
			error = nvlist_lookup_uint64(counts, entry->za_name,
			    &observed);
		if (error != 0 || observed != ref.count || ref.count == 0) {
			error = SET_ERROR(EIO);
			break;
		}
		uint64_t candidates[CD_COLLISIONS];
		uint_t n;
		error = cd_candidates(os, roots, &ref, candidates, &n);
		if (error != 0)
			break;
		boolean_t found = B_FALSE;
		for (uint_t i = 0; i < n; i++) {
			char key[64];
			cd_ref_key(key, candidates[i]);
			if (strcmp(key, entry->za_name) == 0)
				found = B_TRUE;
		}
		if (!found) {
			error = SET_ERROR(EIO);
			break;
		}
	}
	zap_cursor_fini(&cursor);
	if (error == ENOENT)
		error = 0;
out:
	nvlist_free(counts);
	zap_attribute_free(entry);
	return (error);
}

#ifdef ZFS_DEBUG
/* Deliberately invalid metadata is confined to an explicitly enabled lab. */
static int zfs_chunkdedup_test_mode = 0;

int
zfs_chunkdedup_test_collision(nvlist_t *result)
{
	if (zfs_chunkdedup_test_mode != 1)
		return (SET_ERROR(EPERM));
	const uint8_t first[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const uint8_t different[8] = { 1, 2, 3, 4, 5, 6, 7, 9 };
	cd_ref_t expected = { .length = sizeof (first), .hash = { 1, 2, 3,
	    4 } };
	cd_ref_t same_identifier = expected;
	cd_ref_t different_identifier = expected;
	different_identifier.hash[0]++;
	boolean_t rejected = !cd_candidate_matches(&expected, &same_identifier,
	    first, different);
	boolean_t accepted = cd_candidate_matches(&expected, &same_identifier,
	    first, first);
	boolean_t wrong_id = !cd_candidate_matches(&expected,
	    &different_identifier,
	    first, first);
	fnvlist_add_boolean_value(result, "different_bytes_rejected", rejected);
	fnvlist_add_boolean_value(result, "matching_bytes_accepted", accepted);
	fnvlist_add_boolean_value(result, "different_identifier_rejected",
	    wrong_id);
	return (rejected && accepted && wrong_id ? 0 : SET_ERROR(EIO));
}

int
zfs_chunkdedup_test_corrupt(znode_t *zp, const char *kind, nvlist_t *token,
    boolean_t restore)
{
	if (zfs_chunkdedup_test_mode != 1)
		return (SET_ERROR(EPERM));
	boolean_t recipe_test = strcmp(kind, "recipe") == 0;
	if (!recipe_test && strcmp(kind, "refcount") != 0)
		return (SET_ERROR(EINVAL));
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	objset_t *os = zfsvfs->z_os;
	cd_roots_t roots;
	uint64_t file[2], offset = 0, recipe[CD_RECIPE_WORDS];
	uint64_t *saved = NULL;
	uint_t words = 0, saved_words = 0;
	cd_ref_t ref;
	char key[96];
	uint64_t target;
	int error;
	zfs_chunkdedup_lock(zfsvfs);
	if (!(zp->z_pflags & ZFS_CHUNKDEDUP) ||
	    (error = cd_roots_lookup(os, &roots)) != 0 ||
	    (error = cd_file_lookup(os, &roots, zp->z_id, file)) != 0) {
		error = SET_ERROR(EINVAL);
		goto out;
	}
	if (restore) {
		uint64_t object;
		if (nvlist_lookup_uint64(token, "object", &object) != 0 ||
		    object != zp->z_id ||
		    nvlist_lookup_uint64(token, "offset", &offset) != 0 ||
		    offset >= zp->z_size || offset % file[0] != 0 ||
		    nvlist_lookup_uint64_array(token, "value", &saved,
		    &saved_words) != 0) {
			error = SET_ERROR(EINVAL);
			goto out;
		}
	} else {
		if ((error = cd_counts_check(zfsvfs, &roots, B_TRUE)) != 0)
			goto out;
		uint64_t *offsets;
		uint_t n;
		error = cd_file_offsets(zp, &roots, file, &offsets, &n);
		if (error != 0)
			goto out;
		offset = offsets[0];
		kmem_free(offsets, file[1] * sizeof (uint64_t));
		error = cd_recipe_lookup(os, &roots, zp->z_id, offset,
		    recipe, &words);
		if (error != 0)
			goto out;
		fnvlist_add_uint64(token, "object", zp->z_id);
		fnvlist_add_uint64(token, "offset", offset);
	}
	if (recipe_test) {
		target = roots.maps;
		cd_record_key(key, zp->z_id, offset);
		if (restore) {
			if (saved_words == 0 || saved_words > CD_RECIPE_WORDS ||
			    saved_words % 3 != 0 ||
			    zap_contains(os, target, key) != ENOENT ||
			    (error = cd_recipe_validate(os, &roots, saved,
			    saved_words,
			    MIN(file[0], zp->z_size - offset))) != 0) {
				error = SET_ERROR(EINVAL);
				goto out;
			}
			for (uint_t i = 0; i < saved_words; i += 3) {
				if ((error = cd_ref_lookup(os, &roots,
				    saved[i], &ref)) != 0 ||
				    (error = cd_validate_index(os, &roots,
				    saved[i], &ref)) != 0)
					goto out;
			}
		} else {
			fnvlist_add_uint64_array(token, "value", recipe, words);
		}
	} else {
		uint64_t object;
		if (restore) {
			if (saved_words != CD_REF_WORDS ||
			    nvlist_lookup_uint64(token, "chunk_object",
			    &object) != 0 ||
			    (error = cd_recipe_lookup(os, &roots, zp->z_id,
			    offset,
			    recipe, &words)) != 0 || recipe[0] != object) {
				error = SET_ERROR(EINVAL);
				goto out;
			}
		} else {
			object = recipe[0];
		}
		if ((error = cd_ref_lookup(os, &roots, object, &ref)) != 0)
			goto out;
		if (restore) {
			cd_ref_t *original = (cd_ref_t *)saved;
			if (original->length != ref.length ||
			    original->count == 0 ||
			    original->count == UINT64_MAX ||
			    ref.count != original->count + 1 ||
			    memcmp(original->hash, ref.hash,
			    sizeof (ref.hash)) != 0) {
				error = SET_ERROR(EINVAL);
				goto out;
			}
		} else {
			if (ref.count == UINT64_MAX) {
				error = SET_ERROR(EOVERFLOW);
				goto out;
			}
			fnvlist_add_uint64(token, "chunk_object", object);
			fnvlist_add_uint64_array(token, "value",
			    (uint64_t *)&ref,
			    CD_REF_WORDS);
			ref.count++;
		}
		target = roots.refs;
		cd_ref_key(key, object);
	}
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, target, restore, key);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		goto out;
	}
	cd_cache_invalidate_locked(zfsvfs);
	if (restore)
		VERIFY0(zap_update(os, target, key, sizeof (uint64_t),
		    saved_words,
		    saved, tx));
	else if (recipe_test)
		VERIFY0(zap_remove(os, target, key, tx));
	else
		VERIFY0(zap_update(os, target, key, sizeof (uint64_t),
		    CD_REF_WORDS,
		    &ref, tx));
	dmu_tx_commit(tx);
	error = restore ? cd_counts_check(zfsvfs, &roots, B_TRUE) : 0;
out:
	zfs_chunkdedup_unlock(zfsvfs);
	return (error);
}

ZFS_MODULE_PARAM(zfs, zfs_, chunkdedup_test_mode, INT, ZMOD_RW,
	"Enable debug chunkdedup fault tests (default 0)");
#endif /* ZFS_DEBUG */

static int
cd_key_offset(const char *text, uint64_t *offset)
{
	if (strlen(text) != 16)
		return (SET_ERROR(EIO));
	*offset = 0;
	for (uint_t i = 0; i < 16; i++) {
		uint_t digit;
		if (text[i] >= '0' && text[i] <= '9')
			digit = text[i] - '0';
		else if (text[i] >= 'a' && text[i] <= 'f')
			digit = text[i] - 'a' + 10;
		else
			return (SET_ERROR(EIO));
		*offset = (*offset << 4) | digit;
	}
	return (0);
}

/* Merge raw native extents with the logical data represented by recipes. */
int
zfs_chunkdedup_seek(znode_t *zp, uint64_t *offset, boolean_t hole)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	objset_t *os = zfsvfs->z_os;
	cd_roots_t roots;
	uint64_t file[2];
	uint64_t original = *offset, position = original;
	int error;
	zfs_chunkdedup_lock(zfsvfs);
	error = cd_roots_lookup(os, &roots);
	if (error == 0)
		error = cd_file_lookup(os, &roots, zp->z_id, file);
	if (error == ENOENT) {
		error = (zp->z_pflags & ZFS_CHUNKDEDUP) ? SET_ERROR(EIO) :
		    dmu_offset_next(os, zp->z_id, hole, offset);
		goto out;
	}
	if (error != 0)
		goto out;
	if (!(zp->z_pflags & ZFS_CHUNKDEDUP) ||
	    (error = cd_counts_check(zfsvfs, &roots, B_FALSE)) != 0) {
		error = SET_ERROR(EIO);
		goto out;
	}
	while (position < zp->z_size) {
		uint64_t start = (position / file[0]) * file[0];
		uint64_t recipe[CD_RECIPE_WORDS];
		uint_t words;
		error = cd_recipe_lookup(os, &roots, zp->z_id, start,
		    recipe, &words);
		if (error == 0) {
			error = cd_recipe_validate(os, &roots, recipe, words,
			    MIN(file[0], zp->z_size - start));
			if (error != 0)
				goto out;
			if (!hole) {
				*offset = position;
				goto out;
			}
			position = MIN(start + file[0], zp->z_size);
			continue;
		}
		if (error != ENOENT)
			goto out;
		uint64_t raw = position;
		error = dmu_offset_next(os, zp->z_id, hole, &raw);
		if (error != 0 && error != ESRCH)
			goto out;
		if (hole) {
			if (error == ESRCH || raw >= zp->z_size) {
				position = zp->z_size;
				break;
			}
			if (raw == position) {
				*offset = raw;
				error = 0;
				goto out;
			}
			position = raw;
			continue;
		}
		/* Find the earliest mapped record after the raw candidate. */
		uint64_t best = error == 0 ? raw : UINT64_MAX;
		char prefix[64];
		cd_record_key(prefix, zp->z_id, 0);
		prefix[19] = '\0';
		zap_cursor_t cursor;
		zap_attribute_t *entry = zap_attribute_alloc();
		uint64_t visits = 0;
		for (zap_cursor_init(&cursor, os, roots.maps);
		    (error = zap_cursor_retrieve(&cursor, entry)) == 0;
		    zap_cursor_advance(&cursor)) {
			if (++visits > ZFS_CHUNKDEDUP_MAX_ENTRIES) {
				error = SET_ERROR(E2BIG);
				break;
			}
			if (strncmp(prefix, entry->za_name, 19) != 0)
				continue;
			uint64_t mapped;
			error = cd_key_offset(entry->za_name + 19, &mapped);
			if (error != 0 || mapped % file[0] != 0 ||
			    mapped >= zp->z_size) {
				error = SET_ERROR(EIO);
				break;
			}
			if (mapped >= original && mapped < best)
				best = mapped;
		}
		zap_cursor_fini(&cursor);
		zap_attribute_free(entry);
		if (error != ENOENT)
			goto out;
		if (best >= zp->z_size) {
			error = SET_ERROR(ESRCH);
			goto out;
		}
		*offset = best;
		error = 0;
		goto out;
	}
	*offset = zp->z_size;
	error = hole ? 0 : SET_ERROR(ESRCH);
out:
	zfs_chunkdedup_unlock(zfsvfs);
	return (error);
}

static int
cd_offset_compare(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;
	return (a < b ? -1 : a > b ? 1 : 0);
}

/* Skip arbitrarily large sparse gaps; work is bounded by metadata entries. */
static int
cd_file_offsets(znode_t *zp, const cd_roots_t *roots, const uint64_t *file,
    uint64_t **result, uint_t *count)
{
	if (file[1] == 0 || file[1] > ZFS_CHUNKDEDUP_MAX_ENTRIES)
		return (SET_ERROR(E2BIG));
	objset_t *os = ZTOZSB(zp)->z_os;
	uint64_t *offsets = kmem_alloc(file[1] * sizeof (uint64_t), KM_SLEEP);
	zap_cursor_t cursor;
	zap_attribute_t *entry = zap_attribute_alloc();
	char prefix[64];
	cd_record_key(prefix, zp->z_id, 0);
	prefix[19] = '\0';
	uint_t n = 0;
	uint64_t visits = 0;
	int error;
	for (zap_cursor_init(&cursor, os, roots->maps);
	    (error = zap_cursor_retrieve(&cursor, entry)) == 0;
	    zap_cursor_advance(&cursor)) {
		if (++visits > ZFS_CHUNKDEDUP_MAX_ENTRIES) {
			error = SET_ERROR(E2BIG);
			break;
		}
		if (strncmp(prefix, entry->za_name, 19) != 0)
			continue;
		uint64_t offset;
		error = cd_key_offset(entry->za_name + 19, &offset);
		if (error != 0 || offset >= zp->z_size ||
		    offset % file[0] != 0 || n >= file[1]) {
			error = SET_ERROR(EIO);
			break;
		}
		offsets[n++] = offset;
	}
	zap_cursor_fini(&cursor);
	zap_attribute_free(entry);
	if (error == ENOENT && n == file[1]) {
		qsort(offsets, n, sizeof (uint64_t), cd_offset_compare);
		*result = offsets;
		*count = n;
		return (0);
	}
	kmem_free(offsets, file[1] * sizeof (uint64_t));
	return (error == ENOENT ? SET_ERROR(EIO) : error);
}
