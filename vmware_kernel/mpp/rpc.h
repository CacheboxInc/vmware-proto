#ifndef __RPC_H__
#define __RPC_H__

#include "vmkapi.h"
#include "vmware_include.h"
#include <stddef.h>
#include "bufpool.h"
#include "hash.h"
#include "bufpool.h"
#include "network.h"
#include "threadpool.h"

#define PAYLOADMAX	(1024 * 1024)

struct rpc_msg;
typedef void (*rpchandler_t)(struct rpc_msg *);

#define _RPC_USE_SEMA_

#ifdef _RPC_USE_SEMA_
#define RPC_CHAN_LOCK(rcp)   vmk_SemaLock(&((rcp)->lock))
#define RPC_CHAN_UNLOCK(rcp) vmk_SemaUnlock(&((rcp)->lock))
#else
#define RPC_CHAN_LOCK(rcp)   vmk_SpinlockLock(((rcp)->lock))
#define RPC_CHAN_UNLOCK(rcp) vmk_SpinlockUnlock(((rcp)->lock))
#endif

typedef struct rpc_chan {
	vmk_Socket      socket;
	thread_pool_t   tp;
	int             enabled;
	vmk_atomic64    seqid;
	hash_table_t    hash;
	bufpool_t       msgpool;
	bufpool_t       ploadpool;
	rpchandler_t    req_handler;
	rpchandler_t    resp_handler;
	pthread_t       recv_thread;
#ifdef _RPC_USE_SEMA_
	vmk_Semaphore   lock;
#else
	vmk_Lock        lock;
#endif
} rpc_chan_t;

/*
 * access flag in rpc_msg_hdr.type
 * Request : LSB is 1. Response: LSB is 0
 */
#define RPC_ISREQ(msgp)         (((msgp)->hdr.type & 1) ? 1 : 0)
#define RPC_SETREQ(msgp)        { (msgp)->hdr.type |= 1; }
#define RPC_SETRESP(msgp)       { (msgp)->hdr.type &= ~1; }

/*
 * 2nd bit of rpc_msg_hdr.type indicates CONNECTION CLOSED
 */
#define RPC_IS_CONNCLOSED(msgp) (((msgp)->hdr.type & 2) ? 1 : 0)
#define RPC_SET_CONNCLOSED(msgp) { (msgp)->hdr.type |= 2; }

#define RPC_TYPE_RESERVED_BITS 4

#define RPC_SETMSGTYPE(msgp, mtype) \
	(msgp)->hdr.type |= (mtype << RPC_TYPE_RESERVED_BITS);
#define RPC_GETMSGTYPE(msgp) \
	(msgp)->hdr.type >> RPC_TYPE_RESERVED_BITS

#define RPC_MSG_SEQID(msgp) (msgp->hdr.seqid)

typedef struct rpc_msghdr {
	uint64_t seqid;
	uint16_t type;
	uint16_t msglen;
	uint64_t payloadlen;
	uint16_t status;
} rpc_msghdr_t;

typedef struct rpc_msg {
	hash_entry_t    h_entry;
	char            *payload;
	struct rpc_msg  *resp;
	struct rpc_chan *rcp;
	void            *opaque;
	rpc_msghdr_t    hdr;
} rpc_msg_t;


rpc_chan_t * rpc_chan_new(void);
void rpc_chan_free(rpc_chan_t *rcp);
int rpc_chan_init(rpc_chan_t *, module_global_t *module, vmk_Socket,
		size_t nway, int reserve, size_t msgsz, size_t hashsz,
		rpchandler_t req_h, rpchandler_t resp_h);
void rpc_chan_deinit(rpc_chan_t *rcp);
void rpc_chan_close(rpc_chan_t *rcp);

int rpc_async_request(rpc_chan_t *rcp, rpc_msg_t *msgp);
int rpc_response(rpc_chan_t *rcp, rpc_msg_t *msgp);

void rpc_msg_get(rpc_chan_t *rcp, int msgtype, size_t msglen, rpc_msg_t **msgp);
void rpc_msg_put(rpc_chan_t *rcp, rpc_msg_t *msgp);

void rpc_payload_get(rpc_chan_t *rcp, uint64_t len, char **bufp);
void rpc_payload_put(rpc_chan_t *rcp, char *buf);
void rpc_payload_set(rpc_chan_t *rcp, rpc_msg_t *msgp, char *payload,
		uint64_t len);
#endif
