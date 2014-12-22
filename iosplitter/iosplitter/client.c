#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include "rpc.h"
#include "bufpool.h"
#include "libtask/task.h"
#include "libtask/taskio.h"
#include "tst-rpc.h"

typedef struct props {
	char *serverip;
	int  port;
}props_t;

props_t p = {0};

rpc_chan_t      rcp;

Rendez tmain_cond;
Rendez iodone;
int no_tasks;

static inline int get_offset(void)
{
	return (random() % (1UL << 10));
}

int do_write(uint64_t len, uint64_t offset, int fill)
{
	int             rc = 0;
	char		*b;
	rpc_msg_t	*rm;
	write_cmd_t	*w;
	write_cmd_t	*res_w;
	rpc_msg_t	*res_rm;
	uint16_t	s;

	rpc_databuf_get(&rcp, &b);

	if (fill != 0) {
		memset(b, fill, PAYLOADSZ);
	}

	rpc_msg_get(&rcp, RPC_WRITE_MSG, sizeof(*w), len, b, &rm);
	w = (write_cmd_t *) &rm->hdr;

	w->offset		= offset;
	w->len                  = len;

	rc = rpc_request(&rcp, rm);
	assert(rc == 0);

	res_rm = rm->resp;
	res_w = (write_cmd_t *) &res_rm->hdr;

	s = res_w->hdr.status;

	if (s != 0) {
		rc = -1;
	}

	rpc_msg_put(&rcp, rm);
	return rc;
}

int do_read(uint64_t len, uint64_t offset)
{
	rpc_msg_t		*rm;
	read_cmd_t		*r;
	read_cmd_t		*res_r;
	int			rc = 0;

	rpc_msg_t		*res_rm;

	rpc_msg_get(&rcp, RPC_READ_MSG, sizeof(*r), 0, NULL, &rm);
	r = (read_cmd_t *) &rm->hdr;

	r->offset	= offset;
	r->len          = len;

	rc = rpc_request(&rcp, rm);
	if (rc != 0) {
		fprintf(stderr, "rpc_request failed. rerun.\n");
		rc = -1;
		goto error;
	}

	res_rm = rm->resp;
	res_r  = (read_cmd_t *) &res_rm->hdr;
	assert(res_r && res_r->hdr.payloadlen == len);

	if (res_r->hdr.status != 0) {
		printf("read from server failed.\n");
		rc = -1;
		goto error;
	}
error:
	rpc_msg_put(&rcp, rm);
	return (rc);
}

static void task_done(void)
{
	no_tasks--;
	// printf("no_tasks = %d\n", no_tasks);
	if (no_tasks == 0) {
		taskwakeup(&iodone);
	}
}

void task_do_io(void *arg)
{
	unsigned long	i;
	unsigned long	MAX = 10000;
	int		rc;
	uint64_t	offset;
	uint64_t        len;


	for (i = 0; i < MAX; i++) {
		len = 8192;
#ifdef RANDOM
		offset = get_offset() << 12;
#else
		offset = i << 12;
#endif

		rc = do_write(len, offset, 0);
		assert(rc == 0);
		// printf("write%lu done\n", i);

#if defined ALWAYS_NEW_OFFSET && defined RANDOM
		offset = get_offset() << 12;
#endif
		rc = do_read(len, offset);
		assert(rc == 0);
	}

	task_done();
}


int do_io(void)
{
	int	 tasks;
	int	 j;

	printf("Running IO test: ");
	fflush(stdout);
	memset(&iodone, 0, sizeof(iodone));
	tasks    = 10;
	no_tasks = tasks;

	for (j = 0; j < tasks; j++) {
		taskcreate(task_do_io, NULL, 32 * 1024);
	}

	tasksleep(&iodone);

	printf(" PASS\n");
	fflush(stdout);

	return 0;
}

void default_handler(void *arg)
{
	rpc_msg_t *rm = arg;

	if (RPC_IS_CONNCLOSED(rm)) {
		rpc_chan_close(rm->rcp);
		task_fd_deregister(rm->rcp->infd);
		rpc_chan_deinit(rm->rcp);
		return;
	} else {
		rpc_default_handler(arg);
	}
}

int send_open(void)
{

	rpc_msg_t  *rm;
	rpc_msg_t  *res_rm;
	open_cmd_t *w;
	open_cmd_t *res_w;

	int rc = 0;
	int s;

	rpc_msg_get(&rcp, RPC_OPEN_MSG, sizeof(*w), 0, NULL, &rm);

	rc = rpc_request(&rcp, rm);
	assert(rc == 0);

	res_rm = rm->resp;
	res_w = (open_cmd_t *) &res_rm->hdr;

	s = res_w->hdr.status;

	if (s != 0) {
		rc = -1;
	}
	rpc_msg_put(&rcp, rm);

	return rc;
}

int make_session(int fd)
{
	int rc = -1;

	session_t client_session;
	client_session.type = SESSION_CLIENT;
	
	rc = write(fd, &client_session, sizeof(session_t));
	if (rc != sizeof(session_t)) {
		printf("make session failed exiting");
		exit(1);
	}
	return 0;
}

void tmain(void *arg)
{
	int rc;
	int infd;

	rc = taskio_init();
	assert(rc == 0);

	taskio_start();

	rc = tasknet_connect(p.serverip, p.port, &infd);
	assert(rc == 0);

	rc = make_session(infd);
	assert(rc == 0);
	printf("session established\n");

	rc = rpc_chan_init(&rcp, infd, infd, NTASK, MAXMSGSZ, PAYLOADSZ,
			NTASK * 2, default_handler, NULL);
	assert(rc == 0);


	rc = do_io();
	assert(rc == 0);

	taskyield();
	taskio_deinit();
	taskcachefree();
	taskwakeup(&tmain_cond);
}

int main(int argc, char **argv)
{
	memset(&tmain_cond, 0, sizeof(tmain_cond));
	assert(argv[1] != 0);
	p.serverip = strdup(argv[1]);
	p.port      = SPORT;

	libtask_start(tmain, NULL);
	tasksleep(&tmain_cond);
	return 0;
}
