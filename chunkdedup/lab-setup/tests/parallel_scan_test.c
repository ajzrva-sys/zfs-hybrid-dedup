/* SPDX-License-Identifier: MIT
 * Exercise the actual bounded scan executor. No filesystem, libzfs or ioctl
 * operations are mocked: callbacks represent independent scan jobs only.
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "chunkdedup_parallel.h"

#define TEST_JOBS 200

struct test_state {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t owner;
    unsigned started, finished, active, peak;
    int gated, release_first, release_others, fini_entered, fini_done;
};

struct test_item {
    struct test_state *state;
    unsigned id;
    int error, finished;
    uint64_t magic;
};

static void
state_init(struct test_state *state, int gated)
{
    *state = (struct test_state){ .owner = pthread_self(), .gated = gated };
    assert(pthread_mutex_init(&state->lock, NULL) == 0);
    assert(pthread_cond_init(&state->changed, NULL) == 0);
}

static void
state_fini(struct test_state *state)
{
    assert(state->active == 0 && state->started == state->finished);
    assert(pthread_cond_destroy(&state->changed) == 0);
    assert(pthread_mutex_destroy(&state->lock) == 0);
}

static struct test_item
item_init(struct test_state *state, unsigned id)
{
    return ((struct test_item){ .state = state, .id = id,
        .error = id == 3 ? EIO : 0, .magic = UINT64_C(0xaaccee1234567890) });
}

static int
work(void *argument)
{
    struct test_item *item = argument;
    struct test_state *state = item->state;
    assert(!pthread_equal(pthread_self(), state->owner));
    assert(pthread_mutex_lock(&state->lock) == 0);
    assert(item->magic == UINT64_C(0xaaccee1234567890));
    state->started++;
    state->active++;
    if (state->active > state->peak)
        state->peak = state->active;
    assert(pthread_cond_broadcast(&state->changed) == 0);
    while (state->gated && !(item->id == 0 ?
        state->release_first : state->release_others))
        assert(pthread_cond_wait(&state->changed, &state->lock) == 0);
    item->finished = 1;
    state->finished++;
    state->active--;
    assert(pthread_cond_broadcast(&state->changed) == 0);
    assert(pthread_mutex_unlock(&state->lock) == 0);
    return (item->error);
}

static void
overlap_and_order(void)
{
    struct cd_parallel pool;
    struct test_state state;
    struct test_item items[9];
    state_init(&state, 1);
    assert(cd_parallel_init(&pool, 4, work) == 0);
    assert(cd_parallel_count(&pool) == 0 && !cd_parallel_full(&pool));
    void *result = NULL;
    int error = -1;
    assert(cd_parallel_take(&pool, &result, &error) == ENOENT);
    for (unsigned i = 0; i < 9; i++)
        items[i] = item_init(&state, i);
    for (unsigned i = 0; i < 8; i++) {
        assert(cd_parallel_submit(&pool, &items[i]) == 0);
        assert(cd_parallel_count(&pool) == i + 1);
    }
    assert(cd_parallel_full(&pool));
    assert(cd_parallel_submit(&pool, &items[8]) == EAGAIN);
    assert(cd_parallel_count(&pool) == 8);
    assert(pthread_mutex_lock(&state.lock) == 0);
    while (state.started < 4)
        assert(pthread_cond_wait(&state.changed, &state.lock) == 0);
    assert(state.active == 4 && state.peak == 4);
    state.release_others = 1;
    assert(pthread_cond_broadcast(&state.changed) == 0);
    while (state.finished < 7)
        assert(pthread_cond_wait(&state.changed, &state.lock) == 0);
    assert(!items[0].finished);
    assert(state.started == 8);
    state.release_first = 1;
    assert(pthread_cond_broadcast(&state.changed) == 0);
    assert(pthread_mutex_unlock(&state.lock) == 0);
    for (unsigned i = 0; i < 8; i++) {
        assert(cd_parallel_take(&pool, &result, &error) == 0);
        assert(result == &items[i] && error == items[i].error);
        assert(items[i].finished);
        assert(cd_parallel_count(&pool) == 7 - i);
    }
    assert(cd_parallel_take(&pool, &result, &error) == ENOENT);
    cd_parallel_fini(&pool);
    assert(!items[8].finished);
    assert(state.started == 8 && state.finished == 8);
    state_fini(&state);
    puts("PASS four scan callbacks overlap; eight-item window rejects overflow");
    puts("PASS out-of-order completion merges in dispatch order with exact errors");
}

static void
slot_reuse(unsigned threads)
{
    struct cd_parallel pool;
    struct test_state state;
    struct test_item items[TEST_JOBS];
    unsigned taken = 0;
    state_init(&state, 0);
    assert(cd_parallel_init(&pool, threads, work) == 0);
    for (unsigned i = 0; i < TEST_JOBS; i++) {
        items[i] = item_init(&state, i);
        if (cd_parallel_full(&pool)) {
            void *result = NULL;
            int error = -1;
            assert(cd_parallel_take(&pool, &result, &error) == 0);
            assert(result == &items[taken]);
            assert(error == items[taken].error);
            taken++;
        }
        assert(cd_parallel_submit(&pool, &items[i]) == 0);
        assert(cd_parallel_count(&pool) <= 2 * threads);
    }
    while (cd_parallel_count(&pool) != 0) {
        void *result = NULL;
        int error = -1;
        assert(cd_parallel_take(&pool, &result, &error) == 0);
        assert(result == &items[taken] && error == items[taken].error);
        taken++;
    }
    assert(taken == TEST_JOBS);
    cd_parallel_fini(&pool);
    assert(state.started == TEST_JOBS && state.peak <= threads);
    state_fini(&state);
    printf("PASS %u-thread executor reuses bounded slots for %u ordered jobs\n",
        threads, TEST_JOBS);
}

struct fini_context {
    struct cd_parallel *pool;
    struct test_state *state;
};

static void *
finish(void *argument)
{
    struct fini_context *context = argument;
    struct test_state *state = context->state;
    assert(pthread_mutex_lock(&state->lock) == 0);
    state->fini_entered = 1;
    assert(pthread_cond_broadcast(&state->changed) == 0);
    assert(pthread_mutex_unlock(&state->lock) == 0);
    cd_parallel_fini(context->pool);
    assert(pthread_mutex_lock(&state->lock) == 0);
    assert(state->active == 0 && state->started == state->finished);
    state->fini_done = 1;
    assert(pthread_cond_broadcast(&state->changed) == 0);
    assert(pthread_mutex_unlock(&state->lock) == 0);
    return (NULL);
}

static void
join_before_cleanup(void)
{
    struct cd_parallel pool;
    struct test_state state;
    struct test_item *items = calloc(4, sizeof (*items));
    assert(items != NULL);
    state_init(&state, 1);
    assert(cd_parallel_init(&pool, 2, work) == 0);
    for (unsigned i = 0; i < 4; i++) {
        items[i] = item_init(&state, i);
        assert(cd_parallel_submit(&pool, &items[i]) == 0);
    }
    assert(pthread_mutex_lock(&state.lock) == 0);
    while (state.started < 2)
        assert(pthread_cond_wait(&state.changed, &state.lock) == 0);
    assert(state.active == 2);
    assert(pthread_mutex_unlock(&state.lock) == 0);
    struct fini_context context = { &pool, &state };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, finish, &context) == 0);
    assert(pthread_mutex_lock(&state.lock) == 0);
    while (!state.fini_entered)
        assert(pthread_cond_wait(&state.changed, &state.lock) == 0);
    assert(!state.fini_done && state.active == 2);
    state.release_first = state.release_others = 1;
    assert(pthread_cond_broadcast(&state.changed) == 0);
    assert(pthread_mutex_unlock(&state.lock) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(state.fini_done && state.started >= 2 && state.started <= 4);
    for (unsigned i = 0; i < 4; i++) {
        assert(items[i].magic == UINT64_C(0xaaccee1234567890));
        items[i].magic = 0;
    }
    free(items);
    state_fini(&state);
    puts("PASS shutdown joins active scans before caller-owned item cleanup");
}

int
main(void)
{
    (void) alarm(30);
    struct cd_parallel invalid;
    assert(cd_parallel_init(&invalid, 0, work) == EINVAL);
    assert(cd_parallel_init(&invalid, 65, work) == EINVAL);
    overlap_and_order();
    slot_reuse(1);
    slot_reuse(4);
    join_before_cleanup();
    puts("PASS actual production parallel executor; native integration is separate");
    return (0);
}
