/*
 * Copyright(2013) Cachebox Inc.
 *
 * err.h
 * 
 */

#ifndef ERR_H
#define ERR_H

typedef enum {
	ERR = -1, /* catch all error */
	OK = 0,
	/* generic error */

	/* module specific errors */
	/** hash bmap full */
	BMAP_HFULL = 501 
} err_t;

#endif /* end of ERR_H */
