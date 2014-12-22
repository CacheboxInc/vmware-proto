/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"
#include <fcntl.h>
#include <stdio.h>

#define CACHE_STACK_SIZE	(32 * 1024)
#define TASKS_CACHE_MAX		(32)

__thread int	taskdebuglevel;
__thread int	taskcount;
__thread int	tasknswitch;
__thread int	taskexitval;
__thread Task	*taskrunning;

__thread Context	taskschedcontext;
__thread Tasklist	taskrunqueue;
__thread Tasklist	taskscache;
__thread int		taskcachecount;


__thread Task	**alltask;
__thread int	nalltask;

static __thread char *argv0;
static __thread int taskidgen;


static void contextswitch(Context *from, Context *to);

static void
taskdebug(char *fmt, ...)
{
	va_list arg;
	char buf[128];
	Task *t;
	char *p;
	static int fd = -1;

return;
	va_start(arg, fmt);
	vfprint(1, fmt, arg);
	va_end(arg);
return;

	if(fd < 0){
		p = strrchr(argv0, '/');
		if(p)
			p++;
		else
			p = argv0;
		snprint(buf, sizeof buf, "/tmp/%s.tlog", p);
		if((fd = open(buf, O_CREAT|O_WRONLY, 0666)) < 0)
			fd = open("/dev/null", O_WRONLY);
	}

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	t = taskrunning;
	if(t)
		fprint(fd, "%d.%d: %s\n", getpid(), t->id, buf);
	else
		fprint(fd, "%d._: %s\n", getpid(), buf);
}

static void
taskstart(uint y, uint x)
{
	Task *t;
	ulong z;

	z = x<<16;	/* hide undefined 32-bit shift from 32-bit compilers */
	z <<= 16;
	z |= y;
	t = (Task*)z;

//print("taskstart %p\n", t);
	t->startfn(t->startarg);
//print("taskexits %p\n", t);
	taskexit(0);
//print("not reacehd\n");
}

static void _task_init(Task *t, void (*fn)(void*), void *arg)
{
	t->name[0]	= 0;
	t->state[0]	= 0;
	t->next		= NULL;
	t->prev		= NULL;
	t->allnext	= NULL;
	t->allprev	= NULL;
	t->alarmtime	= 0;
	t->exiting	= 0;
	t->system	= 0;
	t->ready	= 0;
	t->startfn	= fn;
	t->startarg	= arg;
	t->udata	= NULL;
	t->cached	= 0;
}

static void task_init(Task *t, void (*fn)(void*), void *arg)
{
	sigset_t zero;
	uint x, y;
	ulong z;

	_task_init(t, fn, arg);

	/* do a reasonable initialization */
	memset(&t->context.uc, 0, sizeof t->context.uc);
	sigemptyset(&zero);
	sigprocmask(SIG_BLOCK, &zero, &t->context.uc.uc_sigmask);

	/* must initialize with current context */
	if(getcontext(&t->context.uc) < 0){
		fprint(2, "getcontext: %r\n");
		abort();
	}

	/* call makecontext to do the real work. */
	/* leave a few words open on both ends */
	t->context.uc.uc_stack.ss_sp = t->stk+8;
	t->context.uc.uc_stack.ss_size = t->stksize-64;
#if defined(__sun__) && !defined(__MAKECONTEXT_V2_SOURCE)		/* sigh */
#warning "doing sun thing"
	/* can avoid this with __MAKECONTEXT_V2_SOURCE but only on SunOS 5.9 */
	t->context.uc.uc_stack.ss_sp = 
		(char*)t->context.uc.uc_stack.ss_sp
		+t->context.uc.uc_stack.ss_size;
#endif
	/*
	 * All this magic is because you have to pass makecontext a
	 * function that takes some number of word-sized variables,
	 * and on 64-bit machines pointers are bigger than words.
	 */
//print("make %p\n", t);
	z = (ulong)t;
	y = z;
	z >>= 16;	/* hide undefined 32-bit shift from 32-bit compilers */
	x = z>>16;
	makecontext(&t->context.uc, (void(*)())taskstart, 2, y, x);
}

