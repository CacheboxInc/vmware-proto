/*
 * bitmap.h
 *	a bitmap object with get, set and traversal
 *
 *	bitmap_init():	a bitmap_t of desired size is allocated 
 *	bitmap_set(), bitmap_reset(), bitmap_get():  manipulate individual bit
 *	bitmap_iter_reset():	initialize the bitmap iterator
 *	bitmap_iter_next():	get the next set bit address.
 */
#ifndef _BITMAP_H
#define _BITMAP_H
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
/*
 * Iterator state is folded into the object itself.
 */
typedef struct {
	uint64_t	*map;
	unsigned int	nbits;
	unsigned int	cur_bit;
	unsigned int	mapsz;	// number of elements in map
} bitmap_t;
#define BITMAP_ELEMSZ 64

int bitmap_init(bitmap_t **bmpp, unsigned int nbits);
int bitmap_iter_next(bitmap_t *bip, int *ndxp);
void bitmap_setall(bitmap_t *bip);

static inline void bitmap_set(bitmap_t *bmp, unsigned int k)
{
	int b = k / BITMAP_ELEMSZ;
	int c = k % BITMAP_ELEMSZ;

	assert(bmp && k < bmp->nbits);
	bmp->map[b] |= (1LL << c);
}

static inline void bitmap_reset(bitmap_t *bmp, unsigned int k)
{
	int b = k / BITMAP_ELEMSZ;
	int c = k % BITMAP_ELEMSZ;

	assert(bmp && k < bmp->nbits);
	bmp->map[b] &= ~(1LL << c);
}

static inline int bitmap_get(bitmap_t *bmp, int k)
{
	int b = k / BITMAP_ELEMSZ;
	int c = k % BITMAP_ELEMSZ;

	assert(bmp && k < bmp->nbits);
	return (bmp->map[b] & (1LL << c)) ? 1 : 0;
}

static inline void bitmap_iter_init(bitmap_t *bmp)
{
	bmp->cur_bit = 0;
}

static inline void bitmap_free(bitmap_t *bmp)
{
	free(bmp);
}

static inline unsigned int bitmap_getsize(bitmap_t *bmp)
{
	return bmp->nbits;
}

#endif /* _BITMAP_H */
