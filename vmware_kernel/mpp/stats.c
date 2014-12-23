#include "vmkapi.h"
#include "vmware_include.h"
#include "stats.h"

static inline int _cb_stat_queue_init(cb_stat_t *stat)
{
	assert(stat && stat->stats);
	assert(stat->type == QUEUE);
	assert(stat->stats->initialized == VMK_TRUE);

	return 0;
}

static inline int _cb_stat_queue_deinit(cb_stat_t *stat)
{
	assert(stat && stat->stats);
	assert(stat->type == QUEUE);
	assert(stat->stats->initialized == VMK_TRUE);

	return 0;
}

static inline int _cb_stat_queue_enter(cb_stat_t *stat)
{
	assert(stat && stat->stats);
	assert(stat->type == QUEUE);
	assert(stat->stats->initialized == VMK_TRUE);

	vmk_AtomicInc64(&stat->q.running);
	return 0;
}

static inline int _cb_stat_queue_exit(cb_stat_t *stat)
{
	assert(stat && stat->stats);
	assert(stat->type == QUEUE);
	assert(stat->stats->initialized == VMK_TRUE);

	vmk_AtomicDec64(&stat->q.running);
	return 0;
}

static inline void _cb_stat_queue_update(cb_stat_t *stat, int samples, vmk_Bool print)
{
	vmk_uint64 r = vmk_AtomicRead64(&stat->q.running);
	vmk_uint64 a;

	assert(stat && stat->stats);
	assert(stat->type == QUEUE);
	assert(stat->stats->initialized == VMK_TRUE);
	assert(samples != 0);

	stat->q.total += r;
	if (print == VMK_FALSE) {
		return;
	}

	a             = stat->q.total / samples;
	stat->q.total = 0;

	vmk_WarningMessage("=== STAT %s QUEUE AVG = %lu\n", stat->name, a);
}

static inline int _cb_stat_funp_init(cb_stat_t *stat)
{
	int rc;

	assert(stat && stat->stats);
	assert(stat->stats->initialized == VMK_TRUE);
	assert(stat->type == FUN_PROFILE);

	rc = pthread_mutex_init(&stat->f.lock, stat->name, stat->stats->module);
	assert(rc == 0);
	return 0;
}

static inline int _cb_stat_funp_deinit(cb_stat_t *stat)
{
	int rc;

	assert(stat && stat->stats);
	assert(stat->stats->initialized == VMK_TRUE);
	assert(stat->type == FUN_PROFILE);

	rc = pthread_mutex_destroy(stat->f.lock);
	assert(rc == 0);
	return 0;
}

static inline int _cb_stat_funp_find_free(cb_stat_t *stat, int *h)
{
	int           c;
	int           i;
	funp_cntxt_t *f;

	c = sizeof(stat->f.cntxts)/sizeof(stat->f.cntxts[0]);

	for (i = 0; i < c; i++) {
		f = &stat->f.cntxts[i];
		if (f->used == VMK_FALSE) {
			*h = i;
			return 0;
		}
	}

	*h = -1;
	return -1;
}

static inline int _cb_stat_funp_enter(cb_stat_t *stat, stat_handle_t *handle)
{
	int          rc;
	int          h;
	funp_cntxt_t *f;

	assert(stat && stat->stats && handle);
	assert(stat->stats->initialized == VMK_TRUE);
	assert(stat->type == FUN_PROFILE);

	rc = pthread_mutex_lock(stat->f.lock);
	assert(rc == 0);

	rc = _cb_stat_funp_find_free(stat, &h);
	if (rc < 0) {
		/* skip it silently */
		*handle = -1;
		pthread_mutex_unlock(stat->f.lock);
		return 0;
	}

	assert(h >= 0 && h < sizeof(stat->f.cntxts));

	f       = &stat->f.cntxts[h];
	f->used = VMK_TRUE;
	rc      = pthread_mutex_unlock(stat->f.lock);
	assert(rc == 0);

	f->st   = vmk_GetTimerCycles();
	*handle = h;
	return 0;
}

