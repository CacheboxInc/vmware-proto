#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "threadpool.h"

void *_worker_thread(void *args)
{
	thread_pool_t *tp = args;
	int           rc;
	dll_t         *f;
	work_t        *w;

	while (1) {
		rc = pthread_mutex_lock(&tp->work_lock);
		assert(rc == 0);

		if (DLL_ISEMPTY(&tp->work_list)) {
			/* block */
			if (tp->state == TP_SHUTTING) {
				break;
			}

			rc = pthread_cond_wait(&tp->work_cond, &tp->work_lock);
			assert(rc == 0);

			if (DLL_ISEMPTY(&tp->work_list)) {
				if (tp->state == TP_SHUTTING) {
					break;
				}
				pthread_mutex_unlock(&tp->work_lock);
				continue;
			}
		}

		/* get next work */
		assert(!DLL_ISEMPTY(&tp->work_list));
		f = DLL_NEXT(&tp->work_list);
		DLL_REM(f);
		pthread_mutex_unlock(&tp->work_lock);

		/* work on it */
		w = container_of(f, work_t, list);
		w->work_fn(w, w->data);

		/* give work buffer back */
		bufpool_put(&tp->pool, (char *) w);
	}

	tp->active--;
	pthread_mutex_unlock(&tp->work_lock);
	return NULL;
}

void _thread_pool_deinit(thread_pool_t *tp)
{
	int       rc;
	int       i;
	pthread_t *h;

	if (tp->threads != NULL) {
		rc = pthread_mutex_lock(&tp->work_lock);
		assert(rc == 0);

		tp->state = TP_SHUTTING;

		rc = pthread_cond_broadcast(&tp->work_cond);
		assert(rc == 0);

		rc = pthread_mutex_unlock(&tp->work_lock);
		assert(rc == 0);

		for (i = 0; i < tp->nthreads; i++) {
			h = &tp->threads[i];
			if (h == NULL) {
				break;
			}

			(void) pthread_join(*h, NULL);
		}
		free(tp->threads);
	}

	assert(tp->active == 0);
	assert(DLL_ISEMPTY(&tp->work_list));

	pthread_mutex_destroy(&tp->work_lock);
	pthread_cond_destroy(&tp->work_cond);

	bufpool_deinit(&tp->pool);
	memset(tp, 0, sizeof(*tp));
}

int thread_pool_init(thread_pool_t *tp, int nthreads)
{
	int       i;
	int       rc;
	int       nwork;
	int       nwork_max;
	pthread_t *h;

	memset(tp, 0, sizeof(*tp));

	nwork     = nthreads;
	nwork_max = nthreads * 4;

	rc = bufpool_init(&tp->pool, "thread-pool", sizeof(struct work), nwork,
			nwork_max);
	if (rc < 0) {
		goto error;
	}

	/* init locks */
	rc = pthread_mutex_init(&tp->work_lock, NULL);
	if (rc < 0) {
		goto error;
	}

	/* init conditional vars */
	rc = pthread_cond_init(&tp->work_cond, NULL);
	if (rc < 0) {
		goto error;
	}

	tp->threads = calloc(nthreads, sizeof(*tp->threads));
	if (tp->threads == NULL) {
		goto error;
	}

	DLL_INIT(&tp->work_list);

	for (i = 0; i < nthreads; i++) {
		h = &tp->threads[i];
		rc = pthread_create(h, NULL, _worker_thread, tp);
		if (rc < 0) {
			goto error;
		}
	}

	tp->state    = TP_INITIALIZED;
	tp->active   = nthreads;
	tp->nthreads = nthreads;
	return 0;

error:
	_thread_pool_deinit(tp);
	return -1;
}

void thread_pool_deinit(thread_pool_t *tp)
{
	if (tp->state != TP_INITIALIZED) {
		return;
	}
	_thread_pool_deinit(tp);
}

work_t *new_work(thread_pool_t *tp)
{
	int    rc;
	work_t *w = NULL;;

	if (tp->state != TP_INITIALIZED) {
		return NULL;
	}

	rc = bufpool_get(&tp->pool, (char **) &w, 0);
	assert(rc == 0 && w != NULL);

	return w;
}

void free_work(thread_pool_t *tp, work_t *w)
{
	pthread_mutex_lock(&tp->work_lock);
	DLL_REM(&w->list);
	pthread_mutex_unlock(&tp->work_lock);

	bufpool_put(&tp->pool, (char *) w);
}

int schedule_work(thread_pool_t *tp, work_t *w)
{
	assert(tp->state == TP_INITIALIZED);

	pthread_mutex_lock(&tp->work_lock);
	DLL_REVADD(&tp->work_list, &w->list);
	pthread_cond_signal(&tp->work_cond);
	pthread_mutex_unlock(&tp->work_lock);

	return 0;
}
