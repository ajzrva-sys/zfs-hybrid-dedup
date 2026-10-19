/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef _SYS_ZFS_BLOCKDEDUP_H
#define	_SYS_ZFS_BLOCKDEDUP_H

#define	ZFS_BLOCKDEDUP_MAX_READ (256U << 10)

#ifdef _KERNEL
#include <sys/zfs_vfsops.h>
#include <sys/nvpair.h>

/* Caller holds a mounted-filesystem reference; operation takes file locks. */
int zfs_blockdedup(zfsvfs_t *, nvlist_t *, nvlist_t *);
#endif

#endif /* _SYS_ZFS_BLOCKDEDUP_H */
