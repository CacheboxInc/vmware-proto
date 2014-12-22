#ifndef BUFPOOL_H
#define BUFPOOL_H

#include <inttypes.h>
#include <stdint.h>
#include "libtask/task.h"
#include "dll.h"
#include "queue.h"
#include "cdevtypes.h"
#include "cdevcor.h"

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
	struct queue		freelist;
	size_t			issued;
	size_t			owned;
	size_t			bufsize;
	size_t			nbufs;
	size_t			nmax;
	Rendez			rendez;
	Rendez			deinit;   /* used to block bufpool_deinit */
	BUFPOOL_STATE		state;
#ifdef  BUFPOOL_STATS_ENABLED
	int			nmallocs;
	int			nfrees;
#endif
} bufpool_t;

static inline size_t bufpool_bufsize(bufpool_t *bp) { return bp->bufsize; }

int bufpool_init(bufpool_t *bp, size_t bufsize, size_t nbufs, size_t nmax);
void bufpool_deinit(bufpool_t *bp);
int bufpool_get(bufpool_t *bp, char **bufp, int noblock);/* pseudo-blocking */
int bufpool_get_zero(bufpool_t *bp, char **bufp, int noblock);
void bufpool_put(bufpool_t *bp, char *buf);
void bufpool_dump(bufpool_t *bp, char *msg);

#endif /* BUFPOOL_H */
