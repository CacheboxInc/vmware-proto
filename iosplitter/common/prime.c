#include "dll.h"
#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <math.h>

typedef struct plist{
	dll_t	nxt;
	uint64_t	prime;
} plist_t;

/* find the first prime number larger than the given num */
uint64_t next_prime(uint64_t num)
{
	dll_t	head, *curr;
	uint64_t	lastprime, next, found;
	plist_t *item;
	double max;

	lastprime = 2;
	DLL_INIT(&head);

	item = malloc(sizeof(plist_t));
	item->prime = 2;
	DLL_ADD(&head, &item->nxt);

	next = lastprime + 1;
	while(lastprime < num) {
		curr = DLL_NEXT(&head);
		found = 0;
		max = sqrt(next);
		while(curr != &head) {
			item = (plist_t *)curr;
			if (next > 100 && (item->prime > max + 1))
				break;
			if (next % item->prime == 0) {
				found = 1;
				break;
			}
			curr = DLL_NEXT(curr);
		}
		if (!found) {
			item = malloc(sizeof(plist_t));
			item->prime = next;
			DLL_REVADD(&head, &item->nxt);
			lastprime = next;
		}
		next++;
	}
	/* free the list of primes */
	while (!DLL_ISEMPTY(&head)) {
		curr = DLL_NEXT(&head);
		DLL_REM(curr);
		free(curr);
	}
		
	return lastprime;
}

