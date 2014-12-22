/*
 * rpc.h
 *	remote procedure call library for" libtask environment
 *
 */
#ifndef RPC_H
#define RPC_H

#include "bufpool.h"
#include "hash.h"

#define RPC_PORT  12333
#define NTASK     128

/*
 * access flag in rpc_msg_hdr.type
 * Request : LSB is 1. Response: LSB is 0
 */
#define RPC_ISREQ(msgp)		(((msgp)->hdr.type & 1) ? 1 : 0)
#define RPC_SETREQ(msgp)	{ (msgp)->hdr.type |= 1; }
#define RPC_SETRESP(msgp)	{ (msgp)->hdr.type &= ~1; }
/*
 * 2nd bit of rpc_msg_hdr.type indicates CONNECTION CLOSED
 */
#define RPC_IS_CONNCLOSED(msgp)	(((msgp)->hdr.type & 2) ? 1 : 0)
#define RPC_SET_CONNCLOSED(msgp) { (msgp)->hdr.type |= 2; }

#define RPC_TYPE_RESERVED_BITS 4

#define RPC_EBASE		2000
#define RPC_EDISABLED	(1 + RPC_EBASE)
#define RPC_ENOMEM		(2 + RPC_EBASE)
#define RPC_ENOSYS		(3 + RPC_EBASE)
//#define RPC_ENOMEM		(4 + RPC_EBASE)
//#define RPC_ENOMEM		(5 + RPC_EBASE)

/*
 *  handler task is passed a rpc_msg_t * as the arg.
 */
typedef void (*rpchandler_t)(void *);

/*
 * clients will build their own msg header structures where the first member
 * is struct rpc_msg_hdr.
 * msglen is the size in bytes of the client's message structure.
 * Client structure will be followed by a payload of length = payloadlen.
 * client can use message types 0 to 2^12-1. This is stored left shifted by 4 bits.
 * First 4 bits of type are reserved for rpc internal use.
 */
typedef struct rpc_msghdr {
	seqid_t		seqid;		/* 32 bit sequence id */
	uint16_t	type;		/* 12 MSB bit message type & 4 LSB bits of flags*/
	uint16_t	msglen;		/* 16 bit, number of bytes of full msg hdr */
	uint16_t	payloadlen;	/* 16 bit, number of bytes; n < 64K */
	uint16_t	status;		/* 16 bit status code in response */
} rpc_msghdr_t;

#define RPC_MSG_SEQID(msgp) (msgp->hdr.seqid)
#define RPC_MSGTYPE_MAX (4 * 1024 - 1)

struct rpc_chan;
/*
 * hash_t and queue_t expect an element with first member dll_t and second seqid_t
 * rpc_msg must match this.
 */
typedef struct rpc_msg {
	hash_entry_t		h_entry;
	char			*payload;	/* XXX payload pointer can be replaced by io vector*/
	struct rpc_msg		*resp;		/* resp is stored in request. */
	struct rpc_chan		*rcp;
	Rendez			rendez;		/* sleep for response */
	rpc_msghdr_t		hdr;		/* over the wire header. MUST BE LAST MEMBER */
} rpc_msg_t;

#define RPC_SETMSGTYPE(msgp, utype) { (msgp)->hdr.type = (utype) << RPC_TYPE_RESERVED_BITS; }
#define RPC_GETMSGTYPE(msgp) ((msgp)->hdr.type >> RPC_TYPE_RESERVED_BITS)

typedef struct rpc_chan {
	int			infd;		/* XXX change to support other IPC mechanisms */
	int			outfd;
	//int			refcnt;		/* a hold count taken by each thread */
	int			enabled;	/* channel status */
	seqid_t			seqid;		/* generate new seqid for each client message */
	hash_table_t		hash;
	bufpool_t		msgpool;	/* message buffers */
	bufpool_t		datapool;	/* fixed size data buffers */
	//Rendez		rendez;		/* drain out channel users */
	rpchandler_t		handler;	/* recv task will pass request to handler task*/
	QLock			fdlock;		/* taken by writers on this fd */
	void			*usrcntxt;	/* user context */
} rpc_chan_t;

#define RPC_CHAN_LOCK(rcp) { qlock(&rcp->fdlock); }
#define RPC_CHAN_UNLOCK(rcp) { qunlock(&rcp->fdlock); }

rpc_chan_t * rpc_chan_new(void);
void rpc_chan_free(rpc_chan_t *rcp);
int rpc_chan_init(rpc_chan_t *rcp, int infd, int outfd,
		  size_t nway, size_t msgsz, size_t datasz, size_t hashsz,
		  rpchandler_t, void *usrcntxt);
void rpc_chan_deinit(rpc_chan_t *rcp);
void rpc_chan_close(rpc_chan_t *rcp);
void rpc_default_handler(void *arg);

void rpc_databuf_get(rpc_chan_t *rcp, char **bufp);
void rpc_databuf_put(rpc_chan_t *rcp, char *buf);

void rpc_msg_get(rpc_chan_t *rcp, int msgtype, size_t msglen, size_t payloadlen,
				char *payload, rpc_msg_t **msgpp);
void rpc_msg_put(rpc_chan_t *rcp, rpc_msg_t *msgp);

int rpc_request(rpc_chan_t *rcp, rpc_msg_t *msgp);
void rpc_response(rpc_chan_t *rcp, rpc_msg_t *msgp);

void dump_rpc_msghdr(rpc_msghdr_t *p);
void dump_rpc_msg(rpc_msg_t *p);

#endif /*RPC_H*/
