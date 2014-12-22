#ifndef __TIMER_H__
#define __TIMER_H__

typedef void (*timer_cb) (void *, uint64_t expire_count);

typedef struct task_timer {
	Rendez          rendez;			/**/
	struct timespec ts;			/**/
	void            *opaque;		/**/
	timer_cb        cbfn;			/**/
	int             timerfd;		/**/
	int             repeat;
	int             timer_set;
	int		init;
	int             closed;
} task_timer_t;

int timer_init(task_timer_t *t, timer_cb cbfn, void *opaque);

void timer_deinit(task_timer_t *t);
void timer_unset(task_timer_t *t);

int timer_set(task_timer_t *t, struct timespec *ts, int repeat);
void timer_unset(task_timer_t *t);
int timer_wait(task_timer_t *t, struct timespec *ts);
int task_delay(struct timespec *ts);

#define TIMER_FOREVER (-1)

#endif
