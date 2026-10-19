/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * FreeBSD verified background block sharing.
 *
 * The caller supplies stable file identities and candidate ranges. Only the
 * kernel's current byte comparison authorizes a share. Transactions use the
 * existing BRT format: no file layout, feature flag or persistent index is
 * introduced, and ordinary ZFS implementations retain normal access.
 */
#include <sys/zfs_context.h>
#include <sys/zfs_blockdedup.h>

#ifdef __FreeBSD__
#include <sys/zfs_znode.h>
#include <sys/zfs_vnops.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_impl.h>
#include <sys/dmu_tx.h>
#include <sys/dbuf.h>
#include <sys/dsl_dataset.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/brt.h>
#include <sys/zfeature.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/txg.h>
#include <sys/vm.h>
#include <vm/vm.h>
#include <vm/vm_object.h>

static int
bd_dataset_check(zfsvfs_t *zfsvfs, boolean_t mutate)
{
	objset_t *os = zfsvfs->z_os;
	if (dmu_objset_type(os) != DMU_OST_ZFS || os->os_encrypted ||
	    os->os_dedup_checksum != ZIO_CHECKSUM_OFF)
		return (SET_ERROR(ENOTSUP));
	if (mutate && zfs_is_readonly(zfsvfs))
		return (SET_ERROR(EROFS));
	if (mutate && (zfsvfs->z_vfs->vfs_flag & MNT_EXPORTED) != 0)
		return (SET_ERROR(ENOTSUP));
	return (0);
}

/*
 * A combined experimental build must not mistake a non-native file's backing
 * records for its logical contents. This guard has no helper/link dependency;
 * a clean upstream build has neither the marker nor the alternate format.
 * Refuse the entire alternate-format dataset, including malformed recipes.
 */
static int
bd_ordinary(znode_t *zp)
{
#ifdef ZFS_CHUNKDEDUP
	if ((zp->z_pflags & ZFS_CHUNKDEDUP) != 0)
		return (SET_ERROR(ENOTSUP));
	int error = zap_contains(ZTOZSB(zp)->z_os, MASTER_NODE_OBJ,
	    "CHUNKDEDUP");
	if (error != ENOENT)
		return (error == 0 ? SET_ERROR(ENOTSUP) : error);
#else
	(void) zp;
#endif
	return (0);
}

typedef struct bd_identity {
	uint64_t object;
	uint64_t generation;
	uint64_t size;
	uint64_t mtime[2];
} bd_identity_t;

static int
bd_identity_get(nvlist_t *nvl, const char *prefix,
    bd_identity_t *id)
{
	const char *names[] = { "object", "generation", "size", "mtime_sec",
	    "mtime_nsec" };
	uint64_t *values[] = { &id->object, &id->generation, &id->size,
	    &id->mtime[0], &id->mtime[1] };
	for (uint_t i = 0; i < ARRAY_SIZE(names); i++) {
		char key[64];
		(void) snprintf(key, sizeof (key), "%s%s", prefix, names[i]);
		if (nvlist_lookup_uint64(nvl, key, values[i]) != 0)
			return (SET_ERROR(EINVAL));
	}
	return (0);
}

/* Caller holds the vnode lock and filesystem teardown reader. */
static int
bd_identity_check(znode_t *zp, const bd_identity_t *id)
{
	uint64_t mtime[2];
	if (ZTOV(zp)->v_type != VREG || zp->z_unlinked ||
	    zp->z_gen != id->generation || zp->z_size != id->size)
		return (SET_ERROR(ESTALE));
	int error = sa_lookup(zp->z_sa_hdl, SA_ZPL_MTIME(ZTOZSB(zp)), mtime,
	    sizeof (mtime));
	if (error != 0)
		return (error);
	return (memcmp(mtime, id->mtime, sizeof (mtime)) == 0 ? 0 :
	    SET_ERROR(ESTALE));
}

static boolean_t
bd_idle(znode_t *zp)
{
	vm_object_t vmobj = ZTOV(zp)->v_object;
	/* One scanner descriptor and one ioctl vnode reference. */
	return (ZTOV(zp)->v_usecount <= 2 && ZTOV(zp)->v_writecount == 0 &&
	    (vmobj == NULL || (vmobj->ref_count <= 1 &&
	    !vm_object_mightbedirty(vmobj))));
}

static int
bd_bp(znode_t *zp, uint64_t offset, blkptr_t *bp)
{
	uint64_t block = zp->z_blksz;
	if (block == 0 || block > SPA_MAXBLOCKSIZE || offset >= zp->z_size ||
	    offset % block != 0 || (!ISP2(block) && zp->z_size > block))
		return (SET_ERROR(EINVAL));
	size_t count = 1;
	int error = dmu_read_l0_bps(ZTOZSB(zp)->z_os, zp->z_id, offset,
	    MIN(block, zp->z_size - offset), bp, &count);
	if (error == 0 && count != 1)
		error = SET_ERROR(EIO);
	return (error);
}

static void
bd_geometry(objset_t *os, nvlist_t *outnvl)
{
	spa_t *spa = dmu_objset_spa(os);
	uint64_t ashift = SPA_MINBLOCKSHIFT, parity = 0, columns = UINT64_MAX;
	boolean_t supported = B_TRUE, found = B_FALSE;
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	vdev_t *root = spa->spa_root_vdev;
	for (uint64_t i = 0; i < root->vdev_children; i++) {
		vdev_t *vd = root->vdev_child[i];
		if (!vdev_is_concrete(vd) || vd->vdev_islog ||
		    vd->vdev_alloc_bias == VDEV_BIAS_DEDUP)
			continue;
		found = B_TRUE;
		ashift = MAX(ashift, vd->vdev_ashift);
		if (vd->vdev_ops == &vdev_raidz_ops) {
			uint64_t p = vdev_get_nparity(vd);
			parity = MAX(parity, p);
			if (vd->vdev_children <= p)
				supported = B_FALSE;
			else
				columns = MIN(columns, vd->vdev_children - p);
		} else if (vd->vdev_ops != &vdev_mirror_ops &&
		    vd->vdev_ops != &vdev_disk_ops &&
		    vd->vdev_ops != &vdev_file_ops) {
			supported = B_FALSE;
		}
	}
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	fnvlist_add_uint64(outnvl, "ashift", ashift);
	fnvlist_add_uint64(outnvl, "parity", parity);
	fnvlist_add_uint64(outnvl, "data_columns",
	    columns == UINT64_MAX ? 1 : columns);
	fnvlist_add_boolean_value(outnvl, "geometry_supported",
	    supported && found);
	fnvlist_add_uint64(outnvl, "copies", os->os_copies);
	fnvlist_add_uint64(outnvl, "compression", os->os_compress);
}

