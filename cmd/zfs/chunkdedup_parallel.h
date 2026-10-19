/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef _CHUNKDEDUP_PARALLEL_H
#define _CHUNKDEDUP_PARALLEL_H

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * A bounded, ordered worker pool. Only the coordinator submits, takes and
 * destroys the pool. Callbacks own no queue state and run without its mutex.
 * Items always remain caller-owned, including untaken items after fini().
 */
#define	CD_PARALLEL_MAX_THREADS	64

enum cd_parallel_state {
	CD_PARALLEL_EMPTY, CD_PARALLEL_QUEUED,
	CD_PARALLEL_RUNNING, CD_PARALLEL_DONE
};

struct cd_parallel_slot {
	void *item;
	int error;
	enum cd_parallel_state state;
};

struct cd_parallel {
	pthread_mutex_t lock;
	pthread_cond_t work_ready, result_ready;
	pthread_t *threads;
	struct cd_parallel_slot *slots;
	size_t capacity, count, head, tail, next;
	unsigned int created;
	int stopping, initialized;
	int (*work)(void *);
};

static void *
cd_parallel_worker(void *argument)
{
	struct cd_parallel *pool = argument;
	(void) pthread_mutex_lock(&pool->lock);
	for (;;) {
		while (!pool->stopping &&
		    pool->slots[pool->next].state != CD_PARALLEL_QUEUED)
			(void) pthread_cond_wait(&pool->work_ready, &pool->lock);
		if (pool->stopping)
			break;
		struct cd_parallel_slot *slot = &pool->slots[pool->next];
		pool->next = (pool->next + 1) % pool->capacity;
		slot->state = CD_PARALLEL_RUNNING;
		void *item = slot->item;
		(void) pthread_mutex_unlock(&pool->lock);
		int error = pool->work(item);
		(void) pthread_mutex_lock(&pool->lock);
		slot->error = error;
		slot->state = CD_PARALLEL_DONE;
		(void) pthread_cond_broadcast(&pool->result_ready);
	}
	(void) pthread_mutex_unlock(&pool->lock);
	return (NULL);
}

static inline void
cd_parallel_fini(struct cd_parallel *pool)
{
	if (pool == NULL || !pool->initialized)
		return;
	(void) pthread_mutex_lock(&pool->lock);
	pool->stopping = 1;
	(void) pthread_cond_broadcast(&pool->work_ready);
	(void) pthread_cond_broadcast(&pool->result_ready);
	(void) pthread_mutex_unlock(&pool->lock);
	for (unsigned int i = 0; i < pool->created; i++)
		(void) pthread_join(pool->threads[i], NULL);
	(void) pthread_cond_destroy(&pool->result_ready);
	(void) pthread_cond_destroy(&pool->work_ready);
	(void) pthread_mutex_destroy(&pool->lock);
	free(pool->slots);
	free(pool->threads);
	memset(pool, 0, sizeof (*pool));
}

static inline int
cd_parallel_init(struct cd_parallel *pool, unsigned int threads,
    int (*work)(void *))
{
	if (pool == NULL || work == NULL || threads == 0 ||
	    threads > CD_PARALLEL_MAX_THREADS)
		return (EINVAL);
	memset(pool, 0, sizeof (*pool));
	pool->capacity = (size_t)threads * 2;
	pool->work = work;
	pool->threads = calloc(threads, sizeof (*pool->threads));
	pool->slots = calloc(pool->capacity, sizeof (*pool->slots));
	int error = ENOMEM;
	if (pool->threads == NULL || pool->slots == NULL)
		goto free_storage;
	if ((error = pthread_mutex_init(&pool->lock, NULL)) != 0)
		goto free_storage;
	if ((error = pthread_cond_init(&pool->work_ready, NULL)) != 0)
		goto destroy_lock;
	if ((error = pthread_cond_init(&pool->result_ready, NULL)) != 0)
		goto destroy_work;
	pool->initialized = 1;
	for (unsigned int i = 0; i < threads; i++) {
		error = pthread_create(&pool->threads[i], NULL,
		    cd_parallel_worker, pool);
		if (error != 0) {
			cd_parallel_fini(pool);
			return (error);
		}
		pool->created++;
	}
	return (0);

destroy_work:
	(void) pthread_cond_destroy(&pool->work_ready);
destroy_lock:
	(void) pthread_mutex_destroy(&pool->lock);
free_storage:
	free(pool->slots);
	free(pool->threads);
	memset(pool, 0, sizeof (*pool));
	return (error);
}

static inline size_t
cd_parallel_count(struct cd_parallel *pool)
{
	(void) pthread_mutex_lock(&pool->lock);
	size_t count = pool->count;
	(void) pthread_mutex_unlock(&pool->lock);
	return (count);
}

static inline int
cd_parallel_full(struct cd_parallel *pool)
{
	return (cd_parallel_count(pool) == pool->capacity);
}

static inline int
cd_parallel_submit(struct cd_parallel *pool, void *item)
{
	(void) pthread_mutex_lock(&pool->lock);
	int error = 0;
	if (pool->stopping) {
		error = ECANCELED;
	} else if (pool->count == pool->capacity) {
		error = EAGAIN;
	} else {
		struct cd_parallel_slot *slot = &pool->slots[pool->tail];
		slot->item = item;
		slot->error = 0;
		slot->state = CD_PARALLEL_QUEUED;
		pool->tail = (pool->tail + 1) % pool->capacity;
		pool->count++;
		(void) pthread_cond_signal(&pool->work_ready);
	}
	(void) pthread_mutex_unlock(&pool->lock);
	return (error);
}

/* Results leave the pool in dispatch order, regardless of completion order. */
static inline int
cd_parallel_take(struct cd_parallel *pool, void **item, int *work_error)
{
	if (item == NULL || work_error == NULL)
		return (EINVAL);
	(void) pthread_mutex_lock(&pool->lock);
	int error = 0;
	if (pool->count == 0) {
		error = ENOENT;
	} else {
		struct cd_parallel_slot *slot = &pool->slots[pool->head];
		while (slot->state != CD_PARALLEL_DONE && !pool->stopping)
			(void) pthread_cond_wait(&pool->result_ready, &pool->lock);
		if (slot->state != CD_PARALLEL_DONE) {
			error = ECANCELED;
		} else {
			*item = slot->item;
			*work_error = slot->error;
			memset(slot, 0, sizeof (*slot));
			pool->head = (pool->head + 1) % pool->capacity;
			pool->count--;
		}
	}
	(void) pthread_mutex_unlock(&pool->lock);
	return (error);
}

#endif /* _CHUNKDEDUP_PARALLEL_H */
