/* SPDX-License-Identifier: CDDL-1.0 */
#include "chunkdedup_hybrid.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
cd_hybrid_add(uint64_t *value, uint64_t amount)
{
	if (amount > UINT64_MAX - *value)
		return (EOVERFLOW);
	*value += amount;
	return (0);
}

/* Upper bound for an uncompressed allocation on supported non-dRAID vdevs. */
int
cd_hybrid_alloc(uint64_t bytes, uint64_t ashift, uint64_t parity,
    uint64_t columns, uint64_t copies, uint64_t *result)
{
	if (ashift > 24 || ashift < 9 || columns == 0 || copies == 0 ||
	    copies > 3 || parity > 3)
		return (ENOTSUP);
	uint64_t sector = UINT64_C(1) << ashift;
	if (bytes > UINT64_MAX - sector + 1)
		return (EOVERFLOW);
	uint64_t sectors = (bytes + sector - 1) / sector;
	if (parity != 0) {
		uint64_t rows = sectors / columns + (sectors % columns != 0);
		if (rows > UINT64_MAX / parity ||
		    cd_hybrid_add(&sectors, rows * parity) != 0)
			return (EOVERFLOW);
		/* RAIDZ rounds allocations to a multiple of parity + one. */
		uint64_t rem = sectors % (parity + 1);
		if (rem != 0 && cd_hybrid_add(&sectors,
		    parity + 1 - rem) != 0)
			return (EOVERFLOW);
	}
	if (sectors > UINT64_MAX / sector / copies)
		return (EOVERFLOW);
	*result = sectors * sector * copies;
	return (0);
}

int
cd_hybrid_sort(FILE *input, FILE **output, const char *directory)
{
	if (fflush(input) != 0 || fseeko(input, 0, SEEK_SET) != 0)
		return (errno);
	FILE *sorted = tmpfile();
	if (sorted == NULL)
		return (errno);
	pid_t pid = fork();
	if (pid == -1) {
		int error = errno;
		(void) fclose(sorted);
		return (error);
	}
	if (pid == 0) {
		if (dup2(fileno(input), STDIN_FILENO) < 0 ||
		    dup2(fileno(sorted), STDOUT_FILENO) < 0 ||
		    setenv("LC_ALL", "C", 1) != 0 ||
		    setenv("TMPDIR", directory, 1) != 0)
			_exit(126);
		execl("/usr/bin/sort", "sort", "-S", "4M", "-k1,1",
		    "-k2,2", "-k3,3n", (char *)NULL);
		_exit(127);
	}
	int status;
	while (waitpid(pid, &status, 0) == -1) {
		if (errno == EINTR)
			continue;
		int error = errno;
		(void) fclose(sorted);
		return (error);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		(void) fclose(sorted);
		return (EIO);
	}
	if (fseeko(sorted, 0, SEEK_SET) != 0) {
		int error = errno;
		(void) fclose(sorted);
		return (error);
	}
	*output = sorted;
	return (0);
}

#ifndef CD_HYBRID_SORT_MAX_THREADS
#define	CD_HYBRID_SORT_MAX_THREADS	64U
#endif
#ifndef CD_HYBRID_SORT_BUFFER_SIZE
#define	CD_HYBRID_SORT_BUFFER_SIZE	65536U
#endif

struct cd_sort_run {
	pid_t pid;
	int pipe[2], fd;
	char path[PATH_MAX];
	unsigned char *buffer;
	size_t used;
};

static int
cd_sort_stopped(const atomic_int *stop)
{
	return (stop != NULL && atomic_load(stop) != 0);
}

/* Keep dup2 destinations independent even if the caller closed a standard fd. */
static int
cd_sort_descriptor(int *fd)
{
	if (*fd >= STDERR_FILENO + 1)
		return (0);
	int moved = fcntl(*fd, F_DUPFD, STDERR_FILENO + 1);
	if (moved < 0)
		return (errno);
	(void) close(*fd);
	*fd = moved;
	return (0);
}

/* Buffered writes normally need one syscall per 64 KiB, not one per record.
 * Nonblocking pipes keep TERM/cancellation observable when a child is slow.
 */
