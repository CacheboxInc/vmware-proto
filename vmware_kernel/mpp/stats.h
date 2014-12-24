#ifndef __CB_STAT_H__
#define __CB_STAT_H__

#include "vmware_include.h"
#include "dll.h"

typedef int stat_handle_t;

typedef struct cb_stat_sys {
	vmk_Timer       timer;
	vmk_TimerQueue  timer_q;
	pthread_mutex_t lock;
	dll_t           stats_list;
	module_global_t *module;
	int             nstats;
	int             sample;
	vmk_Bool        initialized;
} cb_stat_sys_t;

typedef enum {
	NO_STAT = 0,
	QUEUE,
	FUN_PROFILE,
	NSTATS,
} cb_stat_type_t;

typedef struct cb_stats_queue {
	vmk_atomic64 total;
	vmk_atomic64 running;
} cb_stat_queue_t;

typedef struct funp_cntxt {
	vmk_TimerCycles st;
	vmk_Bool        used;
} funp_cntxt_t;

typedef struct cb_stats_funp {
	pthread_mutex_t lock;
	funp_cntxt_t    cntxts[64];
	vmk_atomic64    total;
	vmk_atomic64    calls;
} cb_stat_funp_t;

typedef struct cb_stat {
	char           name[32];
	cb_stat_sys_t  *stats;
	dll_t          list;
	cb_stat_type_t type;

	union {
		cb_stat_queue_t q;
		cb_stat_funp_t  f;
	};
} cb_stat_t;

int cb_stat_sys_init(cb_stat_sys_t *stats, int nstats, const char *n, module_global_t *module);
int cb_stat_sys_deinit(cb_stat_sys_t *stats);

int cb_stat_init(cb_stat_sys_t *stats, cb_stat_t *stat, cb_stat_type_t type, const char *name);
int cb_stat_deinit(cb_stat_t *stat);
int cb_stat_enter(cb_stat_t *stat, stat_handle_t *handle);
int cb_stat_exit(cb_stat_t *stat, stat_handle_t handle);

extern VMK_ReturnStatus vmk_TimerAdd (vmk_TimerCallback callback,
	vmk_TimerCookie data, vmk_int32 timeoutUs, vmk_Bool periodic,
	vmk_uint32 rank, vmk_Timer *timer);
#endif
