/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef _CHUNKDEDUP_CHECKPOINT_H
#define _CHUNKDEDUP_CHECKPOINT_H

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
	bool loaded, complete;
	uint64_t count, skipped;
	unsigned sorted_mask;
	int lockfd, dirfd;
	/* Internal state: callers must not change committed prefixes or contexts. */
	uint64_t sizes[CD_CHECKPOINT_STREAMS], hashed[CD_CHECKPOINT_STREAMS];
	struct cd_checkpoint_hash hashes[CD_CHECKPOINT_STREAMS];
	unsigned char binding[CD_CHECKPOINT_BINDING_MAX];
	size_t binding_length;
	bool initialized, failed;
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
	disk.version = 1;
	disk.binding_length = (uint32_t)cp->binding_length;
	memcpy(disk.binding, cp->binding, cp->binding_length);
	disk.count = count;
	disk.skipped = skipped;
	disk.complete = complete;
	disk.sorted_mask = sorted_mask;
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
	if (fd >= 0 && close(fd) != 0 && error == 0)
		error = errno;
	if (error == 0 && renameat(cp->dirfd, temporary,
	    cp->dirfd, "manifest") != 0)
		error = errno;
	if (error == 0) {
		cp->count = count;
		cp->skipped = skipped;
		cp->complete = complete;
		cp->sorted_mask = sorted_mask;
		memcpy(cp->sizes, disk.sizes, sizeof (cp->sizes));
		if (fsync(cp->dirfd) != 0)
			error = errno;
	}
	if (error != 0) {
		/* Remove only the unique temporary created by this attempt. */
		if (fd >= 0 && temporary[0] != '\0')
			(void) unlinkat(cp->dirfd, temporary, 0);
		cp->failed = true;
	}
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
		    disk.version != 1 || disk.binding_length != length ||
		    memcmp(disk.binding, cp->binding, length) != 0 ||
		    memcmp(disk.checksum, checksum, 32) != 0 || disk.complete > 1 ||
		    disk.sorted_mask > 3 || (!disk.complete && disk.sorted_mask != 0)))
			error = EINVAL;
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
		cp->sorted_mask = (unsigned)disk.sorted_mask;
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