static Task*
taskalloc(void (*fn)(void*), void *arg, uint stack)
{
	Task *t;

	/* allocate the task and stack together */
	t = malloc(sizeof *t+stack);
	if(t == nil){
		fprint(2, "taskalloc malloc: %r\n");
		abort();
	}
	memset(t, 0, sizeof *t);
	t->stk = (uchar*)(t+1);
	t->stksize = stack;
	t->id = ++taskidgen;

#ifdef VALGRIND
	t->stkid = VALGRIND_STACK_REGISTER(t->stk, t->stk + t->stksize);
#endif

	task_init(t, fn, arg);
	return t;
}

int
taskcreate(void (*fn)(void*), void *arg, uint stack)
{
	int id;
	Task *t;

	t = nil;
	if (stack == CACHE_STACK_SIZE) {
		/* we cache a task iff stack size is CACHE_STACK_SIZE */
		t = taskscache.head;
	}

	if (t == nil) {
		assert(taskcachecount == 0);
		t = taskalloc(fn, arg, stack);
		taskcount++;
		if(nalltask%64 == 0){
			alltask = realloc(alltask, (nalltask+64)*sizeof(alltask[0]));
			if(alltask == nil){
				fprint(2, "out of memory\n");
				abort();
			}
		}
		t->alltaskslot = nalltask;
		alltask[nalltask++] = t;
	} else {
		deltask(&taskscache, t);
		assert(taskcachecount > 0);
		taskcachecount--;
		task_init(t, fn, arg);
	}

	id = t->id;
	taskready(t);
	return id;
}

void
tasksystem(void)
{
	if(!taskrunning->system){
		taskrunning->system = 1;
		--taskcount;
	}
}

void
taskswitch(void)
{
	needstack(0);
	contextswitch(&taskrunning->context, &taskschedcontext);
}

void
taskready(Task *t)
{
	t->ready = 1;
	addtask(&taskrunqueue, t);
}

int
taskyield(void)
{
	int n;

	n = tasknswitch;
	taskready(taskrunning);
	taskstate("yield");
	taskswitch();
	return tasknswitch - n - 1;
}

int
anyready(void)
{
	return taskrunqueue.head != nil;
}

void
taskexitall(int val)
{
	pthread_exit(&val);
}

void
taskexit(int val)
{
	taskexitval = val;
	taskrunning->exiting = 1;
	taskswitch();
}

static void
contextswitch(Context *from, Context *to)
{
	if(swapcontext(&from->uc, &to->uc) < 0){
		fprint(2, "swapcontext failed: %r\n");
		assert(0);
	}
}

static void taskfree(Task *t)
{
	int i;

	if (!t->system)
		taskcount--;
	i = t->alltaskslot;
	alltask[i] = alltask[--nalltask];
	alltask[i]->alltaskslot = i;
#ifdef VALGRIND
	VALGRIND_STACK_DEREGISTER(t->stkid);
#endif
	free(t);
}

static void
taskscheduler(void)
{
	int i;
	Task *t;

	taskdebug("scheduler enter");
	for(;;){
		if(taskcount == 0)
			pthread_exit(&taskexitval);
		t = taskrunqueue.head;
		if(t == nil){
			taskcachefree();
			if (taskcount == 0) {
				pthread_exit(&taskexitval);
			}

			fprint(2, "no runnable tasks! %d tasks stalled\n", taskcount);
			i = 1;
			pthread_exit(&i);
		}
		deltask(&taskrunqueue, t);
		t->ready = 0;
		taskrunning = t;
		tasknswitch++;
		taskdebug("run %d (%s)", t->id, t->name);
		contextswitch(&taskschedcontext, &t->context);
		//print("back in scheduler\n");
		taskrunning = nil;
		if(t->exiting){
			if (t->stksize != CACHE_STACK_SIZE ||
			    taskcachecount >= TASKS_CACHE_MAX) {
				/* do not cache */
				taskfree(t);
			} else {
				/* do not free this task - cache it */
				if (t->system == 1) {
					taskcount++;
				}
				_task_init(t, NULL, NULL);
				t->cached = 1;
				addtask(&taskscache, t);
				taskcachecount++;
			}
		}
	}
}

