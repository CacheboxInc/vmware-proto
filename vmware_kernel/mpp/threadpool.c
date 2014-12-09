#include "vmware_include.h"

#include "threadpool.h"

VMK_ReturnStatus _worker_thread(void *args)
{
	thread_pool_t *tp = args;
	int           rc;
	dll_t         *f;
	work_t        *w;

	while (1) {
		vmk_WarningMessage("%s taking lock.\n", __func__);
		rc = pthread_mutex_lock(tp->work_lock);
		assert(rc == 0);

		if (DLL_ISEMPTY(&tp->work_list)) {
			if (tp->state == TP_SHUTTING) {
				tp->active--;
				pthread_mutex_unlock(tp->work_lock);
				break;
			}

			/* block */
			rc = vmk_WorldWait((vmk_WorldEventID) &tp->work_pend,
					tp->work_lock, VMK_TIMEOUT_UNLIMITED_MS,
					"_worker_thread blocked.\n");

			assert(rc == VMK_OK);
			continue;
		}

		/* get next work */
		assert(!DLL_ISEMPTY(&tp->work_list));
		f = DLL_NEXT(&tp->work_list);
		DLL_REM(f);
		tp->work_pend--;
		pthread_mutex_unlock(tp->work_lock);

		/* work on it */
		w = container_of(f, work_t, list);
		w->work_fn(w, w->data);

		/* give work buffer back */
		bufpool_put(&tp->pool, (char *) w);
	}

	vmk_WarningMessage("%s unlocking spin lock.\n", __func__);
	return VMK_OK;
}

void _thread_pool_deinit(thread_pool_t *tp)
{
	int       rc;
	int       i;
	pthread_t *h;

	if (tp->threads != NULL) {
		rc = pthread_mutex_lock(tp->work_lock);
		assert(rc == 0);

		tp->state = TP_SHUTTING;

		rc = pthread_mutex_unlock(tp->work_lock);
		assert(rc == 0);

		vmk_WarningMessage("%s waking up waiting worlds.\n", __func__);

		rc = vmk_WorldWakeup((vmk_WorldEventID) &tp->work_pend);
		assert(rc == VMK_OK);

		vmk_WarningMessage("%s done waking up waiting worlds.\n", __func__);
		for (i = 0; i < tp->nthreads; i++) {
			h = &tp->threads[i];
			if (h == NULL) {
				break;
			}

		vmk_WarningMessage("%s calling pthread_join.\n", __func__);
			(void) pthread_join(*h, NULL);
		}
		free(tp->heap_id, tp->threads);
	}

		vmk_WarningMessage("%s all worlds ended. almost finished.\n", __func__);
	assert(tp->active == 0);
	assert(DLL_ISEMPTY(&tp->work_list));

	pthread_mutex_destroy(tp->work_lock);

	bufpool_deinit(&tp->pool);
	vmk_HeapDestroy(tp->heap_id);
	memset(tp, 0, sizeof(*tp));
}

int thread_pool_init(thread_pool_t *tp, const char *name,
		module_global_t *module, int nthreads)
{
	int  i;
	int  rc;
	int  nwork;
	int  nwork_max;
	char n[128];
	char n1[128];
	pthread_t *h;

	memset(tp, 0, sizeof(*tp));

	rc = vmware_name(n, name, "threadpool", sizeof(n));
	assert(rc == 0);

	rc = vmware_heap_create(&tp->heap_id, n, module, nthreads,
			sizeof(*tp->threads));
	if (rc < 0) {
		return -1;
	}

	nwork     = nthreads;
	nwork_max = nthreads * 4;

	rc = bufpool_init(&tp->pool, n, module, sizeof(struct work), nwork,
			nwork_max);
	if (rc < 0) {
		goto error;
	}

	/* init locks */
	rc = pthread_mutex_init(&tp->work_lock, n, module);
	if (rc < 0) {
		goto error;
	}

	tp->threads = calloc(tp->heap_id, nthreads, sizeof(*tp->threads));
	if (tp->threads == NULL) {
		goto error;
	}

	DLL_INIT(&tp->work_list);

	for (i = 0; i < nthreads; i++) {
		h  = &tp->threads[i];
		rc = vmk_StringFormat(n1, sizeof(n1), NULL, "%s-%d", n, i);
		if (rc != VMK_OK) {
			goto error;
		}

		rc = pthread_create(h, n1, module, _worker_thread, tp);
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

int schedule_work(thread_pool_t *tp, work_t *w)
{
	assert(tp->state == TP_INITIALIZED);

	pthread_mutex_lock(tp->work_lock);
	DLL_REVADD(&tp->work_list, &w->list);
	tp->work_pend++;
	pthread_mutex_unlock(tp->work_lock);

	vmk_WorldWakeup((vmk_WorldEventID) &tp->work_pend);
	return 0;
}
