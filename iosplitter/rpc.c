#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rpc.h"
#include "threadpool.h"

static inline int seqid_cmp(hash_entry_t *e, void *opaque)
{
	rpc_msg_t *m1 = container_of(e, rpc_msg_t, h_entry);
	rpc_msg_t *m2 = (rpc_msg_t *)opaque;

	return !(m1->hdr.seqid - m2->hdr.seqid);
}

static inline int get_bucket(hash_table_t *ht, uint32_t id)
{
	return id % hash_no_buckets(ht);
}

rpc_chan_t *rpc_chan_new(void)
{
	return calloc(1, sizeof(rpc_chan_t));
}

void rpc_chan_free(rpc_chan_t *rcp)
{
	free(rcp);
}

static inline void _rpc_response(rpc_chan_t *rcp, rpc_msg_t *resp)
{
	int          b;
	hash_entry_t *e;
	int          rc;
	rpc_msg_t    *msgp;

	b = get_bucket(&rcp->hash, RPC_MSG_SEQID(resp));
	e = NULL;

	RPC_CHAN_LOCK(rcp);
	rc = hash_lookup(&rcp->hash, b, &e, resp);
	if (rc < 0) {
		assert(0);
		return;
	}
	assert(e != NULL);
	hash_rem(&rcp->hash, e);
	RPC_CHAN_UNLOCK(rcp);

	msgp       = container_of(e, rpc_msg_t, h_entry);
	msgp->resp = resp;

	rcp->resp_handler(msgp);
}

static void _rpc_recv_handler(work_t *w, void *data)
{
	rpc_msg_t  *msgp;
	rpc_chan_t *rcp;

	msgp = (rpc_msg_t *) data;
	rcp  = msgp->rcp;

	if (RPC_ISREQ(msgp) || RPC_IS_CONNCLOSED(msgp)) {
		rcp->req_handler(msgp);
	} else {
		assert(!RPC_IS_CONNCLOSED(msgp));
		_rpc_response(rcp, msgp);
	}
}

static int _rpc_recv(rpc_chan_t *rcp, rpc_msg_t **msgpp)
{
	rpc_msg_t *rm;
	char      *p;
	bufpool_t *bp;
	char      *payload;
	ssize_t   rc;
	size_t    r;

	assert(rcp   != NULL);
	assert(msgpp != NULL);

	rm      = NULL;
	p       = NULL;
	bp      = NULL;
	payload = NULL;

	rc      = bufpool_get_reserve_zero(&rcp->msgpool, (char **) &rm, 0);
	assert(rc == 0);
	assert(rm != NULL);

	rm->rcp = rcp;

	/* read common header */
	rc = socket_read(rcp->socket, (char *) &rm->hdr, sizeof(rm->hdr));
	if (rc != sizeof(rm->hdr)) {
		goto error;
	}
	assert(rm->hdr.msglen <= bufpool_bufsize(&rcp->msgpool));

	/* read complete msg */
	r  = rm->hdr.msglen - sizeof(rm->hdr);
	p  = ((char *) &rm->hdr) + sizeof(rm->hdr);
	rc = socket_read(rcp->socket, p, r);
	if (rc != r) {
		goto error;
	}

	if (rm->hdr.payloadlen) {
		rpc_payload_get(rcp, rm->hdr.payloadlen, &payload);
		assert(payload != NULL);

		rc = socket_read(rcp->socket, payload, rm->hdr.payloadlen);
		if (rc != rm->hdr.payloadlen) {
			goto error;
		}
		rm->payload = payload;
	}

	*msgpp = rm;
	return 0;

error:
	if (payload) {
		bufpool_put(bp, payload);
	}

	/* mark rpc connection closed */
	RPC_SET_CONNCLOSED(rm);
	*msgpp = rm;
	return -1;
}

