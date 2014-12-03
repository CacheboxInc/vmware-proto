/* **********************************************************
   * userProgram.c
   * **********************************************************/
#include <stdio.h> /* Using for printf, etc */
#include <string.h> /* for strncpy et al */
#include "vmkapi.h" /* Required for management interface support */
#include "mgmtInterface.h" 
#include <dlfcn.h>
/*
			      * This declares your callback ID numbers that
			      * you'll need for invoking callbacks in the kernel.
			      */
/*
   * This is defined inside mgmtInterface.c and in this case is compiled with this
   * file (userProgram.c) to create the overall executable.
   */
extern vmk_MgmtApiSignature mgmtSig;
/*
   * You'll need this handle when sending any callbacks to kernel-space.
   */
vmk_MgmtUserHandle mgmtHandle;
/*
   * The prototype for your user-space callback handler must match the information
   * in your vmk_MgmtCallbackInfo structure.
   */
int testUserCallback(vmk_uint64 cookie,
		vmk_uint64 instanceId,
		vmk_uint64 *eventCount,
		statisticsType *statParm);
int
main(int argc, char **argv)
{
	int rc = 0;
	void *handle;

	int (*mgmt_usr_init)(vmk_MgmtApiSignature *, vmk_uint64, vmk_MgmtUserHandle *);
	int (*mgmt_usr_begin) (vmk_MgmtUserHandle);
	int (*mgmt_usr_cnt) (vmk_MgmtUserHandle);
	int (*mgmt_usr_destroy) (vmk_MgmtUserHandle);

	vmk_uint64 myCookie = 401; /*
				      * This could also be a heap-allocated pointer,
				      * or any other unique identifier that your
				      * code wants to receive back when a callback
				      * fires in user-space.
				      */

	if (!(handle = dlopen("/lib/libvmkmgmt.so", RTLD_LAZY))) {
		fprintf(stderr, "dlopen Failed\n");
		goto out;
	}

	//rc = vmk_MgmtUserInit(&mgmtSig, myCookie, &mgmtHandle);
	mgmt_usr_init = dlsym(handle, "vmk_MgmtUserInit");
	rc = mgmt_usr_init(&mgmtSig, myCookie, &mgmtHandle);
	if (rc != 0) {
		/*
		   * This shouldn't happen if your kernel-side management instance
		   * is initialized and your signature is well-formed.
		   */
		fprintf(stderr, "Initialization failed\n");
		goto out;
	}
	
	/*
	   * From here on out, you are able to invoke kernel-side callbacks.
	   * In this example, however, there are no kernel callbacks.
	   * To start accepting user-side callback requests from the kernel,
	   * you must invoke vmk_MgmtUserBegin, which starts a pthread context
	   * to receive those requests and process them.
	   */
	//rc = vmk_MgmtUserBegin(mgmtHandle);

	mgmt_usr_begin = dlsym(handle, "vmk_MgmtUserBegin");
	rc = mgmt_usr_begin(mgmtHandle);
	/*
	   * From this point forward, your application can receive user-space
	   * callbacks from the kernel. Since this application does not send
	   * any callbacks *to* the kernel, we have nothing more to do here.
	   *
	   * If the application intends to continue monitoring indefinitely,
	   * use vmk_MgmtUserContinue() to signify that you wish to run unless and until
	   * the monitoring thread were to crash (or be killed) or a separate thread from
	   * within the same process invokes vmk_MgmtUserEnd(). Typically you'd
	   * do this in a CIM provider, where the CIM
	   * infrastructure runs a plugin indefinitely for monitoring. That's the
	   * behavior we mimic here - indefinite monitoring.
	   */
	//rc = vmk_MgmtUserContinue(mgmtHandle);
	mgmt_usr_cnt = dlsym(handle, "vmk_MgmtUserContinue");
	rc = mgmt_usr_cnt(mgmtHandle);
	/*
	   * If we get to this point, monitoring has actually ended.
	   */
	if (rc != 0) {
		/*
		   * An error was encountered. We don't need to handle it, since
		   * we will shut down the application now anyway.
		   */
		fprintf(stderr, "vmk_MgmtUserContinue concluded with an error: %d", rc);
	}

	//vmk_MgmtUserDestroy(mgmtHandle);
	mgmt_usr_destroy = dlsym(handle, "vmk_MgmtUserDestroy");
	mgmt_usr_destroy(mgmtHandle);

out:
	dlclose(handle);
	return rc;
}

/*
   * This is our user-space handler for callback requests sent from the kernel.
   * Here we just print out the values that are given to us, to stderr. This
   * will run whenever the kernel invokes vmk_MgmtCallbackInvoke() with the
   * callback ID TEST_CB_TO_USER.
   */
int
testUserCallback(vmk_uint64 cookie,
		vmk_uint64 instanceId, // unused
		vmk_uint64 *eventCount,
		statisticsType *statParm)
{
	/*
	   * The cookie is the value that was registered with vmk_MgmtUserInit.
	   * The instanceId will be VMK_MGMT_NO_INSTANCE_ID.
	   */
	fprintf(stderr, "Cookie value: %llu\n", (unsigned long long) cookie);
	/*
	   * Note that the event count and statistics change over time,
	   * and that the kernel module sends these requests to user-space even
	   * before the user application begins monitoring. So the first data
	   * received by this application correspond to the state when the
	   * application first executed vmk_MgmtUserBegin().
	   */
	fprintf(stderr, "Event count: %llu\n", (unsigned long long) *eventCount);
	fprintf(stderr, "stats.model: %s\n", statParm->modelString);
	fprintf(stderr, "stats.ioFailures: %llu\n", statParm->ioFailures);
	fprintf(stderr, "stats.linkSpeed: %llu\n", statParm->linkSpeed);
	/*
	   * The expected output would be:
	   * Cookie value: 401
	   * Event count: <an increasing number>
	   * stats.model: Acme Model
	   * stats.ioFailures: <an increasing number by 5>
	   * stats.linkSpeed: 1000
	   */
	return 0; /* Simply indicates complete execution */
}


