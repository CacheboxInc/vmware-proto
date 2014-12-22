#include <sys/timerfd.h>

#include "taskimpl.h"
#include "task.h"
#include "taskio.h"
#include "timer.h"

#define TASKSZ	(32 * 1024)

/*
 * Task based timer functionality
 * ==============================
 *
 * Helper functions to delay/block/periodic callback functionality for tasks.
 * Works at nanosecond granularity.
 *
 * A task might need to use timer functionality in 3 situations
 *
 * 1. Periodic poll or wakeup
 *	A task may need to accomplish some work after specific time interval. For
 *	example: flusher task needs to periodically wakeup and check if the flush
 *	is required. Another use case may to display some stats after every 5
 *	seconds.
 *
 *	Typically, user (of this API) would want a call back function to be called,
 *	periodically after specific time interval.
 *
 *	Following sequence of APIs should be used in such situation
 *
 *	task_timer_t t;
 *	struct timespec ts;

 *	timer_init(&t, callback_fn, data);
 *
 *	ts.tv_sec = 1;
 *	ts.tv_nsec = 0;
 *	timer_set(t, &ts, -1);
 *
 *	This will result in callback_fn be called repeated after each second.
 *
 * 2. Delay task execution (single time)
 *	This mode of operation is suitable, iff the task must be blocked just
 *	once. Caller, in this mode, does not have to initialize any timer, it simply
 *	have to call task_delay() function.
 *
 *	An example usage could be:
 *
 *	struct timespec *ts;
 *
 *	ts.tv_sec = 0;
 *	ts.tv_nsec = 500 * 1000 * 1000;	// 500 milliseconds
 *
 *	task_delay(&ts);
 *
 *	Please note if you have to delay the task multiple times, then it is better
 *	to next mode of operation.
 *
 * 3. Delay task execution (multiple times)
 * 	This mode of operations is suitable, when a task needs to be blocked
 *	regularly for specific time period. An example usage could be:
 *
 *	task_timer_t *t;
 *	struct timespec ts;
 *
 *	timer_init(&t, NULL, NULL);
 *
 *	while (1) {
 *		< some work >
 *
 *		ts.tv_sec = 0;
 *		ts.tv_nsec = 100000; // 100 micro seconds
 *		timer_wait(t, &ts);
 *
 *		< some work >
 *	}
 */

/*
 * timer_init: Initialize Timer
 *
 * Input:
 *	t      - Uninitialized task_timer_t
 *	cbfn   - Call back function
 *	opaque - Argument passed to call back function
 *
 * Return:
 *	0      - Sucess
 *	< 0    - Error
 */
int timer_init(task_timer_t *t, timer_cb cbfn, void *opaque)
{
	int fd;
	int rc;

	assert(t->init != 1);

	memset(t, 0, sizeof(*t));

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}

	rc = task_fifofd_register(fd, 1);
	if (rc < 0) {
		goto error;
	}

	t->timerfd = fd;
	t->cbfn    = cbfn;
	t->opaque  = opaque;
	t->init    = 1;
	return 0;
error:
	close(fd);
	t->closed = 1;
	t->init   = 0;
	return rc;
}

/*
 * Uninitialize Timer
 *
 * Input:
 *	t	- Initialized task_timer_t
 */
void timer_deinit(task_timer_t *t)
{
	assert(t != NULL);

	t->closed  = 1;
	t->init    = 0;

	if (t->timerfd > 0) {
		close(t->timerfd);
		task_fd_deregister(t->timerfd);
	}

	t->timerfd = -1;
}

/*
 * Internal function
 */
void _timer_func(void *arg)
{
	task_timer_t *t = arg;
	uint64_t     v;
	int          rc;

	tasksystem();

	while (1) {
		v = 0;
		rc = task_netread(t->timerfd, (char *) &v, sizeof(v));
		if (rc != 0) {
			if (rc == TASKIO_ECONN || rc == EBADF) {
				/* timerfd closed */
				assert(t->closed == 1);
				taskexit(0);
				break;
			}

			assert(0);
		}

		taskwakeupall(&t->rendez);

		if (t->cbfn) {
			t->cbfn(t->opaque, v);
		}

		if (t->repeat > 0) {
			t->repeat--;
		}

		if (t->repeat == 0) {
			/* disarm timer */
			timer_unset(t);
			taskexit(0);
			break;
		}
	}
}

