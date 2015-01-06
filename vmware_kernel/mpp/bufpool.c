#include "vmkapi.h"
#include "vmware_include.h"
#include "bufpool.h"

void bufpool_dump(bufpool_t *bp, char *msg)
{
	VMK_ReturnStatus s;
	vmk_HeapGetProps props;

	vmk_WarningMessage("-----bufpool dump:  %s -----\n", msg ? msg : "");
	vmk_WarningMessage("\tbp         %p\n", bp);
	vmk_WarningMessage("\tissued     %ld\n", bp->issued);
	vmk_WarningMessage("\towned      %ld\n", bp->owned);
	vmk_WarningMessage("\tbufsize    %ld\n", bp->bufsize);
	vmk_WarningMessage("\tnbufs      %ld\n", bp->nbufs);
	vmk_WarningMessage("\tnmax       %ld\n", bp->nmax);
	vmk_WarningMessage("\tstate      %d\n", bp->state);
	vmk_WarningMessage("\tnmallocs   %d\n", bp->nmallocs);
	vmk_WarningMessage("\tnfrees     %d\n", bp->nfrees);
	vmk_WarningMessage("\treserve    %d\n", bp->reserve);
//	dump_queue(&bp->freelist);

	s = vmk_HeapGetProperties(bp->heap_id, &props);
	assert(s == VMK_OK);

	vmk_WarningMessage("\tHeap Name %s\n", vmk_NameToString(&props.name));

	vmk_WarningMessage();
	vmk_WarningMessage("\n----------------\n");
}

/*
 * initialize a buffer pool of up to nbufs count of
 * fixed size buffers of size = bufsize.
 * If more than nbufs buffers are requested, will try to malloc more
 * up to a limit of nmax buffers.
 * Returns number of buffers allocated.
 */

int bufpool_init_reserve(bufpool_t *bp, const char *name,
	module_global_t *module, size_t bufsize, size_t nbufs, int reserve,
	size_t nmax)
{
	char             n[128];
	int              r;
	dll_t            *buf;

	assert(bp);
	assert(nbufs > 0 && nmax >= nbufs);

	BZERO(bp);

	r = vmware_name(n, name, "bp", sizeof(n));
	assert(r == 0);

	if (bufsize < sizeof(dll_t)) {
		bufsize = sizeof(dll_t);
	}

	r = vmware_heap_create(&bp->heap_id, n, module, nmax, bufsize);
	if (r < 0) {
		vmk_WarningMessage("%s vmware_heap_create failed\n", __func__);
		return -1;
	}

	queue_init(&bp->freelist);
	bp->bufsize = bufsize;
	bp->nbufs   = nbufs;
	bp->nmax    = nmax;
	bp->state   = INITIALIZED;
	bp->reserve = reserve;

	r = pthread_mutex_init(&bp->lock, n, module);
	assert(r == 0);

	for (bp->owned = 0; bp->owned < nbufs; bp->owned++) {
		if ((buf = (dll_t *) malloc(bp->heap_id, bufsize)) == NULL) {
			return bp->owned;
		}
		DLL_INIT(buf);
		queue_add(&bp->freelist, buf);
	}
	return bp->owned;
}

int bufpool_init(bufpool_t *bp, const char *name, module_global_t *module,
	size_t bufsize, size_t nbufs, size_t nmax)
{
	return bufpool_init_reserve(bp, name, module, bufsize, nbufs, 0, nmax);
}
/*
 * free all buffers on the freelist.
 */
void bufpool_deinit(bufpool_t *bp)
{
	int        rc;
	dll_t      *dllp;

	assert(bp != NULL);

	rc = pthread_mutex_lock(bp->lock);
	assert(rc == 0);

	if (bp->state != INITIALIZED) {
		rc = pthread_mutex_unlock(bp->lock);
		return;
	}

	assert(bp->state == INITIALIZED);
	bp->state = DEINITING;

	while (bp->issued != 0) {
		/* block deinit until last buffer is freed */
		vmk_WorldWait((vmk_WorldEventID) &bp->issued, bp->lock,
			VMK_MSEC_PER_SEC, "bufpool_deinit: blocked.\n");

		rc = pthread_mutex_lock(bp->lock);
		assert(rc == 0);
	}
	assert(bp->issued == 0);

	while (queue_rem(&bp->freelist, &dllp) == 0) {
		free(bp->heap_id, dllp);
		bp->owned--;
	}
	queue_deinit(&bp->freelist);

	bp->state = DEINTED;

	pthread_mutex_unlock(bp->lock);
	pthread_mutex_destroy(bp->lock);
	vmk_HeapDestroy(bp->heap_id);
}

