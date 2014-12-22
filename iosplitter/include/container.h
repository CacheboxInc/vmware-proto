/*
 * container.h
 */
#ifndef CONTAINER_H
#define CONTAINER_H

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "cdevcor.h"
#include "cdevtypes.h"
#include "dll.h"
#include "queue.h"
#include "cJSON.h"

#define BZERO(memptr)  memset((memptr), 0, sizeof(*memptr))

#define HASHFN(hashp, key)		((key) % ((hashp->h_tabsize)))
#define HASH_NDX_VALID(hashp, ndx)	((ndx) < (hashp->h_tabsize))

/*
 * hash table
 *		Concurrency control managed by the caller
 */

typedef uint64_t hashkey_t;

typedef struct hashtable {
	size_t		h_tabsize;	/* size of h_tab */
	int		h_count;	/* number of elements */
	struct queue	*h_tab;		/* array of bucket chain heads*/
} hashtable_t;

typedef struct hashbucket {
	dll_t		dll;
	hashkey_t	key;
} hashbucket_t;

#define HASHBUCKET_INIT(hbktp, id) { 	\
	DLL_INIT(&(hbktp)->dll) 			\
	(hbktp)->key = (id);				\
}

typedef struct
{
	hashtable_t	*hi_hp;
	int		hi_ndx;
	queue_iter_t	hi_qiter;
} hashtable_iter_t;

/*------------------------ phash_t -----------------------------------*/
/*
 * Note: h2j_ptr_t callback returns a cJSON array or object
 * that contains data of one hash table element. phash_flush will
 * free it up.
 * j2h_ptr_t callback is given a cJSON array or object constructed
 * from disk file. It should construct a hash table element from it.
 * Do not free 'item'; phash_reload or phash_init will take care of that.
 *
 * NOTE: if a cJSON array is constructed using cJSON_InsertInArray(),
 * the array elements will be stored in LIFO order. So j2h_ptr_t should
 * expect to get array elements in the reverse order to what h2j_ptr_t
 * inserted.
 */

typedef hashbucket_t *	(*j2h_ptr_t)(cJSON *item);
typedef cJSON *		(*h2j_ptr_t)(hashbucket_t *hbp);

typedef struct {
	hashtable_t	*hp;
	char		*fname;
	char		*tempname;
	j2h_ptr_t	j2h;	// JSON to hashbucket_t
	h2j_ptr_t	h2j; 	// hashbucket_t to JSON
} phash_t;



/*------------------------ function declarations ----------------------*/
int hashtable_init(hashtable_t *hashp, size_t tablesize);
void hashtable_deinit(hashtable_t *hashp);
void hashtable_add(hashtable_t *hashp, hashbucket_t *hbp);
int hashtable_lookup(hashtable_t *hashp, hashkey_t key, hashbucket_t **hbpp);
int hashtable_rem(hashtable_t *hashp, hashkey_t key,  hashbucket_t **hbpp);
void hashtable_drop(hashtable_t *hashp, hashbucket_t *hbp);
#define HASHTABLESZ(hashp) ((hashp)->h_tabsize)
#define HASHTABLECOUNT(hashp) ((hashp)->h_count)

void hashtable_iter_init(hashtable_iter_t *hip, hashtable_t *hp);
void *hashtable_iter_next(hashtable_iter_t *hip);

int filesize(int fd, off_t *size); /* do fstat and set st_size */
void cJSON_InsertItemInArray(cJSON *array, cJSON *item);
int phash_init(
	phash_t		*php,
	hashtable_t	*hp,
	char 		*fname,
	char		*tempname,
	j2h_ptr_t	j2h,
	h2j_ptr_t	h2j
	);
int phash_deinit(phash_t *php);
int phash_flush(phash_t *php);
int phash_reload(phash_t *php);


#endif /*CONTAINER_H*/