static int
cd_sort_flush(struct cd_sort_run *run, const atomic_int *stop)
{
	size_t offset = 0;
	while (offset < run->used) {
		if (cd_sort_stopped(stop))
			return (ECANCELED);
		ssize_t written = write(run->pipe[1], run->buffer + offset,
		    run->used - offset);
		if (written > 0) {
			offset += (size_t)written;
			CD_RECOVERY_POINT("sort-writing");
			continue;
		}
		if (written == 0)
			return (EIO);
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return (errno);
		struct pollfd ready = { .fd = run->pipe[1], .events = POLLOUT };
		int result = poll(&ready, 1, 100);
		if (result < 0 && errno != EINTR)
			return (errno);
		if (result > 0 && (ready.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return (EPIPE);
	}
	run->used = 0;
	return (0);
}

/* Reading/dispatching needs bounded storage even for a line larger than the
 * buffer. A newline advances the shard; fragments of one line stay together.
 */
static int
cd_sort_dispatch(FILE *input, struct cd_sort_run *runs, unsigned int count,
    const atomic_int *stop)
{
	unsigned char buffer[CD_HYBRID_SORT_BUFFER_SIZE];
	unsigned int current = 0;
	for (;;) {
		if (cd_sort_stopped(stop))
			return (ECANCELED);
		size_t length = fread(buffer, 1, sizeof (buffer), input);
		if (length == 0)
			return (ferror(input) ? (errno != 0 ? errno : EIO) : 0);
		size_t offset = 0;
		while (offset < length) {
			unsigned char *newline = memchr(buffer + offset, '\n',
			    length - offset);
			size_t span = newline != NULL ?
			    (size_t)(newline - buffer) + 1 - offset : length - offset;
			struct cd_sort_run *run = &runs[current];
			while (span != 0) {
				size_t room = CD_HYBRID_SORT_BUFFER_SIZE - run->used;
				size_t copied = span < room ? span : room;
				memcpy(run->buffer + run->used, buffer + offset, copied);
				run->used += copied;
				offset += copied;
				span -= copied;
				if (run->used == CD_HYBRID_SORT_BUFFER_SIZE) {
					int error = cd_sort_flush(run, stop);
					if (error != 0)
						return (error);
				}
			}
			if (newline != NULL)
				current = (current + 1) % count;
		}
	}
}

static int
cd_sort_wait(struct cd_sort_run *runs, unsigned int count,
    const atomic_int *stop)
{
	for (;;) {
		if (cd_sort_stopped(stop))
			return (ECANCELED);
		unsigned int active = 0;
		for (unsigned int i = 0; i < count; i++) {
			if (runs[i].pid <= 0)
				continue;
			int status;
			pid_t result = waitpid(runs[i].pid, &status, WNOHANG);
			if (result == 0) {
				active++;
				continue;
			}
			if (result < 0) {
				if (errno == EINTR) {
					active++;
					continue;
				}
				return (errno);
			}
			runs[i].pid = -1;
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				return (EIO);
		}
		if (active == 0)
			return (0);
		(void) poll(NULL, 0, 20);
	}
}

/* Reap only children created here. sort normally cleans its own spills on
 * TERM; the private operation directory also covers an exec/crash/KILL exit.
 */
static void
cd_sort_abort(struct cd_sort_run *runs, unsigned int count)
{
	for (unsigned int i = 0; i < count; i++)
		if (runs[i].pid > 0)
			(void) kill(runs[i].pid, SIGTERM);
	for (unsigned int attempt = 0; attempt < 20; attempt++) {
		unsigned int active = 0;
		for (unsigned int i = 0; i < count; i++) {
			if (runs[i].pid <= 0)
				continue;
			pid_t result = waitpid(runs[i].pid, NULL, WNOHANG);
			if (result > 0 || (result < 0 && errno == ECHILD))
				runs[i].pid = -1;
			else
				active++;
		}
		if (active == 0)
			return;
		(void) poll(NULL, 0, 100);
	}
	for (unsigned int i = 0; i < count; i++) {
		if (runs[i].pid <= 0)
			continue;
		(void) kill(runs[i].pid, SIGKILL);
		while (waitpid(runs[i].pid, NULL, 0) < 0 && errno == EINTR)
			;
		runs[i].pid = -1;
	}
}

static int
cd_sort_remove(const char *directory)
{
	DIR *stream = opendir(directory);
	if (stream == NULL)
		return (errno);
	int error = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0 && error == 0)
				error = errno;
			break;
		}
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		if (unlinkat(dirfd(stream), entry->d_name, 0) != 0 && error == 0)
			error = errno;
	}
	if (closedir(stream) != 0 && error == 0)
		error = errno;
	if (rmdir(directory) != 0 && error == 0)
		error = errno;
	return (error);
}