static void *rpc_recv_thread(void *args)
{
	rpc_chan_t *rcp = args;
	int        rc;
	rpc_msg_t  *msgp;
	work_t     *w;

	while (1) {
		rc = _rpc_recv(rcp, &msgp);
		if (rc < 0) {
			break;
		}

		w          = new_work(&rcp->tp);
		w->data    = msgp;
		w->work_fn = _rpc_recv_handler;
		rc         = schedule_work(&rcp->tp, w);
		assert(rc  == 0);
	}

	/* INSTRUCT HANDLER THAT CONNECTION IS CLOSED */
	RPC_SET_CONNCLOSED(msgp);
	w          = new_work(&rcp->tp);
	w->data    = msgp;
	w->work_fn = _rpc_recv_handler;
	rc         = schedule_work(&rcp->tp, w);
	assert(rc  == 0);
	return NULL;
}

static inline void _rpc_chan_deinit(rpc_chan_t *rcp)
{
	/* TODO: ensure socket is closed */

	if (rcp->recv_thread) {
		pthread_join(rcp->recv_thread, NULL);
	}
	thread_pool_deinit(&rcp->tp);
	bufpool_deinit(&rcp->ploadpool);
	bufpool_deinit(&rcp->msgpool);
	hash_deinit(&rcp->hash);
	pthread_mutex_destroy(&rcp->lock);
	memset(rcp, 0, sizeof(*rcp));
}

int rpc_chan_init(rpc_chan_t *rcp, sock_handle_t socket, size_t nway,
	int reserve, size_t msgsz, size_t hashsz, rpchandler_t req_handler,
	rpchandler_t resp_handler)
{
	int	 nmsg;
	int	 nmsg_max;
	int	 ndata;
	int	 ndata_max;
	int	 gap;
	int	 rc;

	assert(resp_handler != NULL);
	assert(req_handler  != NULL);

	memset(rcp, 0, sizeof(*rcp));

	nmsg      = nway + reserve;
	nmsg_max  = nway * 3;
	ndata     = 2 * nway + reserve;
	ndata_max = ndata * 3;
	gap       = ((char *) &(((rpc_msg_t *) 0)->hdr)) - ((char *) 0);

	rc = hash_init(&rcp->hash, hashsz, seqid_cmp);
	if (rc < 0) {
		return -1;
	}

	rc = bufpool_init_reserve(&rcp->msgpool, "msg-pool", msgsz + gap, nmsg,
			reserve, nmsg_max);
	if (rc < 0) {
		fprintf(stderr, "bufpool_init failed.\n");
		goto error;
	}

	rc = bufpool_init_reserve(&rcp->ploadpool, "pload-pool", PAYLOADMAX,
			ndata, reserve, ndata_max);
	if (rc < 0) {
		goto error;
	}

	rc = pthread_mutex_init(&rcp->lock, NULL);
	if (rc < 0) {
		goto error;
	}

	rcp->req_handler  = req_handler;
	rcp->resp_handler = resp_handler;
	rcp->socket       = socket;
	rcp->enabled      = 1;
	rcp->seqid        = 1;

	rc = thread_pool_init(&rcp->tp, nway);
	if (rc < 0) {
		goto error;
	}

	rc = pthread_create(&rcp->recv_thread, NULL, rpc_recv_thread, rcp);
	if (rc < 0) {
		goto error;
	}

	return 0;

error:
	_rpc_chan_deinit(rcp);
	return -1;
}

void rpc_chan_close(rpc_chan_t *rcp)
{
	int rc;

	if (!rcp->enabled) {
		return;
	}

	rcp->enabled = 0;
	rc = socket_close(rcp->socket);
	assert(rc == 0);
}

void rpc_chan_deinit(rpc_chan_t *rcp)
{
	_rpc_chan_deinit(rcp);
}

int rpc_async_request(rpc_chan_t *rcp, rpc_msg_t *msgp)
{
	int     b;
	ssize_t rc;

	assert(rcp  != NULL);
	assert(msgp != NULL);
	assert((msgp->payload == NULL && msgp->hdr.payloadlen == 0) ||
		(msgp->payload != NULL && msgp->hdr.payloadlen != 0));
	assert(msgp->resp == NULL);

	if (!rcp->enabled) {
		return -1;
	}

	b = get_bucket(&rcp->hash, msgp->hdr.seqid);

	RPC_SETREQ(msgp);

	RPC_CHAN_LOCK(rcp);
	hash_add(&rcp->hash, &msgp->h_entry, b);

	rc = socket_write(rcp->socket, (char *) &msgp->hdr, msgp->hdr.msglen);
	if (rc != msgp->hdr.msglen) {
		goto error;
	}

	if (msgp->payload) {
		rc = socket_write(rcp->socket, msgp->payload, msgp->hdr.payloadlen);
		if (rc != msgp->hdr.payloadlen) {
			goto error;
		}
	}
	RPC_CHAN_UNLOCK(rcp);

	/* do not wait for response */
	return 0;

error:
	rpc_chan_close(rcp);
	RPC_CHAN_UNLOCK(rcp);
	return -1;
}

