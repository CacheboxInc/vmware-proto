/*
 * rpc.c
 *	remote procedure calls using libtask environment
 */

#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <assert.h>
#include <libaio.h>
#include <string.h>
#include <fcntl.h>

#include "rpc.h"
#include "../libtask/taskio.h"

#define TRACE(...) {}
#define TRACE2(...) 	{                                \
  fprintf(stderr, "TRACE: %s(%d):", __FILE__, __LINE__); \
  fprintf(stderr, __VA_ARGS__);                          \
  fflush(stdout);                                        \
}

#define PRINT(...) { \
	fprintf(stderr, __VA_ARGS__); fflush(stderr); \
}

#define NOPRINT(...) { }

#define TASKSTACKSZ (32*1024)
#define STATIC

unsigned long long g_rsp_sent;
unsigned long long g_rpc_recv_task_reads_done;
unsigned long long g_rpc_recv_task_reads_issued;

STATIC void rpc_recv_task(void *arg);
STATIC void _rpc_response(rpc_chan_t *rcp, rpc_msg_t *resp);
STATIC void _rpc_receive(rpc_chan_t *rcp, rpc_msg_t **msgpp);

static inline int get_bucket(hash_table_t *ht, seqid_t id)
{
	return id % hash_no_buckets(ht);
}

/*
 * Just read a message from channel into new buffers and return msgp
 * return 0 on success. set CONNCLOSED bit on error.
 */
STATIC void
_rpc_receive(rpc_chan_t *rcp, rpc_msg_t **msgpp)
{
	rpc_msg_t	*msgp = NULL;
	char		*buf = NULL;
	char		*ptr;			// temp ptr
	int			nbytes, res;

	assert(rcp && msgpp);
	bufpool_get_zero(&rcp->msgpool, (char **)&msgp, 1);
	assert(msgp != NULL);

	msgp->rcp = rcp;
	if (!rcp->enabled) {
		goto errout;
	}
	/* Read bare header because we don't know the msglen yet*/
	TRACE("read header A \n")

	if ((res = task_netread(rcp->infd, (char*)&msgp->hdr, sizeof(msgp->hdr))) != 0) {
		//PRINT("_rpc_receive read1: failed with %d\n", res)
		goto errout;
	}
	if (msgp->hdr.msglen > bufpool_bufsize(&rcp->msgpool)) {
		//PRINT("_rpc_receive: bad msglen %u\n", msgp->hdr.msglen);
		goto errout;
	}
	/* XXX validate msg hdr ?? */
	nbytes = msgp->hdr.msglen - sizeof(msgp->hdr);
	assert(nbytes >= 0);
	/* Now read in remaining portion of message header*/
	if(nbytes > 0) {
		ptr = (char *)&msgp->hdr + sizeof(msgp->hdr);
		if ((res = task_netread(rcp->infd, ptr, nbytes))!= 0) {
			//PRINT("_rpc_receive read2: failed with %d\n", res)
			goto errout;
		}
	}
	/* now the payload, if any */
	if ((nbytes = msgp->hdr.payloadlen) > 0) {
		if (nbytes > bufpool_bufsize(&rcp->datapool)) {
			PRINT("_rpc_receive read3: cannot handle payload %d, max %"PRIu64"\n", nbytes,
					(uint64_t)bufpool_bufsize(&rcp->datapool))
			goto errout;
		}
		rpc_databuf_get(rcp, &buf);
		//TRACE("_rpc_receive GOT buf=%p\n", buf)
		if ((res = task_netread(rcp->infd, buf, nbytes)) !=0 ) {
			//PRINT("_rpc_receive: fd read failed with %d\n", res)
			goto errout;
		}
		msgp->payload = buf;
	}
	//printf("_rpc_receive: message received\n");
	//dump_rpc_msg(msgp);
	*msgpp = msgp;
	return;

errout:
	assert(msgp);
	if (buf) {
		//TRACE("_rpc_receive PUT buf=%p\n", buf)
		bufpool_put(&rcp->datapool, buf);
	}
	RPC_SET_CONNCLOSED(msgp); //printf("connection closed\n");
	*msgpp = msgp;
}