static int
cd_hybrid_sort_parallel_impl(FILE *input, FILE **output, const char *directory,
    unsigned int threads, const atomic_int *stop, FILE *destination, int hold_fd)
{
	if (input == NULL || output == NULL || directory == NULL ||
	    threads == 0 || threads > CD_HYBRID_SORT_MAX_THREADS)
		return (EINVAL);
	if (cd_sort_stopped(stop))
		return (ECANCELED);
	if (fflush(input) != 0 || fseeko(input, 0, SEEK_SET) != 0)
		return (errno != 0 ? errno : EIO);
	char temporary[PATH_MAX];
	int length = snprintf(temporary, sizeof (temporary),
	    "%s/parallel-sort.XXXXXX", directory);
	if (length < 0 || (size_t)length >= sizeof (temporary))
		return (ENAMETOOLONG);
	if (mkdtemp(temporary) == NULL)
		return (errno);
	struct cd_sort_run runs[CD_HYBRID_SORT_MAX_THREADS];
	memset(runs, 0, sizeof (runs));
	for (unsigned int i = 0; i < threads; i++) {
		runs[i].pid = -1;
		runs[i].pipe[0] = runs[i].pipe[1] = runs[i].fd = -1;
	}
	FILE *sorted = NULL;
	int error = 0;
	sigset_t blocked, original, pending;
	int masked = 0, had_pipe = 1;
	(void) sigemptyset(&blocked);
	(void) sigaddset(&blocked, SIGPIPE);
	if ((error = pthread_sigmask(SIG_BLOCK, &blocked, &original)) != 0)
		goto done;
	masked = 1;
	if (sigpending(&pending) != 0) {
		error = errno;
		goto done;
	}
	had_pipe = sigismember(&pending, SIGPIPE);
	for (unsigned int i = 0; i < threads; i++) {
		struct cd_sort_run *run = &runs[i];
		run->buffer = malloc(CD_HYBRID_SORT_BUFFER_SIZE);
		if (run->buffer == NULL) {
			error = ENOMEM;
			goto done;
		}
		length = snprintf(run->path, sizeof (run->path), "%s/run-%u",
		    temporary, i);
		if (length < 0 || (size_t)length >= sizeof (run->path)) {
			error = ENAMETOOLONG;
			goto done;
		}
		run->fd = open(run->path, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (run->fd < 0) {
			error = errno;
			goto done;
		}
		if ((error = cd_sort_descriptor(&run->fd)) != 0)
			goto done;
		if (pipe(run->pipe) != 0) {
			error = errno;
			goto done;
		}
		if ((error = cd_sort_descriptor(&run->pipe[0])) != 0 ||
		    (error = cd_sort_descriptor(&run->pipe[1])) != 0)
			goto done;
		int flags = fcntl(run->pipe[1], F_GETFL);
		if (flags < 0 || fcntl(run->pipe[1], F_SETFL,
		    flags | O_NONBLOCK) != 0) {
			error = errno;
			goto done;
		}
	}
	for (unsigned int i = 0; i < threads; i++) {
		pid_t pid = fork();
		if (pid < 0) {
			error = errno;
			goto done;
		}
		if (pid == 0) {
			/* An orphan must retain the checkpoint lock until it exits. */
			if (hold_fd >= 0 && fcntl(hold_fd, F_SETFD, 0) != 0)
				_exit(126);
			if (dup2(runs[i].pipe[0], STDIN_FILENO) < 0 ||
			    dup2(runs[i].fd, STDOUT_FILENO) < 0)
				_exit(126);
			for (unsigned int j = 0; j < threads; j++) {
				if (runs[j].pipe[0] >= 0)
					(void) close(runs[j].pipe[0]);
				if (runs[j].pipe[1] >= 0)
					(void) close(runs[j].pipe[1]);
				if (runs[j].fd >= 0)
					(void) close(runs[j].fd);
			}
			(void) pthread_sigmask(SIG_SETMASK, &original, NULL);
			if (setenv("LC_ALL", "C", 1) != 0 ||
			    setenv("TMPDIR", temporary, 1) != 0)
				_exit(126);
			execl("/usr/bin/sort", "sort", "-S", "4M", "-k1,1",
			    "-k2,2", "-k3,3n", (char *)NULL);
			_exit(127);
		}
		runs[i].pid = pid;
		(void) close(runs[i].pipe[0]);
		runs[i].pipe[0] = -1;
		(void) close(runs[i].fd);
		runs[i].fd = -1;
	}
	error = cd_sort_dispatch(input, runs, threads, stop);
	for (unsigned int i = 0; error == 0 && i < threads; i++)
		error = cd_sort_flush(&runs[i], stop);
	for (unsigned int i = 0; i < threads; i++) {
		(void) close(runs[i].pipe[1]);
		runs[i].pipe[1] = -1;
	}
	if (error != 0 || (error = cd_sort_wait(runs, threads, stop)) != 0)
		goto done;
	if (threads == 1 && destination == NULL) {
		sorted = fopen(runs[0].path, "r+");
		if (sorted == NULL)
			error = errno;
		goto done;
	}
	sorted = destination != NULL ? destination : tmpfile();
	if (sorted == NULL) {
		error = errno;
		goto done;
	}
	pid_t pid = fork();
	if (pid < 0) {
		error = errno;
		goto done;
	}
	if (pid == 0) {
		if (hold_fd >= 0 && fcntl(hold_fd, F_SETFD, 0) != 0)
			_exit(126);
		if (dup2(fileno(sorted), STDOUT_FILENO) < 0 ||
		    setenv("LC_ALL", "C", 1) != 0 ||
		    setenv("TMPDIR", temporary, 1) != 0)
			_exit(126);
		char *arguments[CD_HYBRID_SORT_MAX_THREADS + 9];
		char options[][8] = { "sort", "-m", "-S", "4M", "-k1,1",
		    "-k2,2", "-k3,3n", "--" };
		unsigned int n = 0;
		for (; n < sizeof (options) / sizeof (options[0]); n++)
			arguments[n] = options[n];
		for (unsigned int i = 0; i < threads; i++)
			arguments[n++] = runs[i].path;
		arguments[n] = NULL;
		execv("/usr/bin/sort", arguments);
		_exit(127);
	}
	runs[0].pid = pid;
	CD_RECOVERY_POINT("sort-merge");
	error = cd_sort_wait(runs, 1, stop);
done:
	cd_sort_abort(runs, threads);
	for (unsigned int i = 0; i < threads; i++) {
		if (runs[i].pipe[0] >= 0)
			(void) close(runs[i].pipe[0]);
		if (runs[i].pipe[1] >= 0)
			(void) close(runs[i].pipe[1]);
		if (runs[i].fd >= 0)
			(void) close(runs[i].fd);
		free(runs[i].buffer);
	}
	int removed = cd_sort_remove(temporary);
	if (error == 0)
		error = removed;
	if (masked) {
		/* A failed pipe write must not become a delayed fatal signal when
		 * the caller's mask is restored. Preserve a pre-existing SIGPIPE.
		 */
		if (!had_pipe && sigpending(&pending) == 0 &&
		    sigismember(&pending, SIGPIPE)) {
			int signal;
			(void) sigwait(&blocked, &signal);
		}
		int restored = pthread_sigmask(SIG_SETMASK, &original, NULL);
		if (error == 0)
			error = restored;
	}
	if (error == 0 && fseeko(sorted, 0, SEEK_SET) != 0)
		error = errno;
	if (error != 0) {
		if (sorted != NULL && sorted != destination)
			(void) fclose(sorted);
		return (error);
	}
	*output = sorted;
	return (0);
}

int
cd_hybrid_sort_parallel(FILE *input, FILE **output, const char *directory,
    unsigned int threads, const atomic_int *stop)
{
	return (cd_hybrid_sort_parallel_impl(input, output, directory, threads,
	    stop, NULL, -1));
}

/* The caller owns the durable destination, including failed partial output. */
int
cd_hybrid_sort_parallel_to(FILE *input, FILE *destination,
    const char *directory, unsigned int threads, const atomic_int *stop,
    int hold_fd)
{
	if (destination == NULL)
		return (EINVAL);
	/* Keep the inherited lock clear of stdin/stdout even when either is closed. */
	int lock = fcntl(hold_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
	if (lock < 0)
		return (errno);
	FILE *output = NULL;
	int error = cd_hybrid_sort_parallel_impl(input, &output, directory, threads,
	    stop, destination, lock);
	(void) close(lock);
	return (error);
}
