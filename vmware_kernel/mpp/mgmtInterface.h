/* **********************************************************
   * mgmtInterface.h
   * **********************************************************/
#ifndef __MGMTINTERFACE_H__
#define __MGMTINTERFACE_H__
#include <vmkapi.h> /* Required for VMKAPI type definitions */

/** Each callback must have a unique 64-bit integer identifier.
  * The identifiers 0 through VMK_MGMT_RESERVED_CALLBACKS
  * are reserved and must not be used by consumers of the
  * management APIs. Here, we declare just one callback
  * identifier. These identifiers are used by consumers of
  * the API at runtime to invoke the associated callback.
  *
  * Technically this identifier could start at
  * (VMK_MGMT_RESERVED_CALLBACKS + 1), but for clarity and
  * consistency with other examples, we'll choose a new
  * value here.
  */
#define TEST_CB_TO_USER (VMK_MGMT_RESERVED_CALLBACKS + 2)
/*
   * The total number of callbacks (both user and kernel).
   */
#define MY_NUM_CALLBACKS 1
/*
   * The name used to describe this interface.
   */
#define MY_INTERFACE_NAME "mgmtName"
/*
   * The vendor for this interface.
   */
#define MY_INTERFACE_VENDOR "acmeVendor"
/*
   * You should place type definitions that are shared between
   * user and kernel-space (such as those passed between user
   * and kernel-space) in this file.
   */
#define MGMT_PARM_STRLEN 128

/*
   * In this case, we're demonstrating how to define a
   * compound (structure) type to send across the user-kernel
   * boundary. This structure defines some statistics
   * that might be sent up to user-space from the kernel.
   */
typedef struct statisticsType {
	/*
	   * All types, including types embedded in structures,
	   * must be derived from VMKAPI fixed-length types. They
	   * must NOT be derived from potentially variable-length
	   * parameters that depend on host-compiler settings (ie,
	   * do not use "int", "long int", and so on).
	   */
	/* An unsigned integer parameter - number of IO Failures */
	vmk_uint64 ioFailures;
	/* An unsigned integer parameter - link speed */
	vmk_uint64 linkSpeed;
	/* A string parameter - model reported */
	vmk_uint8 modelString[MGMT_PARM_STRLEN];
	/*
	   * ALL structures used as parameter types to the VMKAPI
	   * management APIs must be packed.
	   */
} __attribute__((__packed__)) statisticsType;
/*
   * Here, we conditionally define the prototypes for the
   * callback functions that will be invoked by the VMKAPI
   * management APIs.
   */
#ifdef __VMKERNEL_MODULE__
/*
   * These are the definitions of prototypes as viewed from
   * kernel-facing code. Kernel callbacks have their prototypes
   * defined. User callbacks, in this section, will be
   * #define'd as NULL, since their callback pointer has no
   * meaning from the kernel perspective.
   *
   */
#define testUserCallback NULL
#else
/*
   * This section is where callback definitions, as visible to
   * user-space, go. In this example, there are is one user-run
   * callback: testUserCallback.
   *
   * All callbacks must return an integer, and must take a
   * vmk_uint64 as the first two arguments. The return type merely
   * indicates that the callback ran to completion without
   * error - it does not indicate the semantic success or failure
   * of the operation. The first vmk_uint64 argument is the
   * cookie argument. The cookie argument passed to the callback
   * is the same value that was given as the 'cookie' parameter
   * during initialization. Thus kernel callbacks get the cookie
   * provided to vmk_MgmtInit(), and user callbacks get the cookie
   * provided to vmk_MgmtUserInit(). The second vmk_uint64 argument
   * is the instanceId argument. For kernel callbacks, this indicates
   * the unique instance ID for which this callback is destined. In this
   * example, we aren't using unique instances and instead are supplying
   * VMK_MGMT_NO_INSTANCE_ID as this parameter.
   For user callbacks,
   * the instance ID indicates from which unique kernel instance this
   * callback originates.
   *
   * This callback takes two payload parameters: eventCount
   * and statParm. The semantics of the buffers used for
   * eventCount and statParm are determined by the individual
   * parameter type, as specified in the vmk_MgmtCallbackInfo
   * corresponding to this callback. In the case of user-running
   * callbacks (which this is), all callbacks are asynchronous
   * and therefore all parameters can only be of the type
   * VMK_MGMT_PARMTYPE_IN. This means that any changes by
   * testUserCallback to the buffers used for those parameters
   * will not be reflected in the kernel.
   */
int testUserCallback(vmk_uint64 cookie,
		vmk_uint64 instanceId, // Unused.
		// Indicate if this came from a particular instance.
		vmk_uint64 *eventCount,
		statisticsType *statParm);
#endif /* VMKERNEL */
#endif /* __MGMTINTERFACE */
