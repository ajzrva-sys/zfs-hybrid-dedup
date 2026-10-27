/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef _CHUNKDEDUP_CHECKPOINT_H
#define _CHUNKDEDUP_CHECKPOINT_H

/* Qualification builds may override this; normal binaries have no fault path. */
#ifndef CD_RECOVERY_POINT
#define CD_RECOVERY_POINT(name) ((void)0)
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

#define CD_CHECKPOINT_STREAMS 5
#define CD_CHECKPOINT_BINDING_MAX 4096
_Static_assert(sizeof (off_t) >= sizeof (int64_t), "64-bit checkpoint offsets required");

/* This host-local format deliberately includes an ABI/schema binding supplied
 * by the driver. Payloads are append-only; selection must use a separate copy
 * of files.raw. Only a committed manifest authorizes a resumable prefix.
 */
struct cd_checkpoint_hash {
#ifdef __APPLE__
	CC_SHA256_CTX context;
#else
	EVP_MD_CTX *context;
#endif
	bool initialized;
};

struct cd_checkpoint {
	FILE *streams[CD_CHECKPOINT_STREAMS];
	bool loaded, complete, selected;
	uint64_t count, skipped;
	unsigned sorted_mask;
	int lockfd, dirfd;
	/* Internal state: callers must not change committed prefixes or contexts. */
	uint64_t sizes[CD_CHECKPOINT_STREAMS], hashed[CD_CHECKPOINT_STREAMS];
	struct cd_checkpoint_hash hashes[CD_CHECKPOINT_STREAMS];
	unsigned char binding[CD_CHECKPOINT_BINDING_MAX];
	size_t binding_length;
	bool initialized, failed;
	/* Optional immutable completion proof; sharing.state authenticates all bytes. */
	unsigned char *proven;
	size_t proven_size;
	struct stat proven_stat;
	unsigned char proven_hash[32];
	bool proven_checked;
};

struct cd_checkpoint_disk {
	unsigned char magic[16];
	uint32_t version, binding_length;
	uint64_t count, skipped, complete, sorted_mask;
	uint64_t sizes[CD_CHECKPOINT_STREAMS];
	unsigned char hashes[CD_CHECKPOINT_STREAMS][32];
	unsigned char binding[CD_CHECKPOINT_BINDING_MAX];
	unsigned char checksum[32];
};

static const char *const cd_checkpoint_names[CD_CHECKPOINT_STREAMS] = {
	"files.raw", "native.raw", "chunks.raw", "native.sorted", "chunks.sorted"
};
static const unsigned char cd_checkpoint_magic[16] = "CD-CHECKPOINT-1";

static int
cd_checkpoint_hash_init(struct cd_checkpoint_hash *hash)
{
#ifdef __APPLE__
	if (CC_SHA256_Init(&hash->context) != 1)
		return (EIO);
#else
	hash->context = EVP_MD_CTX_new();
	if (hash->context == NULL)
		return (ENOMEM);
	if (EVP_DigestInit_ex(hash->context, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(hash->context);
		hash->context = NULL;
		return (EIO);
	}
#endif
	hash->initialized = true;
	return (0);
}

static int
cd_checkpoint_hash_update(struct cd_checkpoint_hash *hash,
    const void *data, size_t length)
{
#ifdef __APPLE__
	if (length > UINT_MAX ||
	    CC_SHA256_Update(&hash->context, data, (CC_LONG)length) != 1)
		return (EIO);
#else
	if (EVP_DigestUpdate(hash->context, data, length) != 1)
		return (EIO);
#endif
	return (0);
}

static int
cd_checkpoint_hash_final(const struct cd_checkpoint_hash *hash,
    unsigned char result[32])
{
#ifdef __APPLE__
	CC_SHA256_CTX copy = hash->context;
	return (CC_SHA256_Final(result, &copy) == 1 ? 0 : EIO);
#else
	EVP_MD_CTX *copy = EVP_MD_CTX_new();
	if (copy == NULL)
		return (ENOMEM);
	unsigned int length = 0;
	int success = EVP_MD_CTX_copy_ex(copy, hash->context) == 1 &&
	    EVP_DigestFinal_ex(copy, result, &length) == 1 && length == 32;
	EVP_MD_CTX_free(copy);
	return (success ? 0 : EIO);
#endif
}

static int
cd_checkpoint_checksum(const struct cd_checkpoint_disk *disk,
    unsigned char result[32])
{
	struct cd_checkpoint_hash hash = { 0 };
	int error = cd_checkpoint_hash_init(&hash);
	if (error == 0)
		error = cd_checkpoint_hash_update(&hash, disk,
		    offsetof(struct cd_checkpoint_disk, checksum));
	if (error == 0)
		error = cd_checkpoint_hash_final(&hash, result);
#ifndef __APPLE__
	EVP_MD_CTX_free(hash.context);
#endif
	return (error);
}

static int
cd_checkpoint_regular(int fd)
{
	struct stat status;
	if (fstat(fd, &status) != 0)
		return (errno);
	return (S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
	    (status.st_mode & 07777) == 0600 && status.st_nlink == 1 &&
	    status.st_size >= 0 ? 0 : EPERM);
}

static int
cd_checkpoint_read(int fd, void *buffer, size_t length, off_t offset)
{
	unsigned char *bytes = buffer;
	while (length != 0) {
		ssize_t amount = pread(fd, bytes, length, offset);
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0)
			return (amount == 0 ? EIO : errno);
		bytes += amount;
		length -= (size_t)amount;
		offset += amount;
	}
	return (0);
}

