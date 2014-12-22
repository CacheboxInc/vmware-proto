/*
 * taskio.h
 *	pseudo-blocking IO for sockets and files/devices
 *	uses libaio and epoll
 */

#ifndef TASKIO_H
#define TASKIO_H

#include <unistd.h>
#include <inttypes.h>
#include <stdint.h>

#include "task.h"

/* macro to suppress unused variable warning */
#define unused(x) { (void)x; }

#define TASKIO_EBASE	1000
#define TASKIO_EBIGFD	(1 + TASKIO_EBASE)
#define TASKIO_ENOENT	(2 + TASKIO_EBASE)
#define TASKIO_EBADFD	(3 + TASKIO_EBASE)
#define TASKIO_EEXIST	(4 + TASKIO_EBASE)
#define TASKIO_ENOMEM	(5 + TASKIO_EBASE)
#define TASKIO_EINVAL	(6 + TASKIO_EBASE)
#define TASKIO_IOERR	(7 + TASKIO_EBASE)
#define TASKIO_ECONN	(8 + TASKIO_EBASE)


#define TASKIO_NIOEVENT	128

/*
 * we don't expect one CVA to support more than eighty sessions.
 *	each session will require upto {4 sockets, 1 event fd, 1 epoll fd}
 *  so N >  80 * 6 = 480
 */
#define TASKIO_MAXFDVALUE	512

typedef enum ti_rw {
	TASKIO_READ = 'r',
	TASKIO_WRITE = 'w',
} tirw_t;

typedef enum ti_tasktype {
	TASKIO_TYPE_LIBAIO = 1,
	TASKIO_TYPE_SOCKET = 2,
	TASKIO_TYPE_EVENT  = 3,
	TASKIO_TYPE_FIFO   = 4,
} titasktype_t;

int taskio_init(void);
void taskio_start(void);
void taskio_deinit(void);

int task_sockfd_register(int fd);
int task_eventfd_register(int fd, taskfnptr_t task, void *arg);
int task_fifofd_register(int fd, int input);
int task_fd_deregister(int fd);

int task_aiorw(int fd, char *buf, size_t nbytes, off_t offset, tirw_t rw,
		ssize_t *ret);
int task_netrw(int fd, char *buf, size_t nbytes, tirw_t rw);

static inline int task_netread(int fd, char *buf, size_t nbytes)
{
	return task_netrw(fd, buf, nbytes, TASKIO_READ);
}

static inline int task_netwrite(int fd, char *buf, size_t nbytes)
{
	return task_netrw(fd, buf, nbytes, TASKIO_WRITE);
}

static inline int task_aioread(int fd, char *buf, size_t nbytes, off_t offset, ssize_t *ret)
{
	return task_aiorw(fd, buf, nbytes, offset, TASKIO_READ, ret);
}

static inline int task_aiowrite(int fd, char *buf, size_t nbytes, off_t offset, ssize_t *ret)
{
	return task_aiorw(fd, buf, nbytes, offset, TASKIO_WRITE, ret);
}

int tasknet_setnoblock(int fd);
int tasknet_announce(char *server, int port, int *fdp);
int tasknet_accept(int fd, char *server, int *port, int *cfdp);
int tasknet_connect(char *server, int port, int *fdp);


int taskfifo_announce(const char *in, const char *out);
void taskfifo_denounce(const char *in, const char *out);
int taskfifo_accept(const char *in, const char *out, int *infd, int *outfd);
int taskfifo_connect(const char *in, const char *out, int *infd, int *outfd);
#endif /*TASKIO_H*/
