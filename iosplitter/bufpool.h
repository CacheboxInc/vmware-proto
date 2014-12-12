#ifndef BUFPOOL_H
#define BUFPOOL_H

#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>
#include "dll.h"
#include "queue.h"

#define BZERO(memptr)  memset((memptr), 0, sizeof(*memptr))

/*
 *  buffer pool of fixed size buffers
 */
#define BUFPOOL_STATS_ENABLED

typedef enum {
	UNINITIALIZED = 0,
	INITIALIZED   = 1,
	DEINITING,
	DEINTED,
} BUFPOOL_STATE;

typedef struct bufpool {
	char                    *name;
	pthread_mutex_t		lock;
	pthread_cond_t		rendez;
	struct queue		freelist;
	size_t			issued;
	size_t			owned;
	size_t			bufsize;
	size_t			nbufs;
	size_t			nmax;
	int                     reserve;

#ifdef  BUFPOOL_STATS_ENABLED
	int			nmallocs;
	int			nfrees;
#endif

	BUFPOOL_STATE		state;
} bufpool_t;

static inline size_t bufpool_bufsize(bufpool_t *bp) { return bp->bufsize; }

int bufpool_init(bufpool_t *bp, char *name, size_t bufsize, size_t nbufs, size_t nmax);
int bufpool_init_reserve(bufpool_t *bp, char *name, size_t bufsize, size_t nbufs,
		int reserve, size_t nmax);
void bufpool_deinit(bufpool_t *bp);
int bufpool_get(bufpool_t *bp, char **bufp, int noblock);/* pseudo-blocking */
int bufpool_get_reserve(bufpool_t *bp, char **bufp, int noblock);
int bufpool_get_zero(bufpool_t *bp, char **bufp, int noblock);
int bufpool_get_reserve_zero(bufpool_t *bp, char **bufp, int noblock);

void bufpool_put(bufpool_t *bp, char *buf);
void bufpool_dump(bufpool_t *bp, char *msg);

#endif /* BUFPOOL_H */
