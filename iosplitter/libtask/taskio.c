/*
 * taskio.c
 *	pseudo-blocking IO for sockets and files/devices
 *	uses libaio and epoll
 */

#define _MULTI_THREADED		/* to enable __thread */
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <task.h>
#include <errno.h>
#include <libaio.h>
#include <limits.h>
#include <unistd.h>
#include "taskio.h"
#include "taskimpl.h"
#define STATIC 

/*#define TRACE(X)	{ printf("TRACE: %s(%d):", __FILE__, __LINE__); printf X; fflush(stdout); }*/
#define TRACE(...)	{					\
  fprintf(stderr, "TRACE: %s(%d):", __FILE__, __LINE__);	\
  fprintf(stderr, __VA_ARGS__);					\
  fflush(stdout); 						\
}
#define TRC(t)
#define PERROR(X)   perror(X)
#define ERROR(...) { fprintf(stderr, __VA_ARGS__); fflush(stderr); }

unsigned long long g_task_net_io_sleep;
unsigned long long g_task_net_io_wakeup;
unsigned long long g_task_aio_io_sleep;
unsigned long long g_task_aio_io_wakeup;
unsigned long long g_libaio_done;
unsigned long long g_libaio_wakeup;

struct TaskContext {
	uint32_t	tasktype;  // TASKIO_LIBAIO or TASKIO_SOCK
	int		fd;
};

struct TaskLibaioContext {
	struct TaskContext	hdr;
	int			result;
	Task			*task;
};

struct TaskSocketContext {
	struct TaskContext	hdr;
	//int			bits; 	// EPOLLIN EPOLLOUT
	Task			*reader;
	Task			*writer;
};

struct TaskEventContext {
	struct TaskContext	hdr;
	taskfnptr_t		task;
	void			*arg;
};

struct LibaioState {
	Task	*waiter;
	int	res;
	int	res2;
};

void dump_epoll_event(char *msg, struct epoll_event *p);
void dump_event(char * msg, struct io_event *p);
void dump_iocb(char * msg, struct iocb *p);

STATIC void eventio_done(struct epoll_event *evp);
STATIC void aiotask(void *v);
STATIC int ctxt_insert(int fd, struct TaskContext *p);
STATIC int ctxt_lookup(int fd, struct TaskContext **pp);
STATIC int ctxt_delete(int fd);


/* ----- global thread-local variables ---*/
// XXX We can make this array grow on demand by using realloc
STATIC __thread struct TaskContext *TCarray[TASKIO_MAXFDVALUE];
STATIC __thread int taskio_epollfd = -1;			//see taskio_init
STATIC __thread int taskio_eventfd = -1;			//see taskio_init
STATIC __thread io_context_t taskio_ioctx;			//see taskio_init

#if 0
static void
dump_sock_cntxt(struct TaskSocketContext *p)
{
	printf("TaskSocketContext dump %p\n", p);
	printf("\tfd          %d\n", p->hdr.fd);
	printf("\treader      %p\n", p->reader);
	printf("\twriter      %p\n", p->writer);
}
#endif

/*
 *  Set up epoll fd,
 *  for libaio:
 *		setup io_context, event fd, register event fd with epoll fd
 */
int
taskio_init(void)
{
	int							res;
	struct epoll_event			ev;
	struct TaskLibaioContext	*tlcp = NULL;

	memset(&taskio_ioctx, 0, sizeof(taskio_ioctx));
	if ((res = io_setup(TASKIO_NIOEVENT, &taskio_ioctx)) != 0) {
		return res;
	}
	if ((taskio_epollfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
		res = errno;
		goto out;
	}
	// XXX why eventfd is non blocking? there is some subtle reason but cant recall
	if ((taskio_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) == -1) {
		res = errno;
		goto out;
	}
	assert(taskio_eventfd < TASKIO_MAXFDVALUE);
	assert(taskio_eventfd >= 0 && TCarray[taskio_eventfd] == NULL);
	if ((tlcp = (struct TaskLibaioContext*)calloc(1, sizeof(*tlcp))) == NULL) {
		res = TASKIO_ENOMEM;
		goto out;
	}
	tlcp->hdr.tasktype = TASKIO_TYPE_LIBAIO;
	tlcp->hdr.fd = taskio_eventfd;
	tlcp->task = NULL;
	ev.events = EPOLLIN;
	ev.data.ptr = tlcp;
	if (epoll_ctl(taskio_epollfd, EPOLL_CTL_ADD, taskio_eventfd, &ev) != 0) {
		res = errno;
		free(tlcp);
		goto out;
	}
	ctxt_insert(taskio_eventfd, (struct TaskContext*)tlcp);
	return 0;

out:
	taskio_deinit();
	return res;
}

void
taskio_start(void)
{
	taskcreate(aiotask, 0, 32*1024);
}


/*
 *  shutdown epoll fd, event fd io_context
 *  XXX also clear TCarray.
 *  XXX shut down aiotask
 */
void
taskio_deinit(void)
{
	if (taskio_eventfd != -1) {
		struct TaskContext *t;
		struct epoll_event e = {0};

		if (ctxt_lookup(taskio_eventfd, &t) == 0) {
			ctxt_delete(taskio_eventfd);
			free(t);
		}

		epoll_ctl(taskio_epollfd, EPOLL_CTL_DEL, taskio_eventfd, &e);

		close(taskio_eventfd);
		taskio_eventfd = -1;
	}

	if (taskio_epollfd) {
		close(taskio_epollfd);
		taskio_epollfd = -1;
	}
	io_destroy(taskio_ioctx);
	memset(&taskio_ioctx, 0, sizeof(taskio_ioctx));
}

/*
 * register a socket fd with taskio_epollfd for both read and write
 */
static int register_epoll(int fd, uint32_t events, struct TaskContext *tcp)
{
	struct epoll_event ev;

	ev.events   = events;
	ev.data.ptr = tcp;
	if (epoll_ctl(taskio_epollfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		return (errno);
	}
	return (0);
}

static int task_fd_register(int fd, titasktype_t type, uint32_t events,
		int reg_epoll)
{
	struct TaskSocketContext	*tscp;
	int				res;

	tasknet_setnoblock(fd);

	/* cover all cases: fd value is bad or fd already registered */
	res = ctxt_lookup(fd, NULL);
	if (res != TASKIO_ENOENT) {
		return (res ? res : TASKIO_EEXIST);
	}
	if ((tscp = (struct TaskSocketContext*)calloc(1, sizeof(*tscp))) == NULL) {
		return (TASKIO_ENOMEM);
	}
	tscp->hdr.tasktype = type;
	tscp->hdr.fd = fd;

	res = ctxt_insert(fd, (struct TaskContext*)tscp);
	if (res != 0) {
		free(tscp);
		return (res);
	}

	if (reg_epoll == 0) {
		return (0);
	}

	res = register_epoll(fd, events, (struct TaskContext *) tscp);
	if (res != 0) {
		ctxt_delete(fd);
		free(tscp);
		return (res);
	}

	return (0);
}

int task_sockfd_register(int fd)
{
	uint32_t event = EPOLLOUT | EPOLLIN | EPOLLET;
	return (task_fd_register(fd, TASKIO_TYPE_SOCKET, event, 1));
}
/*
 * Register a bare eventfd with epoll loop
 */
int
task_eventfd_register(int fd, taskfnptr_t task, void *arg)
{
	struct TaskEventContext	*tscp;
	int			res;
	struct epoll_event	ev;

	/* cover all cases: fd value is bad or fd already registered */
	res = ctxt_lookup(fd, NULL);
	if (res != TASKIO_ENOENT) {
		return res ? res : TASKIO_EEXIST;
	}
	if ((tscp = (struct TaskEventContext*)calloc(1, sizeof(*tscp))) == NULL) {
		return TASKIO_ENOMEM;
	}
	tscp->hdr.tasktype = TASKIO_TYPE_EVENT;
	tscp->hdr.fd = fd;
	tscp->task = task;
	tscp->arg = arg;

	/* register only for incoming writes on eventfd */
	ev.events = EPOLLIN;
	ev.data.ptr = tscp;
	if (epoll_ctl(taskio_epollfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		res = errno;
		free(tscp);
		return res;
	}
	ctxt_insert(fd, (struct TaskContext*)tscp);
	return 0;
}

int task_fifofd_register(int fd, int input)
{
	uint32_t	events;
	if (input == 1) {
		events    = EPOLLIN;
	} else {
		events    = EPOLLOUT;
	}
	return (task_fd_register(fd, TASKIO_TYPE_FIFO, events, input));
}

int task_fd_deregister(int fd)
{
	int				rc;
	struct TaskSocketContext	*p;
	struct epoll_event		e;

	rc = fcntl(fd, F_GETFD);
	if (rc >= 0 || errno != EBADF) {
		fprintf(stderr, "%s: fd(%d) must be closed before deregistering.\n",
				__func__, fd);
		return (-1);
	}

	rc = ctxt_lookup(fd, (struct TaskContext **) &p);
	if (rc == 0) {
		int yield = 0;

		if (p->reader != NULL) {
			taskready(p->reader);
			yield = 1;
		}
		if (p->writer != NULL) {
			taskready(p->writer);
			yield = 1;
		}
		p->reader = NULL;
		p->writer = NULL;

		if (ctxt_delete(fd) < 0) {
			return (-1);
		}

		free(p);

		if (yield == 1) {
			taskyield();
		}
	}

	return (epoll_ctl(taskio_epollfd, EPOLL_CTL_DEL, fd, &e));
}

static inline ssize_t task_aiorw_ret(struct LibaioState *ls)
{
	return ((ssize_t)(((uint64_t)ls->res2 << 32) | ls->res));
}

int
task_aiorw(int fd, char *buf, size_t nbytes, off_t offset, tirw_t rw,
		ssize_t *ret)
{
	struct TaskLibaioContext	*tlcp = NULL;
	struct iocb 			*iba[1];
	struct LibaioState		ls;
	struct iocb			cb;
	int				res;

	switch(rw) {
	case TASKIO_READ: break;
	case TASKIO_WRITE: break;
	default:
		assert(0);
		return TASKIO_EINVAL;
	}
	/* note: we don't really need tlcp except to verify that taskio_eventfd was registered */
	if ((res = ctxt_lookup(taskio_eventfd, (struct TaskContext**)&tlcp)) != 0) {
		return res;
	}
	assert(tlcp && tlcp->hdr.tasktype == TASKIO_TYPE_LIBAIO);

	if (rw == TASKIO_WRITE) {
		io_prep_pwrite(&cb, fd, buf, nbytes, offset);
	} else {
		io_prep_pread(&cb, fd, buf, nbytes, offset);
	}
	io_set_eventfd(&cb, taskio_eventfd);
	memset(&ls, 0, sizeof(ls));
	ls.waiter = taskrunning;
	cb.data = &ls;
	iba[0] = &cb;
	res = io_submit(taskio_ioctx, 1, iba);
	assert(res == 1);
	g_task_aio_io_sleep++;
	taskswitch();
	g_task_aio_io_wakeup++;
	/* wake up after io completion. LibaioState ls has been filled up for us */
	// XXX can we do a better job of reporting errors here?
	*ret = task_aiorw_ret(&ls);
	return (*ret == nbytes) ? 0 : TASKIO_IOERR;
}

int
task_netrw(int fd, char *buf, size_t nbytes, tirw_t rw)
{
	int							n = nbytes;
	int							res;
	struct TaskSocketContext	*tscp;

	switch(rw){
	case TASKIO_READ: break;
	case TASKIO_WRITE:break;
	default:
		assert(0);
		return TASKIO_EINVAL;
	}

	if ((res = ctxt_lookup(fd, (struct TaskContext**)&tscp)) != 0) {
		return res;
	}
	assert(tscp && (tscp->hdr.tasktype == TASKIO_TYPE_SOCKET ||
			tscp->hdr.tasktype == TASKIO_TYPE_FIFO) &&
			tscp->hdr.fd == fd);
	//printf("task_netrw: %c: fd =%d, nbytes=%d\n", rw, fd, nbytes);
	//dump_sock_cntxt(tscp);

	do {
		//printf("task_netrw: issue read/write ...\n");
		res = (rw == TASKIO_WRITE)? write(fd, buf, n) : read(fd, buf, n);
		if (res  > 0) {
			//printf("task_netrw: %c: %d bytes\n", rw, res);
			buf += res;
			n -= res;
		} else if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (rw == TASKIO_WRITE) {
					assert(tscp->writer == NULL);
					tscp->writer = taskrunning;
				} else {
					assert(tscp->reader == NULL);
					tscp->reader = taskrunning;
				}
				//printf("task_netrw: sleeping on fd %d\n", fd);
				//dump_sock_cntxt(tscp);
				g_task_net_io_sleep++;
				taskswitch();
				g_task_net_io_wakeup++;
				//printf("task_netrw: resumed on fd %d\n", fd);
			} else {
				res = errno;
				assert(res);
				//fprintf(stderr, "task_netrw: %c: error %d\n", rw, res);
				return res; /* fatal error */
			}
		} else {
			/* other side closed the connection */
			//fprintf(stderr, "task_netrw: %c: connection closed\n", rw);
			return TASKIO_ECONN;
		}
	} while (n > 0);

	return 0;

}

/*
 * case EPOLLERR or EPOLLHUP: wake up both reader and writer
 * otherwise, wake up reader for EPOLLIN, writer for EPOLLOUT
 * XXX if needed, we can pass error status to reader or writer by adding members in tscp
 */
STATIC void
sockio_done(struct epoll_event *evp)
{
	struct TaskSocketContext	*tscp = evp->data.ptr;
	int				wakey = 0;

	assert(tscp && (tscp->hdr.tasktype == TASKIO_TYPE_SOCKET ||
				tscp->hdr.tasktype == TASKIO_TYPE_FIFO));
	assert (tscp->reader || tscp->writer);

	if (evp->events & EPOLLERR) wakey |= 0x3;
	if (evp->events & EPOLLHUP) wakey |= 0x3;
	if (evp->events & EPOLLIN) wakey |= 0x1;
	if (evp->events & EPOLLOUT) wakey |= 0x2;

	//printf("sockio_done on fd %d: events = 0x%x\n", tscp->hdr.fd, evp->events);
	//printf("sockio_done: wakey 0x%x, reader %p, writer %p\n", wakey, tscp->reader, tscp->writer);
	if ((wakey & 0x1) && tscp->reader) {
		taskready(tscp->reader);
		//printf("sockio_done on fd %d: taskready reader\n", tscp->hdr.fd);
		tscp->reader = NULL;
	}
	if ((wakey & 0x2) && tscp->writer) {
		taskready(tscp->writer);
		//printf("sockio_done on fd %d: taskready writer\n", tscp->hdr.fd);
		tscp->writer = NULL;
	}
}

STATIC int fifoio_done(struct epoll_event *e)
{
	struct TaskSocketContext *t = e->data.ptr;
	int hup;

	hup = !!(e->events & EPOLLHUP);

	if (hup && !t->writer && !t->reader) {
		return 0;
	}

	sockio_done(e);

	if (e->events & EPOLLERR) {
		return (-1);
	}
	return (0);
}

/*
 * Note: evp->data.ptr points to TaskLibaioContext * but we don't use it
 * because there is a single global taskio_eventfd.
 * in case multiple eventfd's are created, we will pick up the pointer from evp.
 * Note: evp->obj points to a struct iocb but we don't use it
 */
STATIC void
libaio_done(struct epoll_event *evp)
{
	uint64_t		events;
	uint64_t		nio;
	int			res;
	struct LibaioState	*lsp;
	struct io_event		*ep;
	static __thread struct io_event	ea[TASKIO_NIOEVENT];

	res = read(taskio_eventfd, &events, sizeof(events));
	assert(res == 8);

	while (events > 0) {

		nio = events;
		if (nio > TASKIO_NIOEVENT) {
			nio = TASKIO_NIOEVENT;
		}

		/* reap the events */
restart:
		memset(ea, 0, sizeof(ea[0]) * nio);
		res = io_getevents(taskio_ioctx, nio, nio, ea, NULL);
		if (res < 0) {
			fprintf(stderr, "io_getevents failed: %s\n", strerror(errno));
			if (errno == EINTR) {
				goto restart;
			}
		}
		assert(res == nio);

		/* signal each sleeper on task_aiorw */
		/* note: ep res,res2 are unsigned but may carry negative error codes */
		g_libaio_done++;
		for (ep = ea; ep < ea + res; ep++) {
			//dump_event("libaio_done", ep);
			lsp = ep->data;
			lsp->res = ep->res;
			lsp->res2 = ep->res2;
			assert(lsp && lsp->waiter);
			taskready(lsp->waiter);
			g_libaio_wakeup++;
		}

		events -= nio;
	}
}

STATIC void
eventio_done(struct epoll_event *evp)
{
	uint64_t		nio;
	int			res, k;
	struct TaskEventContext	*tscp = evp->data.ptr;

	res = read(taskio_eventfd, &nio, sizeof(nio));
	assert(res == 8);
	assert(nio > 0);
	// XXX this is dangerous. Need to put upper limit on nio
	for(k = 0; k < nio; k++) {
		taskcreate(tscp->task, tscp->arg, 32 * 1024);
	}
}

STATIC void
aiotask(void *v)
{
	struct epoll_event	ev[TASKIO_NIOEVENT];
	int			k, n;

	taskname("aiotask");
	tasksystem();
	assert(taskio_epollfd >= 0);
	for (;;) {
		while((k = taskyield()) > 0) {
			/* wait until all other tasks sleep */
			//printf("aiotask: taskyield gave %d\n", k);
		}
		//printf("aiotask: starting pollwait\n");
		memset(ev, 0, sizeof(ev));
		if (taskio_epollfd == -1) {
			/*
			 * TODO:
			 * =====
			 * Find a better way to detect for aiotask to know that
			 * it must exit.
			 */

			/* taskio_deinit must have been called */
			assert(taskio_eventfd == -1);
			break;
		}

		n = epoll_wait(taskio_epollfd, ev, TASKIO_NIOEVENT, -1);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
		}
		assert(n > 0); // XXX handle signals n==-1, errno==EINTR
		for(k = 0; k < n; k++) {
			//dump_epoll_event("someio_epoll_wait", &ev[k]);
			switch(((struct TaskContext*)ev[k].data.ptr)->tasktype) {
			case TASKIO_TYPE_LIBAIO:
				libaio_done(&ev[k]);
				break;
			case TASKIO_TYPE_SOCKET:
				sockio_done(&ev[k]);
				break;
			case TASKIO_TYPE_FIFO:
				fifoio_done(&ev[k]);
				break;
			case TASKIO_TYPE_EVENT:
				eventio_done(&ev[k]);
				break;
			default:
				assert(0);
			}
		}
	}

	taskexit(0);
}

/*
 * insert TaskContext pointer against given fd
 * 	return 0 on success, TASKIO_ERR_{BIGFD, ENOENT} errors
 *  XXX it is possible to return existing ptr instead of just EEXIST error, 
 *  XXX but don't see the need for it now.
 */
STATIC int
ctxt_insert(int fd, struct TaskContext *p)
{
	if (fd < 0) {
		return TASKIO_EBADFD;
	}
	if (fd >= TASKIO_MAXFDVALUE) {
		return TASKIO_EBIGFD;
	}
	if (TCarray[fd] != NULL ) {
		return TASKIO_EEXIST;
	}
	TCarray[fd] = p;
	return 0;
}

/*
 * delete the pointer from TCarray.
 * caller will free the allocated memory
 */
STATIC int
ctxt_delete(int fd)
{
	if (fd < 0) {
		return TASKIO_EBADFD;
	}
	if (fd >= TASKIO_MAXFDVALUE) {
		return TASKIO_EBIGFD;
	}
	assert(TCarray[fd] != NULL);
	TCarray[fd] = NULL;
	return 0;
}

/*
 * pp can be null.
 */
STATIC int
ctxt_lookup(int fd, struct TaskContext **pp)
{
	if (fd < 0) {
		return TASKIO_EBADFD;
	}
	if (fd >= TASKIO_MAXFDVALUE) {
		return TASKIO_EBIGFD;
	}
	if (TCarray[fd] == NULL ) {
		return TASKIO_ENOENT;
	}
	if (pp) {
		*pp = TCarray[fd];
	}
	return 0;
}

void dump_iocb(char * msg, struct iocb *p)
{
	printf("struct iocb: %s:\n", msg);
	printf("\tdata          %p\n", p->data);
	printf("\tkey           0x%x\n", p->key);
	printf("\tfildes        %d\n", p->aio_fildes);
	printf("\tbuf           %p\n", p->u.c.buf);
	printf("\toffset        %lld\n", p->u.c.offset);
	printf("\tnbytes        %ld\n", p->u.c.nbytes);
}

void dump_event(char * msg, struct io_event *p)
{
	printf("----------------\nstruct io_event: %s\n", msg);
	printf("\tdata           %p\n", p->data);
	printf("\tobj            %p\n", p->obj);
	printf("\tres            %ld\n", p->res);
	printf("\tres2           %ld\n", p->res2);
	dump_iocb("obj", p->obj);
	printf("--------------\n");
}

void dump_epoll_event(char *msg, struct epoll_event *p)
{
	struct TaskSocketContext *tscp =  p->data.ptr;
	printf("-----------------\nstruct epoll_event: %s\n", msg);
	printf("\tevents          0x%x\n", p->events);
	printf("\tptr             %p\n", p->data.ptr);
	printf("\tfd              %d\n", tscp->hdr.fd);
}

/* ################  Network Related ################## */

/* XXX netlookup blocks and it uses deprecated gethostbyname.
 * There are async libraries available.
 * http://c-ares.haxx.se/otherlibs.html
 */

int
tasknet_setnoblock(int fd)
{
	int		res;

	if ((res = fcntl(fd, F_GETFL)) == -1) {
		return errno;
	}
	if (fcntl(fd, F_SETFL, res | O_NONBLOCK) == -1) {
		return errno;
	}
	return 0;
}

/*
 *  create and bind a TCP socket to given address, put it in *fdp
 *  char *server may be NULL, then listen on all available addresses.
 *	return 0 on success, else error code
 *  (adapted from libtask netannounce)
 */
int
tasknet_announce(char *server, int port, int *fdp)
{
	int 			fd, n;
	socklen_t		sn;
	uint32_t		ip;
	int			res;
	struct sockaddr_in	sa;


	assert(fdp);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	if (server != nil && strcmp(server, "*") != 0) {
		/* using a libtask function */
		if(netlookup(server, &ip) < 0) {
			return -1;
		}
		memmove(&sa.sin_addr, &ip, 4);
	}
	sa.sin_port = htons(port);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		return errno;
	}
	/* set reuse flag for tcp */
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (void*)&n, &sn) == 0) {
		n = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&n, sizeof n);
	} else {
		PERROR("tasknet_announce: getsockopt failed");
	}
	if (bind(fd, (struct sockaddr*)&sa, sizeof sa))  {
		PERROR("tasknet_announce: bind");
		goto errout;
	}
	if (listen(fd, 16)) {
		PERROR("tasknet_announce: listen");
		goto errout;
	}
	/* We want a non blocking socket, as we sleep after getting a error in accept()*/
	/* XXX handle error return:  */
	tasknet_setnoblock(fd);
	if ((res = task_sockfd_register(fd)) != 0) {
		ERROR("task_sockfd_regiser failed %d\n", res)
		errno = res;
		goto errout;
	}
	*fdp = fd;
	return 0;

errout:
	close(fd);
	return errno;
}

/*
 * Accept incoming TCP connections from client
 * fd is a listen socket on well-known port
 * returns 0 on success and sets new socket fd for incoming connection in *cfdp.
 * fills up name and port of client if server and port are non-null, respectively
 * (adapted from libtask netaccept)
 * WARNING: only one task can invoke this at a time on a given fd.
 */
int
tasknet_accept(int fd, char *server, int *port, int *cfdp)
{
	int cfd, one;
	struct sockaddr_in sa;
	uchar *ip;
	socklen_t len;
	int							res;
	struct TaskSocketContext	*tscp;

	assert(cfdp);
	if ((res = ctxt_lookup(fd, (struct TaskContext**)&tscp)) != 0) {
		return res;
	}
	assert(tscp && tscp->hdr.tasktype == TASKIO_TYPE_SOCKET && tscp->hdr.fd == fd);
	while(1) {
		len = sizeof(sa);
		if((cfd = accept(fd, (void*)&sa, &len)) != -1) {
			break;
		}
		res = errno;
		if (res != EAGAIN && res != EWOULDBLOCK) {
			PERROR("accept");
			return res;
		}
		/* sleep until epoll wakes us up */
		assert(tscp->reader == NULL); /* XXX handle this case, not just assert */
		tscp->reader = taskrunning;
		taskswitch();
	}
	/* got a connection */
	assert(cfd > 0);
	if(server){
		ip = (uchar*)&sa.sin_addr;
		snprint(server, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
	}
	if(port) {
		*port = ntohs(sa.sin_port);
	}
	tasknet_setnoblock(cfd);
	one = 1;
	/* Turn off Nagle algorithm, we expect to do write-write-read patterns */
	setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);
	if ((res = task_sockfd_register(cfd)) != 0) {
		ERROR("tasknet_accept: task_sockfd_reigster failed: %d\n",
						res)
		close(cfd);
		return res;
	}
	*cfdp = cfd;
	return 0;
}

/*
 * Client connects to given server, port.
 * returns 0 on success and sets *fdp to socket fd connected to server
 * else returns error code.
 * Internally, registers socket fd with taskio
 * Note: pseudo-blocking on connect call.
 */
int
tasknet_connect(char *server, int port, int *fdp)
{
	int					fd, n;
	uint32_t			ip;
	socklen_t			sn;
	struct sockaddr_in	sa;
	int							res;
	struct TaskSocketContext	*tscp;

	assert(server && fdp);
	/* using libtask function: */
	if (netlookup(server, &ip) < 0) {
		return -1;
	}
	if((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		res = errno;
		PERROR("tasknet_connect: socket");
		return res;
	}
	tasknet_setnoblock(fd);
	if ((res = task_sockfd_register(fd)) != 0) {
		ERROR("tasknet_connect: task_sockfd_reigster failed: %d\n", res)
		goto errout;
	}

	/* start connecting */
	memset(&sa, 0, sizeof sa);
	memmove(&sa.sin_addr, &ip, 4);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (connect(fd, (struct sockaddr*)&sa, sizeof sa) == 0) {
		goto connected;
	}
	if (errno != EINPROGRESS) {
		res = errno;
		goto errout;
	}
	if ((res = ctxt_lookup(fd, (struct TaskContext**)&tscp)) != 0) {
		goto errout;
	}
	assert(tscp && tscp->hdr.tasktype == TASKIO_TYPE_SOCKET && tscp->hdr.fd == fd);
	/* sleep until epoll wakes us up */
	assert(tscp->writer == NULL); /* XXX handle this case */
	tscp->writer = taskrunning;
	taskswitch();
	/* we wake up when connect completes. check for error using getsockopt*/

connected:
	sn = sizeof sa;
	/* XXX is this the best test for successful connection?  */
	if (getpeername(fd, (struct sockaddr*)&sa, &sn) == 0) {
		*fdp = fd;
		return 0;
	}
	/* report error */
	sn = sizeof(n);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&n, &sn); /* XXX handle call error */
	res = n ? n : ECONNREFUSED;

errout:
	close(fd);
	return res;
}

/*
 * taskfifo_announce() - create fifo pipes
 */
int taskfifo_announce(const char *in, const char *out)
{
	int rc;

	rc = mkfifo(in, 0777);
	if (rc < 0) {
		return (rc);
	}

	rc = mkfifo(out, 0777);
	if (rc < 0) {
		remove(in);
		return (rc);
	}

	return (0);
}

/*
 * taskfifo_denounce() - remove fifo pipes
 */
void taskfifo_denounce(const char *in, const char *out)
{
	remove(in);
	remove(out);
}

/*
 * taskfifo_accept() - accept a new client connection
 *
 * Please NOTE: this is a BLOCKING CALL. The function would be blocked until a
 * connection from client is established.
 *
 * Please read taskfifo_connect for details.
 */
