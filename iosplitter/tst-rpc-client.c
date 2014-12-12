#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include "tst-rpc.h"
#include "rpc.h"
#include "network.h"

typedef struct vmk_Scsi {
	uint64_t  cmd_id;
	int       is_read;
	int       cdb[16];
	char      *buf;
	uint16_t  len;
} vmk_Scsi_t;

rpc_chan_t      *rcp;
sock_handle_t   sock;
pthread_t       thread;
pthread_mutex_t lock;
pthread_cond_t  cond;
uint64_t        cmd_id;
uint64_t        responded;

void mpp_req_handler(rpc_msg_t *msgp)
{
	if (RPC_IS_CONNCLOSED(msgp)) {
		rpc_msg_put(msgp->rcp, msgp);
		rpc_chan_close(rcp);
		printf("rpc connection closed.\n");
		return;
	}

	assert(0);
}

void mpp_resp_handler(rpc_msg_t *msgp)
{
	rpc_chan_t *rcp;
	vmk_Scsi_t *s;
	uint64_t   r;

	printf("%s ==>\n", __func__);
	rcp = msgp->rcp;

	switch (RPC_GETMSGTYPE(msgp)) {
	case RPC_READ_MSG:
		s = msgp->opaque;
		assert(s->len == msgp->hdr.payloadlen);
		free(s);
		rpc_msg_put(rcp, msgp);
		break;
	case RPC_WRITE_MSG:
		s = msgp->opaque;
		assert(s->len == msgp->hdr.payloadlen);
		free(s);
		rpc_msg_put(rcp, msgp);
		break;
	default:
		assert(0);
	}

	pthread_mutex_lock(&lock);
	r = responded++;
	pthread_mutex_unlock(&lock);
	printf("%s <== %lu\n", __func__, r);
}

static inline vmk_Scsi_t *new_scsi_command(void)
{
	vmk_Scsi_t *s;

	s = calloc(1, sizeof(*s));
	assert(s != NULL);
	s->cmd_id  = cmd_id++;
	s->is_read = 1;
	while (!s->len) {
		s->len = random() % PAYLOADMAX;
	}

	return s;
}

void *send_msgs(void *args)
{
	uint64_t   i;
	vmk_Scsi_t *s;
	rpc_msg_t  *m;
	int        rc;
	char       *p;

	for (i = 0; i < 10000; i++) {
		printf("sending msg: %lu\n", i);
		s = new_scsi_command();

		m = NULL;
		rpc_msg_get(rcp, RPC_READ_MSG, sizeof(*m), &m);
		assert(m != NULL);
		m->opaque = s;

		rpc_payload_get(rcp, s->len, &p);
		rpc_payload_set(rcp, m, p, s->len);

		rc = rpc_async_request(rcp, m);
		assert(rc == 0);
	}

	sleep(5);

	rpc_chan_close(rcp);
	return NULL;
}

int main(void)
{
	int rc;

	rcp = rpc_chan_new();
	assert(rcp != NULL);

	rc = client_setup(SIP, strlen(SIP), SPORT, CIP, strlen(CIP), CPORT, &sock);
	assert(rc == 0);

	rc = rpc_chan_init(rcp, sock, IODEPTH, 4, sizeof(rpc_maxmsg_t),
			IODEPTH * 2, mpp_req_handler, mpp_resp_handler);
	assert(rc == 0);

	rc = pthread_cond_init(&cond, NULL);
	assert(rc == 0);

	pthread_mutex_init(&lock, NULL);
	assert(rc == 0);

	rc = pthread_create(&thread, NULL, send_msgs, rcp);
	assert(rc == 0);

	pthread_join(thread, NULL);
	rpc_chan_deinit(rcp);
	return 0;
}
