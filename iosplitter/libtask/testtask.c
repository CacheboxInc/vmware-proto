/*
 * testtask.c
 *	Start three pthreads
 *	two threads will generate primes and use aio to write to separate files
 *	another thread will screw up the global variables (start this first)
 *	
 */
#define _MULTI_THREADED		/* to enable __thread */
#define _GNU_SOURCE		/* to declare pthread_yield in pthread.h */
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "task.h"
#include "taskio.h"

void *shiva(void *arg);
void *eratos(void *arg);

int
main(int argc, char *argv[])
{
	pthread_t	tid[3];
	int		res;
	int		k;

	res = pthread_create(&tid[0], NULL, shiva, NULL);
	assert(res == 0);
	res = pthread_create(&tid[1], NULL, eratos, "eratos1.dat");
	res = pthread_create(&tid[2], NULL, eratos, "eratos2.dat");
	for (k = 0; k < 3; k++) {
		pthread_join(tid[k], NULL);
	}
	return 0;
}

void
shiva_main(void *arg)
{
	printf("shiva_main\n");
}

void *
shiva(void *arg)
{
	libtask_start(shiva_main, arg);
	return 0;
}

__thread int quiet;
__thread int goal;
__thread int buffer;
__thread char *filename;
__thread int fd;

void primetask(void *arg);
void prime_main(void *arg);

void *
eratos(void *arg)
{
	char	*fname = arg;
	int	res;
	
	printf ("%s starts\n", fname);
	filename = fname;
	goal = 100;
	res = taskio_init();
	assert(res == 0);
	taskio_start();
	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	assert(fd != -1);
	libtask_start(prime_main, arg);
	printf ("%s eratos ends\n", fname);
	close(fd);
	taskio_deinit();
	return 0;
}

void
primetask(void *arg)
{
	Channel *c, *nc;
	int p, i;
	int res = 0;
	c = arg;

	p = chanrecvul(c);
	if(p > goal)
		taskexitall(0);
	printf("%d ", p); fflush(stdout);
	pthread_yield();
	//res = write(fd, &p, sizeof(p));
	res = task_aiowrite(fd, (char *)&p, sizeof(p), p * 512); // independent blocks
	assert(res == 0);
	pthread_yield();
	nc = chancreate(sizeof(unsigned long), buffer);
	taskcreate(primetask, nc, 32768);
	pthread_yield();
	for(;;){
		i = chanrecvul(c);
		if(i%p)
			chansendul(nc, i);
	}
}

void
prime_main(void *arg) // arg not used
{
	int i;
	Channel *c;

	printf("%s: goal=%d\n", filename, goal);

	c = chancreate(sizeof(unsigned long), buffer);
	taskcreate(primetask, c, 32768);
	for(i=2;; i++)
		chansendul(c, i);
}
