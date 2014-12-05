#ifndef __TST_RPC_H__
#define __TST_RPC_H__

#include "rpc.h"

#define IODEPTH 32

#define IP    "127.0.0.1"
#define SPORT 45678
#define CPORT (SPORT + 1)

enum {
	RPC_READ_MSG = 32 + 1,
	RPC_WRITE_MSG,
	RPC_MAX_MSGS
};

typedef struct read_cmd {
	rpc_msghdr_t hdr;
	int          cdb[16];
	ssize_t      len;
} read_cmd_t;

typedef struct write_cmd {
	rpc_msghdr_t hdr;
	int          cdb[16];
	ssize_t      len;
} write_cmd_t;

typedef union {
	read_cmd_t  read_cmd;
	write_cmd_t write_cmd;
} rpc_maxmsg_t;

#endif
