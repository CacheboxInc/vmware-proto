/*
 * Copyright(2013) Cachebox Inc.
 *
 * queue.c
 */

#include "queue.h"
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

 /*
  * TBD: create a free list of qnode_t of size qsz
  */
void queue_init(queue_t *qp)
{
	assert(qp != NULL);

	DLL_INIT(&qp->q_dll);
	qp->q_len = 0;
}

/* XXX 
 * this function is not really doing anything useful now
 * but later it will deallocate into free qnode list.
 */
void queue_clear(queue_t *qp)
{
	assert(qp->q_len == 0);
	assert(DLL_ISEMPTY(&qp->q_dll));
}


void queue_add(queue_t *qp, queue_entry_t *entry)
{
	assert(entry != NULL);

	DLL_ADD(&qp->q_dll, entry)
	qp->q_len++;
}

void queue_iter_init(queue_iter_t *qip, queue_t *qp)
{
	qip->qi_qp = qp;
	qip->qi_curdllp = &qp->q_dll;
}

void *queue_iter_next(queue_iter_t *qip)
{
	if (DLL_PREV(qip->qi_curdllp) == &qip->qi_qp->q_dll) {
		return NULL;
	}

	qip->qi_curdllp = DLL_PREV(qip->qi_curdllp);
	return qip->qi_curdllp;
}

int queue_rem(queue_t *qp, queue_entry_t **entry)
{
	queue_entry_t *e;

	assert(entry != NULL);
	*entry = NULL;
	if (DLL_ISEMPTY(&qp->q_dll)) {
		return (-1);
	}
	e = DLL_PREV(&qp->q_dll);
	DLL_REM(e);
	qp->q_len--;
	assert(qp->q_len >= 0);
	*entry = e;
	return 0;
}

void queue_deinit(queue_t *qp)
{
	assert(qp->q_len == 0);
	assert(DLL_ISEMPTY(&qp->q_dll));
	memset(qp, 0, sizeof(*qp));
}