void taskcachefree(void)
{
	Task *t;

	while (1) {
		t = taskscache.head;

		if (t == nil) {
			break;
		}

		deltask(&taskscache, t);
		taskfree(t);
	}

	taskcachecount = 0;
}

void**
taskdata(void)
{
	return &taskrunning->udata;
}

/*
 * debugging
 */

int get_taskcachecount(void)
{
	return taskcachecount;
}

void
taskname(char *fmt, ...)
{
	va_list arg;
	Task *t;

	t = taskrunning;
	va_start(arg, fmt);
	vsnprint(t->name, sizeof t->name, fmt, arg);
	va_end(arg);
}

char*
taskgetname(void)
{
	return taskrunning->name;
}

void
taskstate(char *fmt, ...)
{
	va_list arg;
	Task *t;

	t = taskrunning;
	va_start(arg, fmt);
	vsnprint(t->state, sizeof t->name, fmt, arg);
	va_end(arg);
}

char*
taskgetstate(void)
{
	return taskrunning->state;
}

void
needstack(int n)
{
	Task *t;

	t = taskrunning;

	if((char*)&t <= (char*)t->stk
	|| (char*)&t - (char*)t->stk < 256+n){
		fprint(2, "task stack overflow: &t=%p tstk=%p n=%d\n", &t, t->stk, 256+n);
		abort();
	}
}

void
taskinfo(int s)
{
	int i;
	Task *t;
	char *extra;

	fprint(2, "task list:\n");
	for(i=0; i<nalltask; i++){
		t = alltask[i];
		if(t == taskrunning)
			extra = " (running)";
		else if(t->ready)
			extra = " (ready)";
		else if (t->cached)
			extra = "(cached)";
		else
			extra = "";
		fprint(2, "%6d%c %-20s %s%s\n",
			t->id, t->system ? 's' : ' ',
			t->name, t->state, extra);
	}
}

/*
 * startup
 */

int mainstacksize;

#if 1
void
libtask_start(taskfnptr_t taskp, void *arg)
{
	struct sigaction sa, osa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = taskinfo;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGQUIT, &sa, &osa);

	if (mainstacksize == 0) {
		mainstacksize = 256*1024;
	}
	if (taskp) {
		taskcreate(taskp, arg, mainstacksize);
	}
	taskscheduler();
}

#else
static int taskargc;
static char **taskargv;

static void
taskmainstart(void *v)
{
	taskname("taskmain");
	taskmain(taskargc, taskargv);
}

int
main(int argc, char **argv)
{
	struct sigaction sa, osa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = taskinfo;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGQUIT, &sa, &osa);

#ifdef SIGINFO
	sigaction(SIGINFO, &sa, &osa);
#endif

	argv0 = argv[0];
	taskargc = argc;
	taskargv = argv;

	if(mainstacksize == 0)
		mainstacksize = 256*1024;
	taskcreate(taskmainstart, nil, mainstacksize);
	taskscheduler();
	fprint(2, "taskscheduler returned in main!\n");
	abort();
	return 0;
}
#endif

/*
 * hooray for linked lists
 */
void
addtask(Tasklist *l, Task *t)
{
	if(l->tail){
		l->tail->next = t;
		t->prev = l->tail;
	}else{
		l->head = t;
		t->prev = nil;
	}
	l->tail = t;
	t->next = nil;
}

void
deltask(Tasklist *l, Task *t)
{
	if(t->prev)
		t->prev->next = t->next;
	else
		l->head = t->next;
	if(t->next)
		t->next->prev = t->prev;
	else
		l->tail = t->prev;
}

unsigned int
taskid(void)
{
	return taskrunning->id;
}