int rpc_response(rpc_chan_t *rcp, rpc_msg_t *msgp)
{
	ssize_t rc;

	assert(rcp  != NULL);
	assert(msgp != NULL);
	assert((msgp->payload == NULL && msgp->hdr.payloadlen == 0) ||
		(msgp->payload != NULL && msgp->hdr.payloadlen != 0));
	assert(msgp->resp == NULL);

	if (!rcp->enabled) {
		return -1;
	}

	RPC_SETRESP(msgp);

	RPC_CHAN_LOCK(rcp);
	rc = socket_write(rcp->socket, (char *) &msgp->hdr, msgp->hdr.msglen);
	if (rc != msgp->hdr.msglen) {
		goto error;
	}

	if (msgp->payload) {
		rc = socket_write(rcp->socket, (char *) msgp->payload, msgp->hdr.payloadlen);
		if (rc != msgp->hdr.payloadlen) {
			goto error;
		}
	}
	RPC_CHAN_UNLOCK(rcp);

	rpc_msg_put(rcp, msgp);
	return 0;

error:
	rpc_chan_close(rcp);
	RPC_CHAN_UNLOCK(rcp);
	rpc_msg_put(rcp, msgp);
	return -1;
}

void rpc_msg_get(rpc_chan_t *rcp, int msgtype, size_t msglen, rpc_msg_t **msgpp)
{
	rpc_msg_t *rm;

	assert(rcp   != NULL);
	assert(msgpp != NULL);

	rm = NULL;
	bufpool_get_zero(&rcp->msgpool, (char **) &rm, 0);
	assert(rm != NULL);

	hash_entry_init(&rm->h_entry);
	rm->rcp = rcp;

	RPC_SETMSGTYPE(rm, msgtype);
	rm->hdr.seqid      = rcp->seqid++;
	rm->hdr.msglen     = msglen;
	rm->hdr.payloadlen = 0;

	*msgpp = rm;
}

void rpc_msg_put(rpc_chan_t *rcp, rpc_msg_t *msgp)
{
	assert(rcp  != NULL);
	assert(msgp != NULL);
	assert(msgp->resp == NULL || msgp->resp->resp == NULL);

	if (msgp->resp) {
		if (msgp->resp->payload) {
			rpc_payload_put(rcp, (char *) msgp->resp->payload);
		}

		bufpool_put(&rcp->msgpool, (char *) msgp->resp);
		msgp->resp = NULL;
	}

	if (msgp->payload) {
		rpc_payload_put(rcp, (char *) msgp->payload);
		msgp->payload = NULL;
	}

	bufpool_put(&rcp->msgpool, (char *) msgp);
}

void rpc_payload_get(rpc_chan_t *rcp, uint64_t len, char **bufp)
{
	assert(rcp  != NULL);
	assert(bufp != NULL);
	assert(bufpool_bufsize(&rcp->ploadpool) >= len);

	bufpool_get_zero(&rcp->ploadpool, bufp, 0);
}

void rpc_payload_put(rpc_chan_t *rcp, char *buf)
{
	assert(rcp != NULL);
	assert(buf != NULL);

	bufpool_put(&rcp->ploadpool, buf);
}

void rpc_payload_set(rpc_chan_t *rcp, rpc_msg_t *msgp, char *payload,
		uint64_t len)
{
	assert(rcp != NULL);
	assert(msgp != NULL);
	assert(msgp->rcp == rcp);
	assert((payload == NULL && len == 0) || (payload != NULL && len != 0));

	msgp->payload        = payload;
	msgp->hdr.payloadlen = len;
}
