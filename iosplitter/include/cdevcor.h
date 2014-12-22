/*
 * cdevcor.h
 * 		Wrapper for libtask/qemu coroutines switch
 */
#ifndef CDEVCOR_H
#define CDEVCOR_H

#ifdef CDEV_LIBTASK

#define TASK_STACKSZ		(32 * 1024)
#define TASKRENDEZ_INIT(ptr)	memset((ptr), 0, sizeof(*ptr))

/* Task management */
#define TASKMAIN_START	libtask_start
#define TASKCREATE	taskcreate
#define TASKSWITCH	taskswitch
#define TASKYIELD	taskyield
#define TASKRUNNING	taskrunning

/* Rendez related */
#define TASKSLEEP	tasksleep
#define TASKWAKEUP	taskwakeup
#define TASKSYSTEM	tasksystem
#define TASKNAME	taskname

/* Locking related */
#define QLOCK	qlock
#define QUNLOCK	qunlock

/* taskio.c functions */
#define TASKIO_INIT taskio_init
#define TASKIO_START taskio_start
#define TASKIO_DEINIT taskio_deinit
#define TASK_SOCKFD_REGISTER task_sockfd_register
#define TASK_SOCKFD_DEREGISTER task_fd_deregister
#define TASK_EVENTFD_REGISTER task_eventfd_register
#define TASK_EVENTFD_DEREGISTER task_eventfd_deregister
#define TASK_AIORW task_aiorw
#define TASK_NETRW task_netrw


/* networking and misc */
#define NETLOOKUP	netlookup

/*
 * NOTE:
 * taskio.c functions, such as task_accept, use several functions from libtask
 * but that is okay, a non-libtask linkage will not use taskio.c either.
 */
#else

/* Task Management */
#define TASKCREATE	taskcreate
#define TASKSWITCH	taskswitch
#define TASKYIELD	taskyield
#define TASKRUNNING	taskrunning

/* Rendez related */
#define TASKSLEEP	tasksleep
#define TASKWAKEUP	taskwakeup
#define TASKSYSTEM	tasksystem

/* Locking related */
#define QLOCK	qlock
#define QUNLOCK	qunlock

#endif

#endif /* CDEVCOR_H */