static int
bd_inspect(zfsvfs_t *zfsvfs, nvlist_t *innvl, nvlist_t *outnvl)
{
	bd_identity_t id;
	znode_t *zp = NULL;
	zfs_locked_range_t *lr = NULL;
	boolean_t entered = B_FALSE, locked = B_FALSE;
	uint64_t offset;
	boolean_t range = nvlist_lookup_uint64(innvl, "offset", &offset) == 0;
	int error = bd_identity_get(innvl, "", &id);
	if (error != 0)
		return (error);
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	entered = B_TRUE;
	if ((error = zfs_zget(zfsvfs, id.object, &zp)) != 0)
		goto out;
	if ((error = bd_dataset_check(zfsvfs, B_FALSE)) != 0)
		goto out;
	/* The initial file query settles writes before per-block inspection. */
	if (!range && !zfs_is_readonly(zfsvfs)) {
		if ((error = zil_commit(zfsvfs->z_log, id.object)) != 0)
			goto out;
		txg_wait_synced(dmu_objset_pool(zfsvfs->z_os), 0);
	}
	zfs_exit(zfsvfs, FTAG);
	entered = B_FALSE;
	if ((error = vn_lock(ZTOV(zp), LK_SHARED | LK_RETRY)) != 0)
		goto out;
	locked = B_TRUE;
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		goto out;
	entered = B_TRUE;
	if ((error = bd_identity_check(zp, &id)) != 0 ||
	    (error = bd_dataset_check(zfsvfs, B_FALSE)) != 0)
		goto out;
	lr = zfs_rangelock_enter(&zp->z_rangelock, 0, UINT64_MAX, RL_READER);
	int ordinary = bd_ordinary(zp);
	if (ordinary != 0) {
		error = ordinary;
		goto out;
	}
	dmu_object_info_t doi;
	if ((error = dmu_object_info(zfsvfs->z_os, zp->z_id, &doi)) != 0)
		goto out;
	fnvlist_add_uint64(outnvl, "block_size", zp->z_blksz);
	fnvlist_add_uint64(outnvl, "allocated_bytes",
	    doi.doi_physical_blocks_512 * SPA_MINBLOCKSIZE);
	fnvlist_add_boolean_value(outnvl, "chunked", B_FALSE);
	fnvlist_add_boolean_value(outnvl, "eligible",
	    bd_idle(zp) && !zfsvfs->z_os->os_encrypted &&
	    !zfs_is_readonly(zfsvfs) &&
	    (zfsvfs->z_vfs->vfs_flag & MNT_EXPORTED) == 0);
	bd_geometry(zfsvfs->z_os, outnvl);
	if (range) {
		blkptr_t bp;
		if ((error = bd_bp(zp, offset, &bp)) != 0)
			goto out;
		boolean_t real = !BP_IS_HOLE(&bp) && !BP_IS_EMBEDDED(&bp);
		fnvlist_add_uint64(outnvl, "block_length",
		    MIN(zp->z_blksz, zp->z_size - offset));
		fnvlist_add_uint64(outnvl, "block_asize", BP_GET_ASIZE(&bp));
		fnvlist_add_boolean_value(outnvl, "block_hole",
		    BP_IS_HOLE(&bp));
		fnvlist_add_boolean_value(outnvl, "block_embedded",
		    BP_IS_EMBEDDED(&bp));
		/* Conservative region hint, including an absent BRT. */
		fnvlist_add_boolean_value(outnvl, "block_shared", real &&
		    (BP_GET_DEDUP(&bp) || brt_maybe_exists(
		    dmu_objset_spa(zfsvfs->z_os), &bp)));
		if (real) {
			uint64_t birth = BP_GET_PHYSICAL_BIRTH(&bp);
			BP_SET_BIRTH(&bp, 0, birth);
			BP_SET_REWRITE(&bp, 0);
		} else {
			BP_ZERO(&bp);
		}
		fnvlist_add_uint8_array(outnvl, "block_identity",
		    (uint8_t *)&bp, sizeof (bp));
	}
out:
	if (lr != NULL)
		zfs_rangelock_exit(lr);
	if (entered)
		zfs_exit(zfsvfs, FTAG);
	if (locked)
		VOP_UNLOCK(ZTOV(zp));
	if (zp != NULL)
		zfs_zrele_async(zp);
	return (error);
}

static int
bd_lock_pair(znode_t *src, znode_t *dst)
{
	if (src == dst) {
		return (vn_lock(ZTOV(dst), LK_EXCLUSIVE | LK_RETRY));
	}
#if (__FreeBSD_version >= 1302506 && __FreeBSD_version < 1400000) || \
	__FreeBSD_version >= 1400086
	vn_lock_pair(ZTOV(src), false, LK_EXCLUSIVE, ZTOV(dst), false,
	    LK_EXCLUSIVE);
#else
	vn_lock_pair(ZTOV(src), false, ZTOV(dst), false);
#endif
	return (0);
}

static void
bd_unlock_pair(znode_t *src, znode_t *dst)
{
	VOP_UNLOCK(ZTOV(dst));
	if (src != dst)
		VOP_UNLOCK(ZTOV(src));
}

/*
 * Metadata-neutral, one-block deduplication. Like upstream afb4010's dedupe
 * path, it neither logs a content change nor refreshes file pages: the bytes
 * are unchanged, and a crash may lose only optimization progress. Existing
 * clone_range and its content-changing ZIL replay remain untouched.
 */
