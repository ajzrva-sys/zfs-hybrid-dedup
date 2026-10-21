/* Direct tests of unchanged candidate functions with userspace kstat/lock stubs.
 * This harness does not load a module or access a pool. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KSTAT_DATA_UINT64 4
#define KSTAT_TYPE_NAMED 1
#define KSTAT_STRLEN 255
#define KSTAT_READ 0
#define KSTAT_WRITE 1
#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))
typedef struct { char name[32]; unsigned char data_type; union { uint64_t ui64; } value; } kstat_named_t;
typedef struct kstat { void *ks_private, *ks_data; int (*ks_update)(struct kstat *, int); } kstat_t;
typedef struct { uint64_t guid; const char *name; } spa_t;
typedef struct { spa_t *dp_spa; pthread_mutex_t dp_lock; uint64_t dp_dirty_total; kstat_t *dp_dirty_kstat; } dsl_pool_t;
static uint64_t zfs_dirty_data_max;
static const char *expected_pool = "testpool";
static int allocation_failure, installed, removed, active_readers, deleting;
static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static uint64_t spa_guid(spa_t *spa) { return (spa->guid); }
static const char *spa_name(spa_t *spa) { return (spa->name); }
static void mutex_enter(pthread_mutex_t *lock) { assert(pthread_mutex_lock(lock) == 0); }
static void mutex_exit(pthread_mutex_t *lock) { assert(pthread_mutex_unlock(lock) == 0); }
static kstat_t *kstat_create(const char *module, int instance, const char *name,
    const char *class, unsigned char type, unsigned int count, unsigned char flags)
{
    assert(strncmp(module, "zfs/", 4) == 0 && strcmp(module + 4, expected_pool) == 0 && instance == 0);
    assert(strcmp(name, "stats") == 0 && strcmp(class, "dirty_data") == 0);
    assert(type == KSTAT_TYPE_NAMED && count == 3 && flags == 0);
    if (allocation_failure) return (NULL);
    kstat_t *ksp = calloc(1, sizeof (*ksp)); assert(ksp);
    ksp->ks_data = calloc(count, sizeof (kstat_named_t)); assert(ksp->ks_data);
    return (ksp);
}
static void kstat_install(kstat_t *ksp) { assert(ksp->ks_private && ksp->ks_update); installed++; }
static void kstat_delete(kstat_t *ksp)
{
    mutex_enter(&lifecycle);
    deleting = 1; pthread_cond_broadcast(&changed);
    while (active_readers) assert(pthread_cond_wait(&changed, &lifecycle) == 0);
    mutex_exit(&lifecycle);
    free(ksp->ks_data); free(ksp); removed++;
}
#include "dirty_data_impl.inc"

static void *blocked_reader(void *arg)
{
    kstat_t *ksp = arg;
    mutex_enter(&lifecycle); active_readers++; pthread_cond_broadcast(&changed); mutex_exit(&lifecycle);
    assert(ksp->ks_update(ksp, KSTAT_READ) == 0);
    mutex_enter(&lifecycle); active_readers--; pthread_cond_broadcast(&changed); mutex_exit(&lifecycle);
    return (NULL);
}
static void *close_pool(void *arg) { dsl_pool_dirty_kstat_fini(arg); return (NULL); }
int main(void)
{
    spa_t spa = { UINT64_C(18446744073709550001), "testpool" };
    dsl_pool_t dp = { .dp_spa = &spa, .dp_lock = PTHREAD_MUTEX_INITIALIZER };
    zfs_dirty_data_max = UINT64_C(17179869184);
    dsl_pool_dirty_kstat_init(&dp); assert(installed == 1 && dp.dp_dirty_kstat);
    kstat_t *ksp = dp.dp_dirty_kstat; kstat_named_t *stats = ksp->ks_data;
    assert(!strcmp(stats[0].name, "pool_guid") && !strcmp(stats[1].name, "dirty_bytes") &&
        !strcmp(stats[2].name, "dirty_limit_bytes"));
    for (int i = 0; i < 3; i++) assert(stats[i].data_type == KSTAT_DATA_UINT64);
    puts("PASS ABI names and UINT64 types");
    assert(ksp->ks_update(ksp, KSTAT_READ) == 0);
    assert(stats[0].value.ui64 == spa.guid && stats[1].value.ui64 == 0 && stats[2].value.ui64 == zfs_dirty_data_max);
    puts("PASS idle zero and full-width pool identity");
    dp.dp_dirty_total = UINT64_C(2305843009213693951);
    assert(ksp->ks_update(ksp, KSTAT_READ) == 0);
    assert(stats[1].value.ui64 == dp.dp_dirty_total && stats[1].value.ui64 > stats[2].value.ui64);
    puts("PASS large current gauge is not clamped to limit");
    assert(ksp->ks_update(ksp, KSTAT_WRITE) == EACCES);
    assert(dp.dp_dirty_total == UINT64_C(2305843009213693951));
    puts("PASS write rejected and native accounting unchanged");
    mutex_enter(&dp.dp_lock);
    pthread_t reader, closer;
    assert(pthread_create(&reader, NULL, blocked_reader, ksp) == 0);
    mutex_enter(&lifecycle);
    while (!active_readers) assert(pthread_cond_wait(&changed, &lifecycle) == 0);
    mutex_exit(&lifecycle);
    assert(pthread_create(&closer, NULL, close_pool, &dp) == 0);
    mutex_enter(&lifecycle);
    while (!deleting) assert(pthread_cond_wait(&changed, &lifecycle) == 0);
    assert(active_readers == 1 && removed == 0);
    mutex_exit(&lifecycle);
    mutex_exit(&dp.dp_lock);
    assert(pthread_join(reader, NULL) == 0 && pthread_join(closer, NULL) == 0);
    assert(dp.dp_dirty_kstat == NULL && removed == 1);
    puts("PASS callback takes pool lock; close waits for reader without lock inversion");
    dsl_pool_dirty_kstat_fini(&dp); assert(removed == 1);
    allocation_failure = 1; dsl_pool_dirty_kstat_init(&dp);
    assert(dp.dp_dirty_kstat == NULL && installed == 1);
    dsl_pool_dirty_kstat_fini(&dp); assert(removed == 1);
    puts("PASS allocation failure and absent/idempotent cleanup");
    allocation_failure = 0;
    const char *reserved[] = { "dataset", "dirty_data" };
    for (unsigned int i = 0; i < ARRAY_SIZE(reserved); i++) {
        expected_pool = spa.name = reserved[i];
        dsl_pool_dirty_kstat_init(&dp); assert(dp.dp_dirty_kstat);
        assert(dp.dp_dirty_kstat->ks_update(dp.dp_dirty_kstat, KSTAT_READ) == 0);
        dsl_pool_dirty_kstat_fini(&dp); assert(dp.dp_dirty_kstat == NULL);
    }
    puts("PASS reserved pool names use constant class and independent per-pool namespace");
    char long_name[KSTAT_STRLEN + 1]; memset(long_name, 'a', sizeof (long_name));
    long_name[sizeof (long_name) - 1] = '\0';
    expected_pool = spa.name = long_name;
    int previous_installed = installed;
    dsl_pool_dirty_kstat_init(&dp);
    assert(dp.dp_dirty_kstat == NULL && installed == previous_installed);
    long_name[KSTAT_STRLEN - 5] = '\0'; /* 250 pool chars plus zfs/ fit. */
    dsl_pool_dirty_kstat_init(&dp); assert(dp.dp_dirty_kstat);
    dsl_pool_dirty_kstat_fini(&dp);
    puts("PASS module-name length boundary omits oversized names without truncation");
    assert(pthread_mutex_destroy(&dp.dp_lock) == 0);
    return (0);
}
