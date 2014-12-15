#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bufpool.h"

/*
 * initialize a buffer pool of up to nbufs count of
 * fixed size buffers of size = bufsize.
 * If more than nbufs buffers are requested, will try to malloc more
 * up to a limit of nmax buffers.
 * Returns number of buffers allocated.
 */

int bufpool_init_reserve(bufpool_t *bp, char *name, size_t bufsize, size_t nbufs,
		int reserve, size_t nmax)
{
	char  *n;
	int   rc;
	dll_t *buf;

	n = strdup(name);
	assert(n != NULL);

	assert(bp);
	assert(nbufs > 0 && nmax >= nbufs);
	if (bufsize < sizeof(dll_t)) {
		bufsize = sizeof(dll_t);
	}
	BZERO(bp);
	queue_init(&bp->freelist);
	bp->bufsize = bufsize;
	bp->nbufs   = nbufs;
	bp->nmax    = nmax;
	bp->state   = INITIALIZED;
	bp->reserve = reserve;
	bp->name    = n;

	rc = pthread_mutex_init(&bp->lock, NULL);
	assert(rc == 0);

	rc = pthread_cond_init(&bp->rendez, NULL);
	assert(rc == 0);

	for (bp->owned = 0; bp->owned < nbufs; bp->owned++) {
		if ((buf = (dll_t *)malloc(bufsize)) == NULL) {
			return bp->owned;
		}
		DLL_INIT(buf);
		queue_add(&bp->freelist, buf);
	}
	return bp->owned;
}

int bufpool_init(bufpool_t *bp, char *name, size_t bufsize, size_t nbufs, size_t nmax)
{
	return bufpool_init_reserve(bp, name, bufsize, nbufs, 0, nmax);
}
/*
 * free all buffers on the freelist.
 */
void bufpool_deinit(bufpool_t *bp)
{
	int             rc;
	struct timespec ts;
	dll_t           *dllp;

	assert(bp != NULL);

	rc = pthread_mutex_lock(&bp->lock);
	assert(rc == 0);

	if (bp->state != INITIALIZED) {
		rc = pthread_mutex_unlock(&bp->lock);
		return;
	}

	assert(bp->state == INITIALIZED);
	bp->state = DEINITING;

	while (bp->issued != 0) {
		/* block deinit until last buffer is freed */
		ts.tv_sec  = 1;
		ts.tv_nsec = 0;

		rc = pthread_cond_timedwait(&bp->rendez, &bp->lock, &ts);
	}
	assert(bp->issued == 0);

	while (queue_rem(&bp->freelist, &dllp) == 0) {
		free(dllp);
		bp->owned--;
	}
	queue_deinit(&bp->freelist);

	bp->state = DEINTED;

	pthread_mutex_unlock(&bp->lock);

	pthread_mutex_destroy(&bp->lock);
	pthread_cond_destroy(&bp->rendez);

	free(bp->name);
}

/*
 * Get a buffer from the freelist
 * Otherwise try to calloc a new one
 * Otherwise sleep for one
 * returns 0 on success, -1 if buffer not found and noblock true
 */
int _bufpool_get(bufpool_t *bp, char **bufp, int noblock, int alloc_reserve)
{
	int   rc;
	dll_t *dllp;
	int   res;

	rc = pthread_mutex_lock(&bp->lock);
	assert(rc == 0);

	assert(bp && (bp->state == INITIALIZED) && bufp);
	assert(!(alloc_reserve != 0 && bp->reserve == 0));

	while(1) {
		if (queue_rem(&bp->freelist, &dllp) == 0) {
			*bufp = (char *)dllp;
			bp->issued++;
			res = 0;
			break;
		}

		if ((bp->owned + bp->reserve < bp->nmax) ||
				(alloc_reserve && (bp->reserve != 0) &&
				 (bp->owned < bp->nmax))) {
			if ((dllp = (dll_t *)calloc(1, bp->bufsize)) == NULL) {
				if (noblock) {
					res = -1;
					break;
				}
			} else {
				*bufp = (char *)dllp;
				bp->owned++;
				bp->issued++;
				res = 0;
				break;
			}
		} else {
			if (noblock) {
				res = -1;
				break;
			}
		}
		pthread_cond_wait(&bp->rendez, &bp->lock);
	}

	rc = pthread_mutex_unlock(&bp->lock);
	assert(rc == 0);
	return res;
}

int bufpool_get(bufpool_t *bp, char **bufp, int noblock)
{
	return _bufpool_get(bp, bufp, noblock, 0);
}

int bufpool_get_reserve(bufpool_t *bp, char **bufp, int noblock)
{
	return _bufpool_get(bp, bufp, noblock, 1);
}

/*
 * Get a zeroed out buffer from the freelist
 */
int bufpool_get_zero(bufpool_t *bp, char **bufp, int noblock)
{
	int rc;

	rc = bufpool_get(bp, bufp, noblock);
	if (rc < 0) {
		return rc;
	}

	memset(*bufp, 0, bp->bufsize);
	return 0;
}

int bufpool_get_reserve_zero(bufpool_t *bp, char **bufp, int noblock)
{
	int rc;

	rc = bufpool_get_reserve(bp, bufp, noblock);
	if (rc < 0) {
		return rc;
	}

	memset(*bufp, 0, bp->bufsize);
	return 0;
}
/*
 * Return buffer to pool if number of owned buffers <= nbufs
 * Otherwise just free the buffer and decrement owned.
 */
