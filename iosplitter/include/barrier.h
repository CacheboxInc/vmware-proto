/*
 *
 */
#ifndef BARRIER_H
#define BARRIER_H

#include "libtask/task.h"

typedef struct task_barrier {
	int		n;
	Rendez	r;
} task_barrier_t;

void task_barrier_init(task_barrier_t *bp, int ntasks); // create a barrier
void task_barrier_wait(task_barrier_t *bp); // each of n callers blocks until all have called.

#endif /* BARRIER_H */
