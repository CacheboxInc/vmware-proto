#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include "rpc.h"
#include "bufpool.h"
#include "network.h"
#include "tst-rpc.h"

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

void server_req_handler(rpc_msg_t *msgp)
{
	uint64_t r;

	printf("%s ==>\n", __func__);
	if (RPC_IS_CONNCLOSED(msgp)) {
		rpc_msg_put(msgp->rcp, msgp);
		rpc_chan_close(rcp);
		printf("%s: <== rpc connection closed\n", __func__);
		return;
	}

	switch (RPC_GETMSGTYPE(msgp)) {
	case RPC_READ_MSG:
		rpc_response(msgp->rcp, msgp);
		break;
	case RPC_WRITE_MSG:
		rpc_response(msgp->rcp, msgp);
		break;
	default:
		assert(0);
	}

	pthread_mutex_lock(&lock);
	r = req_recv++;
	pthread_mutex_unlock(&lock);
	printf("%s <== %lu\n", __func__, r);
}

void server_resp_handler(rpc_msg_t *msgp)
{
	assert(0);
}

int setup_server(void)
{
	int rc;

	rc = pthread_mutex_init(&lock, NULL);
	assert(rc == 0);

	rcp  = rpc_chan_new();
	assert(rcp != NULL);

	rc = server_setup(IP, strlen(IP), SPORT, &sock);
	assert(rc == 0);

	client_sock = socket_accept(sock, NULL, NULL);

	rc = rpc_chan_init(rcp, client_sock, IODEPTH, 4, sizeof(rpc_maxmsg_t),
			IODEPTH * 2, server_req_handler, server_resp_handler);

	printf("server rpc channel created. sleeping....\n");
	sleep(10000);

	printf("RPC channel must have been closed by now.\n");
	printf("deiniting rpc channel\n");
	rpc_chan_deinit(rcp);
	printf("server ended.\n");
	return 0;
}

int main(void)
{
	setup_server();

	return 0;
}
