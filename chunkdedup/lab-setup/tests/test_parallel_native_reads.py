#!/usr/bin/env python3
"""Exercise actual native-read core with 40 threads and the existing DMU model."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--core', type=Path, help='optional predecessor core for negative control')
a = p.parse_args()
repo = a.repo.resolve()
kit = repo / 'chunkdedup'
model = (kit / 'native-core-mock/core_test.c').read_text()
core = (a.core or repo / 'module/zfs/zfs_chunkdedup.c').resolve()
assert model.count('int dmu_read(') == 1
assert model.count('int main(void)') == 1
assert model.count('#include "zfs_chunkdedup.c"') == 1
instrument = r'''
#include <pthread.h>
#include <time.h>
static pthread_mutex_t metadata;
static pthread_once_t metadata_once = PTHREAD_ONCE_INIT;
static _Thread_local int metadata_held;
static _Thread_local kmutex_t *held_locks[4];
static void metadata_init(void) {
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&metadata, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
}
static int test_held(kmutex_t *m) {
    for (int i = 0; i < metadata_held; i++) if (held_locks[i] == m) return 1;
    return 0;
}
static void test_lock(kmutex_t *m) {
    assert(!test_held(m) && metadata_held < 4);
    assert(!pthread_once(&metadata_once, metadata_init));
    assert(!pthread_mutex_lock(&metadata)); held_locks[metadata_held++] = m;
}
static void test_unlock(kmutex_t *m) {
    assert(metadata_held && held_locks[metadata_held - 1] == m); metadata_held--;
    assert(!pthread_mutex_unlock(&metadata));
}
#undef MUTEX_HELD
#define MUTEX_HELD(m) test_held(m)
#define mutex_enter(m) test_lock(m)
#define mutex_exit(m) test_unlock(m)
static pthread_mutex_t arrival = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t together = PTHREAD_COND_INITIALIZER;
static int concurrent_test, arrived;
#define READERS 40
'''
wrapper = r'''
int dmu_read(objset_t *os, uint64_t id, uint64_t off, uint64_t len,
    void *buf, int flags) {
    if (concurrent_test) {
        /* No dataset mutex may be held while ordinary reads wait for I/O. */
        assert(!metadata_held);
        struct timespec until;
        assert(!clock_gettime(CLOCK_REALTIME, &until));
        until.tv_sec += 5;
        assert(!pthread_mutex_lock(&arrival));
        arrived++;
        assert(!pthread_cond_broadcast(&together));
        while (arrived != READERS) {
            int rc = pthread_cond_timedwait(&together, &arrival, &until);
            assert(rc == 0 || rc == ETIMEDOUT);
            assert(rc != ETIMEDOUT || arrived == READERS);
        }
        assert(!pthread_mutex_unlock(&arrival));
    }
    return mock_dmu_read(os, id, off, len, buf, flags);
}
'''
extra = r'''
typedef struct reader_arg { znode_t *file; unsigned char byte; } reader_arg_t;
static void *reader(void *opaque) {
    reader_arg_t *a = opaque;
    unsigned char bytes[4096];
    assert(zfs_chunkdedup_read_buf(a->file, 0, sizeof(bytes), bytes) == 0);
    for (size_t i = 0; i < sizeof(bytes); i++) assert(bytes[i] == a->byte);
    return NULL;
}
int main(void) {
    /* Independent files in one dataset must all reach native I/O together. */
    objset_t *os = new_os();
    zfsvfs_t fs = { .z_os = os };
    znode_t files[READERS]; reader_arg_t args[READERS];
    pthread_t workers[READERS];
    unsigned char bytes[4096];
    for (int i = 0; i < READERS; i++) {
        memset(bytes, i + 1, sizeof(bytes));
        files[i] = make_file(&fs, bytes, sizeof(bytes));
        args[i] = (reader_arg_t){ &files[i], i + 1 };
    }
    concurrent_test = 1;
    for (int i = 0; i < READERS; i++)
        assert(!pthread_create(&workers[i], NULL, reader, &args[i]));
    for (int i = 0; i < READERS; i++) assert(!pthread_join(workers[i], NULL));
    concurrent_test = 0;
    assert(arrived == READERS);
    /* Unmapped files stay parallel when a different file has chunk mappings. */
    unsigned char payload[131072]; memset(payload, 0x6b, sizeof(payload));
    znode_t mapped = make_file(&fs, payload, sizeof(payload));
    zfs_chunkdedup_chunk_t chunk; const size_t lengths[] = { sizeof(payload) };
    plan(&chunk, payload, lengths, 1);
    assert(zfs_chunkdedup_optimize(&mapped, 0, sizeof(payload), &chunk, 1) == 0);
    arrived = 0; concurrent_test = 1;
    for (int i = 0; i < READERS; i++)
        assert(!pthread_create(&workers[i], NULL, reader, &args[i]));
    for (int i = 0; i < READERS; i++) assert(!pthread_join(workers[i], NULL));
    concurrent_test = 0; assert(arrived == READERS);
    expect_data(&mapped, payload);
    /* Preserve bounds and fail closed on a marker without a mapping. */
    memset(bytes, 0xa5, sizeof(bytes));
    assert(zfs_chunkdedup_read_buf(&files[0], 4097, 1, bytes) == EINVAL);
    assert(zfs_chunkdedup_read_buf(&files[0], 4096, 1, bytes) == EINVAL);
    assert(zfs_chunkdedup_read_buf(&files[0], 4096, 0, bytes) == 0);
    files[0].z_pflags |= ZFS_CHUNKDEDUP;
    assert(zfs_chunkdedup_read_buf(&files[0], 0, sizeof(bytes), bytes) == EIO);
    for (size_t i = 0; i < sizeof(bytes); i++) assert(bytes[i] == 0xa5);
    assert(cd_model_main() == 0);
    puts("PASS: 40 simultaneous native reads; bytes, bounds and corrupt-map regression");
    return 0;
}
'''
model = model.replace('#include <sys/zfs_chunkdedup.h>', '#include <sys/zfs_chunkdedup.h>\n' + instrument, 1)
model = model.replace('int dmu_read(', 'int mock_dmu_read(', 1)
model = model.replace('#include "zfs_chunkdedup.c"', wrapper + '\n#include "' + str(core) + '"', 1)
model = model.replace('int main(void)', 'int cd_model_main(void)', 1) + extra
with tempfile.TemporaryDirectory(prefix='chunkdedup-parallel-read-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(model)
    command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-g', '-pthread',
               '-I' + str(kit / 'native-core-mock/include'), '-I' + str(repo / 'include'),
               str(tmp / 'test.c'), '-o', str(tmp / 'test')]
    if sys.platform != 'darwin': command.append('-lcrypto')
    subprocess.run(command, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    subprocess.run([str(tmp / 'test')], env=env, check=True, timeout=30)