static inline int _cb_stat_funp_exit(cb_stat_t *stat, stat_handle_t handle)
{
	vmk_TimerCycles et;
	funp_cntxt_t    *f;

	assert(stat && stat->stats);
	assert(stat->stats->initialized == VMK_TRUE);
	assert(stat->type == FUN_PROFILE);
	assert(handle < sizeof(stat->f.cntxts));

	if (handle < 0) {
		return 0;
	}

	et = vmk_GetTimerCycles();
	f  = &stat->f.cntxts[handle];

	assert(f->used == VMK_TRUE);

	if (f->st >= et) {
		/* probably scheduled on different pcpu */
	} else {
		vmk_AtomicAdd64(&stat->f.total, et - f->st);
		vmk_AtomicInc64(&stat->f.calls);
	}

	f->st   = 0;
	f->used = VMK_FALSE;

	return 0;
}

static inline void _cb_stat_funp_update(cb_stat_t *stat, int samples, vmk_Bool print)
{
	vmk_uint64 t;
	vmk_uint64 c;
	vmk_uint64 a;
	vmk_int64  us;
	vmk_int64  ms;

	assert(stat && stat->stats);
	assert(stat->stats->initialized == VMK_TRUE);
	assert(stat->type == FUN_PROFILE);

	if (print == VMK_FALSE) {
		return;
	}

	t  = vmk_AtomicRead64(&stat->f.total);
	c  = vmk_AtomicRead64(&stat->f.calls);

	if (t == 0 || c == 0) {
		return;
	}

	a  = t / c;
	us = vmk_TimerTCToUS(a);
	ms = vmk_TimerTCToMS(a);

	vmk_WarningMessage("=== STAT %s FUNC-PROF CALLS = %lu, AVG = %lu (ms) "
			"or %lu (us) \n", stat->name, c, ms, us);
}

static inline void _cb_stat_update(cb_stat_t *stat, int samples, vmk_Bool print)
{
	assert(stat && stat->stats);
	assert(stat->type > NO_STAT && stat->type < NSTATS);
	assert(stat->stats->initialized == VMK_TRUE);

	switch (stat->type) {
	default:
		assert(0);
	case QUEUE:
		_cb_stat_queue_update(stat, samples, print);
		break;
	case FUN_PROFILE:
		_cb_stat_funp_update(stat, samples, print);
		break;
	}
}

static void _cb_stats_update(vmk_TimerCookie data)
{
	cb_stat_sys_t *stats = data.ptr;
	int           rc;
	dll_t         *l;
	cb_stat_t     *stat;
	VMK_ReturnStatus s;
	int              samples;
	vmk_Bool         p;

	assert(stats);

	if (DLL_ISEMPTY(&stats->stats_list) || !stats->initialized) {
		goto out;
	}

	assert(stats->initialized == VMK_TRUE);
	assert(stats->module);

	p       = VMK_FALSE;
	samples = ++stats->sample;
	if ((samples % 1000) == 0) {
		p             = VMK_TRUE;
		stats->sample = 0;
	}

	if (p == VMK_TRUE) {
		vmk_WarningMessage("$$$$$$$ STATS PRINTS START $$$$$$$$\n");
	}

	rc = pthread_mutex_lock(stats->lock);
	assert(rc == 0);

	l = DLL_NEXT(&stats->stats_list);
	while (l != &stats->stats_list) {
		stat = container_of(l, cb_stat_t, list);
		_cb_stat_update(stat, samples, p);

		l = DLL_NEXT(l);
	}

	rc = pthread_mutex_unlock(stats->lock);
	assert(rc == 0);
	if (p == VMK_TRUE) {
		vmk_WarningMessage("$$$$$$$ STATS PRINTS END $$$$$$$$\n");
	}

out:
	s = vmk_TimerSchedule(stats->timer_q, _cb_stats_update, stats,
		VMK_USEC_PER_MSEC * 10, VMK_TIMER_DEFAULT_TOLERANCE,
		VMK_TIMER_ATTR_NONE, VMK_LOCKDOMAIN_INVALID,
		VMK_SPINLOCK_UNRANKED, &stats->timer);
	assert(s == VMK_OK);
}