/*
 * handle a response message
 */
STATIC void
_rpc_response(rpc_chan_t *rcp, rpc_msg_t *resp)
{
	int		bucket;
	rpc_msg_t	*msgp;
	int		rc = 0;
	hash_entry_t	*e = NULL;

	assert(rcp && resp);

	bucket = get_bucket(&rcp->hash, RPC_MSG_SEQID(resp));
	assert(bucket >= 0 && bucket < rcp->hash.no_buckets);

	rc = hash_lookup(&rcp->hash, bucket, &e, resp);
	if (rc != 0) {
		/* Orphan response. Flag an error */
		PRINT("_rpc_response: matching msgp not found in hash table\n");
		assert(0);
		return;
	}

	assert(e != NULL);
	msgp = container_of(e, rpc_msg_t, h_entry);
	assert(msgp != NULL);

	hash_rem(&rcp->hash, e);

	msgp->resp = resp;
	TASKWAKEUP(&msgp->rendez);
	//PRINT("response seqid=%d signalled\n", RPC_MSG_SEQID(resp));
}

/*
 * channel message receiver task
 * runs on both client and server
 */
STATIC void
rpc_recv_task(void *arg)
{
	rpc_chan_t	*rcp = arg;
	rpc_msg_t	*msgp = NULL;

	assert(rcp && rcp->enabled);
	assert(rcp->handler);

	TASKSYSTEM();
	TASKNAME("rpc_recv_task");

	while(1) {
		g_rpc_recv_task_reads_issued++;
		_rpc_receive(rcp, &msgp);
		assert(msgp);
		if (RPC_IS_CONNCLOSED(msgp)) {
			/* fatal error */
			break;
		}
		g_rpc_recv_task_reads_done++;

		if (RPC_ISREQ(msgp)) {
			TASKCREATE(rcp->handler, msgp, TASKSTACKSZ);
		} else {
			_rpc_response(rcp, msgp);
		}
	}

	//PRINT("rpc_recv_task: fatal error on read: channel disabled\n");
	/* leave the sockfd open until everything is shut down */
	rcp->enabled = 0;
	/* Send handler this message to tell it that channel is closed */
	TASKCREATE(rcp->handler, msgp, TASKSTACKSZ);
}

/*
 *  When user did not set up a handler,
 *  and a request comes over the wire,
 *	and the msglen did not exceed our msg buffer size,
 *  then the default handler will immediately send an error response.
 *  NOTE: this is in a task context.
 */
void
rpc_default_handler(void *arg)
{
	rpc_msg_t	*msgp = arg;
	rpc_chan_t	*rcp;

	if (msgp == NULL) {
		return;
	}
	assert(msgp->rcp);
	rcp = msgp->rcp;
	if (msgp->payload) {
		rpc_databuf_put(rcp, msgp->payload);
		msgp->payload = NULL;
	}
	msgp->hdr.status = RPC_ENOSYS;
	if (RPC_IS_CONNCLOSED(msgp)) {
		rpc_msg_put(rcp, msgp);
		return;
	}
	rpc_response(rcp, msgp);
}


/* ################ PUBLIC RPC API ########################### */
rpc_chan_t *
rpc_chan_new(void)
{
	return calloc(1, sizeof(rpc_chan_t));
}

void
rpc_chan_free(rpc_chan_t *rcp)
{
	free(rcp);
}


static inline int seqid_cmp(hash_entry_t *e, void *opaque)
{
	rpc_msg_t *m1 = container_of(e, rpc_msg_t, h_entry);
	rpc_msg_t *m2 = (rpc_msg_t *)opaque;

	return !(m1->hdr.seqid - m2->hdr.seqid);
}

/*
 * Initialize channel
 * 		handler can be NULL, then rpc will use a default handler.
 * Return 0 on success,
 * 		may fail due to insufficient memory
 * NOTE: call rpc_chan_deinit before reinitializing a used rcp.
 * 	create nway+1 message buffers
 *  create 2*nway+1 data buffers.
 *	create 2* nway hashtable size
 */
