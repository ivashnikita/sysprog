#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include "rlist.h"
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

struct thread_task {
	thread_task_f function;
	void *arg;
	void *result;

	pthread_mutex_t mutex;
	pthread_cond_t is_finished_cond;

	bool is_joined;
	bool is_detached;

	enum task_status status;
	struct rlist list_node;
};

struct thread_pool {
	pthread_t *threads;

	bool is_shutdown;

	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct rlist tasks;

	int max_thread_count;
	int active_thread_count;
	int free_thread_count;
	int tasks_count;
	int in_progress_count;
};

int
is_correct_thread_count(int max_thread_count)
{
	return max_thread_count > 0 && max_thread_count <= TPOOL_MAX_THREADS;
}

void *worker(void *arg) {
	struct thread_pool *pool = arg;

	pthread_mutex_lock(&pool->mutex);

	while (true) {
		while (rlist_empty(&pool->tasks) && !pool->is_shutdown) {
			pthread_cond_wait(&pool->cond, &pool->mutex);
		}

		if (pool->is_shutdown) {
			break;
		}

		struct thread_task *task = rlist_shift_entry(&pool->tasks, struct thread_task, list_node);
		pool->free_thread_count--;
		pool->tasks_count--;
		pool->in_progress_count++;
		pthread_mutex_unlock(&pool->mutex);

		task->status = TASK_RUNNING;
		void *result = task->function(task->arg);
		task->result = result;

		pthread_mutex_lock(&pool->mutex);
		pool->free_thread_count++;
		pool->in_progress_count--;
		pthread_mutex_unlock(&pool->mutex);

		pthread_mutex_lock(&task->mutex);
		task->status = TASK_FINISHED;

		if (task->is_detached) {
			pthread_mutex_unlock(&task->mutex);
			free(task);
		} else {
			pthread_cond_signal(&task->is_finished_cond);
			pthread_mutex_unlock(&task->mutex);
		}

		pthread_mutex_lock(&pool->mutex);
	}

	pthread_mutex_unlock(&pool->mutex);

	return NULL;
}

int
thread_pool_new(int max_thread_count, struct thread_pool **pool)
{
	if (!is_correct_thread_count(max_thread_count)) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	void *threads = calloc(TPOOL_MAX_THREADS, sizeof(pthread_t));
	*pool = calloc(1, sizeof(struct thread_pool));
	(*pool)->threads = threads;

	(*pool)->active_thread_count = 0;
	(*pool)->free_thread_count = 0;
	(*pool)->max_thread_count = max_thread_count;
	(*pool)->is_shutdown = false;
	(*pool)->in_progress_count = 0;

	rlist_create(&(*pool)->tasks);

	pthread_mutex_init(&(*pool)->mutex, NULL);
	pthread_cond_init(&(*pool)->cond, NULL);

	return TPOOL_NO_ERR;
}

int
thread_pool_thread_count(const struct thread_pool *pool)
{
	return pool->active_thread_count;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	pthread_mutex_lock(&pool->mutex);

	if (!rlist_empty(&pool->tasks) || pool->active_thread_count != pool->free_thread_count) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_HAS_TASKS;
	}

	pool->is_shutdown = true;
	pthread_cond_broadcast(&pool->cond);
	pthread_mutex_unlock(&pool->mutex);

	for (int i = 0; i < pool->active_thread_count; i++) {
		pthread_join(pool->threads[i], NULL);
	}

	pthread_mutex_destroy(&pool->mutex);
	pthread_cond_destroy(&pool->cond);

	free(pool->threads);
	free(pool);

	return TPOOL_NO_ERR;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	pthread_mutex_lock(&pool->mutex);

	if (pool->tasks_count + pool->in_progress_count >= TPOOL_MAX_TASKS) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	rlist_add_tail_entry(&pool->tasks, task, list_node);
	pool->tasks_count++;

	if (pool->active_thread_count < pool->max_thread_count && pool->free_thread_count == 0) {
		pthread_create(&pool->threads[pool->active_thread_count], NULL, worker, pool);
		pool->active_thread_count++;
		pool->free_thread_count++;
	}

	task->status = TASK_PUSHED;
	task->is_joined = false;
	task->is_detached = false;
	pthread_mutex_init(&task->mutex, NULL);
	pthread_cond_init(&task->is_finished_cond, NULL);

	pthread_cond_signal(&pool->cond);
	pthread_mutex_unlock(&pool->mutex);

	return TPOOL_NO_ERR;
}

int
thread_task_new(struct thread_task **task, thread_task_f function, void *arg)
{
	*task = calloc(1, sizeof(struct thread_task));

	(*task)->function = function;
	(*task)->arg = arg;

	(*task)->status = TASK_CREATED;
	(*task)->is_joined = false;
	(*task)->is_detached = false;

	pthread_mutex_init(&(*task)->mutex, NULL);
	pthread_cond_init(&(*task)->is_finished_cond, NULL);

	return TPOOL_NO_ERR;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	return task->status == TASK_FINISHED;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	return task->status == TASK_RUNNING;
}

int
thread_task_join(struct thread_task *task, void **result)
{
	if (task->status == TASK_CREATED) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	pthread_mutex_lock(&task->mutex);

	while (task->status != TASK_FINISHED) {
		pthread_cond_wait(&task->is_finished_cond, &task->mutex);
	}

	*result = task->result;
	task->is_joined = true;

	pthread_mutex_unlock(&task->mutex);

	return TPOOL_NO_ERR;
}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
	if (task->status == TASK_CREATED) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	pthread_mutex_lock(&task->mutex);

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	time_t seconds = (time_t)timeout;
	time_t ns = (time_t)((timeout - (long)seconds) * 1e9);

	ts.tv_sec += seconds;
	ts.tv_nsec += ns;

	long ts_total_ns = ts.tv_sec * 1e9 + ts.tv_nsec;

	while (task->status != TASK_FINISHED) {
		pthread_cond_timedwait(&task->is_finished_cond, &task->mutex, &ts);

		struct timespec current_ts;
		clock_gettime(CLOCK_MONOTONIC, &current_ts);

		long current_total_ns = current_ts.tv_sec * 1e9 + current_ts.tv_nsec;  

		if (current_total_ns > ts_total_ns) {
			break;
		}
	}

	if (task->status == TASK_FINISHED) {
		*result = task->result;
		task->is_joined = true;
		pthread_mutex_unlock(&task->mutex);

		return TPOOL_NO_ERR;
	}

	pthread_mutex_unlock(&task->mutex);
	return TPOOL_ERR_TIMEOUT;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	if (task->status != TASK_CREATED && !task->is_joined) {
		return TPOOL_ERR_TASK_IN_POOL;
	}

	free(task);

	return TPOOL_NO_ERR;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	if (task->status == TASK_CREATED) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	pthread_mutex_lock(&task->mutex);

	if (task->status == TASK_FINISHED) {
		pthread_mutex_unlock(&task->mutex);
		free(task);
	} else {
		task->is_detached = true;
		pthread_mutex_unlock(&task->mutex);
	}

	return TPOOL_NO_ERR;
}

#endif