int cb_stat_sys_init(cb_stat_sys_t *stats, int nstats, const char *name,
		module_global_t *module)
{
	char                n[32];
	int                 rc;
	vmk_TimerQueueProps props;
	VMK_ReturnStatus    s;

	memset(stats, 0, sizeof(*stats));

	rc = vmware_name(n, name, "-st", sizeof(n));
	if (rc < 0) {
		return -1;
	}

	rc = pthread_mutex_init(&stats->lock, n, module);
	if (rc < 0) {
		return -1;
	}

	DLL_INIT(&stats->stats_list);

	props.moduleID = module->mod_id;
	props.heapID   = module->heap_id;
	props.attribs  = VMK_TIMER_QUEUE_ATTR_NONE;

	s = vmk_TimerQueueCreate(&props, &stats->timer_q);
	if (s != VMK_OK) {
		pthread_mutex_destroy(stats->lock);
		return -1;
	}

	s = vmk_TimerSchedule(stats->timer_q, _cb_stats_update, stats,
		VMK_USEC_PER_MSEC * 10, VMK_TIMER_DEFAULT_TOLERANCE,
		VMK_TIMER_ATTR_NONE, VMK_LOCKDOMAIN_INVALID,
		VMK_SPINLOCK_UNRANKED, &stats->timer);

	if (s != VMK_OK) {
		vmk_TimerQueueDestroy(stats->timer_q);
		pthread_mutex_destroy(stats->lock);
		return -1;
	}

	stats->nstats      = nstats;
	stats->module      = module;
	stats->initialized = VMK_TRUE;
	return 0;
}

int cb_stat_sys_deinit(cb_stat_sys_t *stats)
{
	if (stats->initialized == VMK_FALSE) {
		return 0;
	}

	assert(DLL_ISEMPTY(&stats->stats_list));
	vmk_TimerCancel(stats->timer, VMK_TRUE);
	vmk_TimerQueueDestroy(stats->timer_q);
	pthread_mutex_destroy(stats->lock);
	return 0;
}

int cb_stat_init(cb_stat_sys_t *stats, cb_stat_t *stat, cb_stat_type_t type,
		const char *name)
{
	int rc;

	assert(stats && stat && name);
	assert(type > NO_STAT && type < NSTATS);
	assert(stats->initialized == VMK_TRUE);

	memset(stat, 0, sizeof(*stat));
	vmk_StringCopy(stat->name, name, sizeof(stat->name));
	stat->stats = stats;
	DLL_INIT(&stat->list);
	stat->type  = type;

	rc = pthread_mutex_lock(stats->lock);
	assert(rc == 0);

	DLL_REVADD(&stats->stats_list, &stat->list);

	rc = pthread_mutex_unlock(stats->lock);
	assert(rc == 0);

	switch (type) {
	default:
		assert(0);
	case QUEUE:
		rc = _cb_stat_queue_init(stat);
		break;
	case FUN_PROFILE:
		rc = _cb_stat_funp_init(stat);
		break;
	}
	return rc;
}

int cb_stat_deinit(cb_stat_t *stat)
{
	cb_stat_sys_t *stats;
	int           rc;

	assert(stat);
	assert(stat->type > NO_STAT && stat->type < NSTATS);
	stats = stat->stats;
	assert(stats->initialized == VMK_TRUE);

	rc = pthread_mutex_lock(stats->lock);
	assert(rc == 0);

	DLL_REM(&stat->list);

	rc = pthread_mutex_unlock(stats->lock);
	assert(rc == 0);

	switch (stat->type) {
	default:
		assert(0);
	case QUEUE:
		rc = _cb_stat_queue_deinit(stat);
		break;
	case FUN_PROFILE:
		rc = _cb_stat_funp_deinit(stat);
		break;
	}

	memset(stat, 0, sizeof(*stat));
	return rc;
}

int cb_stat_enter(cb_stat_t *stat, stat_handle_t *handle)
{
	cb_stat_sys_t *stats;
	int           rc;

	assert(stat);
	assert(stat->type > NO_STAT && stat->type < NSTATS);
	stats = stat->stats;
	assert(stats->initialized == VMK_TRUE);

	switch (stat->type) {
	default:
		assert(0);
	case QUEUE:
		if (handle) {
			*handle = 0;
		}
		rc = _cb_stat_queue_enter(stat);
		break;
	case FUN_PROFILE:
		rc = _cb_stat_funp_enter(stat, handle);
		break;
	}
	return rc;
}

int cb_stat_exit(cb_stat_t *stat, stat_handle_t handle)
{
	cb_stat_sys_t *stats;
	int           rc;

	assert(stat);
	stats = stat->stats;
	assert(stat->type > NO_STAT && stat->type < NSTATS);
	assert(stats->initialized == VMK_TRUE);

	switch (stat->type) {
	default:
		assert(0);
	case QUEUE:
		rc = _cb_stat_queue_exit(stat);
		break;
	case FUN_PROFILE:
		rc = _cb_stat_funp_exit(stat, handle);
		break;
	}
	return rc;
}
