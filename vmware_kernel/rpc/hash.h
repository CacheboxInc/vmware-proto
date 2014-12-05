#if !defined(__HASH_H__)
#define __HASH_H__

#include <assert.h>
#include <stddef.h>
#include "dll.h"


typedef struct hash_entry {
	dll_t	list;
	int	bucket;
} hash_entry_t;

typedef int (*cmp_fn_t) (hash_entry_t *, void *opaque);
typedef void (*cleanup_fn_t) (hash_entry_t *);

typedef struct hash_table {
	dll_t		*buckets;
/* suggest *bucket[] so that it is obvious it is an array */
	cmp_fn_t	cmp;
	long		count;		/* number of elements put on hash */
	int		no_buckets;
} hash_table_t;


int hash_init(hash_table_t *hash, int no_buckets, cmp_fn_t fun);
void hash_deinit(hash_table_t *hash);
void hash_cleanup(hash_table_t *hash, cleanup_fn_t cleanup);
int hash_add(hash_table_t *hash, hash_entry_t *new_entry, int bucket);
int hash_lookup(hash_table_t *hash, int bucket, hash_entry_t **entry, void *opaque);
void hash_rem(hash_table_t *hash, hash_entry_t *entry);

static inline int hash_no_buckets(hash_table_t *hash)
{
	assert(hash != NULL);
	return (hash->no_buckets);
}

static inline void hash_entry_init(hash_entry_t *e)
{
	DLL_INIT(&e->list);
	e->bucket = -1;
}
#endif