static int
cd_checkpoint_hash_to(struct cd_checkpoint *cp, unsigned i, uint64_t size)
{
	unsigned char buffer[65536];
	if (size > INT64_MAX || cp->hashed[i] > size)
		return (EOVERFLOW);
	while (cp->hashed[i] < size) {
		uint64_t remaining = size - cp->hashed[i];
		size_t amount = remaining < sizeof (buffer) ?
		    (size_t)remaining : sizeof (buffer);
		int error = cd_checkpoint_read(fileno(cp->streams[i]), buffer,
		    amount, (off_t)cp->hashed[i]);
		if (error == 0)
			error = cd_checkpoint_hash_update(&cp->hashes[i], buffer, amount);
		if (error != 0)
			return (error);
		cp->hashed[i] += amount;
	}
	return (0);
}

static void
cd_checkpoint_close(struct cd_checkpoint *cp)
{
	if (!cp->initialized) {
		memset(cp, 0, sizeof (*cp));
		cp->lockfd = cp->dirfd = -1;
		return;
	}
	free(cp->proven);
	for (unsigned i = 0; i < CD_CHECKPOINT_STREAMS; i++) {
		if (cp->streams[i] != NULL)
			(void) fclose(cp->streams[i]);
#ifndef __APPLE__
		if (cp->hashes[i].initialized)
			EVP_MD_CTX_free(cp->hashes[i].context);
#endif
	}
	if (cp->lockfd >= 0) {
		(void) flock(cp->lockfd, LOCK_UN);
		(void) close(cp->lockfd);
	}
	if (cp->dirfd >= 0)
		(void) close(cp->dirfd);
	memset(cp, 0, sizeof (*cp));
	cp->lockfd = cp->dirfd = -1;
}

static int
cd_checkpoint_parent_sync(const char *path)
{
	char *parent = strdup(path);
	if (parent == NULL)
		return (ENOMEM);
	size_t length = strlen(parent);
	while (length > 1 && parent[length - 1] == '/')
		parent[--length] = '\0';
	char *slash = strrchr(parent, '/');
	const char *name = ".";
	if (slash != NULL) {
		slash[slash == parent ? 1 : 0] = '\0';
		name = parent;
	}
	int fd = open(name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	int error = fd < 0 ? errno : 0;
	if (error == 0 && fsync(fd) != 0)
		error = errno;
	if (fd >= 0)
		(void) close(fd);
	free(parent);
	return (error);
}

struct cd_checkpoint_report_disk {
	unsigned char magic[16];
	uint64_t version, length;
	unsigned char hash[32];
};
static const unsigned char cd_checkpoint_report_magic[16] = "CD-REPORT-1";

static int
cd_checkpoint_report_hash(const char *path, uint64_t *length,
    unsigned char result[32])
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return (errno);
	struct stat status;
	int error = fstat(fd, &status) == 0 ? 0 : errno;
	/* Publication uses link followed by unlink of the private report name.
	 * A crash between them leaves two legitimate names for the same bytes.
	 * Unlike checkpoint payloads/receipts, report data may therefore be linked;
	 * ownership, private permissions and the recorded digest still must match.
	 */
	if (error == 0 && (!S_ISREG(status.st_mode) ||
	    status.st_uid != geteuid() || (status.st_mode & 07777) != 0600 ||
	    status.st_nlink < 1 || status.st_size < 0))
		error = EPERM;
	struct cd_checkpoint_hash hash = { 0 };
	if (error == 0)
		error = cd_checkpoint_hash_init(&hash);
	uint64_t offset = 0;
	unsigned char buffer[65536];
	while (error == 0 && offset < (uint64_t)status.st_size) {
		uint64_t remaining = (uint64_t)status.st_size - offset;
		size_t amount = remaining < sizeof (buffer) ?
		    (size_t)remaining : sizeof (buffer);
		error = cd_checkpoint_read(fd, buffer, amount, (off_t)offset);
		if (error == 0)
			error = cd_checkpoint_hash_update(&hash, buffer, amount);
		offset += amount;
	}
	if (error == 0) {
		*length = (uint64_t)status.st_size;
		error = cd_checkpoint_hash_final(&hash, result);
	}
#ifndef __APPLE__
	if (hash.initialized)
		EVP_MD_CTX_free(hash.context);
#endif
	(void) close(fd);
	return (error);
}

