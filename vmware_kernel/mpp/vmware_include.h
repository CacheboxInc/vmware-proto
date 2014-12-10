#ifndef __VMWARE_INCLUDE__
#define __VMWARE_INCLUDE__

#include "vmkapi.h"
#include <stddef.h>

#define assert  VMK_ASSERT
#define memset	vmk_Memset

typedef vmk_uint8   uint8_t;
typedef vmk_uint16  uint16_t;
typedef vmk_uint32  uint32_t;
typedef vmk_uint64  uint64_t;
typedef vmk_WorldID pthread_t;
typedef vmk_Lock    pthread_mutex_t;

typedef struct module_global {
	char             *module;
	vmk_ModuleID     mod_id;
	vmk_HeapID       heap_id;
	vmk_LockDomainID lockd_id;
} module_global_t;

static void *malloc(vmk_HeapID id, size_t size)
{
	return vmk_HeapAlign(id, size, 2);
}

static void *calloc(vmk_HeapID id, size_t nmemb, size_t size)
{
	void *p;

	p = vmk_HeapAlign(id, nmemb * size, 2);
	if (p) {
		vmk_Memset(p, 0, nmemb * size);
	}
	return p;
}

static void free(vmk_HeapID id, void *ptr)
{
	return vmk_HeapFree(id, ptr);
}

/*
 * vmware_heap_create
 * ==================
 * - create heap suitable for holding number of members (nmemb) each of fixed
 * size (size).
 *
 * On success:
 *     0 is returned and *heap_id will contain ID of newly created heap.
 *
 * On error:
 *     -1 is returned and *heap_id is initialized to 0.
 */
static int vmware_heap_create(vmk_HeapID *heap_id, const char *name,
		module_global_t *module, size_t nmemb, size_t size)
{
	vmk_HeapAllocationDescriptor desc;
	vmk_ByteCount                as;
	VMK_ReturnStatus             rc;
	vmk_HeapCreateProps          props;

	*heap_id       = 0;
	desc.size      = size;
	desc.alignment = 0;
	desc.count     = nmemb;
	rc             = vmk_HeapDetermineMaxSize(&desc, 1, &as);
	if (rc != VMK_OK) {
		return -1;
	}

	props.type              = VMK_HEAP_TYPE_SIMPLE;
	props.module            = module->mod_id;
	props.initial           = as;
	props.max               = props.initial;
	props.creationTimeoutMS = VMK_TIMEOUT_NONBLOCKING;
	rc                      = vmk_NameInitialize(&props.name, name);
	VMK_ASSERT(rc == VMK_OK);

	rc  = vmk_HeapCreate(&props, heap_id);
	if (rc != VMK_OK) {
		return -1;
	}

	return 0;
}

static inline int vmware_name(char *dst, const char *src1, const char *src2,
		vmk_ByteCount size)
{
	VMK_ReturnStatus rc;

	rc = vmk_StringFormat(dst, size, NULL, "%s-%s", src1, src2);

	if (rc == VMK_OK) {
		return 0;
	}

	return -1;
}

static inline int pthread_mutex_init(vmk_Lock *lock, char *name, module_global_t *module)
{
	vmk_ByteCount           bc;
	char                    n[128];
	vmk_SpinlockCreateProps p;
	VMK_ReturnStatus        rc;
	int                     r;

	r = vmware_name(n, name, "l", sizeof(n));
	if (r < 0) {
		return -1;
	}

	p.domain   = module->lockd_id;
	p.heapID   = module->heap_id;
	p.moduleID = module->mod_id;
	p.rank     = 1; /* TODO: multiple ranks may be required */
	p.type     = VMK_SPINLOCK;
	rc         = vmk_NameInitialize(&p.name, n);
	if (rc != VMK_OK) {
		return -1;
	}

	rc = vmk_SpinlockCreate(&p, lock);
	if (rc != VMK_OK) {
		return -1;
	}

	return 0;
}

static inline int pthread_mutex_lock(vmk_Lock lock)
{
	vmk_SpinlockLock(lock);
	return 0;
}

static inline int pthread_mutex_unlock(vmk_Lock lock)
{
	vmk_SpinlockUnlock(lock);
	return 0;
}

static inline int pthread_mutex_destroy(vmk_Lock lock)
{
	vmk_SpinlockDestroy(lock);
	return 0;
}

static inline int pthread_create(vmk_WorldID *world_id, const char *name,
		module_global_t *module, vmk_WorldStartFunc t_func, void *data)
{
	VMK_ReturnStatus status = VMK_FAILURE;
	vmk_WorldProps props;

	/* Create the world */
	props.name          = name;
	props.moduleID      = module->mod_id;
	props.startFunction = t_func;
	props.data          = data;
	props.schedClass    = VMK_WORLD_SCHED_CLASS_DEFAULT;
	status              = vmk_WorldCreate(&props, world_id);

	if (status == VMK_OK) {
		vmk_WarningMessage("start_send_msg world created succeed\n");
		return 0;
	} else {
		vmk_WarningMessage("start_send_msg world created failed\n");
		return -1;
	}
}

static inline int pthread_join(vmk_WorldID wid, void *unused)
{
	vmk_WorldWaitForDeath(wid);
	vmk_WorldDestroy(wid);
	return 0;
}

static inline int pthread_cancel(pthread_t wid)
{
	vmk_WorldDestroy(wid);
	return 0;
}

#endif // __VMWARE_INCLUDE__
