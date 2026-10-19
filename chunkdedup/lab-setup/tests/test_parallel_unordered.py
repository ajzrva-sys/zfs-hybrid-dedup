#!/usr/bin/env python3
"""Exercise actual pool completion/replenishment behind a blocked first job."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--header', type=Path, help='optional predecessor production header')
p.add_argument('--ordered-control', action='store_true', help='expect head-of-line stall')
a = p.parse_args()
repo = a.repo.resolve()
header = a.header or repo / 'cmd/zfs/chunkdedup_parallel.h'
existing = repo / 'chunkdedup/lab-setup/tests/parallel_scan_test.c'
extra = r'''
#undef main
#ifdef TEST_ORDERED_CONTROL
#define cd_parallel_init_unordered cd_parallel_init
#endif
#define JOBS 400
static void unordered_replenishment(void) {
    struct cd_parallel pool;
    struct test_state state;
    struct test_item items[JOBS];
    unsigned char seen[JOBS] = {0};
    state_init(&state, 1);
    assert(!cd_parallel_init_unordered(&pool, 40, work));
    assert(pool.capacity == 80);
    void *result = NULL; int error = -1;
    assert(cd_parallel_take(&pool, &result, &error) == ENOENT);
    for (unsigned i = 0; i < JOBS; i++) items[i] = item_init(&state, i);
    for (unsigned i = 0; i < 80; i++)
        assert(!cd_parallel_submit(&pool, &items[i]));
    assert(cd_parallel_submit(&pool, &items[80]) == EAGAIN);
    assert(!pthread_mutex_lock(&state.lock));
    while (state.started < 40)
        assert(!pthread_cond_wait(&state.changed, &state.lock));
    assert(state.active == 40 && state.peak == 40);
    state.release_others = 1;
    assert(!pthread_cond_broadcast(&state.changed));
    assert(!pthread_mutex_unlock(&state.lock));
    unsigned next = 80;
    for (unsigned taken = 0; taken < JOBS - 1; taken++) {
        assert(!cd_parallel_take(&pool, &result, &error));
        struct test_item *item = result;
        assert(item >= items && item < items + JOBS);
        assert(item->id != 0 && !seen[item->id]++);
        assert(item->finished && error == item->error);
        assert(cd_parallel_count(&pool) < 80);
        if (next < JOBS) assert(!cd_parallel_submit(&pool, &items[next++]));
        assert(cd_parallel_count(&pool) <= 80);
    }
    assert(next == JOBS && cd_parallel_count(&pool) == 1);
    assert(!pthread_mutex_lock(&state.lock));
    assert(!items[0].finished && state.started == JOBS);
    assert(state.finished == JOBS - 1 && state.active == 1);
    state.release_first = 1;
    assert(!pthread_cond_broadcast(&state.changed));
    assert(!pthread_mutex_unlock(&state.lock));
    assert(!cd_parallel_take(&pool, &result, &error));
    assert(result == &items[0] && error == 0);
    assert(cd_parallel_count(&pool) == 0);
    cd_parallel_fini(&pool); state_fini(&state);
    puts("PASS all 40 workers reach work; 399 jobs replenish/reuse 80 slots behind blocked first job");
}
static void unordered_shutdown(void) {
    struct cd_parallel pool; struct test_state state;
    struct test_item items[4]; state_init(&state, 1);
    assert(!cd_parallel_init_unordered(&pool, 2, work));
    for (unsigned i = 0; i < 4; i++) {
        items[i] = item_init(&state, i);
        assert(!cd_parallel_submit(&pool, &items[i]));
    }
    assert(!pthread_mutex_lock(&state.lock));
    while (state.started < 2)
        assert(!pthread_cond_wait(&state.changed, &state.lock));
    assert(!pthread_mutex_unlock(&state.lock));
    struct fini_context context = { &pool, &state }; pthread_t thread;
    assert(!pthread_create(&thread, NULL, finish, &context));
    assert(!pthread_mutex_lock(&state.lock));
    while (!state.fini_entered)
        assert(!pthread_cond_wait(&state.changed, &state.lock));
    assert(!state.fini_done && state.active == 2);
    state.release_first = state.release_others = 1;
    assert(!pthread_cond_broadcast(&state.changed));
    assert(!pthread_mutex_unlock(&state.lock));
    assert(!pthread_join(thread, NULL)); assert(state.fini_done);
    state_fini(&state);
    puts("PASS unordered shutdown joins active callbacks before item cleanup");
}
int main(void) {
    (void) setvbuf(stdout, NULL, _IONBF, 0);
    assert(cd_ordered_tests_main() == 0);
    (void) alarm(5);
    unordered_replenishment(); unordered_shutdown();
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='parallel-unordered-') as directory:
    directory = Path(directory)
    (directory / 'chunkdedup_parallel.h').write_bytes(header.read_bytes())
    # Reuse the existing ordered API/cancellation suite against the same header.
    (directory / 'ordered.c').write_bytes(existing.read_bytes())
    (directory / 'test.c').write_text('#define main cd_ordered_tests_main\n#include "ordered.c"\n' + extra)
    for sanitized in (False, True):
        command = [os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra',
                   '-Werror', '-pthread', '-g', '-I' + str(directory)]
        if sanitized: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        if a.ordered_control: command += ['-DTEST_ORDERED_CONTROL']
        command += [str(directory / 'test.c'), '-o', str(directory / 'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(directory / 'test')], check=True, timeout=15,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                                UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
