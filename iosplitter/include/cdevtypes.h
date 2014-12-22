/*
 * cdevtypes.h
 */
#ifndef _CDEVTYPES_H
#define _CDEVTYPES_H
#include <inttypes.h>
#include <stdint.h>
#define RPC_DSKBLKSZ (512)

#define BLKSHIFT 12
#define BLKSZ (1<<BLKSHIFT)
#define BLK_MASK (BLKSZ -1)

/*-------------------------------- types ----------------------------------*/
typedef uint32_t	seqid_t;	/* message sequence id */
typedef uint64_t	tranid_t;
typedef uint32_t	blklen_t;	/* number of blocks in units of sector = 512 bytes */
typedef uint32_t	devhandle_t;	/* open device handle */

typedef uint64_t	vssdid_t;
typedef uint64_t	cdevid_t;
typedef uint64_t	hddid_t;	/* a client vm's hdd is registered with CVA */
typedef uint64_t	vmid_t;		/* client vm id is registered with CVA */
typedef uint64_t	hddaddr_t;	/* logical block address in units of sectors= 512 bytes */
typedef uint64_t	tstamp_t;	/* time stamp in milliseconds */
typedef uint32_t	offset_t;

typedef uint32_t	od_size_t;
#define ODSZ_TO_BY(x) (((off_t)x) << 12)
#define BYSZ_TO_OD(x) (((off_t)x) >> 12)

#define ODOFF_TO_BY(x) (((off_t)x) << 12)
#define BYOFF_TO_OD(x) (offset_t)((off_t)(x) >> 12)

#define MAXUINT16 (64*1024 - 1)

enum epoll_key {
	IS_AIO_VSSD = 1,
	IS_SOCK_CLIENT,
};

/* io completion callback posted to upper layer */
//typedef void (*callback_fptr)(void *voidp);

#endif /*_CDEVTYPES_H*/
