/* SPDX-License-Identifier: CDDL-1.0 */
#include "chunkdedup_hybrid.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdlib.h>
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
