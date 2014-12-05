#if !defined(__QUEUE_H__)
#define __QUEUE_H__
#include "dll.h"

typedef dll_t queue_entry_t;

typedef struct queue {
	dll_t	q_dll;	/* head of the list */
	int	q_len;	/* number of items in the list */
} queue_t;

typedef struct {
	queue_t *qi_qp;
	dll_t   *qi_curdllp;	/* serve next element */
} queue_iter_t;


#define QUEUE_ISEMPTY(qp)	((qp)->q_len == 0)

void queue_init(queue_t *qp);
void queue_clear(queue_t *qp);
void queue_add(queue_t *qp, queue_entry_t *entry);
int queue_rem(queue_t *qp, queue_entry_t **entry);
void queue_deinit(queue_t *qp);
void queue_iter_init(queue_iter_t *qip, queue_t *qp);
void *queue_iter_next(queue_iter_t *qip);
#endif