static int
bd_share(zfsvfs_t *zfsvfs, nvlist_t *innvl, nvlist_t *outnvl)
{
	bd_identity_t source, target;
	znode_t *src = NULL, *dst = NULL;
	zfs_locked_range_t *slr = NULL, *dlr = NULL;
	boolean_t entered = B_FALSE, locked = B_FALSE;
	uint64_t inoff, outoff, length;
	uint_t srcseq, dstseq;
	int error = bd_identity_get(innvl, "", &target);
	if (error == 0)
		error = bd_identity_get(innvl, "source_", &source);
	if (error != 0)
		return (error);
	if (nvlist_lookup_uint64(innvl, "source_offset", &inoff) != 0 ||
	    nvlist_lookup_uint64(innvl, "offset", &outoff) != 0 ||
	    nvlist_lookup_uint64(innvl, "length", &length) != 0 ||
	    length == 0 || length > SPA_MAXBLOCKSIZE)
		return (SET_ERROR(EINVAL));
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	entered = B_TRUE;
	if ((error = bd_dataset_check(zfsvfs, B_TRUE)) != 0)
		goto out;
	if ((error = zfs_zget(zfsvfs, target.object, &dst)) != 0)
		goto out;
	if (source.object == target.object)
		src = dst;
	else if ((error = zfs_zget(zfsvfs, source.object, &src)) != 0)
		goto out;
	zfs_exit(zfsvfs, FTAG);
	entered = B_FALSE;
	if ((error = bd_lock_pair(src, dst)) != 0)
		goto out;
	locked = B_TRUE;
	if ((error = zfs_enter_verify_zp(zfsvfs, dst, FTAG)) != 0)
		goto out;
	entered = B_TRUE;
	if ((error = zfs_verify_zp(src)) != 0 ||
	    (error = bd_identity_check(src, &source)) != 0 ||
	    (error = bd_identity_check(dst, &target)) != 0)
		goto out;
	if (!bd_idle(src) || !bd_idle(dst)) {
		error = SET_ERROR(EBUSY);
		goto out;
	}
	if ((error = bd_ordinary(src)) != 0 ||
	    (error = bd_ordinary(dst)) != 0)
		goto out;
	srcseq = src->z_seq;
	dstseq = dst->z_seq;
	/* A clean BP already proves durability; avoid a TXG wait per block. */
	blkptr_t probe;
	boolean_t drain = B_FALSE;
	error = bd_bp(src, inoff, &probe);
	if (error == EAGAIN)
		drain = B_TRUE;
	else if (error != 0)
		goto out;
	error = bd_bp(dst, outoff, &probe);
	if (error == EAGAIN)
		drain = B_TRUE;
	else if (error != 0)
		goto out;
	/* Drain dirty files without vnode, range, or installation locks. */
	zfs_exit(zfsvfs, FTAG);
	entered = B_FALSE;
	bd_unlock_pair(src, dst);
	locked = B_FALSE;
	if (drain) {
		if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
			goto out;
		entered = B_TRUE;
		if ((error = zil_commit(zfsvfs->z_log, source.object)) != 0 ||
		    (src != dst && (error = zil_commit(zfsvfs->z_log,
		    target.object)) != 0))
			goto out;
		txg_wait_synced(dmu_objset_pool(zfsvfs->z_os), 0);
		zfs_exit(zfsvfs, FTAG);
		entered = B_FALSE;
	}
	if ((error = bd_lock_pair(src, dst)) != 0)
		goto out;
	locked = B_TRUE;
	if ((error = zfs_enter_verify_zp(zfsvfs, dst, FTAG)) != 0)
		goto out;
	entered = B_TRUE;
	if ((error = zfs_verify_zp(src)) != 0 ||
	    (error = bd_identity_check(src, &source)) != 0 ||
	    (error = bd_identity_check(dst, &target)) != 0)
		goto out;
	if (src->z_seq != srcseq || dst->z_seq != dstseq) {
		error = SET_ERROR(EAGAIN);
		goto out;
	}
	if (!bd_idle(src) || !bd_idle(dst)) {
		error = SET_ERROR(EBUSY);
		goto out;
	}
	if ((error = bd_dataset_check(zfsvfs, B_TRUE)) != 0)
		goto out;
	if ((error = bd_ordinary(src)) != 0 ||
	    (error = bd_ordinary(dst)) != 0)
		goto out;
	if ((src->z_pflags & ZFS_AV_QUARANTINED) ||
	    (dst->z_pflags & (ZFS_IMMUTABLE | ZFS_APPENDONLY | ZFS_READONLY))) {
		error = SET_ERROR(EPERM);
		goto out;
	}
	uint64_t block = src->z_blksz;
	if (block == 0 || block > SPA_MAXBLOCKSIZE ||
	    block != dst->z_blksz || inoff >= src->z_size ||
	    outoff >= dst->z_size || length > src->z_size - inoff ||
	    length > dst->z_size - outoff || inoff % block != 0 ||
	    outoff % block != 0 || length > block ||
	    (length != block && (length != src->z_size - inoff ||
	    length != dst->z_size - outoff)) ||
	    (!ISP2(block) && (src->z_size > block || dst->z_size > block)) ||
	    (src == dst && inoff < outoff + length &&
	    outoff < inoff + length)) {
		error = SET_ERROR(EINVAL);
		goto out;
	}
	/* Whole-file locks preserve a simple range-lock order. */
	if (src == dst) {
		dlr = zfs_rangelock_enter(&dst->z_rangelock, 0, UINT64_MAX,
		    RL_WRITER);
	} else if (src < dst) {
		slr = zfs_rangelock_enter(&src->z_rangelock, 0, UINT64_MAX,
		    RL_WRITER);
		dlr = zfs_rangelock_enter(&dst->z_rangelock, 0, UINT64_MAX,
		    RL_WRITER);
	} else {
		dlr = zfs_rangelock_enter(&dst->z_rangelock, 0, UINT64_MAX,
		    RL_WRITER);
		slr = zfs_rangelock_enter(&src->z_rangelock, 0, UINT64_MAX,
		    RL_WRITER);
	}
	blkptr_t inbp, outbp;
	if ((error = bd_bp(src, inoff, &inbp)) != 0 ||
	    (error = bd_bp(dst, outoff, &outbp)) != 0)
		goto out;
	if (BP_IS_HOLE(&inbp) || BP_IS_EMBEDDED(&inbp) ||
	    BP_IS_HOLE(&outbp) || BP_IS_EMBEDDED(&outbp) ||
	    BP_GET_DEDUP(&inbp) || BP_GET_DEDUP(&outbp)) {
		error = SET_ERROR(ENOTSUP);
		goto out;
	}
	if (BP_GET_PHYSICAL_BIRTH(&inbp) == BP_GET_PHYSICAL_BIRTH(&outbp) &&
	    DVA_EQUAL(&inbp.blk_dva[0], &outbp.blk_dva[0])) {
		fnvlist_add_uint64(outnvl, "already_shared_bytes", length);
		fnvlist_add_uint64(outnvl, "shared_bytes", 0);
		fnvlist_add_uint64(outnvl, "compared_bytes", 0);
		goto out;
	}
	uint8_t *bytes = vmem_alloc(2 * length, KM_SLEEP);
	error = dmu_read(zfsvfs->z_os, src->z_id, inoff, length, bytes,
	    DMU_READ_PREFETCH);
	if (error == 0)
		error = dmu_read(zfsvfs->z_os, dst->z_id, outoff, length,
		    bytes + length, DMU_READ_PREFETCH);
	if (error == 0 && memcmp(bytes, bytes + length, length) != 0)
		error = SET_ERROR(EILSEQ);
	vmem_free(bytes, 2 * length);
	if (error != 0)
		goto out;
	dmu_tx_t *tx = dmu_tx_create(zfsvfs->z_os);
	dmu_buf_impl_t *db = (dmu_buf_impl_t *)sa_get_db(dst->z_sa_hdl);
	DB_DNODE_ENTER(db);
	dmu_tx_hold_clone_by_dnode(tx, DB_DNODE(db), outoff, length, block);
	DB_DNODE_EXIT(db);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		goto out;
	}
	error = dmu_brt_clone(zfsvfs->z_os, dst->z_id, outoff, length, tx,
	    &inbp, 1);
	dmu_tx_commit(tx);
	if (error == 0) {
		fnvlist_add_uint64(outnvl, "shared_bytes", length);
		fnvlist_add_uint64(outnvl, "already_shared_bytes", 0);
		fnvlist_add_uint64(outnvl, "compared_bytes", length);
	}