/*
 * Get a buffer from the freelist
 * Otherwise try to calloc a new one
 * Otherwise sleep for one
 * returns 0 on success, -1 if buffer not found and noblock true
 */
static int _bufpool_get(bufpool_t *bp, char **bufp, int noblock,
		int alloc_reserve)
{
	int      rc;
	dll_t    *dllp;
	int      res;
	vmk_Bool b;

	rc = pthread_mutex_lock(bp->lock);
	assert(rc == 0);

	assert(bp && (bp->state == INITIALIZED) && bufp);
	assert(!(alloc_reserve != 0 && bp->reserve == 0));

	while(1) {
		assert(bp->state == INITIALIZED);

		if (queue_rem(&bp->freelist, &dllp) == 0) {
			*bufp = (char *)dllp;
			bp->issued++;
			res = 0;
			break;
		}

		if ((bp->owned + bp->reserve < bp->nmax) ||
				(alloc_reserve && (bp->reserve != 0) &&
				 (bp->owned < bp->nmax))) {
			if ((dllp = (dll_t *) malloc(bp->heap_id, bp->bufsize)) == NULL) {
				if (noblock) {
					res = -1;
					break;
				}
			} else {
				*bufp = (char *)dllp;
				bp->owned++;
				bp->issued++;
				bp->nmallocs++;
				res = 0;
				break;
			}
		} else {
			if (noblock) {
				res = -1;
				break;
			}
		}

		b = VMK_FALSE;
		while (!b) {
			vmk_WorldWait((vmk_WorldEventID) &bp->issued, bp->lock,
					100, "bufpool_get: blocked.\n");

			rc = pthread_mutex_lock(bp->lock);
			assert(rc == 0);

			if (alloc_reserve && (bp->owned < bp->nmax)) {
				b = VMK_TRUE;
			} else if ((bp->owned + bp->reserve) < bp->nmax) {
				b = VMK_TRUE;
			}
		}
	}

	rc = pthread_mutex_unlock(bp->lock);
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

	rc = pthread_mutex_lock(bp->lock);
	assert(rc == 0);

	bp->issued--;
	assert(bp->issued >= 0);
	if (bp->owned <= bp->nbufs) {
		DLL_INIT((dll_t*)buf);
		queue_add(&bp->freelist, (dll_t *)buf);
	} else {
		free(bp->heap_id, buf);
		buf=NULL;
		bp->owned--;
		assert(bp->owned >= 0);
		bp->nfrees++;
		assert(bp->owned >= bp->nbufs);
	}

	pthread_mutex_unlock(bp->lock);

	vmk_WorldWakeup((vmk_WorldEventID) &bp->issued);
}

#ifdef SOLOTEST_BUFPOOL
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
	vmk_WarningMessage("bufpool_task: going in for second buffer NOBLOCK\n");
	res = bufpool_get(&pool, &buf2, 1);
	assert(res != 0 && buf2 == NULL);
	vmk_WarningMessage("bufpool_task: going in for second buffer\n");
	t0 = time(0);
	res = bufpool_get(&pool, &buf2, 0);
	t1 = time(0);
	vmk_WarningMessage("bufpool_task: second get took %ld seconds\n", t1 - t0);
	assert(res == 0 && buf2 && pool.owned == 2);
	bufpool_dump(&pool, "task2: after 2nd get");
	bufpool_put(&pool, buf);
	bufpool_dump(&pool, "task2: after 1st put");
	bufpool_put(&pool, buf2);
	bufpool_dump(&pool, "task2: after 2nd put");
	assert(pool.owned == 1 && pool.freelist.q_len == 1);
	vmk_WarningMessage("task2: signaling task1\n");
	taskwakeup(&rendez);
	vmk_WarningMessage("task2: signalled task1\ntask 2 returns\n");
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
	vmk_WarningMessage("bufpool_main: sleeping for 10 seconds\n");
	taskdelay(10*1000);
	vmk_WarningMessage("bufpool_main: continuing after 10 seconds\n");
	bufpool_dump(&pool, "bufpool_main: before second put");
	bufpool_put(&pool, buf);
	bufpool_dump(&pool, "bufpool_main: after second put");
	vmk_WarningMessage("bufpool_main: waiting on signal\n");
	tasksleep(&rendez);
	vmk_WarningMessage("bufpool_main: got signal\nbufpool_main returns\n");
}

void taskmain(int argc, char *argv[])
{
	bufpool_main();
	exit(0);
}

#endif /* SOLOTEST_BUFPOOL*/