static int
cd_checkpoint_report_read(struct cd_checkpoint *cp,
    struct cd_checkpoint_report_disk *receipt)
{
	int fd = openat(cp->dirfd, "report.receipt", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return (errno);
	int error = cd_checkpoint_regular(fd);
	struct stat status;
	if (error == 0 && fstat(fd, &status) != 0)
		error = errno;
	if (error == 0 && (uint64_t)status.st_size != sizeof (*receipt))
		error = EILSEQ;
	if (error == 0)
		error = cd_checkpoint_read(fd, receipt, sizeof (*receipt), 0);
	(void) close(fd);
	if (error == 0 && (memcmp(receipt->magic, cd_checkpoint_report_magic, 16) != 0 ||
	    receipt->version != 1 || receipt->length > INT64_MAX))
		error = EILSEQ;
	return (error);
}

/* The caller flushes/fsyncs/closes the private report before saving this
 * receipt, and publishes that same report only after this call succeeds.
 * A receipt alone never makes an absent published report ready.
 */
static inline int
cd_checkpoint_report_save(struct cd_checkpoint *cp, const char *path)
{
	if (!cp->initialized || cp->failed || !cp->complete ||
	    cp->dirfd < 0 || cp->lockfd < 0 || path == NULL)
		return (EINVAL);
	struct cd_checkpoint_report_disk previous;
	int existing = cd_checkpoint_report_read(cp, &previous);
	if (existing != 0 && existing != ENOENT)
		return (existing);
	struct cd_checkpoint_report_disk receipt;
	memset(&receipt, 0, sizeof (receipt));
	memcpy(receipt.magic, cd_checkpoint_report_magic, sizeof (receipt.magic));
	receipt.version = 1;
	int error = cd_checkpoint_report_hash(path, &receipt.length, receipt.hash);
	char temporary[64] = { 0 };
	int fd = -1;
	if (error == 0) {
		for (unsigned attempt = 0; attempt < 100; attempt++) {
			(void) snprintf(temporary, sizeof (temporary),
			    "report.receipt.tmp.%ld.%u", (long)getpid(), attempt);
			fd = openat(cp->dirfd, temporary,
			    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (fd >= 0 || errno != EEXIST)
				break;
		}
		if (fd < 0)
			error = errno;
	}
	if (error == 0) {
		const unsigned char *bytes = (const unsigned char *)&receipt;
		size_t remaining = sizeof (receipt);
		while (remaining != 0) {
			ssize_t amount = write(fd, bytes, remaining);
			if (amount < 0 && errno == EINTR)
				continue;
			if (amount <= 0) {
				error = amount == 0 ? EIO : errno;
				break;
			}
			bytes += amount;
			remaining -= (size_t)amount;
		}
	}
	if (error == 0 && fsync(fd) != 0)
		error = errno;
	if (fd >= 0 && close(fd) != 0 && error == 0)
		error = errno;
	if (error == 0 && renameat(cp->dirfd, temporary,
	    cp->dirfd, "report.receipt") != 0)
		error = errno;
	if (error == 0 && fsync(cp->dirfd) != 0)
		error = errno;
	if (error != 0) {
		if (fd >= 0 && temporary[0] != '\0')
			(void) unlinkat(cp->dirfd, temporary, 0);
		cp->failed = true;
	}
	return (error);
}

static inline int
cd_checkpoint_report_validate(struct cd_checkpoint *cp, const char *path)
{
	if (!cp->initialized || cp->failed || !cp->complete ||
	    cp->dirfd < 0 || cp->lockfd < 0 || path == NULL)
		return (EINVAL);
	struct cd_checkpoint_report_disk receipt;
	int error = cd_checkpoint_report_read(cp, &receipt);
	uint64_t length;
	unsigned char hash[32];
	if (error == 0)
		error = cd_checkpoint_report_hash(path, &length, hash);
	if (error == 0 && (length != receipt.length || memcmp(hash, receipt.hash, 32) != 0))
		error = EILSEQ;
	return (error);
}

/* A failed commit requires close/reopen before further writes/commits. Before
 * rename the previous manifest remains authoritative. A directory-fsync error
 * after rename leaves a valid, fully payload-synced new manifest; recovery may
 * observe either complete prefix. No commit truncates or deletes payloads.
 */
static int
cd_checkpoint_commit(struct cd_checkpoint *cp, uint64_t count,
    uint64_t skipped, bool complete, unsigned sorted_mask)
{
	if (cp->failed || cp->dirfd < 0 || cp->lockfd < 0)
		return (EINVAL);
	struct cd_checkpoint_disk disk;
	memset(&disk, 0, sizeof (disk));
	memcpy(disk.magic, cd_checkpoint_magic, sizeof (disk.magic));
	disk.version = cp->selected ? 2 : 1;
	disk.binding_length = (uint32_t)cp->binding_length;
	memcpy(disk.binding, cp->binding, cp->binding_length);
	disk.count = count;
	disk.skipped = skipped;
	disk.complete = complete;
	disk.sorted_mask = sorted_mask | (cp->selected ? 4U : 0U);
	int error = 0;
	if (count < cp->count || skipped < cp->skipped ||
	    (cp->complete && !complete) || sorted_mask > 3 ||
	    (cp->complete && (count != cp->count || skipped != cp->skipped)) ||
	    (sorted_mask & cp->sorted_mask) != cp->sorted_mask ||
	    (!complete && sorted_mask != 0))
		error = EINVAL;
	for (unsigned i = 0; error == 0 && i < CD_CHECKPOINT_STREAMS; i++) {
		if (cp->streams[i] == NULL) {
			error = EINVAL;
			break;
		}
		if (fflush(cp->streams[i]) != 0 ||
		    fsync(fileno(cp->streams[i])) != 0) {
			error = errno != 0 ? errno : EIO;
			break;
		}
		error = cd_checkpoint_regular(fileno(cp->streams[i]));
		struct stat status;
		if (error == 0 && fstat(fileno(cp->streams[i]), &status) != 0)
			error = errno;
		if (error != 0)
			break;
		uint64_t size = (uint64_t)status.st_size;
		if (size < cp->sizes[i] || (i < 3 && cp->complete &&
		    size != cp->sizes[i]) || (i >= 3 &&
		    (cp->sorted_mask & (1U << (i - 3))) != 0 &&
		    size != cp->sizes[i])) {
			error = ESTALE;
			break;
		}
		if (i >= 3 && (sorted_mask & (1U << (i - 3))) == 0)
			size = 0;
		disk.sizes[i] = size;
		error = cd_checkpoint_hash_to(cp, i, size);
		if (error == 0)
			error = cd_checkpoint_hash_final(&cp->hashes[i], disk.hashes[i]);
	}
	if (error == 0)
		CD_RECOVERY_POINT("payload-synced");
	if (error == 0)
		error = cd_checkpoint_checksum(&disk, disk.checksum);
	char temporary[64] = { 0 };
	int fd = -1;
	if (error == 0) {
		for (unsigned attempt = 0; attempt < 100; attempt++) {
			(void) snprintf(temporary, sizeof (temporary),
			    "manifest.tmp.%ld.%u", (long)getpid(), attempt);
			fd = openat(cp->dirfd, temporary,
			    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (fd >= 0 || errno != EEXIST)
				break;
		}
		if (fd < 0)
			error = errno;
	}
	if (error == 0) {
		const unsigned char *bytes = (const unsigned char *)&disk;
		size_t remaining = sizeof (disk);
		while (remaining != 0) {
			ssize_t amount = write(fd, bytes, remaining);
			if (amount < 0 && errno == EINTR)
				continue;
			if (amount <= 0) {
				error = amount == 0 ? EIO : errno;
				break;
			}
			bytes += amount;
			remaining -= (size_t)amount;
		}
	}
	if (error == 0 && fsync(fd) != 0)
		error = errno;
	if (error == 0)
		CD_RECOVERY_POINT("manifest-synced");
	if (fd >= 0 && close(fd) != 0 && error == 0)
		error = errno;
	if (error == 0 && renameat(cp->dirfd, temporary,
	    cp->dirfd, "manifest") != 0)
		error = errno;
	if (error == 0) {
		CD_RECOVERY_POINT("manifest-renamed");
		cp->count = count;
		cp->skipped = skipped;
		cp->complete = complete;
		cp->sorted_mask = sorted_mask;
		memcpy(cp->sizes, disk.sizes, sizeof (cp->sizes));
		if (fsync(cp->dirfd) != 0)
			error = errno;
		if (error == 0)
			CD_RECOVERY_POINT("directory-synced");
	}
	if (error != 0) {
		/* Remove only the unique temporary created by this attempt. */
		if (fd >= 0 && temporary[0] != '\0')
			(void) unlinkat(cp->dirfd, temporary, 0);
		cp->failed = true;
	}
	return (error);
}

/* The selected plan is immutable. The master marker makes missing selected or
 * sharing state an error, rather than permission to silently restart sharing. */
struct cd_checkpoint_share_progress {
 unsigned char magic[16];
 uint64_t version, generation, selected_size, deferred_bytes;
 unsigned char base_hash[32], selected_hash[32];
 uint64_t next_offset, in_group, canonical_valid, native_done;
 uint64_t canonical[6]; /* dataset,length,file,offset,bytes,object */
 char canonical_hash[65];
 uint64_t groups, issued, deferred, unequal, successful;
 uint64_t shared_bytes, already_shared_bytes, compared_bytes;
 unsigned char checksum[32];
};

/* Host-local, like the checkpoint ABI. Ranges contain only entire hash groups.
 * Their completion provenance is admitted by the external sealer, not inferred
 * from a parser cursor. Missing/changed bytes invalidate sharing.state. */
#define CD_CHECKPOINT_PROVEN_MAX (UINT64_C(64) << 20)
struct cd_checkpoint_proven_header {
 unsigned char magic[16];
 uint64_t version, native_size, boundary, count;
 unsigned char base_hash[32], selected_hash[32], native_hash[32];
};
struct cd_checkpoint_proven_range { uint64_t start, end; };

static inline const struct cd_checkpoint_proven_header *
cd_checkpoint_proven_header(const struct cd_checkpoint *cp)
{
 return ((const struct cd_checkpoint_proven_header *)cp->proven);
}

static inline const struct cd_checkpoint_proven_range *
cd_checkpoint_proven_ranges(const struct cd_checkpoint *cp)
{
 return ((const struct cd_checkpoint_proven_range *)(cp->proven +
     sizeof (struct cd_checkpoint_proven_header)));
}

static inline int
cd_checkpoint_hash_bytes(const void *data, size_t length,
    const void *tail, size_t tail_length, unsigned char digest[32])
{
 struct cd_checkpoint_hash hash = { 0 };
 int error = cd_checkpoint_hash_init(&hash);
 if (error == 0) error = cd_checkpoint_hash_update(&hash, data, length);
 if (error == 0 && tail_length != 0)
  error = cd_checkpoint_hash_update(&hash, tail, tail_length);
 if (error == 0) error = cd_checkpoint_hash_final(&hash, digest);
#ifndef __APPLE__
 EVP_MD_CTX_free(hash.context);
#endif
 return (error);
}

static inline int
cd_checkpoint_base_hash(struct cd_checkpoint *cp, unsigned char digest[32])
{
 struct cd_checkpoint_disk disk = { 0 };
 memcpy(disk.magic, cd_checkpoint_magic, 16);
 disk.version = 1; disk.binding_length = (uint32_t)cp->binding_length;
 disk.count = cp->count; disk.skipped = cp->skipped;
 disk.complete = cp->complete; disk.sorted_mask = cp->sorted_mask;
 memcpy(disk.sizes, cp->sizes, sizeof (disk.sizes));
 memcpy(disk.binding, cp->binding, cp->binding_length);
 int error = 0;
 for (unsigned i = 0; error == 0 && i < CD_CHECKPOINT_STREAMS; i++)
  error = cd_checkpoint_hash_final(&cp->hashes[i], disk.hashes[i]);
 return (error != 0 ? error : cd_checkpoint_checksum(&disk, digest));
}

static inline int
cd_checkpoint_hash_fd(int fd, uint64_t size, unsigned char digest[32])
{
 struct cd_checkpoint_hash hash = { 0 };
 int error = cd_checkpoint_hash_init(&hash);
 unsigned char buffer[65536];
 for (uint64_t at = 0; error == 0 && at < size;) {
  size_t n = size - at < sizeof (buffer) ? (size_t)(size - at) : sizeof (buffer);
  error = cd_checkpoint_read(fd, buffer, n, (off_t)at);
  if (error == 0) error = cd_checkpoint_hash_update(&hash, buffer, n);
  at += n;
 }
 if (error == 0) error = cd_checkpoint_hash_final(&hash, digest);
#ifndef __APPLE__
 EVP_MD_CTX_free(hash.context);
#endif
 return (error);
}

static inline bool
cd_checkpoint_proven_same(const struct stat *a, const struct stat *b)
{
 if (a->st_dev != b->st_dev || a->st_ino != b->st_ino ||
     a->st_size != b->st_size || a->st_uid != b->st_uid ||
     a->st_mode != b->st_mode || a->st_nlink != b->st_nlink ||
     a->st_mtime != b->st_mtime || a->st_ctime != b->st_ctime) return (false);
#ifdef __APPLE__
 return (a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
     a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec);
#else
 return (a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
     a->st_ctim.tv_nsec == b->st_ctim.tv_nsec
#ifdef __FreeBSD__
     && a->st_gen == b->st_gen
#endif
     );
#endif
}

static inline int
cd_checkpoint_proven_check(struct cd_checkpoint *cp,
    const unsigned char base[32], const unsigned char selected[32])
{
 struct stat before, opened, after;
 if (fstatat(cp->dirfd, "sharing.proven", &before, AT_SYMLINK_NOFOLLOW) != 0) {
  if (errno != ENOENT) return (errno);
  if (cp->proven != NULL) return (ESTALE);
  cp->proven_checked = true;
  return (0);
 }
 if (!cp->selected || !cp->complete || cp->sorted_mask != 3) return (ESTALE);
 if (cp->proven_checked && cp->proven == NULL) return (ESTALE);
 if (!S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
     before.st_nlink != 1 || (before.st_mode & 07777) != 0600) return (EPERM);
 if (before.st_size < (off_t)sizeof (struct cd_checkpoint_proven_header) ||
     (uint64_t)before.st_size > CD_CHECKPOINT_PROVEN_MAX) return (EINVAL);
 if (cp->proven != NULL && !cd_checkpoint_proven_same(&before,
     &cp->proven_stat)) return (ESTALE);
 int fd = openat(cp->dirfd, "sharing.proven", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
 if (fd < 0) return (errno);
 int error = fstat(fd, &opened) != 0 ? errno : 0;
 if (error == 0 && !cd_checkpoint_proven_same(&before, &opened)) error = ESTALE;
 unsigned char digest[32];
 if (error == 0) error = cd_checkpoint_hash_fd(fd, (uint64_t)opened.st_size, digest);
 if (error == 0 && cp->proven != NULL && memcmp(digest,
     cp->proven_hash, 32)) error = EILSEQ;
 unsigned char *bytes = NULL;
 if (error == 0 && cp->proven == NULL) {
  bytes = malloc((size_t)opened.st_size);
  if (bytes == NULL) error = ENOMEM;
  else error = cd_checkpoint_read(fd, bytes, (size_t)opened.st_size, 0);
  unsigned char read_hash[32];
  if (error == 0) error = cd_checkpoint_hash_bytes(bytes,
      (size_t)opened.st_size, NULL, 0, read_hash);
  if (error == 0 && memcmp(digest, read_hash, 32)) error = ESTALE;
 }
 if (error == 0 && (fstat(fd, &after) != 0 ||
     !cd_checkpoint_proven_same(&opened, &after))) error = ESTALE;
 (void)close(fd);
 if (error == 0 && (fstatat(cp->dirfd, "sharing.proven", &after,
     AT_SYMLINK_NOFOLLOW) != 0 || !cd_checkpoint_proven_same(&opened,
     &after))) error = ESTALE;
 if (error == 0) {
  const struct cd_checkpoint_proven_header *h =
      (const struct cd_checkpoint_proven_header *)(bytes != NULL ? bytes : cp->proven);
  unsigned char native[32];
  error = cd_checkpoint_hash_final(&cp->hashes[3], native);
  if (error == 0 && (memcmp(h->magic, "CD-PROVEN-1\0\0\0\0\0", 16) ||
      h->version != 1 || h->native_size != cp->sizes[3] ||
      h->boundary == 0 || h->boundary > h->native_size || h->count == 0 ||
      h->count > (CD_CHECKPOINT_PROVEN_MAX - sizeof (*h)) /
      sizeof (struct cd_checkpoint_proven_range) ||
      (uint64_t)opened.st_size != sizeof (*h) + h->count *
      sizeof (struct cd_checkpoint_proven_range) ||
      memcmp(h->base_hash, base, 32) || memcmp(h->selected_hash, selected, 32) ||
      memcmp(h->native_hash, native, 32))) error = EILSEQ;
  const struct cd_checkpoint_proven_range *ranges =
      (const struct cd_checkpoint_proven_range *)((const unsigned char *)h + sizeof (*h));
  uint64_t end = 0;
  for (uint64_t i = 0; error == 0 && i < h->count; i++) {
   if (ranges[i].start < end || ranges[i].start >= ranges[i].end ||
       ranges[i].end > h->boundary) error = EILSEQ;
   end = ranges[i].end;
  }
 }
 if (error == 0 && bytes != NULL) {
  cp->proven = bytes; cp->proven_size = (size_t)opened.st_size;
  cp->proven_stat = opened; memcpy(cp->proven_hash, digest, 32); bytes = NULL;
 }
 free(bytes);
 if (error == 0) cp->proven_checked = true;
 return (error);
}

static inline int
cd_checkpoint_share_checksum(struct cd_checkpoint *cp,
    const struct cd_checkpoint_share_progress *progress,
    const unsigned char *deferred, unsigned char digest[32])
{
 struct cd_checkpoint_hash hash = { 0 };
 int error = cd_checkpoint_hash_init(&hash);
 if (error == 0) error = cd_checkpoint_hash_update(&hash, progress,
     offsetof(struct cd_checkpoint_share_progress, checksum));
 if (error == 0 && cp->count != 0)
  error = cd_checkpoint_hash_update(&hash, deferred, (size_t)cp->count);
 if (error == 0 && cp->proven != NULL)
  error = cd_checkpoint_hash_update(&hash, cp->proven, cp->proven_size);
 if (error == 0) error = cd_checkpoint_hash_final(&hash, digest);
#ifndef __APPLE__
 EVP_MD_CTX_free(hash.context);
#endif
 return (error);
}

static inline int
cd_checkpoint_write_all(int fd, const void *data, size_t length)
{
 const unsigned char *p = data;
 while (length != 0) {
  ssize_t n = write(fd, p, length);
  if (n < 0 && errno == EINTR) continue;
  if (n <= 0) return (n == 0 ? EIO : errno);
  p += n; length -= (size_t)n;
 }
 return (0);
}

static inline int
cd_checkpoint_share_save(struct cd_checkpoint *cp,
    struct cd_checkpoint_share_progress *progress, const unsigned char *deferred)
{
 if (cp->failed || !cp->complete || cp->sorted_mask != 3 ||
     cp->count > SIZE_MAX || progress->selected_size != cp->sizes[0] ||
     (cp->count != 0 && deferred == NULL) || progress->generation == UINT64_MAX ||
     progress->next_offset > cp->sizes[3] || progress->in_group > 1 ||
     progress->canonical_valid > progress->in_group || progress->native_done > 1 ||
     progress->successful > progress->issued || progress->compared_bytes != progress->shared_bytes ||
     (progress->in_group && (memchr(progress->canonical_hash, '\0', 65) == NULL ||
     progress->canonical[1] == 0)) ||
     (progress->canonical_valid && (progress->canonical[2] >= cp->count ||
     progress->canonical[4] != progress->canonical[1])) ||
     (progress->native_done && (progress->next_offset != cp->sizes[3] || progress->in_group)))
  return (EINVAL);
 struct cd_checkpoint_share_progress disk = *progress;
 memcpy(disk.magic, "CD-SHARING-1", 13); disk.version = 1;
 disk.generation++; disk.deferred_bytes = cp->count;
 int error = cd_checkpoint_base_hash(cp, disk.base_hash);
 if (error == 0) error = cd_checkpoint_proven_check(cp, disk.base_hash,
     disk.selected_hash);
 if (error == 0) error = cd_checkpoint_share_checksum(cp, &disk, deferred,
     disk.checksum);
 char temporary[64];
 (void)snprintf(temporary, sizeof (temporary), "sharing.tmp.%ld.%llu",
     (long)getpid(), (unsigned long long)disk.generation);
 int fd = -1;
 if (error == 0) {
  fd = openat(cp->dirfd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) error = errno;
 }
 if (error == 0) error = cd_checkpoint_write_all(fd, &disk, sizeof (disk));
 if (error == 0) error = cd_checkpoint_write_all(fd, deferred, (size_t)cp->count);
 if (error == 0 && fsync(fd) != 0) error = errno;
 if (error == 0) CD_RECOVERY_POINT("share-state-synced");
 if (fd >= 0 && close(fd) != 0 && error == 0) error = errno;
 if (error == 0 && renameat(cp->dirfd, temporary, cp->dirfd, "sharing.state") != 0) error = errno;
 if (error == 0) {
  *progress = disk;
  CD_RECOVERY_POINT("share-state-renamed");
  if (fsync(cp->dirfd) != 0) error = errno;
  if (error == 0) CD_RECOVERY_POINT("share-directory-synced");
 }
 if (error != 0) {
  if (fd >= 0) (void)unlinkat(cp->dirfd, temporary, 0);
  cp->failed = true;
 }
 return (error);
}

/* ENOENT means an old scan/sort-only checkpoint, never missing marked state. */
static inline int
cd_checkpoint_selected_load(struct cd_checkpoint *cp, FILE **selected,
    struct cd_checkpoint_share_progress *progress, unsigned char **deferred)
{
 *selected = NULL; *deferred = NULL;
 if (!cp->selected) {
  struct stat st;
  /* A skip proof must never enter the v1 fallback/staged-pair promotion. */
  if (fstatat(cp->dirfd, "sharing.proven", &st, AT_SYMLINK_NOFOLLOW) == 0)
   return (ESTALE);
  if (errno != ENOENT) return (errno);
  const char *names[] = { "files.selected", "sharing.state" };
  bool exists[2] = { false, false };
  for (unsigned i = 0; i < 2; i++) {
   if (fstatat(cp->dirfd, names[i], &st, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || (st.st_mode & 07777) != 0600) return (EPERM);
    exists[i] = true;
   } else if (errno != ENOENT) return (errno);
  }
  if (!exists[0] && !exists[1]) return (ENOENT);
  if (exists[0] && exists[1]) {
   /* Sharing cannot start until the master marker is durable. Complete only
    * an authenticated initial staged pair; never promote an advanced state. */
   cp->selected = true;
   int error = cd_checkpoint_selected_load(cp, selected, progress, deferred);
   if (error == 0 && (progress->generation != 1 || progress->next_offset ||
       progress->in_group || progress->native_done || progress->groups ||
       progress->issued || progress->deferred || progress->unequal ||
       progress->successful || progress->shared_bytes ||
       progress->already_shared_bytes || progress->compared_bytes)) error = ESTALE;
   for (uint64_t i = 0; error == 0 && i < cp->count; i++)
    if ((*deferred)[i]) error = ESTALE;
   if (error == 0)
    error = cd_checkpoint_commit(cp, cp->count, cp->skipped, true, 3);
   if (error != 0) {
    if (*selected != NULL) (void)fclose(*selected);
    free(*deferred); *selected = NULL; *deferred = NULL;
    cp->failed = true;
   }
   return (error);
  }
  /* Interrupted before both initial payloads existed: retain the uncommitted
   * artifact and use the still-authoritative old scan/sort checkpoint. */
  char archive[80];
  (void)snprintf(archive, sizeof (archive), "uncommitted-selected.%ld", (long)getpid());
  if (mkdirat(cp->dirfd, archive, 0700) != 0) return (errno);
  int archivefd = openat(cp->dirfd, archive, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (archivefd < 0) return (errno);
  int error = 0;
  for (unsigned i = 0; error == 0 && i < 2; i++)
   if (exists[i] && renameat(cp->dirfd, names[i], archivefd, names[i]) != 0) error = errno;
  if (error == 0 && fsync(archivefd) != 0) error = errno;
  if (close(archivefd) != 0 && error == 0) error = errno;
  if (error == 0 && fsync(cp->dirfd) != 0) error = errno;
  if (error != 0) { cp->failed = true; return (error); }
  return (ENOENT);
 }
 int fd = openat(cp->dirfd, "sharing.state", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
 int error = fd < 0 ? (errno == ENOENT ? ESTALE : errno) : cd_checkpoint_regular(fd);
 struct stat st;
 struct cd_checkpoint_share_progress disk;
 if (error == 0 && fstat(fd, &st) != 0) error = errno;
 if (error == 0 && (cp->count > SIZE_MAX || cp->count > INT64_MAX - sizeof (disk) ||
     st.st_size != (off_t)(sizeof (disk) + cp->count))) error = EINVAL;
 if (error == 0) error = cd_checkpoint_read(fd, &disk, sizeof (disk), 0);
 unsigned char *bits = NULL, base[32], digest[32];
 if (error == 0) {
  bits = malloc(cp->count == 0 ? 1 : (size_t)cp->count);
  if (bits == NULL) error = ENOMEM;
 }
 if (error == 0) error = cd_checkpoint_read(fd, bits, (size_t)cp->count, sizeof (disk));
 if (fd >= 0) (void)close(fd);
 if (error == 0) error = cd_checkpoint_base_hash(cp, base);
 if (error == 0) error = cd_checkpoint_proven_check(cp, base, disk.selected_hash);
 if (error == 0) error = cd_checkpoint_share_checksum(cp, &disk, bits, digest);
 if (error == 0 && (memcmp(disk.magic, "CD-SHARING-1", 13) || disk.version != 1 ||
     disk.generation == 0 || disk.selected_size != cp->sizes[0] || disk.deferred_bytes != cp->count ||
     memcmp(disk.base_hash, base, 32) || memcmp(disk.checksum, digest, 32) ||
     disk.next_offset > cp->sizes[3] || disk.in_group > 1 || disk.canonical_valid > disk.in_group ||
     disk.successful > disk.issued || disk.compared_bytes != disk.shared_bytes ||
     (disk.in_group && (memchr(disk.canonical_hash, '\0', 65) == NULL || disk.canonical[1] == 0)) ||
     (disk.canonical_valid && (disk.canonical[2] >= cp->count || disk.canonical[4] != disk.canonical[1])) ||
     disk.native_done > 1 || (disk.native_done && (disk.next_offset != cp->sizes[3] || disk.in_group)))) error = EILSEQ;
 for (uint64_t i = 0; error == 0 && i < cp->count; i++) if (bits[i] > 1) error = EILSEQ;
 fd = -1;
 if (error == 0) {
  fd = openat(cp->dirfd, "files.selected", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  error = fd < 0 ? (errno == ENOENT ? ESTALE : errno) : cd_checkpoint_regular(fd);
 }
 if (error == 0 && fstat(fd, &st) != 0) error = errno;
 if (error == 0 && (uint64_t)st.st_size != disk.selected_size) error = EILSEQ;
 if (error == 0) error = cd_checkpoint_hash_fd(fd, disk.selected_size, digest);
 if (error == 0 && memcmp(digest, disk.selected_hash, 32)) error = EILSEQ;
 if (error == 0) {
  *selected = fdopen(fd, "r");
  if (*selected == NULL) error = errno;
 }
 if (error != 0) { if (fd >= 0) (void)close(fd); free(bits); }
 else { *progress = disk; *deferred = bits; }
 return (error);
}

static inline int
cd_checkpoint_selected_save(struct cd_checkpoint *cp, FILE *selected,
    struct cd_checkpoint_share_progress *progress, const unsigned char *deferred)
{
 if (cp->failed || cp->selected || !cp->complete || cp->sorted_mask != 3) return (EINVAL);
 struct stat st;
 const char *names[] = { "files.selected", "sharing.state" };
 for (unsigned i = 0; i < 2; i++) {
  if (fstatat(cp->dirfd, names[i], &st, AT_SYMLINK_NOFOLLOW) == 0) return (EEXIST);
  if (errno != ENOENT) return (errno);
 }
 if (fflush(selected) != 0) return (errno);
 if (fstat(fileno(selected), &st) != 0) return (errno);
 if (st.st_size < 0 || (uint64_t)st.st_size != cp->sizes[0]) return (ESTALE);
 char temporary[64];
 (void)snprintf(temporary, sizeof (temporary), "selected.tmp.%ld", (long)getpid());
 int fd = openat(cp->dirfd, temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
 int error = fd < 0 ? errno : 0;
 unsigned char buffer[65536];
 for (uint64_t at = 0; error == 0 && at < cp->sizes[0];) {
  size_t n = cp->sizes[0] - at < sizeof (buffer) ? (size_t)(cp->sizes[0] - at) : sizeof (buffer);
  error = cd_checkpoint_read(fileno(selected), buffer, n, (off_t)at);
  if (error == 0) error = cd_checkpoint_write_all(fd, buffer, n);
  at += n;
 }
 progress->selected_size = cp->sizes[0];
 if (error == 0) error = cd_checkpoint_hash_fd(fd, progress->selected_size, progress->selected_hash);
 if (error == 0 && fsync(fd) != 0) error = errno;
 if (fd >= 0 && close(fd) != 0 && error == 0) error = errno;
 if (error == 0 && renameat(cp->dirfd, temporary, cp->dirfd, "files.selected") != 0) error = errno;
 if (error == 0 && fsync(cp->dirfd) != 0) error = errno;
 if (error == 0) CD_RECOVERY_POINT("selected-plan-synced");
 if (error == 0) error = cd_checkpoint_share_save(cp, progress, deferred);
 if (error == 0) {
  cp->selected = true;
  error = cd_checkpoint_commit(cp, cp->count, cp->skipped, true, 3);
 }
 if (error != 0) { if (fd >= 0) (void)unlinkat(cp->dirfd, temporary, 0); cp->failed = true; }
 return (error);
}

static int
cd_checkpoint_open(struct cd_checkpoint *cp, const char *path,
    const void *binding, size_t length)
{
	memset(cp, 0, sizeof (*cp));
	cp->lockfd = cp->dirfd = -1;
	cp->initialized = true;
	/* A trailing slash would make the final named component intermediate for
	 * O_NOFOLLOW. The driver supplies a canonical directory pathname.
	 */
	if (path == NULL || path[0] == '\0' || path[strlen(path) - 1] == '/' ||
	    length > CD_CHECKPOINT_BINDING_MAX ||
	    (length != 0 && binding == NULL))
		return (EINVAL);
	const char *basename = strrchr(path, '/');
	basename = basename == NULL ? path : basename + 1;
	if (strcmp(basename, ".") == 0 || strcmp(basename, "..") == 0)
		return (EINVAL);
	cp->binding_length = length;
	if (length != 0)
		memcpy(cp->binding, binding, length);
	bool created = mkdir(path, 0700) == 0;
	if (!created && errno != EEXIST)
		return (errno);
	cp->dirfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (cp->dirfd < 0)
		return (errno);
	struct stat status;
	int error = fstat(cp->dirfd, &status) == 0 ? 0 : errno;
	if (error == 0 && (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & 07777) != 0700))
		error = EPERM;
	if (error == 0) {
		cp->lockfd = openat(cp->dirfd, "lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC |
		    (created ? O_CREAT | O_EXCL : 0), 0600);
		if (cp->lockfd < 0)
			error = errno;
	}
	if (error == 0)
		error = cd_checkpoint_regular(cp->lockfd);
	if (error == 0 && flock(cp->lockfd, LOCK_EX | LOCK_NB) != 0)
		error = errno;
	struct cd_checkpoint_disk disk;
	memset(&disk, 0, sizeof (disk));
	if (error == 0 && !created) {
		int fd = openat(cp->dirfd, "manifest", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			error = errno;
		else {
			error = cd_checkpoint_regular(fd);
			if (error == 0 && fstat(fd, &status) != 0)
				error = errno;
			if (error == 0 && (uint64_t)status.st_size != sizeof (disk))
				error = EINVAL;
			if (error == 0)
				error = cd_checkpoint_read(fd, &disk, sizeof (disk), 0);
			(void) close(fd);
		}
		unsigned char checksum[32];
		if (error == 0)
			error = cd_checkpoint_checksum(&disk, checksum);
		if (error == 0 && (memcmp(disk.magic, cd_checkpoint_magic, 16) != 0 ||
		    (disk.version != 1 && disk.version != 2) || disk.binding_length != length ||
		    memcmp(disk.binding, cp->binding, length) != 0 ||
		    memcmp(disk.checksum, checksum, 32) != 0 || disk.complete > 1 ||
		    (disk.version == 1 ? disk.sorted_mask > 3 : disk.sorted_mask != 7) || (!disk.complete && disk.sorted_mask != 0)))
			error = EINVAL;
	}
	/* Refuse a stray proof before appending/resuming or trimming any payload. */
	if (error == 0 && (disk.sorted_mask & 4U) == 0) {
		if (fstatat(cp->dirfd, "sharing.proven", &status,
		    AT_SYMLINK_NOFOLLOW) == 0)
			error = ESTALE;
		else if (errno != ENOENT)
			error = errno;
	}
	for (unsigned i = 0; error == 0 && i < CD_CHECKPOINT_STREAMS; i++) {
		int fd = openat(cp->dirfd, cd_checkpoint_names[i],
		    O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC |
		    (created ? O_CREAT | O_EXCL : 0), 0600);
		if (fd < 0) {
			error = errno;
			break;
		}
		error = cd_checkpoint_regular(fd);
		if (error == 0 && fstat(fd, &status) != 0)
			error = errno;
		if (error == 0 && (disk.sizes[i] > INT64_MAX ||
		    (uint64_t)status.st_size < disk.sizes[i] ||
		    (i >= 3 && (disk.sorted_mask & (1U << (i - 3))) == 0 &&
		    disk.sizes[i] != 0)))
			error = EINVAL;
		if (error != 0) {
			(void) close(fd);
			break;
		}
		cp->streams[i] = fdopen(fd, "r+");
		if (cp->streams[i] == NULL) {
			error = errno;
			(void) close(fd);
			break;
		}
		error = cd_checkpoint_hash_init(&cp->hashes[i]);
		if (error == 0)
			error = cd_checkpoint_hash_to(cp, i, disk.sizes[i]);
		unsigned char digest[32];
		if (error == 0)
			error = cd_checkpoint_hash_final(&cp->hashes[i], digest);
		if (error == 0 && !created && memcmp(digest, disk.hashes[i], 32) != 0)
			error = EILSEQ;
	}
	/* Do not discard even uncommitted tails until every committed payload and
	 * the complete manifest/binding have validated successfully.
	 */
	for (unsigned i = 0; error == 0 && i < CD_CHECKPOINT_STREAMS; i++) {
		if (fstat(fileno(cp->streams[i]), &status) != 0)
			error = errno;
		else if ((uint64_t)status.st_size > disk.sizes[i] &&
		    ftruncate(fileno(cp->streams[i]), (off_t)disk.sizes[i]) != 0)
			error = errno;
		if (error == 0 &&
		    fseeko(cp->streams[i], (off_t)disk.sizes[i], SEEK_SET) != 0)
			error = errno;
	}
	if (error == 0) {
		cp->count = disk.count;
		cp->skipped = disk.skipped;
		cp->complete = disk.complete != 0;
		cp->sorted_mask = (unsigned)disk.sorted_mask & 3U;
		cp->selected = (disk.sorted_mask & 4U) != 0;
		memcpy(cp->sizes, disk.sizes, sizeof (cp->sizes));
		cp->loaded = !created;
		if (created)
			error = cd_checkpoint_commit(cp, 0, 0, false, 0);
		if (error == 0 && created)
			error = cd_checkpoint_parent_sync(path);
	}
	if (error != 0)
		cd_checkpoint_close(cp);
	return (error);
}
#endif /* _CHUNKDEDUP_CHECKPOINT_H */
