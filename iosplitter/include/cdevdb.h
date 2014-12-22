/*
 * cdevdb.h
 */
#ifndef CDEVDB_H
#define CDEVDB_H

#include "cdevtypes.h"

/*
 * DATABASE SCHEMA
 * 1. CDEV TABLE
 *	unique key = cdevid_t cache device id
 *	attributes of the cache device such as client vm id, client hdd id.
 *	supports INSERT DELETE LOOKUP UPDATE
 *
 * 2. VSSD TABLE
 *	unique key = vssdid_t vssd device id
 *	flags: on/off attributes of the vssd
 *	supports INSERT DELETE LOOKUP UPDATE
 *
 * 3. VSSDPATH TABLE
 *	unique key = vssd device id
 *	path = "dest:string"
 *		e.g. 	"ip:w.x.y.z" or
 *			"host:host_name"
 *			"file:/dev/xxx"
 *	supports INSERT DELETE LOOKUP
 *
 * CONSISTENCY REQUIREMENTS
 * Mostly, these three tables are accessed independently but sequentially
 * (that is, a global lock serializes all accesses, but one access req
 *  is for exactly one table)
 * 1. key must be unique.
 * 2. records may be created with some NULL attributes
 * 3. NULL attributes may be filled in later UPDATE operations
 * 3. when VSSD.cdevid is filled in, verify that corresponding CDEV.cdevid record exists.
 * 4. no changes are allowed when the cache device or vssd is in use.
 */

 /* over the wire structures */
typedef struct {
	cdevid_t	cdevid;		/* UNIQUE KEY */
	vmid_t		vmid;		/* may be NULL */
	hddid_t		hddid;		/* may be NULL */
} db_cdev_t;

typedef struct {
	vssdid_t	vssdid;		/* UNIQUE KEY */
	cdevid_t	cdevid;		/* may be NULL */
	uint64_t	vattr;		/* vssd attributes. 0 for now */
} db_vssd_t;

typedef struct {
	vssdid_t	vssdid;		/* UNIQUE KEY */
	uint32_t	pattr;		/* path attributes. 1 for remote */
	uint32_t	pathlen;	/* for insert and lookup commands */
	/* path string is carried in msgp payload */
} db_vpath_t;
#define VPATH_IS_REMOTE(vpathp) ((vpathp)->pattr & 0x1)

/* in core structure for vssd commands * /
typedef struct {
	int		namebuflen;
	char		*name;
	db_vssd_t	*dbvp;
} db_vssdic_t; */

#define DB_VSSD_LOCAL	1

/*
 * Access commands.
 * LOOKUP: struct has key value filled in. locate the record with matching key.
 * CDEVLOOKUP: VSSD struct has cdevid filled in. locate records with mathcing cdevid.
 */

#define DB_CMD_LOOKUP		1
#define DB_CMD_INSERT		2
#define DB_CMD_UPDATE		3
#define DB_CMD_DELETE		4
#define DB_CMD_LIST		5

#define DB_BASE 3000
#define DB_ENOENT	(DB_BASE + 1)
#define DB_EIO		(DB_BASE + 2)
#define DB_EINVAL	(DB_BASE + 3)
#define DB_ENOKEY	(DB_BASE + 4)
#define DB_ENOPEN	(DB_BASE + 5)
#define DB_E2BIG	(DB_BASE + 6)
#define DB_EEXIST	(DB_BASE + 7)
#define DB_ENOMEM	(DB_BASE + 8)

/*
 * open database of specified name.
 * dbdirname is a directory in local file system,
 * depending on implementation.
 * return 0 on success.
 * error codes: TBD
 * Note that no database handle is returned in open;
 *   there is only one instance of the database per cva.
 */
int cdevdb_open(char *dbdirname);
void cdevdb_close(void);

/*
 * db_cdev_cmd()  db_vssd_cmd(), db_vpath_cmd:
 * cmd is one of the constants DB_CMD_*
 * user passes in structure with id and possibly other fields filled in
 * return 0 on success,
 * errors:
 *	DB_ENOENT: lookup/delete not found
 *	DB_EIO: db io failed
 *	DB_EINVAL: invalid args
 *	DB_ENOKEY: VSSD update or insert cmd: matching CDEV entry does not exist
 *	DB_ENOPEN: database not open
 * These commands only manipulate the configuration file = cva database.
 * The functions provide atomic, concurrent, durable updates
 * They are pthread safe (use mutex for serialization)
 */
int db_cdev_cmd(int cmd, db_cdev_t *dcp);
int db_vssd_cmd(int cmd, db_vssd_t *dvcp);
int db_vpath_cmd(int cmd, db_vpath_t *vpathp, char *buf, int buflen);

/*
 * used by cdevhandler directly instead of going through db_cdev_cmd:
 */
int db_cdev_lookup(db_cdev_t *dcp);

/*
 * find VSSD records with matching cdevid.
 * *np is set to number of records found.
 * if dvpp is not null, array is filled up to original value of *np
 * return 0 if at least one record found,
 *  DB_E2BIG: too many records for size of dvpp. *np is set to required size.
 *  DB_EIO: db io failed.
 *  DB_ENOENT: no matching entries found
 */
int db_vssd_find(cdevid_t id, int *np, db_vssd_t *dvp);

/* expect incoming/outgoing name with this command */
int db_vpath_name_in(int cmd);
int db_vpath_name_out(int cmd);

/* default values can be changed before calling cdevdb_open */
extern int db_ctab_size;
extern int db_vtab_size;
extern int db_ptab_size;

#endif /* CDEVDB_H */