int
rpc_chan_init(
	rpc_chan_t *rcp,
	int infd,
	int outfd,
	size_t nway,
	size_t msgsz,
	size_t datasz,
	size_t hashsz,
	rpchandler_t handler,
	void *usrcntxt)
{
	size_t	msgnbufs, msgnmax;
	size_t	datanbufs, datanmax;
	size_t	gap;
	int	res = 0;

	assert(rcp);
	assert(msgsz >= sizeof(rpc_msghdr_t) && msgsz <= MAXUINT16);
	//assert(datasz < MAXUINT16);

	BZERO(rcp); // zero out all members in one stroke, including QLock
	if (handler == NULL) {
		printf("handler is NULL. using rpc_default_handler\n");
	}
	rcp->handler = (handler == NULL) ? rpc_default_handler : handler;
	rcp->usrcntxt = usrcntxt;

	//  XXX re-examine logic of setting container sizes
	msgnbufs = nway + 1;
	msgnmax = msgnbufs * 3;
	datanbufs = 2 * nway + 2;
	datanmax = datanbufs * 3;

	/*
	 * user has specified maximum over-the-wire message header size.
	 * We need to allocate maximum in-memory rpc_msg_t size.
	 * char *hdrp = (char *)&((rpc_msg_t *)0)->hdr; // for mesgp starting at 0
	 * So gap = hdrp - (char *)0;
	 */
	gap = ((char *)&((rpc_msg_t *)0)->hdr - (char *)0);

#if 1
	printf("rpc_msg_init:\n"
	       "\tsize of gap:          %"PRIu64"\n", (uint64_t) gap);
	printf("\tsize of pointer:      %"PRIu64"\n", (uint64_t) sizeof(void *));
	printf("\tsize of rpc_msghdr_t: %"PRIu64"\n", (uint64_t) sizeof(rpc_msghdr_t));
	printf("\tsize of rpc_msg_t:    %"PRIu64"\n", (uint64_t) sizeof(rpc_msg_t));
	printf("\tsize of Rendez:       %"PRIu64"\n", (uint64_t) sizeof(Rendez));
#endif

	//assert(sizeof(hashbucket_t) == 2 * sizeof(void *) + 4);
	//assert(gap == sizeof(hashbucket_t) + sizeof(Rendez) + 3 * sizeof(void *));

	if (bufpool_init(&rcp->msgpool, msgsz + gap, msgnbufs, msgnmax) != msgnbufs) {
		res = RPC_ENOMEM;
		goto errout;
	}
	if (bufpool_init(&rcp->datapool, datasz, datanbufs, datanmax) != datanbufs) {
		res = RPC_ENOMEM;
		goto errout;
	}
	rcp->infd  = infd;
	rcp->outfd = outfd;
	rcp->enabled = 1;
	rcp->seqid = 1;   // XXX choose a random starting value?
	if ((res = hash_init(&rcp->hash, hashsz, seqid_cmp)) < 0) {
		goto errout;
	}
	/* start a request handler system task */
	TASKCREATE(rpc_recv_task, rcp, TASKSTACKSZ);
	return 0;

errout:
	rpc_chan_deinit(rcp);
	return res;
}

void rpc_chan_close(rpc_chan_t *rcp)
{
	rcp->enabled = 0;
	if (fcntl(rcp->infd, F_GETFD) == 0) {
		close(rcp->infd);
	}

	if (fcntl(rcp->outfd, F_GETFD) == 0) {
		close(rcp->outfd);
	}

	task_fd_deregister(rcp->infd);
	task_fd_deregister(rcp->outfd);

	rcp->infd  = -1;
	rcp->outfd = -1;
}

void
rpc_chan_deinit(rpc_chan_t *rcp)
{
	bufpool_deinit(&rcp->msgpool);
	bufpool_deinit(&rcp->datapool);
	hash_deinit(&rcp->hash);
	BZERO(rcp);  //  rcp->enabled = 0; too.
}

