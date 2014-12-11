#ifndef __TST_RPC_H__
#define __TST_RPC_H__

#include "rpc.h"

#define IODEPTH 32

#define SIP   "192.168.2.34"
#define SPORT 45678
#define CIP   "192.168.2.32"
#define CPORT (SPORT + 1)

enum {
	RPC_READ_MSG = 32 + 1,
	RPC_WRITE_MSG,
	RPC_MAX_MSGS
};

typedef struct read_cmd {
	rpc_msghdr_t hdr;
	uint64_t     offset;
	uint64_t     len;
} read_cmd_t;

typedef struct write_cmd {
	rpc_msghdr_t hdr;
	uint64_t     offset;
	uint64_t     len;
} write_cmd_t;

typedef union {
	read_cmd_t  read_cmd;
	write_cmd_t write_cmd;
} rpc_maxmsg_t;

#endif