void bufpool_put(bufpool_t *bp, char *buf)
{
	int rc;

	assert(bp && buf);

	rc = pthread_mutex_lock(&bp->lock);
	assert(rc == 0);

	bp->issued--;
	assert(bp->issued >= 0);
	if (bp->owned <= bp->nbufs) {
		DLL_INIT((dll_t*)buf);
		queue_add(&bp->freelist, (dll_t *)buf);
	} else {
		free(buf);
		buf=NULL;
		bp->owned--;
		assert(bp->owned >= 0);
#ifdef  BUFPOOL_STATS_ENABLED
		bp->nfrees++;
#endif
		assert(bp->owned >= bp->nbufs);
	}

	pthread_cond_broadcast(&bp->rendez);

	pthread_mutex_unlock(&bp->lock);
}

#ifdef SOLOTEST_BUFPOOL

void bufpool_dump(bufpool_t *bp, char *msg)
{
	printf("-----bufpool dump:  %s -----\n", msg ? msg : "");
	printf("\tissued     %ld\n", bp->issued);
	printf("\towned      %ld\n", bp->owned);
	printf("\tbufsize    %ld\n", bp->bufsize);
	printf("\tnbufs      %ld\n", bp->nbufs);
	printf("\tnmax       %ld\n", bp->nmax);
	printf("\tstate      %d\n", bp->state);
#ifdef  BUFPOOL_STATS_ENABLED
	printf("\tnmallocs   %d\n", bp->nmallocs);
	printf("\tnfrees     %d\n", bp->nfrees);
#endif
	dump_queue(&bp->freelist);
	printf("\n----------------\n");
}

/*
 * Create a bufpool with one buffer. nbufs= 1, nmax=2
 * Task 1 (bufpool_main):
 * get one buffer. should succeed. owned == 1
 * put one buffer. should succeed. owned == 1
 * get one buffer. should succeed. owned == 1
 * start Task2 (bufpool_task)
 * sleep for some time
 * put one buffer.
 * wait for Task 2
 *   Task 2: get one buffer. should succeed. owned == 2.
 *   Task 2: get second buffer noblock
 *   Task 2: get second buffer. should block.
 *   Task 2: after a delay, should succeed. owned == 2
 *   Task 2: put both buffers. owned == 2. freelist == 1
 *   Signal Task2
 *
 */

void bufpool_task(void *p);
Rendez			rendez;
bufpool_t		pool;

void bufpool_task(void *p)
{
	int		res;
	char	*buf = NULL;
	char	*buf2 = NULL;
	time_t	t0, t1;

	res = bufpool_get(&pool, &buf, 0);
	bufpool_dump(&pool, "task2: after a get");
	assert(res == 0 && buf);
	printf("bufpool_task: going in for second buffer NOBLOCK\n");
	res = bufpool_get(&pool, &buf2, 1);
	assert(res != 0 && buf2 == NULL);
	printf("bufpool_task: going in for second buffer\n");
	t0 = time(0);
	res = bufpool_get(&pool, &buf2, 0);
	t1 = time(0);
	printf("bufpool_task: second get took %ld seconds\n", t1 - t0);
	assert(res == 0 && buf2 && pool.owned == 2);
	bufpool_dump(&pool, "task2: after 2nd get");
	bufpool_put(&pool, buf);
	bufpool_dump(&pool, "task2: after 1st put");
	bufpool_put(&pool, buf2);
	bufpool_dump(&pool, "task2: after 2nd put");
	assert(pool.owned == 1 && pool.freelist.q_len == 1);
	printf("task2: signaling task1\n");
	taskwakeup(&rendez);
	printf("task2: signalled task1\ntask 2 returns\n");
}


void bufpool_main()
{
	int		res;
	char	*buf = NULL;

	res = bufpool_init(&pool, 16, 1, 2);
	BZERO(&rendez);
	bufpool_dump(&pool, "bufpool_main: after init");
	assert(res == 1);

	res = bufpool_get(&pool, &buf, 0);
	assert(res == 0 && buf && pool.owned == 1 && pool.freelist.q_len == 0);
	bufpool_dump(&pool, "bufpool_main: after a get");

	bufpool_put(&pool, buf);
	assert(pool.owned == 1 && pool.freelist.q_len == 1);
	bufpool_dump(&pool, "bufpool_main: after a put");
	res = bufpool_get(&pool, &buf, 0);
	assert(res == 0 && buf && pool.owned == 1 && pool.freelist.q_len == 0);
	bufpool_dump(&pool, "bufpool_main: after second get");
	taskcreate(bufpool_task, 0, 32*1024);
	printf("bufpool_main: sleeping for 10 seconds\n");
	taskdelay(10*1000);
	printf("bufpool_main: continuing after 10 seconds\n");
	bufpool_dump(&pool, "bufpool_main: before second put");
	bufpool_put(&pool, buf);
	bufpool_dump(&pool, "bufpool_main: after second put");
	printf("bufpool_main: waiting on signal\n");
	tasksleep(&rendez);
	printf("bufpool_main: got signal\nbufpool_main returns\n");
}

void taskmain(int argc, char *argv[])
{
	bufpool_main();
	exit(0);
}

#endif /* SOLOTEST_BUFPOOL*/