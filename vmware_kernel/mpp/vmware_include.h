#ifndef __VMWARE_INCLUDE__
#define __VMWARE_INCLUDE__

#include "vmkapi.h"
#include <stddef.h>

#define assert  VMK_ASSERT
#define memset	vmk_Memset

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

#endif // __VMWARE_INCLUDE__
