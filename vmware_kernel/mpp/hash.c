#include "vmware_include.h"
#include "hash.h"

int hash_init(hash_table_t *hash, const char *name, module_global_t *module, int no_buckets, cmp_fn_t fun)
{
	int   i;
	dll_t *t;
	int   rc;
	char  n[128];

	assert(hash != NULL);
	assert(fun != NULL);
	assert(no_buckets > 0);

	memset(hash, 0, sizeof(*hash));

	rc = vmware_name(n, name, "hash", sizeof(n));
	if (rc < 0) {
		vmk_WarningMessage("%s: could not initialize name.\n", __func__);
		return -1;
	}

	rc = vmware_heap_create(&hash->heap_id, name, module, no_buckets,
			sizeof(*hash->buckets));
	if (rc < 0) {
		vmk_WarningMessage("%s: creating heap failed.\n", __func__);
		return -1;
	}

	hash->cmp        = fun;
	hash->no_buckets = no_buckets;
	hash->count      = 0;
	hash->buckets    = calloc(hash->heap_id, no_buckets, sizeof(*hash->buckets));
	if (hash->buckets == NULL) {
		vmk_HeapDestroy(hash->heap_id);
		return (-1);
	}

	for (i = 0; i < no_buckets; i++) {
		t = &hash->buckets[i];
		DLL_INIT(t);
	}
	return (0);
}

void hash_deinit(hash_table_t *hash)
{
	assert(hash != NULL);
	assert(hash->no_buckets > 0);
	assert(hash->count == 0);

	free(hash->heap_id, hash->buckets);
	vmk_HeapDestroy(hash->heap_id);
}

void hash_cleanup(hash_table_t *hash, cleanup_fn_t cb)
{
	int		i;
	dll_t		*t;
	dll_t		*h;
	hash_entry_t	*e;
	dll_t		*d;

	assert(hash != NULL);
	assert(hash->no_buckets > 0);

	for (i = 0; i < hash->no_buckets; i++) {
		h = &hash->buckets[i];
		t = DLL_PREV(h);
		while (t != h) {
			d = t;
			e = container_of(d, hash_entry_t, list);
			t = DLL_PREV(t);

			hash_rem(hash, e);
			if (cb != NULL) {
				cb(e);
			}
		}
	}

	assert(hash->count == 0);
}

int hash_add(hash_table_t *hash, hash_entry_t *new_entry, int bucket)
{
	dll_t *t;

	assert(hash != NULL);
	assert(new_entry != NULL);
	assert(hash->no_buckets > 0);
	assert(bucket >= 0 && bucket < hash->no_buckets);

	t = &hash->buckets[bucket];
	DLL_ADD(t, &new_entry->list);
	new_entry->bucket = bucket;
	hash->count++;

	return (0);
}

int hash_lookup(hash_table_t *hash, int bucket, hash_entry_t **entry, void *opaque)
{
	dll_t *h;
	dll_t *t;
	hash_entry_t *e;

	assert(hash != NULL);
	assert(hash->no_buckets > 0);
	assert(hash->cmp != NULL);
	assert(entry != NULL);
	assert(opaque != NULL);
	assert(bucket >= 0 && bucket < hash->no_buckets);

	t = h = &hash->buckets[bucket];
	t = DLL_NEXT(h);
	while (t != h) {
		e = container_of(t, hash_entry_t, list);
		if (hash->cmp(e, opaque) == 1) {
			*entry = e;
			return (0);
		}
		t = DLL_NEXT(t);
	}
	*entry = NULL;
	return (-1);
}

void hash_rem(hash_table_t *hash, hash_entry_t *entry)
{
	assert(hash != NULL);
	assert(entry != NULL);

	if (!DLL_ISEMPTY(&entry->list)) {
		DLL_REM(&entry->list);
		hash->count--;
	}

	assert(hash->count >= 0);
}
