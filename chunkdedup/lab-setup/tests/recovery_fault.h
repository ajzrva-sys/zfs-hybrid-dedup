/* SPDX-License-Identifier: MIT
 * Test builds only: force-include this header. Never install this binary.
 */
#ifndef CD_RECOVERY_FAULT_H
#define CD_RECOVERY_FAULT_H
#ifndef _KERNEL
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static inline void
cd_recovery_fault(const char *point)
{
    static atomic_uint hits;
    const char *wanted = getenv("CD_CRASH_POINT");
    if (wanted == NULL || strcmp(wanted, point) != 0)
        return;
    const char *occurrence = getenv("CD_CRASH_HIT");
    unsigned long hit = occurrence == NULL ? 1 : strtoul(occurrence, NULL, 10);
    if (hit == 0 || hit > 1000000)
        _exit(198);
    if (atomic_fetch_add(&hits, 1) + 1 != hit)
        return;
    char notice[160];
    int size = snprintf(notice, sizeof (notice), "CDCRASH %s %lu\n", point, hit);
    if (size <= 0 || size >= (int)sizeof (notice) ||
        write(STDERR_FILENO, notice, (size_t)size) != size)
        _exit(198);
    /* SIGSTOP freezes all threads. Controller kills only its owned process
     * group, or powers off the explicitly guarded disposable VM. */
    (void) kill(getpid(), SIGSTOP);
    _exit(199); /* A controller must never continue the faulted process. */
}
#define CD_RECOVERY_POINT(name) cd_recovery_fault(name)
#endif
#endif
