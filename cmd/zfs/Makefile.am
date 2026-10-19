# SPDX-License-Identifier: CDDL-1.0
sbin_PROGRAMS   += zfs
CPPCHECKTARGETS += zfs

zfs_SOURCES = \
	%D%/zfs_iter.c \
	%D%/zfs_iter.h \
	%D%/zfs_main.c \
	%D%/zfs_blockdedup.c \
	%D%/zfs_chunkdedup_user.c \
	%D%/chunkdedup_hybrid.c \
	%D%/chunkdedup_hybrid.h \
	%D%/chunkdedup_parallel.h \
	%D%/chunkdedup_chunker.c \
	%D%/chunkdedup_chunker.h \
	%D%/rabin64_table.h \
	%D%/zfs_project.c \
	%D%/zfs_projectutil.h \
	%D%/zfs_util.h

zfs_CFLAGS = $(AM_CFLAGS) -pthread
zfs_LDFLAGS = -pthread

zfs_LDADD = \
	libzfs.la \
	libzfs_core.la \
	libnvpair.la \
	libuutil.la

zfs_LDADD += $(LTLIBINTL)

if BUILD_FREEBSD
zfs_LDADD += -lgeom -ljail
endif

zfs_LDADD += -lcrypto

EXTRA_DIST = %D%/CHUNKDEDUP-MIT-LICENSE