void
rpc_databuf_get(rpc_chan_t *rcp, char **bufp)
{
	assert(rcp && bufp);
	bufpool_get_zero(&rcp->datapool, bufp, 0);
}

void
rpc_databuf_put(rpc_chan_t *rcp, char *buf)
{
	assert(rcp && buf);
	bufpool_put(&rcp->datapool, buf);
}


/*
 * Sleep until a message buffer is obtained from rcp pool
 * Caller has already obtained a payload buffer (optional)using rpc_databuf_get()
 * Initialize msgp with parameters.
 * Note that msglen is the size of the message header that will be sent over the wire.
 * NOTE: We trust bufpool_get to return a zeroed buffer.
 */
void
rpc_msg_get(
	rpc_chan_t *rcp,
	int msgtype,
	size_t msglen,
	size_t payloadlen,
	char *payload,		// may be NULL
	rpc_msg_t **msgpp)
{
	rpc_msg_t	*msgp;

	assert(rcp && msgpp && msglen >= sizeof(rpc_msghdr_t));
	assert(payload || payloadlen == 0);

	bufpool_get_zero(&rcp->msgpool, (char **)&msgp, 0);
	assert(msgp);

	hash_entry_init(&msgp->h_entry);
	msgp->resp = NULL;
	msgp->rcp  = rcp;
	memset(&msgp->rendez, 0, sizeof(msgp->rendez));

	msgp->hdr.seqid = rcp->seqid++; // hbkt.seqid is set in rpc_request
	RPC_SETMSGTYPE(msgp, msgtype)
	msgp->hdr.msglen = msglen;
	msgp->hdr.payloadlen = payloadlen;
	msgp->payload = payload;
	*msgpp = msgp;
}

/*
 * release msgp and payload as well.
 * goes one deep:  takes care of msgp->resp too
 * XXX handle case rpc is deinited?
 */
void
rpc_msg_put(rpc_chan_t *rcp, rpc_msg_t *msgp)
{
	assert(rcp && msgp);
	if (msgp->resp) {
		assert(msgp->resp->resp == NULL);
		if (msgp->resp->payload) {
			rpc_databuf_put(rcp, msgp->resp->payload);
		}
		bufpool_put(&rcp->msgpool, (char *)(msgp->resp));
		msgp->resp = NULL; // defensive programming
	}
	if (msgp->payload) {
		rpc_databuf_put(rcp, msgp->payload);
		msgp->payload = NULL; // defensive programming
	}
	bufpool_put(&rcp->msgpool, (char *)(msgp));
}

/*
 * complete remaining set up of msgp
 * and then send request over the channel
 * return 0 on success. non zero is fatal error
 */
int
rpc_request(rpc_chan_t *rcp, rpc_msg_t *msgp)
{
	int		bucket;
	int		res;

	assert(rcp && msgp && msgp->hdr.msglen >= sizeof(rpc_msghdr_t));
	assert(msgp->payload || msgp->hdr.payloadlen == 0);
	assert(msgp->resp == NULL);

	if (!rcp->enabled) {
		return RPC_EDISABLED;
	}

	bucket = get_bucket(&rcp->hash, msgp->hdr.seqid);
	assert(bucket >= 0 && bucket < rcp->hash.no_buckets);

	RPC_SETREQ(msgp)
	hash_add(&rcp->hash, &msgp->h_entry, bucket);
	RPC_CHAN_LOCK(rcp)
	/* send the message header */
	if ((res = task_netwrite(rcp->outfd, (char*)&msgp->hdr, msgp->hdr.msglen)) != 0) {
		PRINT("rpc_request: header send failed with %d\n", res);
		goto errout;
	}
	if (msgp->payload) {
		if ((res = task_netwrite(rcp->outfd, msgp->payload, msgp->hdr.payloadlen)) != 0) {
			PRINT("rpc_request: payload send failed with %d\n", res);
			goto errout;
		}
	}
	RPC_CHAN_UNLOCK(rcp)
	/* now wait for response that will appear in msgp->resp */
	TASKSLEEP(&msgp->rendez);
	assert(msgp->resp);
	return 0;

errout:
	/* task_netwrite errors are irrecoverable. close the socket to trigger clean up.*/
	PRINT("rpc_request: error %d, closing fd\n", res);
	rpc_chan_close(rcp);
	RPC_CHAN_UNLOCK(rcp)
	return res;
}