out:
	if (dlr != NULL)
		zfs_rangelock_exit(dlr);
	if (slr != NULL)
		zfs_rangelock_exit(slr);
	if (entered)
		zfs_exit(zfsvfs, FTAG);
	if (locked)
		bd_unlock_pair(src, dst);
	if (src != NULL && src != dst)
		zfs_zrele_async(src);
	if (dst != NULL)
		zfs_zrele_async(dst);
	return (error);
}

/* Atime-neutral candidate hashing; sharing rechecks current bytes. */
static int
bd_read(zfsvfs_t *zfsvfs, nvlist_t *innvl, nvlist_t *outnvl)
{
	bd_identity_t id;
	znode_t *zp = NULL;
	zfs_locked_range_t *lr = NULL;
	boolean_t entered = B_FALSE, locked = B_FALSE;
	uint8_t *bytes = NULL;
	uint64_t offset, length;
	int error = bd_identity_get(innvl, "", &id);
	if (error != 0)
		return (error);
	if (nvlist_lookup_uint64(innvl, "offset", &offset) != 0 ||
	    nvlist_lookup_uint64(innvl, "length", &length) != 0 ||
	    length == 0 || length > ZFS_BLOCKDEDUP_MAX_READ)
		return (SET_ERROR(EINVAL));
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	entered = B_TRUE;
	if ((error = zfs_zget(zfsvfs, id.object, &zp)) != 0)
		goto out;
	zfs_exit(zfsvfs, FTAG);
	entered = B_FALSE;
	if ((error = vn_lock(ZTOV(zp), LK_SHARED | LK_RETRY)) != 0)
		goto out;
	locked = B_TRUE;
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		goto out;
	entered = B_TRUE;
	if ((error = bd_identity_check(zp, &id)) != 0 ||
	    (error = bd_dataset_check(zfsvfs, B_FALSE)) != 0 ||
	    (error = bd_ordinary(zp)) != 0)
		goto out;
	if (offset >= zp->z_size || length > zp->z_size - offset) {
		error = SET_ERROR(EINVAL);
		goto out;
	}
	lr = zfs_rangelock_enter(&zp->z_rangelock, offset, length, RL_READER);
	bytes = vmem_alloc(length, KM_SLEEP);
	error = dmu_read(zfsvfs->z_os, zp->z_id, offset, length, bytes,
	    DMU_READ_PREFETCH);
	if (error == 0)
		fnvlist_add_uint8_array(outnvl, "bytes", bytes, length);
out:
	if (bytes != NULL)
		vmem_free(bytes, length);
	if (lr != NULL)
		zfs_rangelock_exit(lr);
	if (entered)
		zfs_exit(zfsvfs, FTAG);
	if (locked)
		VOP_UNLOCK(ZTOV(zp));
	if (zp != NULL)
		zfs_zrele_async(zp);
	return (error);
}
#endif /* __FreeBSD__ */

int
zfs_blockdedup(zfsvfs_t *zfsvfs, nvlist_t *innvl, nvlist_t *outnvl)
{
#ifndef __FreeBSD__
	(void) zfsvfs;
	(void) innvl;
	(void) outnvl;
	return (SET_ERROR(ENOTSUP));
#else
	const char *op;
	if (nvlist_lookup_string(innvl, "op", &op) != 0)
		return (SET_ERROR(EINVAL));
	if (strcmp(op, "inspect") == 0)
		return (bd_inspect(zfsvfs, innvl, outnvl));
	if (strcmp(op, "read") == 0)
		return (bd_read(zfsvfs, innvl, outnvl));
	if (strcmp(op, "share-blocks") != 0)
		return (SET_ERROR(EINVAL));
	if (!zfs_bclone_enabled || !spa_feature_is_enabled(
	    dmu_objset_spa(zfsvfs->z_os), SPA_FEATURE_BLOCK_CLONING))
		return (SET_ERROR(ENOTSUP));
	return (bd_share(zfsvfs, innvl, outnvl));
#endif
}
