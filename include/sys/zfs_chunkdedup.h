/* SPDX-License-Identifier: CDDL-1.0 */
/* Experimental dataset-local, post-process content chunk storage. */
#ifndef _SYS_ZFS_CHUNKDEDUP_H
#define	_SYS_ZFS_CHUNKDEDUP_H

#include <sys/types.h>

#define	ZFS_CHUNKDEDUP_VERSION 1
#define	ZFS_CHUNKDEDUP_MAX_RECORD (128U << 10)
#define	ZFS_CHUNKDEDUP_MAX_RANGE (1U << 20)
#define	ZFS_CHUNKDEDUP_MAX_CHUNK (128U << 10)
#define	ZFS_CHUNKDEDUP_MAX_CHUNKS 64
#define	ZFS_CHUNKDEDUP_HASH_SIZE 32
/* Independent hard limit for each maps/index/refs metadata ZAP. */
#define	ZFS_CHUNKDEDUP_MAX_ENTRIES 65536
#define	ZFS_CHUNKDEDUP_MASTER "CHUNKDEDUP"

typedef struct zfs_chunkdedup_chunk {
	uint64_t zc_offset; /* absolute logical file offset */
	uint64_t zc_length;
	uint8_t zc_hash[ZFS_CHUNKDEDUP_HASH_SIZE];
} zfs_chunkdedup_chunk_t;

typedef struct zfs_chunkdedup_stats {
	uint64_t zcs_files;
	uint64_t zcs_records;
	uint64_t zcs_chunks;
	uint64_t zcs_chunk_bytes;
	uint64_t zcs_logical_bytes;
	uint64_t zcs_references;
} zfs_chunkdedup_stats_t;

#ifdef _KERNEL
#include <sys/zfs_znode.h>
#include <sys/zfs_vfsops.h>
#include <sys/dmu.h>

/* Callers retain normal vnode/teardown and file range locks. */
void zfs_chunkdedup_lock(zfsvfs_t *);
void zfs_chunkdedup_unlock(zfsvfs_t *);
/* Resume/rollback/receive resets validation; free before destroying the lock. */
void zfs_chunkdedup_cache_invalidate(zfsvfs_t *);
void zfs_chunkdedup_cache_fini(zfsvfs_t *);
/* Generic scratch ownership only; the buffer has no valid cached contents. */
typedef struct zfs_chunkdedup_stage {
	void *buffer;
	size_t allocation;
	void *owner;
	objset_t *os;
	uint64_t epoch;
} zfs_chunkdedup_stage_t;
/* Caller retains vnode/range/teardown protection through release. */
int zfs_chunkdedup_stage_acquire(zfsvfs_t *, size_t,
    zfs_chunkdedup_stage_t *);
void zfs_chunkdedup_stage_release(zfsvfs_t *, zfs_chunkdedup_stage_t *,
    boolean_t);
boolean_t zfs_chunkdedup_active(znode_t *);
int zfs_chunkdedup_read(znode_t *, zfs_uio_t *, size_t);
int zfs_chunkdedup_read_buf(znode_t *, uint64_t, size_t, void *);
/* Caller holds the whole-file reader range lock. */
int zfs_chunkdedup_seek(znode_t *, uint64_t *, boolean_t);

/* Caller holds chunkdedup_lock continuously through transaction commit. */
int zfs_chunkdedup_hold_hydrate(znode_t *, uint64_t, uint64_t, dmu_tx_t *);
int zfs_chunkdedup_hydrate(znode_t *, uint64_t, uint64_t, dmu_tx_t *);

/*
 * Installation interval must contain complete native records (EOF excepted).
 * The contiguous chunks may extend to either side of that interval, allowing
 * chunk boundaries to cross records and worker batches. Kernel rereads bytes
 * and validates all supplied SHA-512/256-truncation identifiers before commit.
 * Caller drains prior writes BEFORE taking file range/installation locks.
 */
int zfs_chunkdedup_optimize(znode_t *, uint64_t, uint64_t,
    const zfs_chunkdedup_chunk_t *, uint_t);
/* Caller holds the whole-file writer range lock for these operations. */
int zfs_chunkdedup_unoptimize(znode_t *);
int zfs_chunkdedup_purge(znode_t *);
int zfs_chunkdedup_verify(znode_t *);
int zfs_chunkdedup_status(zfsvfs_t *, zfs_chunkdedup_stats_t *);
#ifdef __FreeBSD__
/* Read-only hash/length candidates for the userspace allocation estimate. */
int zfs_chunkdedup_lookup(zfsvfs_t *, const zfs_chunkdedup_chunk_t *, uint_t,
    uint64_t *, uint64_t *);
#endif
#ifdef ZFS_DEBUG
#include <sys/nvpair.h>
int zfs_chunkdedup_test_corrupt(znode_t *, const char *, nvlist_t *, boolean_t);
int zfs_chunkdedup_test_collision(nvlist_t *);
#endif
#endif

#endif /* _SYS_ZFS_CHUNKDEDUP_H */
