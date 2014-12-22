/*
 * This file contains user visible vssd information.
 */

#ifndef __VSSD_H__
#define __VSSD_H__

#define _FILE_OFFSET_BITS 64

#include "cdevtypes.h"
#include "err.h"

/* Max number of vssd attached to one cache device */
#define MAX_VSSD 3
//#define VIO_NIOEVENT 8

/* 48 bit cookie */
#define COOKIESZ 6
typedef struct {
	unsigned char	c[COOKIESZ];
} vio_cookie_t;

typedef struct {
	hddaddr_t	hddaddr;
	vio_cookie_t	cookie;
	uint16_t	flags;
} vio_meta_t;

typedef struct {
	hddaddr_t	hddaddr;
} vio_metadirty_t;

enum vioerr {
	VE_OK = 0,
	VE_BADCMD = 1,	// 1 invalid command value
	VE_EXIST,	// 2 file or device exists
	VE_NFILE,	// 3 too many open files or devices
	VE_OPEN,	// 4 failed to open
	VE_IOERR,	// 5 io failure
	VE_METAIOERR,	// 6 metadata io failure
	VE_INVALID_BLK,	// 7 ssd does not have the required block data
	VE_ELSEEK,	// 8 lseek failure
};

enum vioreqtype {
	VIO_WRITEP1 = 1,
	VIO_READ,
	VIO_READDIRTY,
	VIO_MARK,
	VIO_OPEN,
	VIO_CLOSE,
	VIO_METAOPEN,
	VIO_METACLOSE,
	VIO_METANEXT,
	VIO_METASET,
	VIO_COMMIT,
	VIO_ABORT,
	VIO_DONE,
};

typedef struct {
	cdevid_t	cdevid;
	vssdid_t	vssdid;
} vio_open_t;

typedef struct {
} vio_close_t;

typedef struct {
	tranid_t	tranid;
	hddaddr_t	addr;
	blklen_t	len;
	uint32_t	flags;
} vio_writep1_t;

typedef struct {
	hddaddr_t	addr;
	blklen_t	len;
} vio_read_t;
typedef vio_read_t vio_readdirty_t;

enum vio_mark {
	VIO_MARK_CLEAN = 0,
	VIO_MARK_DIRTY,
};

typedef struct {
	tranid_t	tranid;
	hddaddr_t	addr;
	uint16_t	flags; // VIO_MARK_CLEAN | VIO_MARK_DIRTY
} vio_mark_t;

typedef struct {
	tranid_t	tranid;
	uint32_t	mode;	// VIOCOMMIT or VIOABORT or VIODONE
} vio_commit_t;

/*
 * METAOPEN, METACLOSE, META_NEXT, META_SET
 */

 enum vio_metamode {
	VIO_DIRTYDATA = 1,
	VIO_ALLDATA,
 };
 
typedef struct {
	devhandle_t	metahandle;
	uint8_t		mode;
} vio_metaopenclose_t;

typedef struct {
	devhandle_t	metahandle;
	uint32_t	bufsize;
} vio_metanext_t;

typedef struct {
	devhandle_t	metahandle;
	hddaddr_t	hddaddr;
} vio_metaset_t;

/*
 * flush function pointer to be used during invalidation or flush
 */
typedef err_t (* cache_flush_fn_t) (char *buf, size_t size, uint64_t hdd_off,
		void *opaque);

typedef struct vssdic vssd_t;
typedef enum cflag cflag_t;

/*
 * Distinguish between enum viocommit VIOCOMMIT  and VIO_COMMIT of vio_req_t.type
 */
enum viocommit {
	VIOCOMMIT = 1,
	VIOABORT = 2,
	VIODONE = 3,
};

typedef struct {
	devhandle_t	handle;
	//tranid_t	tranid;
	int		type;
	char		*buf;
	union {
		vio_open_t	open;
		vio_close_t	close;
		vio_writep1_t	writep1;
		vio_read_t	read;
		vio_readdirty_t	readdirty;
		vio_commit_t	commit;
		vio_metanext_t	metanext;
		vio_metaopenclose_t	metaopenclose;
		vio_metaset_t	metaset;
		vio_mark_t	mark;
	} u;
} vio_req_t;

int vio_request(vio_req_t *reqp);

#define RPC_DSKBLKSZ (512)
#define RPC_DSKBLK_MASK (0x1FF)
/* used by cdevhandler.c */
enum rflag {
	REMOTE_OK = 1,
	REMOTE_NOT = 2,
};

int vio_open(cdevid_t cdevid, vssdid_t vssdid, enum rflag r, devhandle_t *hp);
int vio_close(devhandle_t h);
int vio_commit(devhandle_t handle, tranid_t tranid, enum viocommit mode);
int vio_writep1(devhandle_t handle, vio_writep1_t *vwp, char *buf);

err_t vio_read(devhandle_t, char * buf, hddaddr_t hddaddr, int sectors);

err_t voi_mark_invl(vssd_t *vssd, tranid_t tid, hddaddr_t hddaddr,
		cache_flush_fn_t cb, void *opaque);
err_t voi_flush(vssd_t *vssd, cache_flush_fn_t cb, void *opaque);

void vssd_stats_display(int ssd_id, vssd_t *vssd);

#endif /* __VSSD_H__ */


