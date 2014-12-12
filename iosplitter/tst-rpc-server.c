#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "rpc.h"
#include "bufpool.h"
#include "network.h"
#include "tst-rpc.h"

int dev_handle = -1;
rpc_chan_t *rcp;
sock_handle_t sock;
sock_handle_t client_sock;

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

void server_req_handler(rpc_msg_t *msgp)
{
	uint64_t r;
	uint64_t len = 0;

	if (RPC_IS_CONNCLOSED(msgp)) {
		rpc_msg_put(msgp->rcp, msgp);
		rpc_chan_close(rcp);
		//TODO:Anup Signal from here
		printf("%s: <== rpc connection closed\n", __func__);
		return;
	}

	switch (RPC_GETMSGTYPE(msgp)) {
	case RPC_READ_MSG:
		assert(msgp->payload == NULL && msgp->hdr.payloadlen == 0);
		len = ((read_cmd_t*)&msgp->hdr)->len;
		rpc_payload_get(msgp->rcp, len, &msgp->payload);
		msgp->hdr.payloadlen = len;

		msgp->hdr.status = ssd_read(dev_handle, msgp);
		assert(msgp->hdr.status == 0);
		break;
	case RPC_WRITE_MSG:
		len = ((read_cmd_t*)&msgp->hdr)->len;
		assert(msgp->payload != NULL && msgp->hdr.payloadlen == len);
		msgp->hdr.status = ssd_write(dev_handle, msgp);

		rpc_payload_put(msgp->rcp, msgp->payload);
		msgp->payload = NULL;
		msgp->hdr.payloadlen = 0;
		break;
	default:
		assert(0);
	}

	pthread_mutex_lock(&lock);
	r = req_recv++;
	pthread_mutex_unlock(&lock);
	rpc_response(msgp->rcp, msgp);

	if (r % 100 == 0) {
		printf("r = %lu\n", r);
	}
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

int setup_server(int argc, char **argv)
{
	int rc;

	assert(argc <= 2);

	assert(argv[1] != NULL);

	rc = open_ssd(argv[1]);
	assert(rc == 0);

	rc = pthread_mutex_init(&lock, NULL);
	assert(rc == 0);

	rcp  = rpc_chan_new();
	assert(rcp != NULL);

	rc = server_setup(SIP, strlen(SIP), SPORT, &sock);
	assert(rc == 0);

	client_sock = socket_accept(sock, NULL, NULL);
	printf("connectioned accepted.\n");

	rc = rpc_chan_init(rcp, client_sock, IODEPTH, 4, sizeof(rpc_maxmsg_t),
			IODEPTH * 2, server_req_handler, server_resp_handler);
	printf("rpc_chan_init done.\n");

	sleep(100000);
	//TODO:Anup wait for an event of rpc_channel close
	printf("RPC channel must have been closed by now.\n");
	printf("deiniting rpc channel\n");
	rpc_chan_deinit(rcp);
	printf("server ended.\n");
	return 0;
}

int main(int argc, char **argv)
{
	setup_server(argc, argv);
	return 0;
}
