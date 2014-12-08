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
	return vmk_HeapAlloc(id, size);
}

static void *calloc(vmk_HeapID id, size_t nmemb, size_t size)
{
	void *p;

	p = vmk_HeapAlloc(id, nmemb * size);
	if (p) {
		vmk_Memset(p, 0, nmemb * size);
	}
	return p;
}

static void free(vmk_HeapID id, void *ptr)
{
	return vmk_HeapFree(id, ptr);
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

static inline int pthread_mutex_init(vmk_Lock lock, char *name, module_global_t *module)
{
	vmk_ByteCount           bc;
	char                    n[128];
	vmk_SpinlockCreateProps p;
	VMK_ReturnStatus        rc;
	int                     r;

	r = vmware_name(n, name, "lock", sizeof(n));
	if (r < 0) {
		return -1;
	}

	p.domain   = module->lockd_id;
	p.heapID   = module->heap_id;
	p.moduleID = module->mod_id;
	p.rank     = 1; /* TODO: multiple ranks may be required */
	rc         = vmk_NameInitialize(&p.name, n);
	if (rc != VMK_OK) {
		return -1;
	}

	rc = vmk_SpinlockCreate(&p, &lock);
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

#endif // __VMWARE_INCLUDE__
