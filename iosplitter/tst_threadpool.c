#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include "threadpool.h"

void  print_i(struct work *w, void *data)
{
	printf(".");
	fflush(stdout);
	sleep(1);
}

int main(void)
{
	int           rc;
	thread_pool_t tp;
	int           i;
	work_t        *w;

	rc = thread_pool_init(&tp, 32);
	assert(rc == 0);

	for (i = 0; i < 1000; i++) {
		w = new_work(&tp);
		assert(w != NULL);

		w->data = (void *) (uintptr_t) i;
		w->work_fn = print_i;

		schedule_work(&tp, w);
	}

	thread_pool_deinit(&tp);
	return 0;
}
