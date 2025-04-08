#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include "rlist.h"

struct thread_task {
	thread_task_f function;
	void *arg;
	void *result;

	pthread_mutex_t mutex;
	pthread_cond_t is_finished_cond;

	bool is_joined;

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
		pthread_mutex_unlock(&pool->mutex);

		task->status = TASK_RUNNING;
		task->result = task->function(task->arg);

		pthread_mutex_lock(&pool->mutex);

		pool->free_thread_count++;
		task->status = TASK_FINISHED;
		pthread_cond_signal(&task->is_finished_cond);
		pthread_mutex_unlock(&task->mutex);
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

	void *threads = malloc(sizeof(pthread_t) * TPOOL_MAX_THREADS);
	*pool = malloc(sizeof(struct thread_pool));
	(*pool)->threads = threads;

	(*pool)->active_thread_count = 0;
	(*pool)->free_thread_count = 0;
	(*pool)->max_thread_count = max_thread_count;
	(*pool)->is_shutdown = false;

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
	if (!rlist_empty(&pool->tasks) || pool->active_thread_count != pool->free_thread_count) {
		return TPOOL_ERR_HAS_TASKS;
	}

	free(pool->threads);
	free(pool);

	return TPOOL_NO_ERR;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	pthread_mutex_lock(&pool->mutex);

	if (pool->tasks_count >= TPOOL_MAX_TASKS) {
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
	task->mutex = pool->mutex;

	pthread_cond_signal(&pool->cond);
	pthread_mutex_unlock(&pool->mutex);

	return TPOOL_NO_ERR;
}

int
thread_task_new(struct thread_task **task, thread_task_f function, void *arg)
{
	*task = malloc(sizeof(struct thread_task));

	(*task)->function = function;
	(*task)->arg = arg;

	(*task)->status = TASK_CREATED;

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
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	(void)timeout;
	(void)result;
	return TPOOL_ERR_NOT_IMPLEMENTED;
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
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif
