#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "rpc.h"
#include "bufpool.h"
#include "libtask/task.h"
#include "libtask/taskio.h"
#include "tst-rpc.h"

#define NO_THREADS 32

/* TODO:
   	0. Change tst-rpc.h so that mpp<-->iosplitter comn would work.
	1. Think about data structure requiremnt in multiple open messages from
	   MPP. Handle initial session establishment in MPP->IOsplitter->CVA
	2. Design RPC messages structures which gets communicated in
	   MPP and IOsplitter
	3. CVA can only handle 4k IOs. Split and communicate IOs from iosplitter
	   to MPP 
*/

struct thread_data {
	pthread_t  thread;
	Rendez     cond;
	int        fd;
	rpc_chan_t *rcp;
};

static struct thread_data	td[NO_THREADS];
static int			tcount = 0;

Rendez main_end;

int dev_handle = -1;

uint64_t        req_recv;
pthread_mutex_t lock;

struct vmk_Scsi {
	int     cmd_id;
	int     is_read;
	int     cdb[16];
	char    *buf;
	ssize_t len;
};

static ssize_t safe_pwrite(int fd, char *buf, size_t count, uint64_t off)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = pwrite(fd, b, count, off);
		if (rc < 0 || rc == 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("pwrite: ");
			break;
		}

		count -= rc;
		b     += rc;
		bc    += rc;
		off   += rc;
	}

	return bc;
}

static ssize_t safe_pread(int fd, char *buf, size_t count, uint64_t off)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = pread(fd, b, count, off);
		if (rc < 0 || rc == 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("pread: ");
			break;
		}

		count -= rc;
		b     += rc;
		bc    += rc;
		off   += rc;
	}

	return bc;
}

static inline int ssd_write(int dev_handle, rpc_msg_t *msgp)
{
	write_cmd_t *w     = (write_cmd_t *) &msgp->hdr;
	char        *buf   = msgp->payload;
	uint64_t    offset = w->offset;
	uint64_t    len    = w->len;

	if (len != safe_pwrite(dev_handle, buf, len, offset)) {
		return -1;
	}

	return 0;
}

static inline int ssd_read(int dev_handle, rpc_msg_t *msgp)
{
	read_cmd_t  *r     = (read_cmd_t *) &msgp->hdr;
	char        *buf   = msgp->payload;
	uint64_t    offset = r->offset;
	uint64_t    len    = r->len;

	if (len != safe_pread(dev_handle, buf, len, offset)) {
		return -1;
	}
	return 0;
}

static void rpc_conn_closed(rpc_chan_t *rcp)
{
	rpc_chan_close(rcp);
	rpc_chan_deinit(rcp);
	taskio_deinit();
}


void rpc_msg_handler(void *arg)
{
	uint64_t r;
	uint64_t len = 0;
	rpc_msg_t *msgp = arg;

	if (RPC_IS_CONNCLOSED(msgp)) {
		rpc_msg_put(msgp->rcp, msgp);
		rpc_conn_closed(msgp->rcp);
		//TODO:Anup Signal from here
		printf("%s: <== rpc connection closed\n", __func__);
		return;
	}

	switch (RPC_GETMSGTYPE(msgp)) {
	case RPC_OPEN_MSG:
		assert(msgp->payload == NULL && msgp->hdr.payloadlen == 0);
		printf("Open Recieved\n");
		break;
	case RPC_READ_MSG:
		assert(msgp->payload == NULL && msgp->hdr.payloadlen == 0);
		len = ((read_cmd_t*)&msgp->hdr)->len;
		rpc_databuf_get(msgp->rcp, &msgp->payload);
		msgp->hdr.payloadlen = len;

		msgp->hdr.status = ssd_read(dev_handle, msgp);
		assert(msgp->hdr.status == 0);
		break;
	case RPC_WRITE_MSG:
		len = ((write_cmd_t *)&msgp->hdr)->len;
		assert(msgp->payload != NULL && msgp->hdr.payloadlen == len);
		msgp->hdr.status = ssd_write(dev_handle, msgp);

		rpc_databuf_put(msgp->rcp, msgp->payload);
		msgp->payload = NULL;
		msgp->hdr.payloadlen = 0;
		break;
	default:
		assert(0);
	}

	r = req_recv++;
	if (r % 100 == 0) {
		printf("r = %lu\n", r);
	}
	rpc_response(msgp->rcp, msgp);

}

void server_resp_handler(rpc_msg_t *msgp)
{
	assert(0);
}

int open_ssd(char *dev)
{
	dev_handle = open(dev, O_RDWR);
	assert(dev_handle != -1);
	return 0;
}

void task_new_session(void *arg)
{
	struct thread_data	*t = arg;
	int			rc;
	session_t		client_session;

	taskname("%s", __func__);

	rc = read(t->fd, &client_session, sizeof(session_t));
	if (rc != sizeof(session_t) || client_session.type != SESSION_CLIENT) {
		printf("client session establishment failed");
		assert(0);
		//TODO: Do corrective measures rather than assert(0)
	}
	printf("session established");

	rc = taskio_init();
	assert(rc == 0);

	taskio_start();
	rc = tasknet_setnoblock(t->fd);
	assert(rc == 0);

	t->rcp = rpc_chan_new();
	assert(t->rcp != NULL);

	rc = task_sockfd_register(t->fd);
	assert(rc == 0);

	rc = rpc_chan_init(t->rcp, t->fd, t->fd, NTASK, MAXMSGSZ, PAYLOADSZ,
			NTASK * 2, rpc_msg_handler, NULL);
	assert(rc == 0);

	memset(&t->cond, 0, sizeof(t->cond));

	printf("sleeping\n");
	tasksleep(&t->cond);
}

void *trd_new_session(void *arg)
{
	libtask_start(task_new_session, arg);
	pthread_exit(NULL);
}

/* TODO:
   1. Think what data structure you need to maintain for 
      multiple open request. think about their necessity.
   2. Design commands and their structures required to be passed over 
      rpc channel of iosplitter-mpp
   3. CVA-IOsplitter communication
   	1. Initial handshake between mpp, iosplitter and cva
	2. IOs passing to CVA. CVA handles only 4k block size vs 
	   mpp-iosplitter works with multiple block sizes.
 */

void server_setup(void *arg)
{
	char           *ip = arg;
	int            rc;
	int            pubfd;
	int            infd;
	int            t;
	pthread_attr_t a;

	taskname("%s", __func__);

	memset(&td, 0, sizeof(td));

	rc = taskio_init();
	assert(rc == 0);

	taskio_start();

	rc = tasknet_announce(ip, SPORT, &pubfd);
	assert(rc == 0);
	assert(pubfd >= 0);

	while (1) {

		while(1) {
			if ((infd = accept(pubfd, NULL, NULL)) == -1) {
				if (errno != EAGAIN || errno != EWOULDBLOCK) {
					perror("accpet:");
					assert(0);
				}
			} else {
				assert(infd >= 0);
				break;
			}
		}

		t = tcount++;
		td[t].fd = infd;
		rc = pthread_attr_init(&a);
		assert(rc == 0);

		rc = pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		assert(rc == 0);

		rc = pthread_create(&td[t].thread, &a, trd_new_session, &td[t]);
		assert(rc == 0);

	}

	taskwakeup(&main_end);
}

static void usage(const char *s)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "%s [-d <SSD>]\n", s);
}

int main(int argc, char *argv[])
{
	char	*ssd;
	int	opt;
	int     rc;

	ssd = NULL;

	while ((opt = getopt(argc, argv, "d:h")) != -1) {
		switch (opt) {
			case 'd':
				ssd = strdup(optarg);
				assert(ssd != NULL);
				break;
			case 'h':
				usage(argv[0]);
				return (0);
		}
	}

	if (ssd == NULL) {
		usage(argv[0]);
		return (EINVAL);
	}

	rc = open_ssd(ssd);
	assert(rc == 0);

	libtask_start(server_setup, SIP);
	memset(&main_end, 0, sizeof(main_end));
	tasksleep(&main_end);
	return (0);
}