/*
 * Internal function
 */
static int timer_mod(task_timer_t *t, struct timespec *ts, int repeat)
{
	struct itimerspec its = {
		.it_value    = {0},
		.it_interval = {0},
	};
	int rc;

	assert(t->timer_set != 0);
	assert(t->init == 1);

	its.it_value.tv_sec  = ts->tv_sec;
	its.it_value.tv_nsec = ts->tv_nsec;
	if (repeat != 0) {
		its.it_interval.tv_sec  = ts->tv_sec;
		its.it_interval.tv_nsec = ts->tv_nsec;
	}

	rc = timerfd_settime(t->timerfd, 0, &its, NULL);
	if (rc < 0) {
		return -1;
	}

	t->ts.tv_sec  = ts->tv_sec;
	t->ts.tv_nsec = ts->tv_nsec;
	t->repeat     = repeat;

	return 0;
}

/*
 * timer_set: set the timer
 *
 * Input:
 *	t	- Initialized task_timer_t
 *	ts	- time specifications for timer expiration
 *	repeat	- determines how many times to invoke call back function
 *		  NOTE: This parameter determines number of times the callback
 *		  function must be invoked and does not determine number of
 *		  timer expirations. For example: If timer is set to expire
 *		  every nanosecond, within our framework, it would be impossible
 *		  to guarantee call back invocation per nanoseconds. Therefore,
 *		  this argument determines number of times callback function
 *		  must be invoked.
 *
 *		if (repeat == 0)
 *			invoke call back function just once
 *		else if (repeat == -1)
 *			call back function is invoked, until timer is unset by
 *			caller
 *		else if (repeat > 0)
 *			invoke call back function #repeat times
 *
 * Return:
 *	0	- Success
 *	< 0	- Error
 */
int timer_set(task_timer_t *t, struct timespec *ts, int repeat)
{
	struct itimerspec its = {
		.it_value    = {0},
		.it_interval = {0},
	};
	int rc;

	assert(t->closed == 0);
	assert(t->init   == 1);

	if (t->timer_set) {
		return timer_mod(t, ts, repeat);
	}

	its.it_value.tv_sec  = ts->tv_sec;
	its.it_value.tv_nsec = ts->tv_nsec;
	if (repeat != 0) {
		its.it_interval.tv_sec  = ts->tv_sec;
		its.it_interval.tv_nsec = ts->tv_nsec;
	}

	rc = timerfd_settime(t->timerfd, 0, &its, NULL);
	if (rc < 0) {
		return -1;
	}

	t->ts.tv_sec  = ts->tv_sec;
	t->ts.tv_nsec = ts->tv_nsec;
	t->repeat     = repeat;
	t->timer_set  = 1;

	taskcreate(_timer_func, t, TASKSZ);

	return 0;
}

/*
 * Unset the timer
 *
 * Input:
 *	t	- Initialized task_timer_t
 */
inline void timer_unset(task_timer_t *t)
{
	int               rc;
	struct itimerspec its = {
		.it_value    = {0},
		.it_interval = {0},
	};

	assert(t->init      == 1);
	assert(t->closed    == 0);

	if (t->timer_set == 0) {
		return;
	}

	rc = timerfd_settime(t->timerfd, 0, &its, NULL);
	assert(rc == 0);

	t->timer_set = 0;
}


/*
 * timer_wait: Blocks task for specified time specification.
 *
 * Input:
 *	t	- Initialized task_timer_t
 *	ts	- time specification
 *
 * Return:
 *	0	- Success
 *	< 0	- Error
 */
int timer_wait(task_timer_t *t, struct timespec *ts)
{
	int rc;

	assert(t->closed == 0);
	assert(t->init   == 1);

	rc = timer_set(t, ts, 0);
	if (rc < 0) {
		return -1;
	}

	tasksleep(&t->rendez);
	return 0;
}

/*
 * task_delay: Blocks task for specified time specification.
 *
 * Input:
 *	ts	- time specification
 *
 * Return:
 *	0	- Success
 *	< 0	- Error
 */
int task_delay(struct timespec *ts)
{
	int          rc;
	task_timer_t t;

	rc = timer_init(&t, NULL, NULL);
	if (rc < 0) {
		return -1;
	}

	rc = timer_wait(&t, ts);
	if (rc < 0) {
		timer_deinit(&t);
		return -1;
	}

	timer_deinit(&t);

	return 0;
}
