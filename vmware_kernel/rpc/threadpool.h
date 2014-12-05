#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__

#include <pthread.h>
#include "dll.h"
#include "bufpool.h"

typedef enum {
	TP_UNINITIALIZED = 0,
	TP_INITIALIZED,
	TP_SHUTTING,
} pool_state_t;

typedef struct thread_pool {
	pthread_t       *threads;
	int             active;

	pthread_mutex_t work_lock;
	pthread_cond_t  work_cond;
	dll_t           work_list;
	int             work_pend;
	pool_state_t    state;

	bufpool_t       pool;

	int             nthreads;
} thread_pool_t;

typedef struct work {
	dll_t list;
	void  *data;
	void  (*work_fn)(struct work *, void *data);
} work_t;

int thread_pool_init(thread_pool_t *tp, int nthreads);
void thread_pool_deinit(thread_pool_t *tp);

work_t *new_work(thread_pool_t *tp);
void free_work(thread_pool_t *tp, work_t *w);
int schedule_work(thread_pool_t *tp, work_t *w);
#endif