int taskfifo_accept(const char *in, const char *out, int *infd, int *outfd)
{
	int ifd;
	int ofd;
	int rc;

	ifd    = -1;
	ofd    = -1;
	*infd  = -1;
	*outfd = -1;

	ifd = open(in, O_RDONLY);
	if (ifd < 0) {
		return (ifd);
	}
	rc = fcntl(ifd, F_SETPIPE_SZ, 10 * 1<<20);
	if (rc < 0) {
		fprintf(stderr, "setting pipe capacity failed.\n");
	}

	tasknet_setnoblock(ifd);
	if ((rc = task_fifofd_register(ifd, 1)) != 0) {
		goto out;
	}

	ofd = open(out, O_WRONLY);
	if (ofd < 0) {
		rc = ofd;
		goto out;
	}
	rc = fcntl(ofd, F_SETPIPE_SZ, 10 * 1<<20);
	if (rc < 0) {
		fprintf(stderr, "setting pipe capacity failed.\n");
	}

	tasknet_setnoblock(ofd);
	if ((rc = task_fifofd_register(ofd, 0)) != 0) {
		goto out;
	}
	*infd  = ifd;
	*outfd = ofd;
	return (0);
out:
	if (ifd != -1) {
		close(ifd);
		task_fd_deregister(ifd);
	}

	if (ofd != -1) {
		close(ofd);
		task_fd_deregister(ofd);
	}
	return (rc);
}

/*
 * taskfifo_connect() - establish a connection with server
 *
 * This function looks similar to taskfifo_accept. However, there is slight
 * difference in order in which fifo pipes are opened by client.
 *
 * FIFO pipes provide mechanism for two processes to communicate with each
 * other. Pipes are similar to network socket with two major
 * differences/limitations
 * 	1. Pipe are unidirectional
 *	2. Opening a pipe to read/write is blocked until writer/reader
 *         opens the pipe.
 *
 * Two pipes can be used to provide bi-directional interprocess communication
 * mechanism. Server can create two pipes, one for reading and other for
 * writing. The client should also use two pipes to communicate with server.
 * However, as mentioned in second limitation above, opening a pipe which has
 * no reader blocks open system call. Therefore, the order in which to open
 * pipes is very important.
 *
 * Following is pseudo algorithm of task_accept() and task_connect()
 *
 * task_accept(in, out) {
 *    open(in, O_RDONLY);
 *    open(out, O_WRONLY);
 * }
 *
 * task_connect(in, out) {
 *    open(out, O_WRONLY);
 *    open(in, O_RDONLY);
 * }
 *
 * Server calls task_accpet and client call task_connect. After the calls,
 *         server.in  is connected to client.out and
 *         server.out is connected to client.in
 */
int taskfifo_connect(const char *in, const char *out, int *infd, int *outfd)
{
	int ifd;
	int ofd;
	int rc;

	ifd    = -1;
	ofd    = -1;
	*infd  = -1;
	*outfd = -1;

	ofd = open(out, O_WRONLY);
	if (ofd < 0) {
		return (ofd);
	}
	rc = fcntl(ofd, F_SETPIPE_SZ, 10 * 1<<20);
	if (rc < 0) {
		fprintf(stderr, "setting pipe capacity failed.\n");
	}

	tasknet_setnoblock(ofd);
	if ((rc = task_fifofd_register(ofd, 0)) != 0) {
		rc = -1;
		goto out;
	}

	ifd = open(in, O_RDONLY);
	if (ifd < 0) {
		rc = ifd;
		goto out;
	}
	rc = fcntl(ifd, F_SETPIPE_SZ, 10 * 1<<20);
	if (rc < 0) {
		fprintf(stderr, "setting pipe capacity failed.\n");
	}

	tasknet_setnoblock(ifd);
	if ((rc = task_fifofd_register(ifd, 1)) != 0) {
		goto out;
	}
	*infd  = ifd;
	*outfd = ofd;
	return (0);
out:
	if (ifd != -1) {
		close(ifd);
		task_fd_deregister(ifd);
	}

	if (ofd != -1) {
		close(ofd);
		task_fd_deregister(ofd);
	}
	return (rc);
}

/* HACKY FIX for undefined references from net.c (because I removed fd.c)*/
void
fdwait(int a, int b) {
	fprintf(stderr, "DO NOT USE fdwait\n");
	abort();
}
int
fdnoblock(int a)
{
	fprintf(stderr, "DO NOT USE fdnoblock\n");
	abort();
	return 0;
}
