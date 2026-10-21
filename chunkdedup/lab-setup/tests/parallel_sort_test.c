/* SPDX-License-Identifier: MIT
 * Actual production sorter and /usr/bin/sort; wrappers observe child lifetime
 * and inject process failures without replacing sorting or filesystem I/O.
 */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "chunkdedup_hybrid.h"

static atomic_int cancelled;
static pid_t children[128];
static unsigned attempts, spawned, live, peak, child_number;
static unsigned gate_target, fork_failure, exec_failure, cancel_after;
static int gate_pipe[2];

pid_t fixture_fork(void)
{
    unsigned number = ++attempts;
    if (number == fork_failure) { errno = EAGAIN; return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        child_number = number;
        (void) close(gate_pipe[1]);
        if (gate_target && number <= gate_target) {
            char byte; ssize_t got;
            do { got = read(gate_pipe[0], &byte, 1); } while (got < 0 && errno == EINTR);
            if (got != 1) _exit(125);
        }
        return 0;
    }
    if (pid > 0) {
        assert(spawned < sizeof (children) / sizeof (children[0]));
        children[spawned++] = pid;
        if (++live > peak) peak = live;
        if (spawned == gate_target) {
            char bytes[64] = {0};
            assert(write(gate_pipe[1], bytes, gate_target) == (ssize_t)gate_target);
        }
        if (number == cancel_after) atomic_store(&cancelled, 1);
    }
    return pid;
}

pid_t fixture_waitpid(pid_t pid, int *status, int options)
{
    pid_t result = waitpid(pid, status, options);
    if (result > 0) {
        unsigned i;
        for (i = 0; i < spawned && children[i] != result; i++) {}
        assert(i < spawned && live != 0);
        children[i] = 0; live--;
    }
    return result;
}

int fixture_execv(const char *path, char *const arguments[])
{
    assert(strcmp(path, "/usr/bin/sort") == 0);
    if (child_number == exec_failure) { errno = ENOENT; return -1; }
    return execv(path, arguments);
}

int fixture_execl(const char *path, const char *first, ...)
{
    char *arguments[160]; unsigned count = 0;
    va_list values; va_start(values, first);
    const char *value = first;
    while (value) {
        assert(count + 1 < sizeof (arguments) / sizeof (arguments[0]));
        arguments[count++] = (char *)value; value = va_arg(values, const char *);
    }
    va_end(values); arguments[count] = NULL;
    return fixture_execv(path, arguments);
}

int fixture_kill(pid_t pid, int signal_number)
{
    unsigned i;
    for (i = 0; i < spawned && children[i] != pid; i++) {}
    assert(pid > 0 && i < spawned);
    return kill(pid, signal_number);
}

#define fork fixture_fork
#define waitpid fixture_waitpid
#define execl fixture_execl
#define execv fixture_execv
#define kill fixture_kill
#include HYBRID_SORT_SOURCE
#undef fork
#undef waitpid
#undef execl
#undef execv
#undef kill

static unsigned descriptors(void)
{
    unsigned count = 0;
    for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) != -1) count++;
    return count;
}

static void model_begin(unsigned workers)
{
    assert(live == 0); memset(children, 0, sizeof (children));
    attempts = spawned = peak = 0; gate_target = workers;
    fork_failure = exec_failure = cancel_after = 0;
    atomic_store(&cancelled, 0); assert(pipe(gate_pipe) == 0);
}