/*
 *  To be called from a request handler task
 *  Send the response, deep free msgp,
 */
void
rpc_response(rpc_chan_t *rcp, rpc_msg_t *msgp)
{
	int		res;

	assert(rcp && msgp && msgp->hdr.msglen >= sizeof(rpc_msghdr_t));
	//dump_rpc_msg(msgp);
	assert(msgp->payload || msgp->hdr.payloadlen == 0);
	assert(msgp->resp == NULL);

	if (!rcp->enabled) {
		goto done;
	}
	RPC_SETRESP(msgp)
	RPC_CHAN_LOCK(rcp)
	/* send the message header */
	if ((res = task_netwrite(rcp->outfd, (char*)&msgp->hdr, msgp->hdr.msglen)) != 0) {
		//PRINT("rpc_response: header send failed with %d\n", res);
		goto errout;
	}
	if (msgp->payload) {
		if ((res = task_netwrite(rcp->outfd, msgp->payload, msgp->hdr.payloadlen)) != 0) {
			//PRINT("rpc_response: payload send failed with %d\n", res);
			goto errout;
		}
	}
	goto done;

errout:
	rpc_chan_close(rcp);
	//PRINT("rpc_response send failed: channel disabled and fd closed\n");
	// XXX Is this sufficient, or does somebody have to kill the receiver task?

done:
	RPC_CHAN_UNLOCK(rcp)
	/* now reclaim payload and msgp */
	rpc_msg_put(rcp, msgp);
	g_rsp_sent++;
}

// XXX wrap debug code around an #ifdef
void
dump_rpc_msghdr(rpc_msghdr_t *p)
{
	printf("\trpc_msghdr: %p\n", p);
	printf("\t\tseqid        %d\n", p->seqid);
	printf("\t\ttype         %d\n", p->type);
	printf("\t\tmsglen       %d\n", p->msglen);
	printf("\t\tpayloadlen   %d\n", p->payloadlen);
	printf("\t\tstatus       %d\n", p->status);
}

void
dump_rpc_msg(rpc_msg_t *p)
{
	printf("rpc_msg_t:  %p\n", p);
	printf("\tpayload      %p\n", p->payload);
	printf("\tresp         %p\n", p->resp);
	printf("\trcp          %p\n", p->rcp);
	dump_rpc_msghdr(&p->hdr);
}

/* ################### UNIT TEST ######################## */
#ifdef SOLOTEST_RPC
void
taskmain(int argc, char *argv[])
{
	rpc_chan_t	rc;
	rpc_msg_t	*msgp;
	char		*buf;
	size_t		msglen = 32 + sizeof(rpc_msghdr_t);

	rpc_chan_init(&rc, 1, 16, 38, 4096, 128, rpc_recv_task);
	bufpool_dump(&rc.msgpool, "msgpool");
	bufpool_dump(&rc.datapool, "datapool");

	rpc_databuf_get(&rc, &buf);
	assert(buf);
	rpc_msg_get(&rc, 333, msglen, 4096,
				buf, &msgp);
	assert(msgp && msgp->payload == buf);
	assert(msgp->hdr.msglen == msglen);
	assert(msgp->hdr.payloadlen == 4096);
	assert(RPC_GETMSGTYPE(msgp) == 333);
	assert(msgp->resp == NULL);
	bufpool_dump(&rc.msgpool, "msgpool");
	bufpool_dump(&rc.datapool, "datapool");

	rpc_msg_put(&rc, msgp);
	bufpool_dump(&rc.msgpool, "msgpool");
	bufpool_dump(&rc.datapool, "datapool");

	printf("clean up ...\n");
	rpc_chan_deinit(&rc);
	exit(0);
}

#endif /*SOLOTEST_RPC*/