static void model_end(const char *directory, unsigned before)
{
    int leaked = live != 0;
    for (unsigned i = 0; i < spawned; i++) if (children[i] > 0) {
        (void) kill(children[i], SIGKILL);
        (void) waitpid(children[i], NULL, 0);
    }
    assert(!leaked);
    errno = 0; assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    if (gate_pipe[0] >= 0) assert(close(gate_pipe[0]) == 0);
    if (gate_pipe[1] >= 0) assert(close(gate_pipe[1]) == 0);
    assert(descriptors() == before);
    DIR *dir = opendir(directory); assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        assert(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
    assert(closedir(dir) == 0);
}

static FILE *input(unsigned count, int unusual)
{
    FILE *file = tmpfile(); assert(file);
    static const char *lengths[] = {"2", "10", "02", "0002", "-1", "0.5", "128", "4096"};
    for (unsigned i = count; i > 0; i--)
        assert(fprintf(file, "%u %064x %s %u %u 0000\n", i % 7,
            i % 5, lengths[i % 8], i % 17, i) > 0);
    if (unusual) {
        const char *ties[] = {"1 aaa 2 10\n", "1 aaa 02 2\n", "1 aaa 2 2\n",
            "1 aaa 2 0002\n", "1 aaa 2 2\n", "\n", "broken text\n",
            "1\t aaa\t2\t99\n", "1 aaa nonsense 4\n"};
        for (unsigned i = 0; i < sizeof (ties) / sizeof (ties[0]); i++)
            assert(fputs(ties[i], file) >= 0);
        assert(fputs("1 aaa 2 ", file) >= 0);
        for (unsigned i = 0; i < 200003; i++) assert(fputc('a' + i % 23, file) != EOF);
        assert(fputs("\n9 zzz 10 unterminated", file) >= 0);
    }
    return file;
}

static void equal(FILE *left, FILE *right)
{
    assert(fseeko(left, 0, SEEK_SET) == 0 && fseeko(right, 0, SEEK_SET) == 0);
    unsigned char a[8192], b[8192]; size_t na, nb;
    do {
        na = fread(a, 1, sizeof (a), left); nb = fread(b, 1, sizeof (b), right);
        assert(na == nb && memcmp(a, b, na) == 0);
    } while (na);
    assert(!ferror(left) && !ferror(right));
}

static void differential(const char *directory, unsigned count, int unusual)
{
    FILE *file = input(count, unusual), *expected = NULL;
    unsigned before = descriptors(); model_begin(0);
    assert(cd_hybrid_sort(file, &expected, directory) == 0 && expected);
    assert(peak == 1); assert(fclose(expected) == 0);
    model_end(directory, before);
    /* Preserve a separate reference while checking each parallel API mode. */
    model_begin(0); assert(cd_hybrid_sort(file, &expected, directory) == 0);
    assert(close(gate_pipe[0]) == 0 && close(gate_pipe[1]) == 0);
    for (unsigned mode = 0; mode < 2; mode++) {
        unsigned threads = mode ? 40 : 1;
        before = descriptors(); model_begin(mode ? 40 : 0);
        FILE *actual = NULL;
        assert(cd_hybrid_sort_parallel(file, &actual, directory, threads, &cancelled) == 0 && actual);
        equal(expected, actual); assert(fclose(actual) == 0);
        if (count && mode) assert(peak == 40 && spawned == 41);
        assert(peak <= threads); model_end(directory, before);
    }
    assert(fclose(expected) == 0 && fclose(file) == 0);
}

static void failure(const char *directory, unsigned fork_at, unsigned exec_at, unsigned stop_at)
{
    FILE *file = input(12000, 0), *actual = NULL;
    unsigned before = descriptors(); model_begin(40);
    fork_failure = fork_at; exec_failure = exec_at; cancel_after = stop_at;
    int error = cd_hybrid_sort_parallel(file, &actual, directory, 40, &cancelled);
    assert(error != 0 && actual == NULL && peak <= 40);
    if (fork_at) assert(error == EAGAIN);
    if (stop_at) assert(error == ECANCELED);
    model_end(directory, before); assert(fclose(file) == 0);
}

static void closed_standard_descriptors(const char *directory)
{
    FILE *file = input(2000, 0), *expected = NULL;
    unsigned before = descriptors(); model_begin(0);
    assert(cd_hybrid_sort(file, &expected, directory) == 0);
    assert(close(gate_pipe[0]) == 0 && close(gate_pipe[1]) == 0);
    assert(live == 0 && descriptors() == before + 1);
    pid_t pid = fork(); assert(pid >= 0);
    if (pid == 0) {
        assert(close(STDIN_FILENO) == 0 && close(STDOUT_FILENO) == 0);
        attempts = spawned = live = peak = gate_target = 0;
        memset(children, 0, sizeof (children));
        gate_pipe[0] = gate_pipe[1] = -1;
        before = descriptors(); FILE *actual = NULL;
        assert(!cd_hybrid_sort_parallel(file, &actual, directory, 40, &cancelled));
        equal(expected, actual); assert(fclose(actual) == 0);
        model_end(directory, before); _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(fclose(file) == 0 && fclose(expected) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2); setvbuf(stdout, NULL, _IONBF, 0); (void) alarm(40);
    differential(argv[1], 12000, 1); differential(argv[1], 0, 0);
    puts("PASS real sort: exact serial bytes, numeric keys/full-line ties, long/unterminated lines and empty input; 1/40 threads");
    failure(argv[1], 1, 0, 0); failure(argv[1], 4, 0, 0); failure(argv[1], 41, 0, 0);
    failure(argv[1], 0, 1, 0); failure(argv[1], 0, 40, 0); failure(argv[1], 0, 41, 0);
    failure(argv[1], 0, 0, 1); failure(argv[1], 0, 0, 17);
    puts("PASS bounded 40-child overlap, fork/exec/merge failures and cancellation reap all children/FDs/run files");
    sigset_t set, old, pending; sigemptyset(&set); sigaddset(&set, SIGPIPE);
    assert(!pthread_sigmask(SIG_BLOCK, &set, &old)); assert(raise(SIGPIPE) == 0);
    differential(argv[1], 200, 0); assert(!sigpending(&pending) && sigismember(&pending, SIGPIPE));
    int caught; assert(!sigwait(&set, &caught) && caught == SIGPIPE);
    assert(!pthread_sigmask(SIG_SETMASK, &old, NULL));
    puts("PASS calling thread's preexisting pending SIGPIPE is preserved");
    closed_standard_descriptors(argv[1]);
    puts("PASS closed stdin/stdout do not alias child pipe/run descriptors");
    FILE *file = input(1, 0), *actual = NULL;
    unsigned before = descriptors(); model_begin(0);
    assert(cd_hybrid_sort_parallel(file, &actual, argv[1], 0, &cancelled) == EINVAL);
    assert(cd_hybrid_sort_parallel(file, &actual, argv[1], 65, &cancelled) == EINVAL);
    atomic_store(&cancelled, 1);
    assert(cd_hybrid_sort_parallel(file, &actual, argv[1], 40, &cancelled) == ECANCELED);
    assert(spawned == 0 && actual == NULL);
    atomic_store(&cancelled, 0);
    assert(cd_hybrid_sort_parallel(file, &actual, "/dev/null", 40, &cancelled) != 0);
    assert(spawned == 0 && actual == NULL);
    model_end(argv[1], before); assert(fclose(file) == 0);
    puts("PASS invalid worker counts, initial cancellation and staging-directory failure leave no resources");
    return 0;
}
