/* **********************************************************
 * Copyright 2004-2012 VMware, Inc.  All rights reserved.
 * **********************************************************/

/************************************************************
 *
 *  mp-plugin-example.c : Sample module using and testing the
 *     ESX Server's Multipathing public API.
 *
 ************************************************************/

#include "vmkapi.h"
#include "vmkapi_socket.h"
#include "vmkapi_socket_ip.h"
#include "threadpool.h"
#include "mgmtInterface.h"

VMK_ReturnStatus rpc_tests(void);

// static VMK_ReturnStatus send_msg(void *);
// VMK_ReturnStatus start_send_msg(void);
//void Callback(vmk_TimerCookie unusedData);
// extern vmk_MgmtApiSignature mgmtSig;
// vmk_MgmtHandle mgmtHandle;


#define EXAMPLE_MISC_HEAP_INITIAL_SIZE (4096)
#define EXAMPLE_MISC_HEAP_MAXIMUM_SIZE (1024*1024*10)

/*
 * The maximum command heap size is determined based on the
 * maximum IOs outstanding of 64K.
 */
#define EXAMPLE_COMMAND_HEAP_INITIAL_SIZE (256 * sizeof(ExampleCommand))
#define EXAMPLE_COMMAND_HEAP_MAXIMUM_SIZE (1024*1024*4)

#define EXAMPLE_PLUGIN_REVISION_MAJOR       1
#define EXAMPLE_PLUGIN_REVISION_MINOR       0
#define EXAMPLE_PLUGIN_REVISION_UPDATE      0
#define EXAMPLE_PLUGIN_REVISION_PATCH_LEVEL 0

#define EXAMPLE_PLUGIN_REVISION  VMK_REVISION_NUMBER(EXAMPLE_PLUGIN)
#define EXAMPLE_PRODUCT_REVISION VMK_REVISION_NUMBER(EXAMPLE_PLUGIN)
#define EXAMPLE_NAME "MP_PLUGIN_EXAMPLE"
/*
 * Spinlock names can be max 19 characters
 */
#define EXAMPLE_SHORT_NAME "MPP_EXAMPLE"
#define EXAMPLE_LOCK_NAME(objectName) EXAMPLE_SHORT_NAME objectName
#define EXAMPLE_MAX_PATHS_PER_DEVICE    16
#define EXAMPLE_CMD_TIMEOUT_MS          (5 * VMK_MSEC_PER_SEC)
#define EXAMPLE_CMD_TRANSIENT_WAIT_US   (10 * VMK_USEC_PER_MSEC)
#define EXAMPLE_WARNING_MS              (30 * VMK_MSEC_PER_SEC)
#define EXAMPLE_RSIO_ABORT_WAIT_MS      (5 * VMK_MSEC_PER_SEC)

typedef enum {
   EXAMPLE_LOWEST_LOCK_RANK = 1,
   EXAMPLE_PLUGIN_LOCK_RANK,
   EXAMPLE_DEVICE_LOCK_RANK,
   EXAMPLE_PATH_LOCK_RANK,
} ExampleLockRank;

typedef enum {
   EXAMPLE_PATH_FAILURE_ACTION_NOP = 0x1,           // No specific action required
   EXAMPLE_PATH_FAILURE_ACTION_FAILOVER,            // Device failover necessary
   EXAMPLE_PATH_FAILURE_ACTION_EVAL                 // Proactive path eval advised
} ExamplePathFailureAction;

#define EXAMPLE_DEVICE_REGISTERED   0x1
#define EXAMPLE_DEVICE_REMOVING     0x2
#define EXAMPLE_DEVICE_NEEDS_UPDATE 0x4

/*
 * Reservation status flags
 */
#define EXAMPLE_DEVICE_RESERVED    0x8
#define EXAMPLE_DEVICE_RESERVING   0x10


/*
 * Types
 * 'd' means field is protected by ExampleDevice->lock
 * '=' means field is set once and does not change.
 */

typedef struct ExamplePath ExamplePath;

typedef struct ExampleResInfo {
   vmk_uint32  pendingReserves; // # of reservation cmds yet to complete
   ExamplePath *resPath;    // path on which reservation was issued.
} ExampleResInfo;

typedef struct ExampleDevice {
   vmk_Lock        lock;
   vmk_uint32      index;           // = into createdDevices array
   vmk_uint32      busyCount;       // d
   vmk_atomic64    blocked;         // device blocked count
   vmk_ScsiDevice  *device;         // =
   vmk_ScsiUid     deviceUid;       // =
   vmk_Bool        inProbe;         // d
   vmk_uint32      flags;           // d
   vmk_uint32      openCount;       // d
   vmk_uint32      numPaths;        // d
   vmk_uint32      activePathIndex; // d into paths array
   vmk_ScsiPath    *paths[EXAMPLE_MAX_PATHS_PER_DEVICE]; // d
   vmk_uint32      activeCmdCount;  // d
   ExampleResInfo  resInfo;         // d
   vmk_Timer       startTimer;      // d
   vmk_ListLinks   resvSensitiveCmdQueue;  // d reservation sensitive command queue
   vmk_ListLinks   resvSensitiveAbtQueue;  // d reservation sensitive abort queue
   vmk_Bool        resvUpdateInProgress;   // d reservation update in progress
} ExampleDevice;

// 'p' means field is protected by ExamplePath->lock
struct ExamplePath {
   vmk_Lock             lock;
   vmk_ScsiPath         *path;          // =
   vmk_uint32           flags;          // p
   vmk_Bool             isPDL;          // p
   vmk_uint32           activeCmdCount; // p
   vmk_uint32           refCount;       // p
   ExampleDevice        *exDev;         // =
};

// 'm' means field is protected by the pluginLock
typedef struct ExampleCommand ExampleCommand;
struct ExampleCommand {
   vmk_ListLinks        links;      // m
   ExampleDevice        *exDev;     // =
   vmk_ScsiPath         *scsiPath;  // =
   vmk_ScsiCommand      *scsiCmd;   // =
   vmk_ScsiCommandDoneCbk done;     // =
   void                 *doneData;  // =
};

typedef enum { 
   EXAMPLE_WORLD_STARTING,
   EXAMPLE_WORLD_RUNNING,
   EXAMPLE_WORLD_STOPPING,
   EXAMPLE_WORLD_STOPPED,
} ExampleWorldState;

// Forward declarations

static VMK_ReturnStatus ExampleDeviceStartCommand(vmk_ScsiDevice *scsiDev);
static VMK_ReturnStatus ExampleDeviceTaskManagement(vmk_ScsiDevice *scsiDev,
                                                    vmk_ScsiTaskMgmt *taskMgmt);
static VMK_ReturnStatus ExampleDeviceOpen(vmk_ScsiDevice *scsiDev);
static VMK_ReturnStatus ExampleDeviceClose(vmk_ScsiDevice *scsiDev);
static VMK_ReturnStatus ExampleDeviceProbe(vmk_ScsiDevice *scsiDev);
static VMK_ReturnStatus ExampleDeviceInquiry(vmk_ScsiDevice *device,
                                             vmk_ScsiInqType inqPage,
                                             vmk_uint8 *inquiryData,
                                             vmk_uint32 inquirySize);
static VMK_ReturnStatus ExampleDeviceDumpCmd(vmk_ScsiDevice *device,
                                             vmk_ScsiCommand *dumpCmd);
static VMK_ReturnStatus ExampleDeviceGetPathNames(vmk_ScsiDevice *device,
                                                  vmk_HeapID *heapID,
                                                  vmk_uint32 *numPathNames,
                                                  char ***pathNames);
static VMK_ReturnStatus ExampleDeviceGetBoolAttr(vmk_ScsiDevice *device,
                                                 vmk_ScsiDeviceBoolAttribute attr,
                                                 vmk_Bool *boolAttr);
static VMK_ReturnStatus ExampleDeviceIsPseudo(vmk_ScsiDevice *device,
                                              vmk_Bool *isPseudo);
static vmk_Bool ExamplePathUnrecHWError(vmk_Bool senseValid,
                                               vmk_uint8 sk,
                                               vmk_uint8 asc,
                                               vmk_uint8 ascq);
static vmk_Bool ExampleLUNotSupported(vmk_Bool senseValid,
                                             vmk_uint8 asc,
                                             vmk_uint8 ascq);
static vmk_Bool ExampleVerifyPathUID(ExamplePath *exPath);
static vmk_Bool ExampleDeviceUpdateState(vmk_ScsiDevice *device,
                                         vmk_ScsiDeviceState *state);
static VMK_ReturnStatus ExampleDestroyDevice(ExampleDevice *exDev);
static inline vmk_Bool ExampleDevice_IsBlocked(const ExampleDevice *exDev);
static void ExampleDeviceSchedStart(ExampleDevice *exDev,
                                    vmk_int32 timeoutUs);
static void ExampleAbortAllRSIOs(ExampleDevice *exDev);
static void ExampleRetryCmd(ExampleCommand *exCmd);

// Global variables

static vmk_LogComponent pluginLog;
static vmk_Lock          pluginLock;
static vmk_ScsiPlugin    *plugin;
static vmk_ModuleID      moduleID;
static vmk_LockDomainID  lockDomain = VMK_LOCKDOMAIN_INVALID;
static ExampleWorldState retryWorldState = EXAMPLE_WORLD_STOPPED;

static vmk_ConfigParamHandle configLogMPCmdErrorsHandle;

// createdDevices is protected by deviceSema
static int              maxDevices;
static ExampleDevice    **createdDevices; 

static vmk_Semaphore     deviceSema;
static vmk_HeapID        miscHeap = VMK_INVALID_HEAP_ID;
static vmk_HeapID        alignedHeap = VMK_INVALID_HEAP_ID;
static vmk_HeapID        exCommandHeap = VMK_INVALID_HEAP_ID;
static vmk_TimerQueue    timerQueue = VMK_INVALID_TIMER_QUEUE;
static vmk_ListLinks     retryQueue; 
static vmk_ServiceAcctID ScsiIoAccountingId;

static vmk_ScsiDeviceOps exampleDeviceOps = {
   .startCommand               = ExampleDeviceStartCommand,
   .taskMgmt                   = ExampleDeviceTaskManagement,
   .open                       = ExampleDeviceOpen,
   .close                      = ExampleDeviceClose,
   .probe                      = ExampleDeviceProbe,
   .getInquiry                 = ExampleDeviceInquiry,
   .issueDumpCmd               = ExampleDeviceDumpCmd,
   .u.mpDeviceOps.getPathNames = ExampleDeviceGetPathNames,
   .getBoolAttr                = ExampleDeviceGetBoolAttr,
};

/************************************************************
 *
 * Utility functions
 *
 ************************************************************/

#define ASSERT_CMD_IS_UNLINKED(exCmd) \
   VMK_ASSERT(vmk_ListIsUnlinkedElement(&(exCmd)->links))

/*
 ***********************************************************************
 * ExampleDeviceGetName --                                        */ /**
 *
 * \brief   Returns a human readable name for an example device.
 *
 * \note    This function should be used instead of vmk_ScsiGetDeviceName()
 *          till the ExampleDevice is known to be registered with the SCSI
 *          subsystem.
 *
 * \return  human readable name for the example device.
 *
 ***********************************************************************
 */
static inline char *
ExampleDeviceGetName(ExampleDevice *exDev)
{
   return exDev->deviceUid.id;
}

/*
 ***********************************************************************
 * ExampleUpdateDevOnRes --                                        */ /**
 *
 * \brief when reserving, update example device with scsi-2 resv state
 *
 * \return void
 *
 ***********************************************************************
 */
static inline void
ExampleUpdateDevOnRes(ExampleDevice *exDev, 
                      ExamplePath *exPath) 
{
   vmk_LogDebug(pluginLog, 3,
                "Updating state for device %s on SCSI-2 reserve",
		vmk_ScsiGetDeviceName(exDev->device));
   vmk_SpinlockLock(exDev->lock);
   exDev->flags &= ~EXAMPLE_DEVICE_RESERVING;
   exDev->flags |= EXAMPLE_DEVICE_RESERVED;
   VMK_ASSERT(exDev->resInfo.pendingReserves > 0);
   exDev->resInfo.pendingReserves--;
   exDev->resInfo.resPath = exPath;
   vmk_SpinlockUnlock(exDev->lock);
}


/*
 ***********************************************************************
 * ExampleUpdateDevOnRel --                                        */ /**
 *
 * \brief when releasing, update example device with scsi-2 resv state
 *
 * \param[in] device Example Plugin device
 * \param[in] incResvGen   Set to False if the function is called due to
 *                         Release command. 
 *                         True indicates the reservation/release cycle 
 *                         may be broken. If the device is reserved 
 *                         the generation count will be incremented.
 * 
 * \sideeffect device reservation generation count may be incremented
 * 
 * \return void
 *
 ***********************************************************************
 */
static inline void
ExampleUpdateDevOnRel(ExampleDevice *exDev, vmk_Bool incResvCount) 
{  
   vmk_Bool cleared = VMK_FALSE;

   vmk_LogDebug(pluginLog, 3,
                "Updating state for device %s on SCSI-2 release",
                vmk_ScsiGetDeviceName(exDev->device));
   vmk_SpinlockLock(exDev->lock);
   if (incResvCount) {
      vmk_ScsiIncReserveGeneration(exDev->device);
   }
   if (exDev->flags & EXAMPLE_DEVICE_RESERVED) {
      exDev->flags &= ~EXAMPLE_DEVICE_RESERVED;
      exDev->resInfo.resPath = NULL;
      cleared = VMK_TRUE;
   }
   vmk_SpinlockUnlock(exDev->lock);
   if (cleared) {
      vmk_LogDebug(pluginLog, 3, "Reservation state cleared for device %s",
                   vmk_ScsiGetDeviceName(exDev->device));
   }
}

/*
 ***********************************************************************
 * ExamplePathIncRefCount --                                      */ /**
 *
 * \brief Increment ref count for a path.
 *
 ***********************************************************************
 */
static inline void
ExamplePathIncRefCount(ExamplePath *exPath, vmk_Bool haveLock)
{
   if (!haveLock) {
      vmk_SpinlockLock(exPath->lock);
   }
   exPath->refCount++;
   if (!haveLock) {
      vmk_SpinlockUnlock(exPath->lock);
   }
}

/*
 ***********************************************************************
 * ExamplePathDecRefCount --                                      */ /**
 *
 * \brief Decrement ref count for a path.
 *
 ***********************************************************************
 */
static inline void
ExamplePathDecRefCount(ExamplePath *exPath, vmk_Bool haveLock)
{
   if (!haveLock) {
      vmk_SpinlockLock(exPath->lock);
   }
   VMK_ASSERT(exPath->refCount > 0);
   if (--exPath->refCount == 0) {
      vmk_WorldWakeup((vmk_WorldEventID)&exPath->refCount);
   }
   if (!haveLock) {
      vmk_SpinlockUnlock(exPath->lock);
   }
}

/*
 ***********************************************************************
 * ExampleDeviceIncBusyCount --                                   */ /**
 *
 * \brief Increment busy count for a device.
 *
 * \sideeffect Device unregister will wait for busy count to drop to
 *             zero.
 *
 *
 ***********************************************************************
 */
static inline void
ExampleDeviceIncBusyCount(ExampleDevice *exDev, vmk_Bool haveLock)
{
   if (!haveLock) {
      vmk_SpinlockLock(exDev->lock);
   }
   exDev->busyCount++;
   if (!haveLock) {
      vmk_SpinlockUnlock(exDev->lock);
   }
}

/*
 ***********************************************************************
 * ExampleDeviceDecBusyCount --                                   */ /**
 *
 * \brief Decrement busy count for a device. 

 * If drained to zero, then wakeup worlds that are waiting on
 * the busy count.
 *
 * \sideeffect Allows device unregister if busy count reaches zero
 *
 *
 ***********************************************************************
 */
static inline void
ExampleDeviceDecBusyCount(ExampleDevice *exDev, vmk_Bool haveLock)
{
   if (!haveLock) {
      vmk_SpinlockLock(exDev->lock);
   }
   VMK_ASSERT(exDev->busyCount > 0);
   if (--exDev->busyCount == 0) {
      vmk_WorldWakeup((vmk_WorldEventID)&exDev->busyCount);
   }
   if (!haveLock) {
      vmk_SpinlockUnlock(exDev->lock);
   }
}

/*
 ***********************************************************************
 * ExampleDevice_Block --                                         */ /**
 *
 * \brief Mark the Example device as blocked.
 *
 * It increments the Example device's blocked counter. The
 * call is reentrant and it takes the same number of calls
 * to ExampleDevice_Unblock() to rewind.
 * IOs issued to a blocked Example device will sit in the 
 * device queue till the device is unblocked.
 *
 ***********************************************************************
 */
static inline void
ExampleDevice_Block(ExampleDevice *exDev)
{
   vmk_AtomicInc64(&exDev->blocked);
   vmk_LogDebug(pluginLog, 5,
                "Example Device \"%s\" block count incremented "
                "(blocked count now %ld).",
                vmk_ScsiGetDeviceName(exDev->device),
                vmk_AtomicRead64(&exDev->blocked));
}

/*
 ***********************************************************************
 * ExampleDevice_Unblock --                                       */ /**
 *
 * \brief Unblock an Example device.
 *
 * It decrements the Example device's blocked counter that
 * has been incremented in ExampleDevice_Block(). The
 * Example device won't actually be unblocked until the
 * counter drops to 0.
 *
 ***********************************************************************
 */
static inline void
ExampleDevice_Unblock(ExampleDevice *exDev)
{
   VMK_ASSERT(ExampleDevice_IsBlocked(exDev));

   if (vmk_AtomicReadDec64(&exDev->blocked) == 1) {
      /*
       * Arrange to issue any commands which were queued while the
       * device was blocked
       */
      ExampleDeviceSchedStart(exDev, 0);
   }
   vmk_LogDebug(pluginLog, 5, 
                "Example Device \"%s\" block count decremented "
                "(blocked count now %ld).",
                vmk_ScsiGetDeviceName(exDev->device),
                vmk_AtomicRead64(&exDev->blocked));
}

/*
 ***********************************************************************
 * ExampleDevice_IsBlocked --                                     */ /**
 *
 * \brief Test if the Example device is blocked.
 *
 ***********************************************************************
 */
static inline vmk_Bool
ExampleDevice_IsBlocked(const ExampleDevice *exDev)
{
   return vmk_AtomicRead64(&exDev->blocked) > 0;
}

/*
 ***********************************************************************
 * ExampleUpdateResvOnFailover --                                 */ /**
 *
 * \brief Update reservation state on failover

 * If there is a scsi-2 reservation outstanding on the device,
 * reset it so that I/O can proceed to the device from the 
 * new path. Before resetting the device it aborts all the 
 * outstanding reservation sensitive IOs.
 *
 * \return VMK_ReturnStatus
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleUpdateResvOnFailover(ExamplePath *exPath,
                            ExampleDevice *exDev)
{
   VMK_ReturnStatus status = VMK_OK;

   vmk_SpinlockLock(exDev->lock);

   if ((exDev->flags & EXAMPLE_DEVICE_RESERVED ||
        exDev->flags & EXAMPLE_DEVICE_RESERVING) &&
       exDev->resInfo.resPath != exPath) {
      vmk_WorldID worldId;
      vmk_ScsiTaskMgmt  vmkTaskMgmt;
      vmk_ScsiCommandId cmdId;

      vmk_SpinlockUnlock(exDev->lock);

      vmk_LogDebug(pluginLog, 1,
                   "Attempting to deal with reservations on failover for "
                   "path \"%s\"", vmk_ScsiGetPathName(exPath->path));

      /*
       * If there are reservation sensitive IO's outstanding we do not
       * want to issue logical unit reset down the newly selected path.
       * So the first thing is to abort those IO's.
       */
      ExampleAbortAllRSIOs(exDev);

      /*
       * A SCSI-2 reservation exists on the old path.  Reset the LUN to clear
       * reservations on the new path in order to avoid conflicts.
       */
      worldId = vmk_WorldGetID();
      VMK_ASSERT(worldId);

      vmk_ScsiIncReserveGeneration(exDev->device);
      cmdId.initiator = NULL;
      cmdId.serialNumber = 0;
      vmk_ScsiInitTaskMgmt(&vmkTaskMgmt, VMK_SCSI_TASKMGMT_LUN_RESET,
                           cmdId, worldId);

      status = vmk_ScsiIssuePathTaskMgmt(exPath->path, &vmkTaskMgmt);
      if (status != VMK_OK) {
         /* Cannot recover from a failed reset. */
         vmk_Warning(pluginLog,
                     "Could not issue task mgmt to perform reset on Logical "
                     "device \"%s\".", vmk_ScsiGetDeviceName(exDev->device));
      } else {
	 ExampleUpdateDevOnRel(exDev, VMK_FALSE);
      }
      vmk_LogDebug(pluginLog, 1,
                   "Reservation update completed for device \"%s\" "
                   "with status \"%s\".",
                   vmk_ScsiGetDeviceName(exDev->device),
                   vmk_StatusToString(status));
      return status;
   }

   vmk_SpinlockUnlock(exDev->lock);
   return status;
}

/*
 ***********************************************************************
 * ExampleDequeueCommand --                                       */ /**
 *
 * \brief Dequeue a command from the head of the queue
 *
 ***********************************************************************
 */
static inline ExampleCommand *
ExampleDequeueCommand(vmk_ListLinks *queue)
{
   vmk_ListLinks *itemPtr;
   
   if (vmk_ListIsEmpty(queue)) {
      return NULL;
   }
   
   itemPtr = vmk_ListFirst(queue);
   vmk_ListRemove(itemPtr);
   return VMK_LIST_ENTRY(itemPtr, ExampleCommand, links);
}

/*
 ***********************************************************************
 * ExampleGetExCmd --                                             */ /**
 *
 * \brief Get the ExampleCommand associated with the vmk_ScsiCommand
 *
 ***********************************************************************
 */
static inline ExampleCommand *
ExampleGetExCmd(vmk_ScsiCommand *scsiCmd)
{
   return (ExampleCommand *) scsiCmd->doneData;
}

/*
 ***********************************************************************
 * ExampleGetExDev --                                             */ /**
 *
 * \brief Get the ExampleDevice associated with the vmk_ScsiDevice
 *
 ***********************************************************************
 */
static inline ExampleDevice *
ExampleGetExDev(vmk_ScsiDevice *scsiDev)
{
   return (ExampleDevice *) scsiDev->pluginPrivateData;
}

/*
 ***********************************************************************
 * ExampleGetExPath --                                            */ /**
 *
 * \brief Get the ExamplePath associated with the vmk_ScsiPath
 *
 ***********************************************************************
 */
static inline ExamplePath *
ExampleGetExPath(vmk_ScsiPath *scsiPath)
{
   return (ExamplePath *) scsiPath->pluginPrivateData;
}

/************************************************************
 *
 * IO Path
 *
 ************************************************************/

/*
 ***********************************************************************
 * ExampleAllocateExCommand --                                    */ /**
 *
 * \brief Allocate a command
 *
 * Allocate commands from a dedicated heap with a guaranteed minimum
 * size. This helps ensuring forward progress under low memory
 * conditions.
 *
 ***********************************************************************
 */
static ExampleCommand *
ExampleAllocateExCommand(void)
{
   /* Note: vmk_SlabAlloc is faster */
   ExampleCommand *exCmd = vmk_HeapAlloc(exCommandHeap, sizeof(ExampleCommand));

   if (VMK_LIKELY(exCmd)) {
      vmk_ListInitElement(&exCmd->links);
   }
   return exCmd;
}

/*
 ***********************************************************************
 * ExampleFreeExCommand --                                        */ /**
 *
 * \brief Free a command allocated with ExampleAllocateExCommand()
 *
 ***********************************************************************
 */
static void
ExampleFreeExCommand(ExampleCommand *exCmd)
{
   ASSERT_CMD_IS_UNLINKED(exCmd);
   vmk_HeapFree(exCommandHeap, exCmd); 
}

/*
 ***********************************************************************
 * ExampleCompleteDeviceCommand --                                */ /**
 *
 * \brief Completion processing for the ExampleDeviceStartCommand() call
 *
 * Complete a command issued on a logical device via
 * ExampleDeviceStartCommand()
 *
 ***********************************************************************
 */
static void 
ExampleCompleteDeviceCommand(ExampleCommand *exCmd)
{
   vmk_ScsiCommand *cmd;
   ExampleDevice *exDev = exCmd->exDev;
   vmk_ScsiSenseDataSimple sense;
   VMK_ReturnStatus status;

   cmd = exCmd->scsiCmd;
   cmd->done = exCmd->done;
   cmd->doneData = exCmd->doneData;

   vmk_WarningMessage("Anup: Command complete issued = %p\n", exCmd);

   if (vmk_ScsiCmdStatusIsCheck(cmd->status)) {
      status = vmk_ScsiCmdGetSenseData(cmd, (vmk_ScsiSenseData *)&sense,
                                       sizeof(sense));
      if (status != VMK_OK) {
         vmk_Log(pluginLog, "Get sense data failed, status %s",
                 vmk_StatusToString(status));
      } else if (vmk_ScsiCmdSenseIsPOR((vmk_ScsiSenseData *)&sense)) {
         // reservation/release cycle may have broken
         ExampleUpdateDevOnRel(exDev, VMK_TRUE);
      }
   }

   /*
    * Reservation sensitive IO must be removed from the queue
    * it is on, upon completion from Example plugin. Normally, 
    * such IO's are on the resvSensitiveCmdQueue. But if they 
    * are being aborted, they may have been moved to resvSensitiveAbtQueue. 
    */
   vmk_SpinlockLock(exDev->lock);
   exDev->activeCmdCount--;

   if (VMK_UNLIKELY(cmd->flags &
                    VMK_SCSI_COMMAND_FLAGS_RESERVATION_SENSITIVE)) {
      vmk_ListRemove(&exCmd->links);

      /*
       * Wake up the reservation update thread upon completion of a
       * reservation sensitive IO. In theory, we may have other
       * reservation sensitive IOs outstanding on the device,
       * but that is not common and we want to show some progress
       * during reservation update.
       */
      if (exDev->resvUpdateInProgress) {
         vmk_WorldWakeup((vmk_WorldEventID)&exDev->resvUpdateInProgress);
      }
   }
   vmk_SpinlockUnlock(exDev->lock);

   ExampleFreeExCommand(exCmd);

   cmd->done(cmd);

   /* Get the next command(s) */
   ExampleDeviceStartCommand(exDev->device);
}

/*
 ***********************************************************************
 * ExampleQueueCmdToRetry --                                      */ /**
 *
 * \brief Queue a command that failed due to a path failure for retry
 *
 ***********************************************************************
 */
static void
ExampleQueueCmdToRetry(ExampleCommand *exCmd)
{
   ASSERT_CMD_IS_UNLINKED(exCmd);
   VMK_ASSERT((exCmd->scsiCmd->flags &
               VMK_SCSI_COMMAND_FLAGS_RESERVATION_SENSITIVE) == 0);
   
   /*
    * The device will be blocked untill there are no IOs in the retry
    * command queue. This helps to preserve command ordering.
    */
   ExampleDevice_Block(exCmd->exDev);

   vmk_SpinlockLock(pluginLock);
   vmk_ListInsert(&exCmd->links, vmk_ListAtRear(&retryQueue));
   vmk_WorldWakeup((vmk_WorldEventID) &retryWorldState);
   vmk_SpinlockUnlock(pluginLock);
}

/*
 ***********************************************************************
 * ExampleShouldLogFailure --                                     */ /**
 *
 * \brief Decide if an IO failure should be logged.
 *
 *
 ***********************************************************************
 */
static vmk_Bool
ExampleShouldLogFailure(vmk_ScsiCommand *cmd)
{
   unsigned int doLog = 0;

   /*
    * Log important command errors.
    *
    * Log if the /Scsi/LogMPCmdErrors advanced configuration option is
    * set. Its default value is VMK_TRUE for debug builds and
    * VMK_FALSE for release builds.
    */
   vmk_ConfigParamGetUint(configLogMPCmdErrorsHandle, &doLog);
   if (!doLog) {
      return VMK_FALSE;
   }
   /* Ignore reservation conflicts. */
   if (vmk_ScsiCmdStatusIsResvConflict(cmd->status)) {
      return VMK_FALSE;
   }
   if (vmk_ScsiCmdStatusIsCheck(cmd->status)) {
      vmk_ScsiSenseDataSimple sense;
      vmk_uint8 key, asc, ascq;
      VMK_ReturnStatus status;

      if ((status = vmk_ScsiCmdGetSenseData(cmd, (vmk_ScsiSenseData *)&sense,
                                            sizeof(sense))) != VMK_OK) {
         vmk_Log(pluginLog, "Get sense data failed, status %s",
                 vmk_StatusToString(status));
         return VMK_FALSE;
      }
      if (vmk_ScsiExtractSenseData((vmk_ScsiSenseData *)&sense, &key, &asc,
                                   &ascq) == VMK_FALSE) {
         vmk_Log(pluginLog, "Extract sense data failed");
         return VMK_FALSE;
      }

      /* Ignore Medium Not Present errors. */
      if (key == VMK_SCSI_SENSE_KEY_NOT_READY &&
          asc == VMK_SCSI_ASC_MEDIUM_NOT_PRESENT) {
         return VMK_FALSE;
      }
      /* 
       * Ignore unsupported cmd error status for IOs sent to probe for
       * support.
       */
      if (cmd->flags & VMK_SCSI_COMMAND_FLAGS_PROBE_FOR_SUPPORT &&
          (key == VMK_SCSI_SENSE_KEY_ILLEGAL_REQUEST &&
           (asc == VMK_SCSI_ASC_INVALID_COMMAND_OPERATION ||
            asc == VMK_SCSI_ASC_INVALID_FIELD_IN_CDB))) {
         return VMK_FALSE;
      }
   }

   return VMK_TRUE;
}

/*
 ***********************************************************************
 * Example_PathPossibleDeviceChange  --                               */ /**
 *
 * \brief Determine whether cmd failure could be a result of device change
 *
 * \details This routine is invoked to determine if the command failure
 *          could be a result of device being changed. If it could,
 *          device is marked for UID verification
 *
 * \return vmk_Bool
 *
 ***********************************************************************
 */
static vmk_Bool
Example_PathPossibleDeviceChange(ExamplePath *exPath,
                                 vmk_ScsiCommand *scsiCmd)
{
   vmk_uint8 sk, asc, ascq;
   vmk_ScsiSenseDataSimple sense;
   vmk_Bool status;
   VMK_ReturnStatus ret;

   if (!vmk_ScsiCmdStatusIsCheck(scsiCmd->status)) {
      return VMK_FALSE;
   }

   if ((ret = vmk_ScsiCmdGetSenseData(scsiCmd, (vmk_ScsiSenseData *)&sense,
                                      sizeof(sense))) != VMK_OK) {
      vmk_Log(pluginLog, "Get sense data failed, status %s",
              vmk_StatusToString(ret));
      return VMK_FALSE;
   }
   if ((status = vmk_ScsiExtractSenseData((vmk_ScsiSenseData *)&sense, &sk,
                                          &asc, &ascq)) == VMK_FALSE) {
      vmk_Log(pluginLog, "Extract sense data failed, status %s",
              vmk_StatusToString(status));
      return VMK_FALSE;
   }
   if (sk == VMK_SCSI_SENSE_KEY_UNIT_ATTENTION &&
       (asc == VMK_SCSI_ASC_MEDIUM_MAY_HAVE_CHANGED ||
        (asc  == VMK_SCSI_ASC_CHANGED &&
         ascq == VMK_SCSI_ASC_CHANGED_ASCQ_REPORTED_LUNS_DATA_CHANGED))) {

      /*
       * It is possible that device was changed from under us
       * so we need to verify  UID
       */
      return VMK_TRUE;
   }

   return VMK_FALSE;
}

/*
 ***********************************************************************
 * Example_PathDetermineFailure  --                                   */ /**
 *
 * \brief Determine the MP plugin action taken upon path failure
 *
 * \details This routine is invoked to determine the action needed
 *          be taken upon the failure of a command over the given
 *          path. It is only called during the command completion
 *          which implies that the command has been successfully
 *          issued down the given path before. The command could
 *          have been failed by the local storage layers below, the
 *          adapter, the SAN, or the array.
 *
 * \return ExamplePathFailureAction
 *
 ***********************************************************************
 */
static ExamplePathFailureAction
Example_PathDetermineFailure(ExamplePath *exPath,
                             vmk_ScsiCommand *cmd)
{
   ExampleCommand *exCmd = ExampleGetExCmd(cmd);
   ExamplePathFailureAction ExampleAction = EXAMPLE_PATH_FAILURE_ACTION_NOP;

   /* Determine action to be taken upon path failure */
   switch (cmd->status.host) {
   case VMK_SCSI_HOST_OK:
      /* Assume OK initially */
      ExampleAction = EXAMPLE_PATH_FAILURE_ACTION_NOP;

      if (Example_PathPossibleDeviceChange(exPath, cmd)) {
         ExampleAction = EXAMPLE_PATH_FAILURE_ACTION_EVAL;
      }
      break;

   case VMK_SCSI_HOST_NO_CONNECT:
      /* Host status(es) triggering device failover */
      ExampleAction = EXAMPLE_PATH_FAILURE_ACTION_FAILOVER;
      break;

   case VMK_SCSI_HOST_BUS_BUSY:
   case VMK_SCSI_HOST_RESET:
   case VMK_SCSI_HOST_ABORT:
   case VMK_SCSI_HOST_TIMEOUT:
   case VMK_SCSI_HOST_ERROR:
      /* Host status(es) triggering path eval */
      ExampleAction = EXAMPLE_PATH_FAILURE_ACTION_EVAL;
      break;

   default:
      /* Host status(es) requiring no action */
      ExampleAction = EXAMPLE_PATH_FAILURE_ACTION_NOP;
      break;
   }

   /*
    * Reservation sensitive IOs should never be retried by plugins.
    * The recommended action is downgraded to a path evaluation
    * instead. We are setting the status to RESERVATION_LOST here
    * because the command should not have been processed on the
    * target if the determined action is to failover. We must be
    * careful to avoid setting the status to RESERVATION_LOST if
    * there is a chance the command may have been processed even
    * partially by the target since that is going to confuse VMFS.
    */
   if (ExampleAction == EXAMPLE_PATH_FAILURE_ACTION_FAILOVER &&
       cmd->flags & VMK_SCSI_COMMAND_FLAGS_RESERVATION_SENSITIVE) {
      if(cmd->status.host == VMK_SCSI_HOST_NO_CONNECT) {
         vmk_ScsiSetPathState(plugin, exCmd->scsiPath, VMK_SCSI_PATH_STATE_DEAD);
      }
      /* Don't bother overriding the bad host/device status */
      cmd->status.plugin = VMK_SCSI_PLUGIN_RESERVATION_LOST;
      ExampleAction = EXAMPLE_PATH_FAILURE_ACTION_EVAL;
   }

   return ExampleAction;
}

/*
 ***********************************************************************
 * Example_CompletePathCommand --                                 */ /**
 *
 * \brief Completion processing for vmk_ScsiIssue[A]SyncPathCommand()
 *
 * Completion callback for all commands issued by the plugin to
 * vmk_ScsiIssue[A]SyncPathCommand().
 * Perform path failover and schedule the IO for retry if the path is
 * dead. Otherwise, complete the command to the logical device. 
 *
 * \note This is one of the plugin's entry points
 * \note CPU accounting
 *       CPU accounting for the completion path is done by the PSA path
 *       layer. MP Plugin should not do its own CPU accounting via 
 *       vmk_ServiceTimeChargeBeginWorld / vmk_ServiceTimeChargeEndWorld
 *       in the completion path.
 *
 ***********************************************************************
 */
static void
Example_CompletePathCommand(vmk_ScsiCommand *cmd)
{
   ExampleCommand *exCmd = ExampleGetExCmd(cmd);
   ExamplePath *exPath = ExampleGetExPath(exCmd->scsiPath);
   ExampleDevice *exDev = exCmd->exDev;

   if (vmk_ScsiCmdIsSuccessful(cmd)) { // Command succeeded 
      if (vmk_ScsiGetPathState(exCmd->scsiPath) == VMK_SCSI_PATH_STATE_DEAD) {
         vmk_ScsiSetPathState(plugin, exCmd->scsiPath, VMK_SCSI_PATH_STATE_ON);
      }

      /* Update SCSI-2 reservation state */
      if (VMK_UNLIKELY(cmd->cdb[0] == VMK_SCSI_CMD_RESERVE_UNIT ||
                       cmd->cdb[0] == VMK_SCSI_CMD_RELEASE_UNIT)) {
         VMK_ASSERT(exDev);

         switch (cmd->cdb[0]) {
         case VMK_SCSI_CMD_RESERVE_UNIT:
            ExampleUpdateDevOnRes(exDev, exPath);
            break;

         case VMK_SCSI_CMD_RELEASE_UNIT:
            ExampleUpdateDevOnRel(exDev, VMK_FALSE);
            break;

         default:
            VMK_ASSERT(0);
            break;
         }
      }
      
      ExampleCompleteDeviceCommand(exCmd);
   } else { // Command failed
      ExamplePathFailureAction ExampleAction;

      ExampleAction = Example_PathDetermineFailure(exPath, cmd);

      /* log the error */
      if (ExampleShouldLogFailure(cmd)) {
         vmk_uint8 key = 0, asc = 0, ascq = 0;
         vmk_Bool senseValid = VMK_FALSE;
         vmk_ScsiSenseDataSimple sense;
         VMK_ReturnStatus status;

         if ((status = vmk_ScsiCmdGetSenseData(cmd, (vmk_ScsiSenseData *)&sense,
                                               sizeof(sense))) != VMK_OK) {
            vmk_Log(pluginLog, "Get sense data failed, status %s",
                    vmk_StatusToString(status));
         } else if (vmk_ScsiExtractSenseData((vmk_ScsiSenseData *)&sense,
                                             &key, &asc, &ascq) == VMK_FALSE) {
            vmk_Log(pluginLog, "Extract sense data failed");
         } else {
            senseValid = VMK_TRUE;
         }
         vmk_Log(pluginLog,
                 "Command 0x%x from world %u failed on path '%s' "
                 "for device '%s' (D:%d/H:%d) %s 0x%2x 0x%2x 0x%2x.",
                 cmd->cdb[0], cmd->worldId,
                 vmk_ScsiGetPathName(exCmd->scsiPath),
                 vmk_ScsiGetDeviceName(exCmd->exDev->device),
                 cmd->status.device, cmd->status.host,
                 senseValid ? "valid sense" : "no sense", key, asc, ascq);
      }

      if (ExampleAction == EXAMPLE_PATH_FAILURE_ACTION_FAILOVER) {
         if (cmd->status.host == VMK_SCSI_HOST_NO_CONNECT) {
            vmk_ScsiSetPathState(plugin, exCmd->scsiPath,
                                 VMK_SCSI_PATH_STATE_DEAD);
         }
         ExampleQueueCmdToRetry(exCmd);
         goto deref_count;
      }

      /* Schedule path eval, request a fast probe of the device */
      if (ExampleAction == EXAMPLE_PATH_FAILURE_ACTION_EVAL) {
         vmk_ScsiDeviceProbeRate currentProbeRate;

         vmk_ScsiSwitchDeviceProbeRate(exCmd->exDev->device,
                                       VMK_SCSI_PROBE_RATE_FAST,
                                       VMK_SCSI_ONE_PROBE_ONLY,
                                       &currentProbeRate, NULL);
      }

      /* complete the cmd */
      ExampleCompleteDeviceCommand(exCmd);

   }

deref_count:
   vmk_SpinlockLock(exPath->lock);
   exPath->activeCmdCount--;
   /*
    * Decrement the path ref count that was incremented either in
    * ExampleIssueCommand() or ExampleRetryCmd().
    */
   ExamplePathDecRefCount(exPath, VMK_TRUE);
   vmk_SpinlockUnlock(exPath->lock);
}

/*
 ***********************************************************************
 * ExampleIssueFail --                                            */ /**
 *
 * \brief Handle failure to issue command to a path
 *
 *
 ***********************************************************************
 */
static void
ExampleIssueFail(vmk_ScsiPath *path,
                 vmk_ScsiCommand *cmd,
                 VMK_ReturnStatus status)
{
   vmk_LogDebug(pluginLog, 2, "Path '%s': could not issue async cmd: %s",
                vmk_ScsiGetPathName(path),
                vmk_StatusToString(status));
   switch (status) {
   case VMK_OK:
      VMK_ASSERT(0);
      break;
   case VMK_TIMEOUT:
      cmd->status.host = VMK_SCSI_HOST_TIMEOUT;
      break;
   case VMK_NO_CONNECT:
      /* Will choose another path if available */
      cmd->status.host = VMK_SCSI_HOST_NO_CONNECT;
      break;
   case VMK_NO_MEMORY:
   default:
      /* Retried by PSA */
      cmd->status.plugin = VMK_SCSI_PLUGIN_REQUEUE;
      break;
   }
   vmk_ScsiSchedCommandCompletion(cmd);
}

/*
 ***********************************************************************
 * ExampleIssueCommand --                                         */ /**
 *
 *
 *
 ***********************************************************************
 */
static void
ExampleIssueCommand(vmk_ScsiDevice *scsiDev,
                    vmk_ScsiCommand *cmd,
                    ExampleCommand *exCmd)
{
   VMK_ReturnStatus status;
   ExampleDevice *exDev = ExampleGetExDev(scsiDev);
   ExamplePath *exPath;

   exCmd->exDev = exDev;
   exCmd->scsiCmd = cmd;
   exCmd->done = cmd->done;
   exCmd->doneData = cmd->doneData;

   vmk_SpinlockLock(exDev->lock);
   ASSERT_CMD_IS_UNLINKED(exCmd);
   VMK_ASSERT(exDev->openCount > 0);
   exDev->activeCmdCount++;

   vmk_WarningMessage("Anup: Command issued to scsci path = %p\n", exCmd);
   /*
    * For a reservation sensitive IO we must check (again) to see if
    * the device has a SCSI-2 reservation update in progress.
    */
   if (VMK_UNLIKELY(cmd->flags &
                    VMK_SCSI_COMMAND_FLAGS_RESERVATION_SENSITIVE)) {
      if (exDev->resvUpdateInProgress) {
         /*
          * Put the IO on the abortQueue as if it had been aborted by
          * the reservation update. ExampleCompleteDeviceCommand() expects
          * the reservation sensitive IO to always be queued.
          */
         vmk_ListInsert(&exCmd->links,
                        vmk_ListAtRear(&exDev->resvSensitiveAbtQueue));
         vmk_SpinlockUnlock(exDev->lock);

         vmk_Log(pluginLog, "Reservation update in progress "
                      "for NMP device \"%s\".",
                      vmk_ScsiGetDeviceName(exDev->device));
         cmd->status.host = VMK_SCSI_HOST_RETRY;
         cmd->status.plugin = VMK_SCSI_PLUGIN_RESERVATION_LOST;
         ExampleCompleteDeviceCommand(exCmd);

         return;
      }

      vmk_ListInsert(&exCmd->links,
                     vmk_ListAtRear(&exDev->resvSensitiveCmdQueue));
   }   

   cmd->done = Example_CompletePathCommand;
   cmd->doneData = exCmd;

   exCmd->scsiPath = exDev->paths[exDev->activePathIndex];
   exPath = ExampleGetExPath(exCmd->scsiPath);
   vmk_SpinlockLock(exPath->lock);
   exPath->activeCmdCount++;
   if (cmd->cdb[0] == VMK_SCSI_CMD_RESERVE_UNIT) {
      exDev->resInfo.pendingReserves++;
      if (!(exDev->flags & EXAMPLE_DEVICE_RESERVED)) {
         exDev->flags |= EXAMPLE_DEVICE_RESERVING;
         exDev->resInfo.resPath = exPath;
      }
   }
   /*
    * Take a ref on path that will be decremented when command completes
    * in Example_CompletePathCommand(). This is done to prevent the path
    * from being unclaimed while I/O is in progress on the path.
    */
   ExamplePathIncRefCount(exPath, VMK_TRUE);
   vmk_SpinlockUnlock(exPath->lock);

   vmk_SpinlockUnlock(exDev->lock);

   status = vmk_ScsiIssueAsyncPathCommandDirect(exCmd->scsiPath, cmd);
   if (status != VMK_OK) {
      ExampleIssueFail(exCmd->scsiPath, cmd, status);
   }
}

/*
 ***********************************************************************
 * ExampleCompletePathCommandDirect --                            */ /**
 *
 * \brief  Completion routine for a command sent on a path directly
 *
 ***********************************************************************
 */
static void
ExampleCompletePathCommandDirect(vmk_ScsiCommand *cmd)
{
   ExampleCommand *exCmd = ExampleGetExCmd(cmd);
   ExamplePath *exPath = ExampleGetExPath(exCmd->scsiPath);
   ExampleDevice *exDev = exPath->exDev;
   vmk_ScsiSenseDataSimple sense;
   vmk_uint8 key = 0, asc = 0, ascq = 0;
   vmk_Bool senseValid = VMK_FALSE;
   VMK_ReturnStatus status;

   if (vmk_ScsiCmdStatusIsCheck(cmd->status)) {
      if ((status = vmk_ScsiCmdGetSenseData(cmd, (vmk_ScsiSenseData *)&sense,
                                            sizeof(sense))) != VMK_OK) {
         vmk_Log(pluginLog, "Get sense data failed, status %s",
                 vmk_StatusToString(status));
      } else if (vmk_ScsiExtractSenseData((vmk_ScsiSenseData *)&sense, &key,
                                          &asc, &ascq) == VMK_FALSE) {
         vmk_Log(pluginLog, "Extract sense data failed");
      } else {
         senseValid = VMK_TRUE;
      }
   }

   if (!vmk_ScsiCmdIsSuccessful(cmd) && ExampleShouldLogFailure(cmd)) {
      vmk_Log(pluginLog,
              "Command 0x%x from world %u failed on path '%s' "
              "for device '%s' (D:%d/H:%d) %s 0x%2x 0x%2x 0x%2x.",
              cmd->cdb[0], cmd->worldId,
              vmk_ScsiGetPathName(exCmd->scsiPath),
              vmk_ScsiGetDeviceName(exDev->device),
              cmd->status.device, cmd->status.host,
              senseValid ? "valid sense" : "no sense",
              key, asc, ascq);
   }

   if (senseValid && vmk_ScsiCmdSenseIsPOR((vmk_ScsiSenseData *)&sense)) {

      /* reservation/release cycle is broken */
      ExampleUpdateDevOnRel(exDev, VMK_TRUE);
   }

   cmd->done = exCmd->done;
   cmd->doneData = exCmd->doneData;

   ExampleFreeExCommand(exCmd);
   
   vmk_SpinlockLock(exPath->lock);
   exPath->activeCmdCount--;
   /*
    * Decrement the path ref count that was incremented in
    * ExampleIssuePathCommandDirect().
    */
   ExamplePathDecRefCount(exPath, VMK_TRUE);
   vmk_SpinlockUnlock(exPath->lock);

   VMK_ASSERT(cmd->done);
   cmd->done(cmd);
}

/*
 ***********************************************************************
 * ExamplePathIssueCommandDirect --                               */ /**
 *
 * \brief  Issue command on the given path
 *
 * This function along with
 * ExampleDeviceStartCommand/ExampleIssueCommand
 * ensures that the plugin sees every command that is sent to a path
 * claimed by it.
 *
 * \return VMK_ReturnStatus
 * \note This is one of the plugin's entry points.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExamplePathIssueCommandDirect(vmk_ScsiPath *path,
                              vmk_ScsiCommand *vmkCmd)
{
   VMK_ReturnStatus status = VMK_OK;
   /* 
    * PSA guarantees that the path will not be unclaimed while
    * a command is being issued. Subsequently, after the command
    * has been issued, the plugin must ensure that a path
    * with outstanding commands is not unclaimed.
    */  
   ExamplePath *exPath = ExampleGetExPath(path);
   ExampleCommand *exCmd;

   // This function should never be invoked on the reserve/release path
   VMK_ASSERT(vmkCmd->cdb[0] != VMK_SCSI_CMD_RESERVE_UNIT);
   VMK_ASSERT(vmkCmd->cdb[0] != VMK_SCSI_CMD_RELEASE_UNIT);
   exCmd = ExampleAllocateExCommand();
   if (exCmd == NULL) {
      vmk_Warning(pluginLog, "Could not allocate command.");
      return VMK_NO_MEMORY;
   }

   /*
    * Don't need to set exCmd->scsiCmd; We don't hold on
    * to these commands and thus don't need to track them
    * in the abort handler.
    */
   exCmd->scsiCmd = NULL;
   // Don't need the device, will get it from exPath when needed
   exCmd->exDev = NULL;
   exCmd->scsiPath = path;
   exCmd->done = vmkCmd->done;
   exCmd->doneData = vmkCmd->doneData;

   vmkCmd->done = ExampleCompletePathCommandDirect;
   vmkCmd->doneData = exCmd;

   vmk_SpinlockLock(exPath->lock);
   exPath->activeCmdCount++;
   /*
    * Take a ref on path that will be decremented when command completes
    * in ExampleCompletePathCommandDirect(). This is done to prevent
    * the path from being unclaimed while I/O is in progress on the path.
    */
   ExamplePathIncRefCount(exPath, VMK_TRUE);
   vmk_SpinlockUnlock(exPath->lock);

   status = vmk_ScsiIssueAsyncPathCommandDirect(path, vmkCmd);
   if (status != VMK_OK) {
      ExampleIssueFail(path, vmkCmd, status);
      return VMK_OK;
   }

   return status;
}

/*
 ***********************************************************************
 * ExampleDeviceStartFromTimer --                                 */ /**
 *
 * \brief Timer callback to process device commands
 *
 ***********************************************************************
 */
static void
ExampleDeviceStartFromTimer(vmk_TimerCookie data)
{
   ExampleDevice *exDev = (ExampleDevice *)data.ptr;
   vmk_ScsiDevice *scsiDev = exDev->device;

   vmk_SpinlockLock(exDev->lock);
   exDev->startTimer = VMK_INVALID_TIMER;
   vmk_SpinlockUnlock(exDev->lock);

   ExampleDeviceStartCommand(scsiDev);

   /*
    * Matches ExampleDeviceIncBusyCount() in ExampleDeviceSchedStart()
    */
   ExampleDeviceDecBusyCount(exDev, VMK_FALSE);
}

/*
 ***********************************************************************
 * ExampleDeviceSchedStart --                                     */ /**
 *
 * \brief Schedule a timer to start processing IOs for the device.
 *
 * \note  If the timer is already scheduled we do not do anything.
 *        This means that the timer may actually fire earlier or
 *        later than \em timeoutUs, depending on the current running
 *        timer.
 *
 ***********************************************************************
 */
static void
ExampleDeviceSchedStart(ExampleDevice *exDev,
                        vmk_int32 timeoutUs)
{
   vmk_SpinlockLock(exDev->lock);
   if (exDev->startTimer == VMK_INVALID_TIMER) {
      VMK_ReturnStatus status;

      /*
       * Before the timer callback is called, PSA can abort all cmds
       * in its device queue, close all handles and unregister the
       * device. Then the timer will fire with an invalid scsiDev. To
       * prevent this, increment a busy count on the device which will
       * be later decremented by the timer callback.
       *
       */
      ExampleDeviceIncBusyCount(exDev, VMK_TRUE);

      status = vmk_TimerSchedule(timerQueue,
                                 ExampleDeviceStartFromTimer,
                                 exDev,
                                 timeoutUs,
                                 VMK_TIMER_DEFAULT_TOLERANCE,
                                 VMK_TIMER_ATTR_NONE,
                                 lockDomain,
                                 EXAMPLE_LOWEST_LOCK_RANK,
                                 &exDev->startTimer);
      VMK_ASSERT(status == VMK_OK);
   }
   vmk_SpinlockUnlock(exDev->lock);
}

/*
 ***********************************************************************
 * ExampleDeviceStartCommand --                                   */ /**
 *
 * \brief Process a SCSI command issued on the logical device
 *
 * \note This is one of the plugin's entry points
 * \note IO CPU accounting:
 * MP plugin has to implement CPU accounting for processing IOs 
 * on behalf of each world via vmk_ServiceTimeChargeBeginWorld / 
 * vmk_ServiceTimeChargeEndWorld API in the issuing and task
 * management paths. The code executed between the
 * vmk_ServiceTimeChargeBeginWorld and
 * vmk_ServiceTimeChargeEndWorld calls can not sleep. CPU
 * accounting for the completion path is done by the PSA path
 * layer. Path probing and other auxiliary tasks are not
 * executed on behalf of any particular world and, therefore, do 
 * not require CPU accounting.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceStartCommand(vmk_ScsiDevice *scsiDev)
{
   vmk_WorldID currentWorld = VMK_INVALID_WORLD_ID;
   vmk_ServiceTimeContext ioAcctCtx = VMK_SERVICE_TIME_CONTEXT_NONE;
   ExampleDevice *exDev = ExampleGetExDev(scsiDev);

   do {
      vmk_ScsiCommand *cmd;
      ExampleCommand  *exCmd;

      if (VMK_UNLIKELY(ExampleDevice_IsBlocked(exDev))) {
         /*
          * The retry world is retrying the failover IOs.
          * It will unblock the device once it is done
          * sending all the failover IOs.
          * Note : The device can be blocked after this 
          *        check which will result in IOs being 
          *        sent on a blocked device. This is not
          *        a problem as "ordered command delivery"
          *        is best-effort. 
          */
         vmk_Warning(pluginLog,
                     "Example Device \"%s\" is blocked. "
                     "Not starting I/O from device.",
                     vmk_ScsiGetDeviceName(exDev->device));
         break;
      }

      exCmd = ExampleAllocateExCommand();
      if (exCmd == NULL) {
         vmk_Warning(pluginLog, "Could not allocate command.");
         ExampleDeviceSchedStart(exDev, EXAMPLE_CMD_TRANSIENT_WAIT_US);
         break;
      }

      cmd = vmk_ScsiGetNextDeviceCommand(scsiDev);
      if (cmd == NULL) {
         ExampleFreeExCommand(exCmd);
         break;
      }

      /*
       * Once the removing flag is set, no IO should be issued from
       * higher layers, because we ensure openCount == 0 before
       * setting the flag and we fail opens after setting this flag.
       *
       * Note that this function can still be called by a leftover
       * timer callback.
       */
      VMK_ASSERT(!(exDev->flags & EXAMPLE_DEVICE_REMOVING));

      /*
       * Track how many CPU cycles are spent processing IOs on behalf
       * of each world.  CPU Accounting changes are somewhat
       * expensive. Switch ioAcctCtx only if starting to issue IO for
       * a different world.
       */
      if (currentWorld != cmd->worldId) {
         vmk_ServiceTimeChargeEndWorld(ioAcctCtx);
         currentWorld = cmd->worldId;
         ioAcctCtx = vmk_ServiceTimeChargeBeginWorld(ScsiIoAccountingId,
                                                     currentWorld);
      }

      ExampleIssueCommand(scsiDev, cmd, exCmd);
   } while (1);

   /* Stop CPU accounting */
   vmk_ServiceTimeChargeEndWorld(ioAcctCtx);

   return VMK_OK;
}

/*
 ***********************************************************************
 * ExampleGetFailoverPathIndex --                                 */ /**
 *
 * \brief Find a path in the device to failover to, and return its index 
 *
 *
 ***********************************************************************
 */
static vmk_uint32
ExampleGetFailoverPathIndex(ExampleDevice *exDev)
{
   vmk_uint32 i;
   vmk_uint32 originalIndex = exDev->activePathIndex;
   vmk_uint32 newIndex = EXAMPLE_MAX_PATHS_PER_DEVICE;
   
   vmk_SpinlockAssertHeldByWorld(exDev->lock);
   
   for (i = (originalIndex + 1) % EXAMPLE_MAX_PATHS_PER_DEVICE; 
        i != originalIndex;
        i = (i + 1) % EXAMPLE_MAX_PATHS_PER_DEVICE) {
      vmk_ScsiPath *path = exDev->paths[i];
      
      if (!path){
         continue;
      }

      if (vmk_ScsiGetPathState(path) == VMK_SCSI_PATH_STATE_ON) {
         return i;
      }

      if (newIndex == EXAMPLE_MAX_PATHS_PER_DEVICE) {
         newIndex = i;
      }
   }

   return (newIndex < EXAMPLE_MAX_PATHS_PER_DEVICE) ? newIndex : i;
}

/*
 ***********************************************************************
 * ExampleRetryCmd --                                             */ /**
 *
 * \brief Re-issue the command on the newly failed over path.
 *
 ***********************************************************************
 */
static void
ExampleRetryCmd(ExampleCommand *exCmd)
{
   VMK_ReturnStatus status = VMK_OK;
   vmk_ScsiPath *scsiPath;
   ExampleDevice *exDev = exCmd->exDev;
   ExamplePath *exPath;
  
   VMK_ASSERT((exCmd->scsiCmd->flags &
               VMK_SCSI_COMMAND_FLAGS_RESERVATION_SENSITIVE) == 0);
   ASSERT_CMD_IS_UNLINKED(exCmd);

   /*
    * While we are retrying commands from the retry queue, fresh commands
    * are blocked.
    */
   VMK_ASSERT(ExampleDevice_IsBlocked(exDev));

   vmk_SpinlockLock(exDev->lock);
   VMK_ASSERT(exDev->openCount > 0);
   scsiPath = exCmd->scsiPath = exDev->paths[exDev->activePathIndex];

   exPath = ExampleGetExPath(scsiPath);

   vmk_SpinlockLock(exPath->lock);   
   exPath->activeCmdCount++;
   /*
    * Increment the path ref count so that the path is not unclaimed
    * while the command is being retried on this path. The ref count will
    * be decremented in Example_CompletePathCommand() when the command
    * completes.
    */
   ExamplePathIncRefCount(exPath, VMK_TRUE);
   vmk_SpinlockUnlock(exPath->lock);

   vmk_SpinlockUnlock(exDev->lock);

   vmk_LogDebug(pluginLog, 2, 
                "Device '%s' going to re-issue command on path '%s'.",
                vmk_ScsiGetDeviceName(exDev->device),
                vmk_ScsiGetPathName(scsiPath));

   status = vmk_ScsiIssueAsyncPathCommandDirect(scsiPath, exCmd->scsiCmd);
   if (status != VMK_OK) {
      ExampleIssueFail(scsiPath, exCmd->scsiCmd, status);
   }
}

/*
 ***********************************************************************
 * ExampleDevice_AttemptFailover --                               */ /**
 *
 * \brief Attempt to failover an Example device.
 *
 ***********************************************************************
 */
static void
ExampleDevice_AttemptFailover(ExampleDevice *exDev)
{
   VMK_ASSERT(ExampleDevice_IsBlocked(exDev));

   vmk_SpinlockLock(exDev->lock);
   VMK_ASSERT(exDev->openCount > 0);

   if (exDev->numPaths == 1) {
      vmk_SpinlockUnlock(exDev->lock);
      vmk_LogDebug(pluginLog, 2, 
                   "Device %s has only one path, cannot failover!",
                   vmk_ScsiGetDeviceName(exDev->device));
   } else {
      vmk_ScsiPath *scsiPath;
      ExamplePath *exPath;
      VMK_ReturnStatus status;

      exDev->activePathIndex = ExampleGetFailoverPathIndex(exDev);
      scsiPath = exDev->paths[exDev->activePathIndex];

      exPath = ExampleGetExPath(scsiPath);

      /*
       * Hold a ref on the exPath so that it is not unclaimed.
       */
      ExamplePathIncRefCount(exPath, VMK_FALSE);

      vmk_SpinlockUnlock(exDev->lock);

      vmk_LogDebug(pluginLog, 2, 
                   "Device '%s' failing over to path '%s'.",
                   vmk_ScsiGetDeviceName(exDev->device),
                   vmk_ScsiGetPathName(scsiPath));

      /*
       * If we were holding a reservation on this device, time to clean
       * up things
       */
      status = ExampleUpdateResvOnFailover(exPath, exDev);
      if (status != VMK_OK) {

         /* 
          * If the reservation cannot be updated, log a message and continue.
          */
         vmk_Warning(pluginLog, 
                     "Could not drop reservation on failover for "
                     "device \"%s\": %s.",
                     vmk_ScsiGetDeviceName(exDev->device),
                     vmk_StatusToString(status));
      }
      ExamplePathDecRefCount(exPath, VMK_FALSE);
   }
}

/*
 ***********************************************************************
 * ExampleAbortAllRSIOs --                                        */ /**
 *
 * \brief Abort Reservation Sensitive IOs outstanding on device exDev
 *
 ***********************************************************************
 */
static void
ExampleAbortAllRSIOs(ExampleDevice *exDev)
{

   VMK_ASSERT(ExampleDevice_IsBlocked(exDev));
   for (;;) {
      VMK_ReturnStatus waitStatus;

      /*
       * We send repeated aborts for all the RSIOs to work around 
       * some adapters/drivers which might return success even 
       * when they have not actually sent the abort.
       */
      vmk_SpinlockLock(exDev->lock);
      
      exDev->resvUpdateInProgress = VMK_TRUE;

      while (!vmk_ListIsEmpty(&exDev->resvSensitiveCmdQueue)) {
         vmk_ListLinks *itemLink;
         ExampleCommand *exCmd;
         vmk_ScsiCommand *scsiCmd;
         ExamplePath *exPath;
         vmk_ScsiTaskMgmt taskMgmt;
         VMK_ReturnStatus abortStatus;

         itemLink = vmk_ListFirst(&exDev->resvSensitiveCmdQueue);
         exCmd = VMK_LIST_ENTRY(itemLink, ExampleCommand, links);
         scsiCmd = exCmd->scsiCmd;

         VMK_ASSERT(scsiCmd->flags &
                    VMK_SCSI_COMMAND_FLAGS_RESERVATION_SENSITIVE);

         exPath = ExampleGetExPath(exCmd->scsiPath);

         /*
          * Move the to-be-aborted IO to resvSensitiveAbtQueue queue so that
          * when the IO completes it doesn't mess up the current list traversal. 
          */
         vmk_ListRemove(itemLink);
         vmk_ListInsert(itemLink, 
                        vmk_ListAtRear(&exDev->resvSensitiveAbtQueue));

         /*
          * Initialize task mgmt with the cmdId found. Note that
          * we will not be referencing the scsiCmd after this
          * point. When we drop the exDev lock, the scsiCmd may
          * complete at any time and take itself out of the queue.
          * We must not trust the value of scsiCmd nor that of
          * the itemLink after the exDev lock is released.
          */
         vmk_ScsiInitTaskMgmt(&taskMgmt, VMK_SCSI_TASKMGMT_ABORT,
                              scsiCmd->cmdId, scsiCmd->worldId);
         vmk_SpinlockUnlock(exDev->lock);

         vmk_LogDebug(pluginLog, 2,
                      "Aborting reservation sensitive IO for "
                      "Example device \"%s\".",
                      vmk_ScsiGetDeviceName(exDev->device));

         abortStatus = vmk_ScsiIssuePathTaskMgmt(exPath->path,
                                                 &taskMgmt);

         /*
          * Things are really bad if we cannot even abort the
          * IO. Just log it. We repeat all the aborts anyway.
          *
          * Don't log a warning if the IO was not found.
          */
         if (abortStatus != VMK_OK && abortStatus != VMK_NOT_FOUND) {
            vmk_Warning(pluginLog, 
                        "Could not abort reservation sensitive IO "
                        "for Example device \"%s\".",
                        vmk_ScsiGetDeviceName(exDev->device));
         }
         vmk_SpinlockLock(exDev->lock);
      }

      /*
       * Check if all the RSIOs have been aborted successfully.
       * Even though we put them on resvSensitiveAbtQueue the completion path
       * simply does a vmk_ListRemove so they're removed from
       * this list just as they would have been from resvSensitiveCmdQueue.
       */
      if (vmk_ListIsEmpty(&exDev->resvSensitiveAbtQueue)) {
         exDev->resvUpdateInProgress = VMK_FALSE;
         VMK_ASSERT(vmk_ListIsEmpty(&exDev->resvSensitiveCmdQueue));
         vmk_SpinlockUnlock(exDev->lock);

         vmk_LogDebug(pluginLog, 2,
                      "All RSIOs aborted for Example device \"%s\".", 
                      vmk_ScsiGetDeviceName(exDev->device));
         break;
      }

      /*
       * Move the not-yet-aborted IOs back to resvSensitiveCmdQueue,
       * so that we can again send ABORT for them.
       */
      vmk_ListSplitAfter(&exDev->resvSensitiveAbtQueue, 
                         &exDev->resvSensitiveCmdQueue,
                         vmk_ListFirst(&exDev->resvSensitiveAbtQueue));
      waitStatus = vmk_WorldWait((vmk_WorldEventID)&exDev->resvUpdateInProgress,
                                 exDev->lock, EXAMPLE_RSIO_ABORT_WAIT_MS,
                                 "Waiting for reservation sensitive IOs to"
                                 " complete");
      /* XXX: Need to handle VMK_OK, VMK_DEATH_PENDING and VMK_WAIT_INTERRUPTED */
      if (waitStatus == VMK_TIMEOUT) {
         vmk_Warning(pluginLog,
                     "Reservation update on hold while waiting "
                     "for reservation sensitive IOs to complete on "
                     "device \"%s\"",
                     vmk_ScsiGetDeviceName(exDev->device));
      }
   }
}

/*
 ************************************************************
 * Example_RetryWorld --                               */ /**
 *
 * \brief Retry IOs that failed due to a path failure
 * \note Do CPU accounting while retrying commands.
 *       See notes for ExampleDeviceStartCommandMP for more
 *       information. 
 *
 ************************************************************
 */
static VMK_ReturnStatus
Example_RetryWorld(void *data)
{
   vmk_WorldID currentWorld = VMK_INVALID_WORLD_ID;
   vmk_ServiceTimeContext ioAcctCtx = VMK_SERVICE_TIME_CONTEXT_NONE;

   vmk_SpinlockLock(pluginLock);

   if (retryWorldState == EXAMPLE_WORLD_STARTING) {
      retryWorldState = EXAMPLE_WORLD_RUNNING;
      vmk_WorldWakeup((vmk_WorldEventID) &retryWorldState);
   }

   while (retryWorldState == EXAMPLE_WORLD_RUNNING) {
      ExampleCommand *exCmd = ExampleDequeueCommand(&retryQueue);

      if (exCmd != NULL) {
         ExampleDevice *exDev = exCmd->exDev;
         vmk_Bool failoverNeeded;
         vmk_ScsiPath *path;
         ExamplePath *exPath;

         vmk_SpinlockUnlock(pluginLock);

         /*
          * Try to failover to a different path iff the currently
          * selected path for this device is not "active". 
          * Though we may have multiple to-be-retried commands queued 
          * for the same ExampleDevice we failover only once.
          */
         vmk_SpinlockLock(exDev->lock);
         path = exDev->paths[exDev->activePathIndex];
         exPath = ExampleGetExPath(path);
         ExamplePathIncRefCount(exPath, VMK_FALSE);
         vmk_SpinlockUnlock(exDev->lock);
         failoverNeeded = vmk_ScsiGetPathState(path) != VMK_SCSI_PATH_STATE_ON;
         ExamplePathDecRefCount(exPath, VMK_FALSE);

         /*
          * Do CPU accounting for retried commands. 
          * Failover actions may block, so end accounting session if we are
          * going to attempt failover.
          */
         if (failoverNeeded) {
            vmk_ServiceTimeChargeEndWorld(ioAcctCtx);
            currentWorld = VMK_INVALID_WORLD_ID;
            ioAcctCtx = VMK_SERVICE_TIME_CONTEXT_NONE;
            ExampleDevice_AttemptFailover(exDev);
         }
         
         if (currentWorld != exCmd->scsiCmd->worldId) {
            vmk_ServiceTimeChargeEndWorld(ioAcctCtx);
            currentWorld = exCmd->scsiCmd->worldId;
            ioAcctCtx = vmk_ServiceTimeChargeBeginWorld(ScsiIoAccountingId,
                                                        currentWorld);
         }

         /*
          * Now we should have a good path for this device, retry the IO
          * over the new path.
          * Hold a ref on the ExampleDevice so that we have it around
          * even after the ExampleRetryCmd() call returns.
          */
         ExampleDeviceIncBusyCount(exDev, VMK_FALSE);
         ExampleRetryCmd(exCmd);

         /*
          * If this was the last command in the retry queue, the device
          * will be unblocked and fresh IOs will be allowed.
          */
         ExampleDevice_Unblock(exDev);
         ExampleDeviceDecBusyCount(exDev, VMK_FALSE);

         vmk_SpinlockLock(pluginLock);
      } else {
         VMK_ReturnStatus status;

         /* Stop CPU accounting - about to sleep */
         vmk_ServiceTimeChargeEndWorld(ioAcctCtx);
         currentWorld = VMK_INVALID_WORLD_ID;
         ioAcctCtx = VMK_SERVICE_TIME_CONTEXT_NONE;

         status = vmk_WorldWait((vmk_WorldEventID)&retryWorldState,
                                pluginLock, VMK_TIMEOUT_UNLIMITED_MS,
                                "Waiting for world to stop running");
         vmk_SpinlockLock(pluginLock);

         if (status == VMK_DEATH_PENDING) {
            break;
         }
      }
   }

   /* Stop CPU accounting - about to exit */
   vmk_ServiceTimeChargeEndWorld(ioAcctCtx);

   retryWorldState = EXAMPLE_WORLD_STOPPED;
   vmk_WorldWakeup((vmk_WorldEventID)&retryWorldState);

   vmk_SpinlockUnlock(pluginLock);
   return VMK_OK;
}

/*
 ***********************************************************************
 * ExampleIssueDumpCommand --                                     */ /**
 *
 * \brief Issue a synchronous dump command to the specified dump device.
 *
 *    This function is invoked during a system core dump. The kernel
 *    might be in an unstable state. The dump code path should keep
 *    interactions with the rest of the system to a minimum. Using
 *    statically allocated data structures is preferred.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleIssueSyncDumpCommand(ExampleDevice *exDev,
                            vmk_ScsiCommand *command)
{
   /*
    * Let's keep the algorithm simple for this example:
    *
    * Forward the command to the active path. Don't do path failover.
    * The code path needs to be simple while dumping core.
    */

   return vmk_ScsiIssueSyncDumpCommand(exDev->paths[exDev->activePathIndex],
                                       command);
}

/************************************************************
 *
 * Task Management
 *
 ************************************************************/

/*
 ***********************************************************************
 * ExampleDeviceTaskManagement --                                 */ /**
 *
 * \brief Process a task management request issued on the logical device
 *
 * \note This is one of the plugin's entry points
 * \note Do CPU accounting while retrying commands.
 *       See notes for ExampleDeviceStartCommandMP for more information. 
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceTaskManagement(vmk_ScsiDevice *scsiDev,
                            vmk_ScsiTaskMgmt *taskMgmt)
{
   VMK_ReturnStatus aggregateStatus = VMK_OK;
   ExampleDevice *exDev;
   ExampleCommand *exCmd;
   vmk_ListLinks abortQueue;
   vmk_uint32 i;
   vmk_ServiceTimeContext ioAcctCtx;
   vmk_ListLinks *itemPtr, *nextLink;
   exDev = ExampleGetExDev(scsiDev);

   /* Do CPU accounting here */
   ioAcctCtx = vmk_ServiceTimeChargeBeginWorld(ScsiIoAccountingId,
                                               taskMgmt->worldId);

   vmk_ListInit(&abortQueue);

   vmk_SpinlockLock(pluginLock);

   /*
    * Establish a list of all commands in the retry queue that need to be
    * aborted.
    */
   VMK_LIST_FORALL_SAFE(&retryQueue, itemPtr, nextLink) {
      vmk_ScsiTaskMgmtAction action;

      exCmd = VMK_LIST_ENTRY(itemPtr, ExampleCommand, links);
      action = vmk_ScsiQueryTaskMgmt(taskMgmt, exCmd->scsiCmd);

      if (action & VMK_SCSI_TASKMGMT_ACTION_ABORT) {
         vmk_ListRemove(&exCmd->links);
         /* All commands in retryQue increemnts a counter 
          * Since comands are being removed from the queue
          * the counter needs to be decremented 
          * using the unblock call below.
          */
         ExampleDevice_Unblock(exDev);
         vmk_ListInsert(&exCmd->links, vmk_ListAtRear(&abortQueue));
      }
   }

   vmk_SpinlockUnlock(pluginLock);

   /* And abort them. */
   while ((exCmd = ExampleDequeueCommand(&abortQueue)) != NULL) {
      exCmd->scsiCmd->status = taskMgmt->status;
      ExampleCompleteDeviceCommand(exCmd);
   }

   /*
    * Stop CPU accounting. The vmk_ScsiIssuePathTaskMgmt() calls 
    * are blocking. Do not hold the accounting context while calling them.
    */
   vmk_ServiceTimeChargeEndWorld(ioAcctCtx);

   /*
    * Otherwise, forward it to all paths that have a command in flight for the
    * logical device, and to one live path to the logical device.
    */

   VMK_ASSERT(exDev != NULL);

   vmk_SpinlockLock(exDev->lock);
   VMK_ASSERT(exDev->openCount > 0);

   for (i = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
      ExamplePath *exPath;
      
      if (!exDev->paths[i]){
         continue;
      }
      
      exPath = ExampleGetExPath(exDev->paths[i]);
      
      vmk_SpinlockLock(exPath->lock);
      /*
       * Take a ref on exPath so that it is not unclaimed, when the task
       * mgmt is in progress on that path.
       */
      ExamplePathIncRefCount(exPath, VMK_TRUE);
    
      if (exPath->activeCmdCount || i == exDev->activePathIndex) {
         VMK_ReturnStatus individualStatus;
   
         vmk_SpinlockUnlock(exPath->lock);
         vmk_SpinlockUnlock(exDev->lock);
         individualStatus = vmk_ScsiIssuePathTaskMgmt(exDev->paths[i],
                                                      taskMgmt);

         if ((taskMgmt->type == VMK_SCSI_TASKMGMT_LUN_RESET    ||
              taskMgmt->type == VMK_SCSI_TASKMGMT_DEVICE_RESET ||
              taskMgmt->type == VMK_SCSI_TASKMGMT_BUS_RESET) &&
             individualStatus == VMK_OK) {
            ExampleUpdateDevOnRel(exDev, VMK_FALSE);
         }
         
         /*
          * If an error occured, remember the status but keep processing the
          * task management (best effort).
          */
         if (aggregateStatus == VMK_OK && individualStatus != VMK_OK) {
            aggregateStatus = individualStatus;
         }

         vmk_SpinlockLock(exDev->lock);
      } else {
         vmk_SpinlockUnlock(exPath->lock);
      }
      ExamplePathDecRefCount(exPath, VMK_FALSE);
   }
   
   vmk_SpinlockUnlock(exDev->lock);

   return aggregateStatus;
}

/************************************************************
 *
 * Physical path
 *
 ************************************************************/

/*
 ***********************************************************************
 * ExampleCreateDevice                                            */ /**
 *
 * \brief Create a logical device and add it to createdDevices array.
 * \note deviceSema is assumed to be held.
 * 
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleCreateDevice(vmk_ScsiUid *uid,
                    ExampleDevice **pExDev)
{
   VMK_ReturnStatus status = VMK_OK;
   vmk_Bool spInitialized = VMK_FALSE;
   ExampleDevice *exDev;
   vmk_uint32 i;
   vmk_SpinlockCreateProps lockProps;

   vmk_SemaAssertIsLocked(&deviceSema);

   exDev = vmk_HeapAlloc(miscHeap, sizeof(ExampleDevice));
   if (exDev == NULL) {
      return VMK_NO_MEMORY;
   }
   vmk_Memset(exDev, 0, sizeof(*exDev));

   lockProps.moduleID = moduleID;
   lockProps.heapID = miscHeap;
   status = vmk_NameInitialize(&lockProps.name, EXAMPLE_LOCK_NAME("exDev"));
   VMK_ASSERT(status == VMK_OK);
   lockProps.type = VMK_SPINLOCK;
   lockProps.domain = lockDomain; 
   lockProps.rank = EXAMPLE_DEVICE_LOCK_RANK;
   status = vmk_SpinlockCreate(&lockProps, &exDev->lock);
   if (status != VMK_OK) {
      goto error;
   }
   spInitialized = VMK_TRUE;

   exDev->numPaths = 0;
   exDev->activePathIndex = 0;
   exDev->startTimer = VMK_INVALID_WORLD_ID;

   vmk_Memcpy(&exDev->deviceUid, uid, sizeof(*uid));

   vmk_AtomicWrite64(&exDev->blocked, 0);
   vmk_ListInit(&exDev->resvSensitiveCmdQueue);
   vmk_ListInit(&exDev->resvSensitiveAbtQueue);
   exDev->resvUpdateInProgress = VMK_FALSE;

   /*
    * Find an empty slot. deviceSema should already be held
    * to protect access to createdDevices.
    */
   for (i = 1; i < maxDevices; i++) {
      if (createdDevices[i] == NULL) {
         break;
      }
   }
   
   if (i == maxDevices) {
      status = VMK_NO_RESOURCES;
      goto error;
   }

   exDev->index = i;
   createdDevices[i] = exDev;

   vmk_LogDebug(pluginLog, 1, "Created logical device '%s'", uid->id);

   *pExDev = exDev;

   return status;

error:
   vmk_Warning(pluginLog, "Could not create device '%s': %s",
               uid->id, vmk_StatusToString(status));
   
   if (spInitialized) {
      vmk_SpinlockDestroy(exDev->lock);
   }

   vmk_HeapFree(miscHeap, exDev);

   *pExDev = NULL;

   return status; 
}

/*
 ***********************************************************************
 * ExampleIsSupportedArray --                                     */ /**
 *
 * \brief Tell whether a device is a supported array.
 *
 *     A real plugin would probably only claim known arrays. For the
 *     sake of simplicity, this example pretends to support everything.
 *
 ***********************************************************************
 */
static vmk_Bool
ExampleIsSupportedArray(vmk_ScsiPath *path)
{
   return VMK_TRUE;
}

/*
 ***********************************************************************
 * ExampleCreateExPath --                                         */ /**
 *
 * \brief Create an ExamplePath associated with vmk_ScsiPath
 *
 *     The function does not add the path to an exDev's paths array.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleCreateExPath(vmk_ScsiPath *path,
                    ExamplePath **pExPath)
{
   ExamplePath *exPath = NULL;
   VMK_ReturnStatus status = VMK_OK;
   vmk_Bool spInitialized = VMK_FALSE;
   vmk_SpinlockCreateProps lockProps;

   exPath = vmk_HeapAlloc(miscHeap, sizeof(*exPath));
   if (exPath == NULL) {
      return VMK_NO_MEMORY;
   }

   vmk_Memset(exPath, 0, sizeof(*exPath));

   exPath->path = path;
   lockProps.moduleID = moduleID;
   lockProps.heapID = miscHeap;
   status = vmk_NameInitialize(&lockProps.name, EXAMPLE_LOCK_NAME("exPath"));
   VMK_ASSERT(status == VMK_OK);
   lockProps.type = VMK_SPINLOCK;
   lockProps.domain = lockDomain; 
   lockProps.rank = EXAMPLE_PATH_LOCK_RANK;
   status = vmk_SpinlockCreate(&lockProps, &exPath->lock);
   if (status != VMK_OK) {
      goto error;
   }

   spInitialized = VMK_TRUE;

   path->pluginPrivateData = (void *)exPath;

   *pExPath = exPath;
   
   return status;

error:
   vmk_Warning(pluginLog, "Could not create path '%s': %s",
               vmk_ScsiGetPathName(path), vmk_StatusToString(status));

   if (spInitialized) {
      vmk_SpinlockDestroy(exPath->lock);
   }
   
   vmk_HeapFree(miscHeap, exPath);
   
   *pExPath = NULL;
   
   return status; 
}

/*
 ***********************************************************************
 * ExampleDestroyExPath --                                        */ /**
 *
 * \brief Destroy the supplied ExamplePath.
 *
 *     Doesn't remove exPath from its device's paths list.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDestroyExPath(ExamplePath *exPath)
{

   VMK_ASSERT(exPath->activeCmdCount == 0);

   vmk_SpinlockDestroy(exPath->lock);
   vmk_HeapFree(miscHeap, exPath);

   return VMK_OK;
}

/*
 ***********************************************************************
 * ExampleHasMoreWorkingPaths --                                  */ /**
 *
 * \brief Has more working path to the device except the one given
 *         If activeOnly is true, will only consider paths in ON state
 *         to be working paths; otherwise, standby is also counted.
 *         lastPath is optional, if provided, indicates if the given
 *         path is the last one remaining
 * \return vmk_Bool
 * 
 ***********************************************************************
 */
static vmk_Bool
ExampleHasMoreWorkingPaths(vmk_ScsiPath *path,
                           vmk_Bool activeOnly,
                           vmk_Bool *lastPath)
{
   int i;
   ExamplePath *exPath = ExampleGetExPath(path);
   ExampleDevice *exDev = exPath->exDev;

   vmk_SpinlockLock(exDev->lock);

   /* Check if this is the last path */
   if (exDev->numPaths == 1) {
      vmk_SpinlockUnlock(exDev->lock);
      vmk_Warning(pluginLog, "Last path only to device \"%s\".",
                  vmk_ScsiGetDeviceName(exDev->device));

      if (lastPath) {
         *lastPath = VMK_TRUE;
      }

      return VMK_FALSE;
   }

   /* The given path is not the last one */
   if (lastPath) {
      *lastPath = VMK_FALSE;
   }

   /* Check for an ON path */
   for (i = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
      if (exDev->paths[i] != NULL && path != exDev->paths[i]) {
         if (vmk_ScsiGetPathState(exDev->paths[i]) == VMK_SCSI_PATH_STATE_ON) {
            vmk_SpinlockUnlock(exDev->lock);
            return VMK_TRUE;
         }
      }
   }

   if (!activeOnly) {
      /* Check for a STANDBY path */
      for (i = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
         if (exDev->paths[i] != NULL && path != exDev->paths[i]) {
            if (vmk_ScsiGetPathState(exDev->paths[i]) ==
                VMK_SCSI_PATH_STATE_STANDBY) {
               vmk_SpinlockUnlock(exDev->lock);
               vmk_Log(pluginLog, "STANDBY path(s) only to device \"%s\".",
                       vmk_ScsiGetDeviceName(exDev->device));
               return VMK_TRUE;
            }
         }
      }
   }

   vmk_SpinlockUnlock(exDev->lock);
   vmk_Warning(pluginLog, "No more working path to device \"%s\".",
               vmk_ScsiGetDeviceName(exDev->device));

   return VMK_FALSE;
}

/*
 ***********************************************************************
 * ExamplePathClaimBegin --                                       */ /**
 *
 * \brief The framework is about to offer paths to this plugin.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExamplePathClaimBegin(vmk_ScsiPlugin *_plugin)
{
   VMK_ASSERT(_plugin == plugin);
   vmk_WarningMessage("Anup: ExamplePathClaimBegin start\n");
   VMK_ASSERT(vmk_ScsiGetPluginState(plugin) == VMK_SCSI_PLUGIN_STATE_ENABLED);
   vmk_ScsiSetPluginState(plugin, VMK_SCSI_PLUGIN_STATE_CLAIM_PATHS);
   vmk_WarningMessage("Anup: ExamplePathClaimBegin end\n");
   return VMK_OK;
}

/*
 ***********************************************************************
 * ExamplePathClaimEnd --                                         */ /**
 * 
 * \brief Framework is done offering us paths
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExamplePathClaimEnd(vmk_ScsiPlugin *_plugin)
{
   VMK_ReturnStatus status = VMK_OK;
   vmk_uint32 i;

   VMK_ASSERT(_plugin == plugin);
   vmk_WarningMessage("Anup: ExamplePathClaimEnd start\n");
   VMK_ASSERT(vmk_ScsiGetPluginState(plugin) ==
              VMK_SCSI_PLUGIN_STATE_CLAIM_PATHS);

   vmk_ScsiSetPluginState(plugin, VMK_SCSI_PLUGIN_STATE_ENABLED);

   /*
    * Once all known paths to a device have been presented and claimed,
    * try to register any logical devices with PSA.
    */

   vmk_SemaLock(&deviceSema);
   
   for (i = 0; i < maxDevices; i++) {
      ExampleDevice *exDev = createdDevices[i];

      if (exDev == NULL) {
         continue;
      }

      vmk_SpinlockLock(exDev->lock);
      if (exDev->flags & EXAMPLE_DEVICE_REGISTERED) {
         /*
          * A path to an already existing device was discovered. Run a
          * probe before completing the claim. This makes sure the
          * path is no longer dead when the rescan or claim operation
          * that started path claiming finishes.
          */
         if (exDev->flags & EXAMPLE_DEVICE_NEEDS_UPDATE) {
            exDev->flags &= ~EXAMPLE_DEVICE_NEEDS_UPDATE;
            vmk_SpinlockUnlock(exDev->lock);
            status = ExampleDeviceProbe(exDev->device);
            if (status != VMK_OK) {
               if (status != VMK_BUSY) {
                  vmk_Warning(pluginLog, "ExampleDeviceProbe failed: %s",
                              vmk_StatusToString(status));
               }
               vmk_ScsiSwitchDeviceProbeRate(exDev->device,
                                             VMK_SCSI_PROBE_RATE_FAST,
                                             VMK_SCSI_ONE_PROBE_ONLY,
                                             NULL, NULL);
            }
         } else {
            vmk_SpinlockUnlock(exDev->lock);
         }
      } else {
         vmk_ScsiUid *uids[1];

         vmk_SpinlockUnlock(exDev->lock);
         uids[0] = &exDev->deviceUid;
         
         /* Set the Primary flag */
         uids[0]->idFlags = VMK_SCSI_UID_FLAG_PRIMARY;
         
         vmk_LogDebug(pluginLog, 2,
                      "Registering logical device with uid '%s'.", 
                      uids[0]->id);

         exDev->device = vmk_ScsiAllocateDevice(plugin);
         if (exDev->device == NULL) {
            vmk_Warning(pluginLog,
                        "Out of memory allocating device with uid '%s'.",
                        uids[0]->id);
            status = VMK_NO_MEMORY;
            continue;
         }

         exDev->device->pluginPrivateData = exDev;
         exDev->device->ops = &exampleDeviceOps;
         exDev->device->moduleID = moduleID;

         status = ExampleDeviceProbe(exDev->device);
         if (status != VMK_OK) {
            vmk_Warning(pluginLog, "ExampleDeviceProbe failed: %s",
                        vmk_StatusToString(status));
            vmk_ScsiFreeDevice(exDev->device);
            exDev->device = NULL;
            vmk_SemaUnlock(&deviceSema);
   	    vmk_WarningMessage("Anup: ExamplePathClaimEnd return failed\n");
            return status;
         }

         status = vmk_ScsiRegisterDevice(exDev->device, uids, 1);
         if (status == VMK_OK) {
            vmk_SpinlockLock(exDev->lock);
            exDev->flags |= EXAMPLE_DEVICE_REGISTERED;
            vmk_SpinlockUnlock(exDev->lock);
         } else {
            vmk_LogDebug(pluginLog, 0,
                         "Failed to register device with uid '%s': %s",
                         uids[0]->id, vmk_StatusToString(status));
            /*
             * The registration failed. 
             * exDev->device may be in an inconsistent state. 
             */
            vmk_ScsiFreeDevice(exDev->device);
            exDev->device = NULL;
         }
      }
   }

   vmk_SemaUnlock(&deviceSema);

   vmk_WarningMessage("Anup: ExamplePathClaimEnd return success\n");
   return status;
}

/*
 ***********************************************************************
 * ExampleClaimPath --                                            */ /**
 *
 * \brief Claim paths to supported physical devices
 * Claim the paths to supported physical devices that are
 * submitted by the SCSI Plugin framework
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleClaimPath(vmk_ScsiPath *path,
                 vmk_Bool *claimed)
{
   ExampleDevice *exDev;
   ExamplePath *exPath;
   vmk_ScsiUid *uid;
   vmk_uint32 i;
   VMK_ReturnStatus status;
   vmk_Bool deviceSlotAvailable = VMK_FALSE;

   VMK_ASSERT(vmk_ScsiGetPluginState(plugin) ==
              VMK_SCSI_PLUGIN_STATE_CLAIM_PATHS);

   vmk_WarningMessage("Anup: ExampleClaimPath start\n");
   if (!ExampleIsSupportedArray(path)) {
      *claimed = VMK_FALSE;
      return VMK_OK;
   }

   /*
    * We support the array and intend to claim the path. If something should
    * happen that prevents us from successfully claiming it, simply return
    * a non-VMK_OK status to let the framework know that a problem occured 
    * and that it should NOT offer the path to any other plugin. The framework
    * will re-submit the path on the next rescan.
    */

   status = ExampleCreateExPath(path, &exPath);
   if (status != VMK_OK) {
      return status;
   }

   /* Get the device's UID */
   uid = vmk_HeapAlloc(miscHeap, sizeof(*uid));
   if (uid == NULL) {
      status = VMK_NO_MEMORY;
      goto error;
   }
   vmk_Memset(uid, 0, sizeof(*uid));

   status = vmk_ScsiGetCachedPathStandardUID(path, uid);
   if (status != VMK_OK) {
      /*
       * The device has no standard UID. Assume that this is a local SCSI disk
       * with a single path, and use the path name as the UID.
       */
      VMK_ASSERT_ON_COMPILE(VMK_SCSI_PATH_NAME_MAX_LEN <=
                            VMK_SCSI_UID_MAX_ID_LEN);
      vmk_Snprintf(uid->id, VMK_SCSI_UID_MAX_ID_LEN - 1, "mpx.%s",
                   vmk_ScsiGetPathName(path));
   }


   /*
    * Grab the semaphore to be sure that no other thread is 
    * accessing createdDevices.
    */
   vmk_SemaLock(&deviceSema);

   /* 
    * If a device with this UID already exists, use it; 
    * otherwise create one
    */
   for (i = 0; i < maxDevices; i++) {
      exDev = createdDevices[i];

      if (exDev == NULL) {
         deviceSlotAvailable = VMK_TRUE;
      } else if (vmk_ScsiUIDsAreEqual(uid, &exDev->deviceUid)) {
         status = VMK_OK;
         break;
      }
   }

   if (i == maxDevices) {
      if (deviceSlotAvailable) {
         status = ExampleCreateDevice(uid, &exDev);
      } else {
         status = VMK_NO_RESOURCES;
      }
   }

   vmk_SemaUnlock(&deviceSema);

   if (status != VMK_OK) {
      goto error;
   }

   /* Add the path to the device */
   vmk_SpinlockLock(exDev->lock);
   for (i = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
      if (exDev->paths[i] == NULL) {
         exDev->paths[i] = path;
         exPath->exDev = exDev;
         exDev->numPaths++;
         break;
      }
   }
   vmk_SpinlockUnlock(exDev->lock);

   if (i == EXAMPLE_MAX_PATHS_PER_DEVICE) {
      vmk_LogDebug(pluginLog, 2,
                   "Too many paths to logical device '%s'. Could not add "
                   "path '%s'", ExampleDeviceGetName(exDev),
                   vmk_ScsiGetPathName(path));
      status = VMK_NO_RESOURCES;
      goto error;
   }

   vmk_SpinlockLock(exDev->lock);
   if (exDev->flags & EXAMPLE_DEVICE_REGISTERED) {
      VMK_ASSERT(exDev->device);
      exDev->flags |= EXAMPLE_DEVICE_NEEDS_UPDATE;
   }
   vmk_SpinlockUnlock(exDev->lock);
   vmk_HeapFree(miscHeap, uid);
   uid = NULL;

   vmk_LogDebug(pluginLog, 3, "Added path '%s' to logical device '%s'",
                vmk_ScsiGetPathName(path),
                ExampleDeviceGetName(exDev));

   *claimed = VMK_TRUE;

   return VMK_OK;

error:
   vmk_LogDebug(pluginLog, 1, "Failed to claim path '%s'",
                vmk_ScsiGetPathName(path));

   if (uid != NULL) {
      vmk_HeapFree(miscHeap, uid);
   }

   if (exPath != NULL) {
      ExampleDestroyExPath(exPath);
   }

   return status;
}

/*
 ***********************************************************************
 * ExampleDrainBusyCount --                                       */ /**
 *
 * \brief Wait for busyCount to drop to zero.
 *
 ***********************************************************************
 */
static void
ExampleDrainBusyCount(ExampleDevice *exDev)
{
   vmk_SpinlockAssertHeldByWorld(exDev->lock);

   vmk_LogDebug(pluginLog, 3, "Waiting for device '%s' busyCount=%u",
                vmk_ScsiGetDeviceName(exDev->device), exDev->busyCount);

   while (exDev->busyCount > 0) {
      VMK_ReturnStatus status;

      status = vmk_WorldWait((vmk_WorldEventID)&exDev->busyCount, exDev->lock,
                             EXAMPLE_WARNING_MS, "Waiting for busycount to drop");
      if (status == VMK_TIMEOUT) {
         vmk_LogDebug(pluginLog, 0,
                      "Device \"%s\" still waiting busyCount=%u",
                      vmk_ScsiGetDeviceName(exDev->device), exDev->busyCount);
      }
      vmk_SpinlockLock(exDev->lock);
      /* XXX: Need to handle VMK_OK, VMK_DEATH_PENDING and VMK_WAIT_INTERRUPTED */
   }
}

/*
 ***********************************************************************
 * ExampleUnclaimPath --                                          */ /**
 *
 * \brief Unclaim a path.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleUnclaimPath(vmk_ScsiPath *path)
{
   int i;
   vmk_Bool foundPathInDevice;
   ExamplePath *exPath;
   ExampleDevice *exDev;
   VMK_ReturnStatus status = VMK_OK;
   vmk_Bool hasWorkingPath;
   vmk_Bool lastPath;

   exPath = ExampleGetExPath(path);
   exDev = exPath->exDev;

   VMK_ASSERT(vmk_ScsiGetPluginState(plugin) ==
              VMK_SCSI_PLUGIN_STATE_CLAIM_PATHS);
   VMK_ASSERT(exDev->numPaths > 0);

   /*
    * Check if the device has other working path(s) and if
    * the path given is the last path remaining. Consider
    * standby paths as working paths.
    */
   hasWorkingPath = ExampleHasMoreWorkingPaths(path, VMK_FALSE, &lastPath);

   /*
    * Path unclaim disallowed if this is the last working
    * path to the device while there are other paths still.
    * It is expected that the user will unclaim the DEAD
    * or OFF paths first before unclaiming the working one.
    */
   if (!hasWorkingPath && !lastPath) {
      vmk_ScsiPathState state = vmk_ScsiGetPathState(path);

      if (state == VMK_SCSI_PATH_STATE_ON ||
          state == VMK_SCSI_PATH_STATE_STANDBY) {
         vmk_Warning(pluginLog,  "Path \"%s\" could not be unclaimed.",
                     vmk_ScsiGetPathName(path));
         return VMK_BUSY;
      }
   }

   /*
    * If there is at least one more working path, or this path
    * is DEAD or OFF, or this is the last path to the device,
    * go ahead and unclaim it.
    */

   vmk_SpinlockLock(exDev->lock);
   if (exDev->openCount && !hasWorkingPath) {
      vmk_SpinlockUnlock(exDev->lock);
      vmk_LogDebug(pluginLog, 0, "Cannot unclaim path '%s'. " 
                   "Device '%s' is open.", vmk_ScsiGetPathName(path),
                   vmk_ScsiGetDeviceName(exDev->device));
      return VMK_BUSY;
   }

   /* Remove the path from its device */
   foundPathInDevice = VMK_FALSE;
   for (i = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
      if (exDev->paths[i] == path) {
         /* 
          * Update the activePathIndex if unclaiming
          * the active path. Note that if this also happens to be the
          * last path on the device, activePathIndex will remain
          * unchanged.
          */
         if (i == exDev->activePathIndex){
            exDev->activePathIndex = ExampleGetFailoverPathIndex(exDev); 
         }
         exDev->paths[i] = NULL;
         exDev->numPaths--;
         foundPathInDevice = VMK_TRUE;
         break;
      }
   }

   if (foundPathInDevice == VMK_FALSE) {
      vmk_SpinlockUnlock(exDev->lock);

      vmk_LogDebug(pluginLog, 0,
                   "Cannot find path '%s' in device '%s' to unclaim.",
                   vmk_ScsiGetPathName(path),
                   vmk_ScsiGetDeviceName(exDev->device));

      /*
       * This will always fail. The path should always be found in
       * the list of paths for the claiming device.
       */
      VMK_ASSERT(foundPathInDevice == VMK_TRUE);

      return VMK_FAILURE;
   }

   if (exDev->numPaths == 0) {
      /* Prevent any new opens on the device and try to destroy it. */
      exDev->flags |= EXAMPLE_DEVICE_REMOVING;
      ExampleDrainBusyCount(exDev);
      vmk_SpinlockUnlock(exDev->lock);

      status = ExampleDestroyDevice(exDev);
      if (status != VMK_OK) {
         /*
          * The device is still in use, therefore it cannot be destroyed.
          * Put the path back in the device and return an error.
          */
         vmk_SpinlockLock(exDev->lock);
         VMK_ASSERT(exDev->numPaths == 0);
         VMK_ASSERT(exDev->flags & EXAMPLE_DEVICE_REMOVING);

         exDev->flags &= ~EXAMPLE_DEVICE_REMOVING;
         exDev->paths[exDev->activePathIndex] = path;
         exDev->numPaths++;
         vmk_SpinlockUnlock(exDev->lock);

         vmk_LogDebug(pluginLog, 0, "Cannot unclaim last path '%s'. " 
                      "Device '%s' is in use.", vmk_ScsiGetPathName(path),
                      vmk_ScsiGetDeviceName(exDev->device));

         return VMK_BUSY;
      }
   } else {
      vmk_SpinlockUnlock(exDev->lock);
   } 

   vmk_SpinlockLock(exPath->lock);
   while (exPath->refCount > 0) {
      VMK_ReturnStatus status;

      status = vmk_WorldWait((vmk_WorldEventID)&exPath->refCount, exPath->lock,
                             EXAMPLE_WARNING_MS,
                             "Waiting for refCount to drop");
      if (status == VMK_TIMEOUT) {
         vmk_LogDebug(pluginLog, 0,
                      "Waiting for ref count to drain on path \"%s\" "
                      "Current ref count is = %u", vmk_ScsiGetPathName(path),
                      exPath->refCount);
      }
      vmk_SpinlockLock(exPath->lock);
   }
   vmk_SpinlockUnlock(exPath->lock);

   VMK_ASSERT(i < EXAMPLE_MAX_PATHS_PER_DEVICE);
 
   /* Unclaim and destroy the path */
   status = ExampleDestroyExPath(exPath);

   return status;
}

/*
 ***********************************************************************
 * ExamplePathUnrecHWError --                                     */ /**
 *
 * \brief Check if it hit an unrecoverable h/w error along the path
 *
 ***********************************************************************
 */
static vmk_Bool
ExamplePathUnrecHWError(vmk_Bool senseValid,
                        vmk_uint8 sk,
                        vmk_uint8 asc,
                        vmk_uint8 ascq)
{
   if (senseValid && sk == VMK_SCSI_SENSE_KEY_HARDWARE_ERROR) {
      if ((asc == VMK_SCSI_ASC_LOGICAL_UNIT_FAILED_SELF_CONFIG && ascq == 0) ||
          (asc == VMK_SCSI_ASC_LOGICAL_UNIT_ERROR &&
           (ascq == VMK_SCSI_ASCQ_LOGICAL_UNIT_FAILED_SELF_TEST ||
            ascq == VMK_SCSI_ASCQ_LOGICAL_UNIT_FAILURE))) {
         return VMK_TRUE;
      }
   }

   return VMK_FALSE;
}

/*
 ***********************************************************************
 * ExampleLUNotSupported --                                       */ /**
 *
 * \brief Check if the LUN was unmapped from the array
 *
 ***********************************************************************
 */
static vmk_Bool
ExampleLUNotSupported(vmk_Bool senseValid,
                      vmk_uint8 asc,
                      vmk_uint8 ascq)
{
   if (senseValid &&
       ((asc == VMK_SCSI_ASC_LU_NOT_SUPPORTED &&
       ascq == 0) ||
       (asc == VMK_SCSI_ASC_LU_NOT_CONFIGURED &&
       ascq == 0))) {
      return VMK_TRUE;
   }

   return VMK_FALSE;
}

/*
 ***********************************************************************
 * ExampleVerifyPathUID --                                        */ /**
 *
 * \brief Check if the device's UID has been changed or not
 *
 ***********************************************************************
 */
static vmk_Bool
ExampleVerifyPathUID(ExamplePath *exPath)
{
   vmk_ScsiUid uid;
   VMK_ReturnStatus uidStatus;

   uidStatus = vmk_ScsiVerifyPathUID(exPath->path, &uid);
   if (uidStatus == VMK_UID_CHANGED || uidStatus == VMK_PERM_DEV_LOSS) {
      if (uidStatus == VMK_UID_CHANGED) {
         vmk_Log(pluginLog, "The physical media represented by device "
                 "%s (path %s) has changed. If this is a data LUN, "
                 "this is a critical error. Detected UID %s",
                 vmk_ScsiGetDeviceName(exPath->exDev->device),
                 vmk_ScsiGetPathName(exPath->path),
                 uid.id);
      } else {
          vmk_Log(pluginLog, "Scsi target reports device %s is no longer "
                  "connected (path %s)",
                  vmk_ScsiGetDeviceName(exPath->exDev->device),
                  vmk_ScsiGetPathName(exPath->path));
      }

      return VMK_TRUE;
   }

   return VMK_FALSE;
}

/*
 ***********************************************************************
 * ExampleUpdatePathState --                                      */ /**
 *
 * \brief Update path state and trigger device probe if path is in PDL
 * 
 ***********************************************************************
 */
static void
ExampleUpdatePathState(vmk_ScsiPath *path,
                       vmk_ScsiCommand *cmd)
{
   vmk_ScsiPathState state = vmk_ScsiGetPathState(path);
   ExamplePath *exPath = ExampleGetExPath(path);
   vmk_Bool isPDL = VMK_FALSE, senseValid = VMK_FALSE;
   vmk_uint8 senseKey = 0, asc = 0, ascq = 0;
   vmk_ScsiDeviceState devState;
   ExampleDevice *exDev;

   /* Don't update the path's state if the path has been turned off */
   if (state == VMK_SCSI_PATH_STATE_OFF) {
      return;
   }

   /* Path probe successful - turn the path on */
   if (vmk_ScsiCmdIsSuccessful(cmd)) {
      if (state != VMK_SCSI_PATH_STATE_ON) {
         vmk_Log(pluginLog, "Path \"%s\" is changing to on from %s.",
                 vmk_ScsiGetPathName(path), vmk_ScsiPathStateToString(state));
      }
      vmk_ScsiSetPathState(plugin, path, VMK_SCSI_PATH_STATE_ON);
      goto updatePDL;
   }

   /* Set path to standby if sense data indicates not ready */
   if (vmk_ScsiCmdStatusIsCheck(cmd->status)) {
      vmk_ScsiSenseDataSimple sense;
      VMK_ReturnStatus status;

      if ((status = vmk_ScsiCmdGetSenseData(cmd, (vmk_ScsiSenseData *)&sense,
                                            sizeof(sense))) != VMK_OK) {
         vmk_Log(pluginLog, "Get sense data failed, status %s",
                 vmk_StatusToString(status));
      } else if (vmk_ScsiExtractSenseData((vmk_ScsiSenseData *) &sense,
                                         &senseKey, &asc, &ascq) == VMK_FALSE) {
         vmk_Log(pluginLog, "Extract sense data failed");
      } else {
         senseValid = VMK_TRUE;
      }
      if (senseValid &&
          senseKey == VMK_SCSI_SENSE_KEY_NOT_READY &&
          asc == VMK_SCSI_ASC_LU_NOT_READY &&
          ascq == VMK_SCSI_ASC_LU_NOT_READY_ASCQ_MANUAL_INTERVENTION_REQUIRED) {
         if (state != VMK_SCSI_PATH_STATE_STANDBY) {
            vmk_Log(pluginLog, "Path \"%s\" is changing to standby from %s.",
                    vmk_ScsiGetPathName(path),
                    vmk_ScsiPathStateToString(state));
         }
         vmk_ScsiSetPathState(plugin, path, VMK_SCSI_PATH_STATE_STANDBY);
         goto updatePDL;
      }

      /**
       *  TUR failed. We need to check if path is in PDL (Permanent Device
       *  Loss) state.
       *  Path is considered as in PDL:
       *  -- if the path is removed from the array, or
       *  -- if hit an unrecoverable H/W error on the path, or
       *  -- if UID has changed, or
       *  -- for any vendor specific reasons.
       */
      if (ExampleLUNotSupported(senseValid, asc, ascq)  ||
          ExamplePathUnrecHWError(senseValid, senseKey, asc, ascq)) {
         isPDL = VMK_TRUE;
      } else if((state == VMK_SCSI_PATH_STATE_STANDBY ||
                 state == VMK_SCSI_PATH_STATE_ON) &&
                ExampleVerifyPathUID(exPath)) {
         isPDL = VMK_TRUE;
      }
   }

   vmk_LogDebug(pluginLog, 3, "TUR to path \"%s\" failed "
                "(D:%d/H:%d) %s 0x%2x 0x%2x 0x%2x.",
                vmk_ScsiGetPathName(path), cmd->status.device,
                cmd->status.host, senseValid ? "valid sense" : "no sense",
                senseKey, asc, ascq);
   /* Leave the path state dead otherwise */
   if (state != VMK_SCSI_PATH_STATE_DEAD) {
      vmk_Warning(pluginLog, "Path \"%s\" is changing to dead from %s.",
                  vmk_ScsiGetPathName(path), vmk_ScsiPathStateToString(state));
   }

   vmk_ScsiSetPathState(plugin, path, VMK_SCSI_PATH_STATE_DEAD);

updatePDL:
   /*
    * Trigger a device probe when path gets in/out of PDL.
    */
   exDev = ExampleGetExPath(path)->exDev;
   if (isPDL != exPath->isPDL) {
      vmk_Bool isRegistered;

      vmk_SpinlockLock(exPath->lock);
      exPath->isPDL = isPDL;
      vmk_SpinlockUnlock(exPath->lock);
      vmk_Log(pluginLog, "Path \"%s\" is getting %s PDL state.",
              vmk_ScsiGetPathName(path),
              isPDL ? "into" : "out of");
      vmk_SpinlockLock(exDev->lock);
      isRegistered = (exDev->flags & EXAMPLE_DEVICE_REGISTERED);
      vmk_SpinlockUnlock(exDev->lock);

      /*
       * PSA serializes ->pathUnclaim and ->pathProbe, so if device
       * was registered successfully in ->pathClaimEnd, it cannot be
       * destroyed while here.
       */
      if (isRegistered) {
         VMK_ASSERT(exDev->device);
         ExampleDeviceProbe(exDev->device);
      }
      return;
   }
   vmk_SpinlockLock(exDev->lock);
   if (exDev->flags & EXAMPLE_DEVICE_REGISTERED) {
      if (ExampleDeviceUpdateState(exDev->device, &devState)) {
            vmk_ScsiSetDeviceState(exDev->device, devState, 
                                   VMK_SCSI_DEVICE_INFO_NONE);
      }
   }  
   vmk_SpinlockUnlock(exDev->lock);
}

/*
 ***********************************************************************
 * ExampleProbePath --                                            */ /**
 *
 * \brief Probe the current state of a path
 * 
 * Issue a TUR on the specified path to probe and update its
 * state.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleProbePath(vmk_ScsiPath *path)
{
   VMK_ReturnStatus status;
   vmk_ScsiCommand *cmd;

   /*
    * The path probe entry point and path unclaim entry point are 
    * serialized by PSA. It is not necessary to contend with 
    * ExampleProbePath racing with ExampleUnclaimPath.
    */
   cmd = vmk_ScsiCreateTURCommand();
   if (cmd == NULL) {
      return VMK_NO_MEMORY;
   }
   cmd->absTimeoutMs = VMK_ABS_TIMEOUT_MS(EXAMPLE_CMD_TIMEOUT_MS);

   status = vmk_ScsiIssueSyncPathCommandWithRetries(path, cmd, NULL, 0);
   if (status == VMK_OK) {
      ExampleUpdatePathState(path, cmd);
   } else {
      vmk_Warning(pluginLog, "Could not probe path \"%s\": %s!",
                  vmk_ScsiGetPathName(path), vmk_StatusToString(status));
   }

   vmk_ScsiDestroyCommand(cmd);

   return status;
}

/*
 ***********************************************************************
 * ExampleDeviceProbe --                                          */ /**
 *
 * \brief Probe the current state of the device's paths.
 * 
 * Issue a TUR on the device's paths to probe and update their
 * state.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceProbe(vmk_ScsiDevice *scsiDev)
{
   VMK_ReturnStatus currStatus, aggStatus = VMK_OK;
   ExampleDevice *exDev;
   vmk_Bool updateState;
   vmk_ScsiDeviceState devState;
   vmk_uint32 i;

   currStatus = ExampleDeviceOpen(scsiDev);
   if (currStatus != VMK_OK) {
      return currStatus;
   }
      
   exDev = ExampleGetExDev(scsiDev);

   vmk_SpinlockLock(exDev->lock);
   
   if (exDev->inProbe == VMK_TRUE) {
      vmk_SpinlockUnlock(exDev->lock);
      vmk_LogDebug(pluginLog, 0, "Device '%s' probe already in process.",
                   vmk_ScsiGetDeviceName(scsiDev));
      ExampleDeviceClose(scsiDev);
      return VMK_BUSY;
   }
   exDev->inProbe = VMK_TRUE;

   for (i = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
      ExamplePath *exPath; 
      vmk_ScsiPath *scsiPath = exDev->paths[i];

      if (scsiPath == NULL) {
         continue;
      }
    
      exPath = ExampleGetExPath(scsiPath); 

      /* Take a ref on the path to prevent it from being unclaimed. */
      ExamplePathIncRefCount(exPath, VMK_FALSE);
 
      vmk_SpinlockUnlock(exDev->lock);
      currStatus = ExampleProbePath(scsiPath);

      ExamplePathDecRefCount(exPath, VMK_FALSE);
      vmk_SpinlockLock(exDev->lock);

      if (currStatus != VMK_OK){
         aggStatus = currStatus;
      }
   }

   updateState = ExampleDeviceUpdateState(scsiDev, &devState);
   exDev->inProbe = VMK_FALSE;
   vmk_SpinlockUnlock(exDev->lock);

   if (updateState) {
         vmk_ScsiSetDeviceState(scsiDev, devState, VMK_SCSI_DEVICE_INFO_NONE);
   }

   ExampleDeviceClose(scsiDev);

   return aggStatus;
}

/*
 ***********************************************************************
 * ExamplePathSetState --                                         */ /**
 *
 * \brief Set path state to on or off
 *        Set path state, usually admin is telling the plugin to turn
 *        a path on or off.
 * 
 ***********************************************************************
 */
static VMK_ReturnStatus
ExamplePathSetState(vmk_ScsiPath *path,
                    vmk_ScsiPathState newState)
{
   vmk_ScsiPathState state;
   vmk_Bool hasWorkingPath;
   VMK_ReturnStatus status;

   /* New path state must be either ON or OFF */
   if (newState != VMK_SCSI_PATH_STATE_ON &&
       newState != VMK_SCSI_PATH_STATE_OFF) {
      VMK_ASSERT(0);
      return VMK_BAD_PARAM;
   }

   /* Do nothing if there is no state change */
   state = vmk_ScsiGetPathState(path);
   if (state == newState) {
      return VMK_OK;
   }

   if (newState == VMK_SCSI_PATH_STATE_ON) {
      /*
       * If the path is OFF, mark it as DEAD so that the state
       * will be evaluated before the path is used. If the path 
       * is in DEAD or STANDBY state, do not change path state
       * since it will evaluated anyways.
       */
      if (state == VMK_SCSI_PATH_STATE_OFF) {
         vmk_ScsiSetPathState(plugin, path, VMK_SCSI_PATH_STATE_DEAD);
      }

      status = ExampleProbePath(path);
      if (status != VMK_OK) {
         vmk_Warning(pluginLog, "Path \"%s\" probe failed - %s.",
                     vmk_ScsiGetPathName(path), vmk_StatusToString(status));
      }

      return VMK_OK;
   }

   /*
    * Check if there are any working paths to the device except
    * this one. Consider standby path as working path too.
    */
   hasWorkingPath = ExampleHasMoreWorkingPaths(path, VMK_FALSE, NULL);
   if (!hasWorkingPath) {
      /*
       * There is no other working path, so the user cannot
       * disable this one. Even if the state of this path
       * is dead, i.e., APD already entered, do not
       * allow setting it to off in order to avoid worsening the 
       * condition.
       */
      vmk_Warning(pluginLog, "Path \"%s\" could not be disabled.",
                  vmk_ScsiGetPathName(path));
      return VMK_BUSY;
   }

   /* Set vmk path state here before calling path probe */
   vmk_ScsiSetPathState(plugin, path, VMK_SCSI_PATH_STATE_OFF);

   status = ExampleProbePath(path);
   if (status != VMK_OK) {
       vmk_Warning(pluginLog, "Path \"%s\" probe failed - %s.",
                   vmk_ScsiGetPathName(path), vmk_StatusToString(status));
   }

   return VMK_OK;
}

/*
 ***********************************************************************
 * ExamplePathGetDeviceName --                                    */ /**
 *
 * \brief return device name associated with the path  
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExamplePathGetDeviceName(vmk_ScsiPath *vmkPath,
                         char deviceName[VMK_SCSI_UID_MAX_ID_LEN])
{
   ExamplePath *exPath = ExampleGetExPath(vmkPath);
   ExampleDevice *exDev = exPath->exDev;

   if (exDev == NULL) {
      return VMK_NOT_FOUND;
   }

   vmk_SpinlockLock(exDev->lock);
   if (exDev->flags & EXAMPLE_DEVICE_REGISTERED) {
      /*
       * Device is registered. Do not unregister it until the last path goes
       * away. The last path won't go away while in this code since the PSA
       * serializes unclaimPath and pathGetDeviceName.
       */
      VMK_ASSERT(exDev->device);
      vmk_SpinlockUnlock(exDev->lock);
      vmk_Strlcpy(deviceName, vmk_ScsiGetDeviceName(exDev->device), 
                  VMK_SCSI_UID_MAX_ID_LEN);
      return VMK_OK;
   }
   vmk_SpinlockUnlock(exDev->lock);

   return VMK_NOT_FOUND;
}

/************************************************************
 *
 * Logical device
 *
 ************************************************************/

/*
 ***********************************************************************
 * ExampleDestroyDevice --                                        */ /**
 *
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDestroyDevice(ExampleDevice *exDev)
{
   VMK_ASSERT(exDev->numPaths == 0);
   VMK_ASSERT(vmk_ListIsEmpty(&exDev->resvSensitiveCmdQueue));

   if (exDev->flags & EXAMPLE_DEVICE_REGISTERED) {
      VMK_ReturnStatus status = vmk_ScsiUnregisterDevice(exDev->device);

      if (status != VMK_OK) {
         vmk_Warning(pluginLog, "Failed to unregister device '%s': %s",
                     vmk_ScsiGetDeviceName(exDev->device),
                     vmk_StatusToString(status));
         return status;
      }
   }
   exDev->flags &= ~EXAMPLE_DEVICE_REGISTERED;

   vmk_SemaLock(&deviceSema);

   VMK_ASSERT(createdDevices[exDev->index] == exDev);
   createdDevices[exDev->index] = NULL;

   vmk_SemaUnlock(&deviceSema);

   if (exDev->device){
      vmk_ScsiFreeDevice(exDev->device);
   }

   vmk_SpinlockDestroy(exDev->lock);

   vmk_HeapFree(miscHeap, exDev);

   return VMK_OK;
} 

/*
 ***********************************************************************
 * ExampleGetLogicalDevicePathNames --                            */ /**
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleGetLogicalDevicePathNames(ExampleDevice *exDev,
                                 vmk_uint32 *numPathNames,
                                 char ***pathNames)
{
   VMK_ReturnStatus status = VMK_OK;
   vmk_uint32 i, n;
   const char *pathName;

   vmk_SpinlockLock(exDev->lock);

   /*
    * The device might not have been opened at this point.
    * In order to avoid a race with ExampleUnclaimPath, make sure 
    * the device is not being removed.
    */

   if (exDev->flags & EXAMPLE_DEVICE_REMOVING){
      vmk_SpinlockUnlock(exDev->lock);
      return VMK_BUSY;
   }
   
   (*numPathNames) = exDev->numPaths;
   (*pathNames) = vmk_HeapAlloc(vmk_ModuleGetHeapID(plugin->moduleID),
                                (*numPathNames) * sizeof(char *));
   VMK_ASSERT(*pathNames);

   for (i = 0, n = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
      if (exDev->paths[i] == NULL) {
         continue;
      }

      /*
       * In general, do not have the plugin call vmk_Scsi APIs with
       * spinlocks held. In this case it is safe, however.
       */

      pathName = vmk_ScsiGetPathName(exDev->paths[i]);
      if (pathName == NULL) {
         continue;
      }

      (*pathNames)[n] = vmk_HeapAlloc(vmk_ModuleGetHeapID(plugin->moduleID), 
                                      VMK_SCSI_PATH_NAME_MAX_LEN);

      if (NULL != (*pathNames)[n]) {
         vmk_Memset((*pathNames)[n], 0, VMK_SCSI_PATH_NAME_MAX_LEN);
         vmk_Strncpy((*pathNames)[n], pathName, VMK_SCSI_PATH_NAME_MAX_LEN);
         n++;
      } else {
         vmk_uint32 j;

         vmk_SpinlockUnlock(exDev->lock);

         for (j = 0; j < n; j++) {
            vmk_HeapFree(vmk_ModuleGetHeapID(plugin->moduleID),
                         (*pathNames)[j]);
         }
         vmk_HeapFree(vmk_ModuleGetHeapID(plugin->moduleID), *pathNames);
         return VMK_NO_MEMORY;
      }
   }
   
   VMK_ASSERT(n == exDev->numPaths);
   vmk_SpinlockUnlock(exDev->lock);

   return status;
}

/*
 ***********************************************************************
 * ExampleIsPseudoDevice --                                       */ /**
 *
 * \brief Tell whether a device is a pseudo device.
 *
 * A pseudo device is a special device like array control device
 * 
 ***********************************************************************
 */
static vmk_Bool
ExampleIsPseudoDevice(ExampleDevice *exDev)
{
   return VMK_FALSE;
}

/*
 ***********************************************************************
 * ExampleDeviceInquiry --                                        */ /**
 *
 * \brief Return inquiry data for device
 * 
 * \note This is one of the plugin's entry points
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceInquiry(vmk_ScsiDevice *device,
                     vmk_ScsiInqType inqPage,
                     vmk_uint8 *inquiryData,
                     vmk_ByteCountSmall inquirySize)
{
   ExampleDevice *exDev = ExampleGetExDev(device);
   vmk_ScsiPath *path;
   VMK_ReturnStatus status;
   ExamplePath *exPath;

   vmk_SpinlockLock(exDev->lock);
   VMK_ASSERT(exDev->openCount > 0);
   path = exDev->paths[exDev->activePathIndex];
   exPath = ExampleGetExPath(path);
   ExamplePathIncRefCount(exPath, VMK_FALSE);
   vmk_SpinlockUnlock(exDev->lock);

   status = vmk_ScsiGetPathInquiry(path, inquiryData, inquirySize, inqPage, NULL);
   ExamplePathDecRefCount(exPath, VMK_FALSE);
   return status;
}

/*
 ***********************************************************************
 * ExampleDeviceDumpCmd --                                        */ /**
 *
 * \brief Issue a command during coredump
 * 
 * \note This is one of the plugin's entry points
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceDumpCmd(vmk_ScsiDevice *device,
                     vmk_ScsiCommand *dumpCmd)
{
   ExampleDevice *exDev = ExampleGetExDev(device);

   return ExampleIssueSyncDumpCommand(exDev, dumpCmd);
}

/*
 ***********************************************************************
 * ExampleDeviceGetPathNames --                                   */ /**
 *
 * \brief Return path names associated with device
 * 
 * \note This is one of the plugin's entry points
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceGetPathNames(vmk_ScsiDevice *device,
                          vmk_HeapID *heapID,
                          vmk_uint32 *numPathNames,
                          char ***pathNames)
{
   ExampleDevice *exDev = ExampleGetExDev(device);

   *heapID = vmk_ModuleGetHeapID(plugin->moduleID);
   return ExampleGetLogicalDevicePathNames(exDev, numPathNames, pathNames);
}

/*
 ***********************************************************************
 * ExampleDeviceGetBoolAttr --                                    */ /**
 *
 * \brief Return device attribute.
 *
 * \param[in]  device   SCSI device to be probed.
 * \param[in]  attr     Device attribute to probe for.
 * \param[out] boolAttr Flag indicating if device supports the attribute.
 *                      Valid only if return status is VMK_OK.
 *
 * \retval VMK_OK         Device attribute status obtained successfully.
 * \retval VMK_BAD_PARAM  Invalid input device or attribute.
 * \retval Error          Could not get device attribute.
 *
 * \note  This is one of the plugin's entry points.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceGetBoolAttr(vmk_ScsiDevice *device,
                         vmk_ScsiDeviceBoolAttribute attr,
                         vmk_Bool *boolAttr)
{
   VMK_ReturnStatus status = VMK_OK;

   VMK_ASSERT(boolAttr);
   if (device == NULL) {
      return VMK_BAD_PARAM;
   }

   *boolAttr = VMK_FALSE;
   switch(attr) {
   case VMK_SCSI_DEVICE_BOOL_ATTR_PSEUDO:
      status = ExampleDeviceIsPseudo(device, boolAttr);
      break;
   case VMK_SCSI_DEVICE_BOOL_ATTR_SSD:
      status = vmk_ScsiDefaultDeviceGetBoolAttr(device,
                                                VMK_SCSI_DEVICE_BOOL_ATTR_SSD,
                                                boolAttr);
      break;
   case VMK_SCSI_DEVICE_BOOL_ATTR_LOCAL:
      status = vmk_ScsiDefaultDeviceGetBoolAttr(device,
                                                VMK_SCSI_DEVICE_BOOL_ATTR_LOCAL,
                                                boolAttr);
      break;
   default:
      status = VMK_BAD_PARAM;
      break;
   }

   return status;
}

/*
 ***********************************************************************
 * ExampleDeviceIsPseudo --                                       */ /**
 *
 * \brief Identify the device as a pseudo device or not
 * 
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleDeviceIsPseudo(vmk_ScsiDevice *device,
                      vmk_Bool *isPseudo)
{
   ExampleDevice *exDev = ExampleGetExDev(device);

   *isPseudo = ExampleIsPseudoDevice(exDev);
   return VMK_OK;
}

/*
 ***********************************************************************
 * ExampleDeviceUpdateState --                                    */ /**
 *
 * \brief   Report to PSA if device state is changed.
 *
 * Check if device state is changed or not since last time.
 * Report the new state to PSA if it's changed. Trigger a
 * device probe in case any path is in PDL.
 *
 ***********************************************************************
 */
static vmk_Bool
ExampleDeviceUpdateState(vmk_ScsiDevice *device, vmk_ScsiDeviceState *state)
{
   ExampleDevice *exDev;
   vmk_uint32 i;
   vmk_ScsiDeviceState curDevState;
   vmk_ScsiPathState curPathState;
   vmk_Bool isPDL = VMK_TRUE;
   vmk_Bool isAPD = VMK_TRUE;

   exDev = ExampleGetExDev(device);

   curDevState = vmk_ScsiGetDeviceState(device);

   for (i = 0; i < EXAMPLE_MAX_PATHS_PER_DEVICE; i++) {
      ExamplePath *exPath;
      vmk_ScsiPath *scsiPath = exDev->paths[i];

      if (scsiPath == NULL) {
         continue;
      }

      exPath = ExampleGetExPath(scsiPath);
      curPathState = vmk_ScsiGetPathState(scsiPath);

      if (curPathState != VMK_SCSI_PATH_STATE_DEAD) {
         isAPD = VMK_FALSE;
      }

      if ((curDevState == VMK_SCSI_DEVICE_STATE_PERM_LOSS ||
           (curDevState == VMK_SCSI_DEVICE_STATE_APD)) &&
          curPathState == VMK_SCSI_PATH_STATE_ON) {
         /* Device is not in PDL/APD if there is any available path */
         vmk_Log(pluginLog, "Device \"%s\" is changing to %s from %s.",
                 vmk_ScsiGetDeviceName(device),
		 vmk_ScsiDeviceStateToString(VMK_SCSI_DEVICE_STATE_ON),
		 vmk_ScsiDeviceStateToString(curDevState));
	 *state = VMK_SCSI_DEVICE_STATE_ON;
         return VMK_TRUE;
      }

      vmk_SpinlockLock(exPath->lock);
      if (exPath->isPDL == VMK_FALSE) {
         isPDL = VMK_FALSE;
      }
      vmk_SpinlockUnlock(exPath->lock);
   }

   if (isPDL && curDevState != VMK_SCSI_DEVICE_STATE_PERM_LOSS) {
      /* All paths are in PDL; device is in PDL. Report to PSA */
      vmk_Log(pluginLog, "Device \"%s\" is changing to %s from %s.",
              vmk_ScsiGetDeviceName(device),
              vmk_ScsiDeviceStateToString(VMK_SCSI_DEVICE_STATE_PERM_LOSS),
              vmk_ScsiDeviceStateToString(curDevState));
      *state = VMK_SCSI_DEVICE_STATE_PERM_LOSS;
      return VMK_TRUE;
   }

   if (!isPDL && isAPD && curDevState != VMK_SCSI_DEVICE_STATE_APD) {
      vmk_Log(pluginLog, "Device \"%s\" is changing to %s from %s.",
              vmk_ScsiGetDeviceName(device),
              vmk_ScsiDeviceStateToString(VMK_SCSI_DEVICE_STATE_APD),
              vmk_ScsiDeviceStateToString(curDevState));
      *state = VMK_SCSI_DEVICE_STATE_APD;
      return VMK_TRUE;
   }
   return VMK_FALSE;
}

/*
 ***********************************************************************
 * ExampleDeviceOpen --                                           */ /**
 *
 * \brief Open a logical device
 * 
 * \note This is one of the plugin's entry points
 *
 ***********************************************************************
 */
VMK_ReturnStatus
ExampleDeviceOpen(vmk_ScsiDevice *device)
{
   ExampleDevice *exDev = ExampleGetExDev(device);

   if (exDev == NULL) {
      return VMK_NOT_FOUND;
   }

   vmk_SpinlockLock(exDev->lock);
   if (exDev->flags & EXAMPLE_DEVICE_REMOVING) {
      vmk_SpinlockUnlock(exDev->lock);
      return VMK_BUSY;
   }

   exDev->openCount++;
   vmk_SpinlockUnlock(exDev->lock);

   return VMK_OK;
}

/*
 ***********************************************************************
 * ExampleDeviceClose --                                          */ /**
 *
 * \brief Close a logical device
 * 
 * \note This is one of the plugin's entry points
 *
 ***********************************************************************
 */
VMK_ReturnStatus
ExampleDeviceClose(vmk_ScsiDevice *device)
{
   ExampleDevice *exDev = ExampleGetExDev(device);

   vmk_SpinlockLock(exDev->lock);
   VMK_ASSERT(exDev->openCount > 0);
   exDev->openCount--;
   vmk_SpinlockUnlock(exDev->lock);
   return VMK_OK;
}

/************************************************************
 *
 * Plugin
 *
 ************************************************************/

/*
 ***********************************************************************
 * ExampleStartWorld                                              */ /**
 *
 * \brief Create a world and wait to see if it starts successfully
 *
 * The world indicates successful start by swithching to the
 * EXAMPLE_WORLD_RUNNING state, and failure by switching to the
 * EXAMPLE_WORLD_STOPPED state.
 *
 * \retval VMK_OK The world started successfully.
 * \retval VMK_FAILURE The world failed to start.
 *
 ***********************************************************************
 */
VMK_ReturnStatus
ExampleStartWorld(const char *worldName,
                  vmk_WorldStartFunc worldStartFunction,
                  void *worldData,
                  ExampleWorldState *worldState)
{
   VMK_ReturnStatus status;
   vmk_WorldProps worldProps;

   /* Create the world */
   *worldState = EXAMPLE_WORLD_STARTING;
   worldProps.name = worldName;
   worldProps.moduleID = plugin->moduleID;
   worldProps.startFunction = worldStartFunction;
   worldProps.data = worldData;
   worldProps.schedClass = VMK_WORLD_SCHED_CLASS_DEFAULT;
   status = vmk_WorldCreate(&worldProps, NULL);

   /* Wait for it to switch to the running or stopped state */
   if (status == VMK_OK) {
      vmk_SpinlockLock(pluginLock);
      while (*worldState != EXAMPLE_WORLD_RUNNING &&
             *worldState != EXAMPLE_WORLD_STOPPED &&
             status != VMK_DEATH_PENDING) {
         status = vmk_WorldWait((vmk_WorldEventID) worldState, pluginLock,
                                VMK_TIMEOUT_UNLIMITED_MS,
                                "Waiting for world to stop running");
         vmk_SpinlockLock(pluginLock);
      }
      if (*worldState != EXAMPLE_WORLD_RUNNING) {
         status = VMK_FAILURE;
      }
      vmk_SpinlockUnlock(pluginLock);
   }

   if (status == VMK_OK) {
      vmk_LogDebug(pluginLog, 0, "World '%s' started successfully.", worldName);
   } else {
      vmk_Warning(pluginLog, "Failed to start world '%s': %s",
                  worldName, vmk_StatusToString(status));
   }

   return status;
}

/*
 ***********************************************************************
 * ExampleStopWorld                                               */ /**
 *
 * Signal a world to stop and wait for it to exit
 *
 ***********************************************************************
 */
void ExampleStopWorld(ExampleWorldState *state)
{
   vmk_SpinlockLock(pluginLock);

   if (*state != EXAMPLE_WORLD_STOPPED) {
      /* Signal the world to exit */
      *state = EXAMPLE_WORLD_STOPPING;
      vmk_WorldWakeup((vmk_WorldEventID)state);

      /* And wait until it does */
      while (*state != EXAMPLE_WORLD_STOPPED) {
         vmk_WorldWait((vmk_WorldEventID)state, pluginLock,
                       VMK_TIMEOUT_UNLIMITED_MS,
                       "Waiting for world to stop running");
         vmk_SpinlockLock(pluginLock);
      }
   }

   vmk_SpinlockUnlock(pluginLock);
}

/*
 ***********************************************************************
 * ExampleStartPlugin --                                          */ /**
 *
 * \return VMK_OK if the plugin was successfully started. Error code
 *     otherwise.
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExampleStartPlugin(void)
{ 
   VMK_ReturnStatus status;

   vmk_LogDebug(pluginLog, 0, "Starting plugin '%s'", EXAMPLE_NAME);

   status = ExampleStartWorld(EXAMPLE_NAME "-retry", Example_RetryWorld,
                              NULL, &retryWorldState);
   if (status != VMK_OK) {
      return status;
   }

   vmk_ScsiSetPluginState(plugin, VMK_SCSI_PLUGIN_STATE_ENABLED);

   vmk_LogDebug(pluginLog, 0, "Plugin '%s' started successfully", EXAMPLE_NAME);

   return VMK_OK; 
}

/*
 ***********************************************************************
 * ExamplePluginIoctl --                                          */ /**
 *
 * \brief Processes ioctls issued by the framework to manage the plugin. 
 *
 * \note This is one of the plugin's entry points
 *
 * \return VMK_OK on success, error code otherwise
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExamplePluginIoctl(vmk_ScsiPlugin *_plugin,
                   vmk_ScsiPluginIoctl cmd,
                   vmk_ScsiPluginIoctlData *data)
{
   VMK_ReturnStatus status = VMK_OK;

   VMK_ASSERT(_plugin == plugin);
   if (_plugin != plugin) {
      return VMK_BAD_PARAM;
   }

   switch(cmd) {
   default:
      vmk_Warning(pluginLog, "Unknown plugin command 0x%x", cmd);
      VMK_ASSERT(VMK_FALSE);
      status = VMK_NOT_IMPLEMENTED;
      break;
   }

   return status;
}

/*
 ***********************************************************************
 * ExamplePluginStateLogger --                                    */ /**
 *
 * \brief Log internal state on request.
 * 
 * \note This is one of the plugin's entry points.
 *
 * \return VMK_OK on success, error code otherwise
 *
 ***********************************************************************
 */
static VMK_ReturnStatus
ExamplePluginStateLogger(struct vmk_ScsiPlugin *plugin,
                         const vmk_uint8 *logParam,
                         vmk_ScsiPluginStatelogFlag logFlags)
{
   int deviceCount;
   int i, j;
   ExampleDevice *exDev;
   ExamplePath *exPath;

   if (pluginLog == NULL) {
      vmk_WarningMessage("ExamplePluginStateLogger invoked without "
                         "valid log structure.");
      return VMK_FAILURE;
   }

   if (logFlags & VMK_SCSI_PLUGIN_STATELOG_GLOBALSTATE) {
      if (logParam) {
         /* The log parameter could be used to select state to be logged: */
         vmk_Log(pluginLog, "Logging '%s':", logParam);
      }
      
      /* Dump the complete plugin state */
      vmk_Log(pluginLog, "Example module ID: %lx.",
              vmk_ModuleGetDebugID(moduleID));
      switch(retryWorldState) {
      case EXAMPLE_WORLD_STARTING:
         vmk_Log(pluginLog, "Example plugin retry world is starting.");
         break;

      case EXAMPLE_WORLD_RUNNING:
         vmk_Log(pluginLog, "Example plugin retry world is running.");
         break;

      case EXAMPLE_WORLD_STOPPING:
         vmk_Log(pluginLog, "Example plugin retry world is stopping.");
         break;

      case EXAMPLE_WORLD_STOPPED:
         vmk_Log(pluginLog, "Example plugin retry world is stopped.");
         break;
      } 
      
      /* Dump the state of every device. */
      if ((logFlags & VMK_SCSI_PLUGIN_STATE_LOG_CRASHDUMP) == 0) {
         vmk_SemaLock(&deviceSema);
      }
      deviceCount = 0;
      for (i = 0; i < maxDevices; i++) {
         if (createdDevices[i]) {
            ++deviceCount;
         }
      }
      if (deviceCount == 0) {
         vmk_Log(pluginLog, "Example plugin is not managing any devices.");
      } else {
         vmk_Log(pluginLog, "Example plugin is managing %d devices:",
                 deviceCount);
      }
      
      for (i = 0; i < maxDevices; i++) {
         vmk_uint32 flags, openCount, activeCmdCount;
         int numPaths;
         struct {
            char name[VMK_SCSI_PATH_NAME_MAX_LEN];
            vmk_ScsiPathState state;
            vmk_uint32 flags;
            vmk_uint32 activeCmdCount;
         } *pathInfo;
         exDev = createdDevices[i];
         if (exDev == NULL) {
            continue;
         }
         
         if ((logFlags & VMK_SCSI_PLUGIN_STATE_LOG_CRASHDUMP) == 0) {
            pathInfo = vmk_HeapAlloc(miscHeap, EXAMPLE_MAX_PATHS_PER_DEVICE *
                                     sizeof(*pathInfo));
            numPaths = 0;
            vmk_SpinlockLock(exDev->lock);
            flags = exDev->flags;
            openCount = exDev->openCount;
            activeCmdCount = exDev->activeCmdCount;
         } else {
            pathInfo = NULL;
            flags = openCount = activeCmdCount = numPaths = 0;
            vmk_Log(pluginLog,
                    "%d: %s (flags=0x%08x, openCount=%d, activeCmdCount=%d):",
                    exDev->index, vmk_ScsiGetDeviceName(exDev->device),
                    exDev->flags, exDev->openCount, exDev->activeCmdCount);
            vmk_Log(pluginLog, "\tNo. of paths: %u", exDev->numPaths);
         }
         for (j = 0; j < EXAMPLE_MAX_PATHS_PER_DEVICE; j++) {
            if (exDev->paths[j]) {
               vmk_ScsiPathState state;

               exPath = ExampleGetExPath(exDev->paths[j]);
               state = vmk_ScsiGetPathState(exPath->path);
               if ((logFlags & VMK_SCSI_PLUGIN_STATE_LOG_CRASHDUMP) == 0) {
                  if (pathInfo) {
                     vmk_Strlcpy(pathInfo[j].name,
                                 vmk_ScsiGetPathName(exPath->path),
                                 sizeof(pathInfo[j].name));
                     pathInfo[j].state = state;
                     pathInfo[j].flags = exPath->flags;
                     pathInfo[j].activeCmdCount = exPath->activeCmdCount;
                  }
                  numPaths++;
               } else {
                  vmk_Log(pluginLog, "\t%s (%s, flags=0x%08x, "
                          "activeCmdCount=%d)",
                          vmk_ScsiGetPathName(exPath->path),
                          vmk_ScsiPathStateToString(state), exPath->flags,
                          exPath->activeCmdCount);
               }
            } else if ((logFlags & VMK_SCSI_PLUGIN_STATE_LOG_CRASHDUMP) == 0) {
               if (pathInfo) {
                  pathInfo[j].name[0] = '\0';
               }
            }
         }
         if ((logFlags & VMK_SCSI_PLUGIN_STATE_LOG_CRASHDUMP) == 0) {
            vmk_SpinlockUnlock(exDev->lock);
            vmk_Log(pluginLog,
                    "%d: %s (flags=0x%08x, openCount=%d, activeCmdCount=%d):",
                    i, vmk_ScsiGetDeviceName(exDev->device),
                    flags, openCount, activeCmdCount);
            vmk_Log(pluginLog, "\tNo. of paths: %u", numPaths);
            if (pathInfo) {
               for (j = 0; j < EXAMPLE_MAX_PATHS_PER_DEVICE; j++) {
                  if (pathInfo[j].name[0] == '\0') {
                     continue;
                  }
                  vmk_Log(pluginLog, "\t%s (%s, flags=0x%08x, "
                          "activeCmdCount=%d)", pathInfo[j].name,
                          vmk_ScsiPathStateToString(pathInfo[j].state),
                          pathInfo[j].flags, pathInfo[j].activeCmdCount);
               }
               vmk_HeapFree(miscHeap, pathInfo);
            }
         }
      }
      if ((logFlags & VMK_SCSI_PLUGIN_STATE_LOG_CRASHDUMP) == 0) {
         vmk_SemaUnlock(&deviceSema);
      }
   }
   
   return VMK_OK;
}




/************************************************************
 *
 * Initialization / Termination
 *
 ************************************************************/

/*
 ***********************************************************************
 * init_module --                                                 */ /**
 *
 * \brief Initialize the plugin
 *
 * This function is called automatically when the VMkernel
 * module is loaded.
 *
 * \retval 0 The plugin was successfully initialized
 * \retval 1 The initialization failed; the module will be
 *     automatically unloaded.
 *
 * \note This is one of the module's entry points
 *
 ***********************************************************************
 */
int
init_module(void)
{
   VMK_ReturnStatus status;
   vmk_Bool spClassInitialized = VMK_FALSE;
   vmk_Bool spInitialized = VMK_FALSE;
   vmk_Bool semaInitialized = VMK_FALSE;
   vmk_Bool pluginRegistered = VMK_FALSE;
   vmk_Bool logRegistered = VMK_FALSE;
   vmk_Bool miscHeapInitialized = VMK_FALSE;
   vmk_Bool exCmdHeapInitialized= VMK_FALSE;
   vmk_Bool moduleRegistered = VMK_FALSE; 
   vmk_Bool configHandleInitialized = VMK_FALSE;
   vmk_Bool deviceArrayCreated = VMK_FALSE;
   vmk_Bool tqInitialized = VMK_FALSE;
   vmk_uint32 i;
   vmk_HeapCreateProps heapProps;
   vmk_LogProperties logProps;
   vmk_SpinlockCreateProps lockProps;
   vmk_Name name;
   vmk_ScsiSystemLimits limits;
   vmk_TimerQueueProps tqProps;

   vmk_ScsiGetSystemLimits(&limits);
   maxDevices = limits.maxDevices;
   /* Register the module with the VMKAPI and validate the revision number */
   status = vmk_ModuleRegister(&moduleID, VMKAPI_REVISION);
   if (status != VMK_OK) {
      vmk_WarningMessage("vmk_ModuleRegister failed: %s",
                         vmk_StatusToString(status));
      goto error;
   }

   moduleRegistered = VMK_TRUE;

   
   /*
    * Create a default heap for the module. VMKAPI uses the default heap
    * for any allocation performed on behalf of the module. So this must
    * be done before any VMKAPI object is created.
    */
   heapProps.type = VMK_HEAP_TYPE_SIMPLE;
   status = vmk_NameInitialize(&heapProps.name, EXAMPLE_NAME "-misc");
   VMK_ASSERT(status == VMK_OK);
   heapProps.module = moduleID;
   heapProps.initial = EXAMPLE_MISC_HEAP_INITIAL_SIZE;
   heapProps.max = EXAMPLE_MISC_HEAP_MAXIMUM_SIZE;
   heapProps.creationTimeoutMS = VMK_TIMEOUT_NONBLOCKING;
   status = vmk_HeapCreate(&heapProps, &miscHeap);
   if (status != VMK_OK) {
      vmk_WarningMessage("vmk_HeapCreate failed");
      goto error;
   }

   vmk_ModuleSetHeapID(moduleID, miscHeap);
   miscHeapInitialized = VMK_TRUE;
   createdDevices = vmk_HeapAlloc(miscHeap,
                                  maxDevices * sizeof(createdDevices[0]));
   if (createdDevices == NULL) {
      vmk_WarningMessage("Device array create failed");
      goto error;
   }
   vmk_Memset(createdDevices, 0, maxDevices * sizeof(createdDevices[0]));
   deviceArrayCreated = VMK_TRUE;

   status = vmk_NameInitialize(&logProps.name, EXAMPLE_NAME);
   VMK_ASSERT(status == VMK_OK);
   logProps.module = moduleID;
   logProps.heap = miscHeap;
   logProps.defaultLevel = 0;
   logProps.throttle = NULL;
   status = vmk_LogRegister(&logProps, &pluginLog);
   if (status != VMK_OK) {
      vmk_WarningMessage("vmk_LogRegister failed: %s",
                         vmk_StatusToString(status));
      goto error;
   }
   vmk_LogSetCurrentLogLevel(pluginLog, 0);

   logRegistered = VMK_TRUE;

   status = vmk_NameInitialize(&name, "mp-plugin-example");
   status = vmk_LockDomainCreate(moduleID, miscHeap, &name, &lockDomain);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog, "vmk_LockDomainCreate failed");
      goto error;
   }

   spClassInitialized = VMK_TRUE;
   
   /*
    * Initialize the module's private data structures.
    *
    * Readjust the storage heap when maximum outstanding IOs
    * supported changes.
    */
   VMK_ASSERT(vmk_ScsiCommandMaxCommands() == 65536);
   VMK_ASSERT(vmk_ScsiCommandMaxFree() == 8192);

#ifdef VMX86_DEBUG
   VMK_ASSERT_ON_COMPILE(sizeof(ExampleCommand) == 56);
#else /* VMX86_DEBUG */
   VMK_ASSERT_ON_COMPILE(sizeof(ExampleCommand) == 56);
#endif /* VMX86_DEBUG */

   status = vmk_NameInitialize(&tqProps.name, EXAMPLE_NAME);
   VMK_ASSERT(status == VMK_OK);
   tqProps.moduleID = moduleID;
   tqProps.heapID = miscHeap;
   tqProps.attribs = VMK_TIMER_QUEUE_ATTR_NONE;

   status = vmk_TimerQueueCreate(&tqProps, &timerQueue);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog, "vmk_TimerQueueCreate failed");
      goto error;
   }
   tqInitialized = VMK_TRUE;

   /* 
    * To aid forward progress in low memory conditions, use a separate heap
    * with a guaranteed minimum size for allocations of ExampleScsiCommand's.
    */
   heapProps.type = VMK_HEAP_TYPE_SIMPLE;
   status = vmk_NameInitialize(&heapProps.name, EXAMPLE_NAME "-exCommand");
   VMK_ASSERT(status == VMK_OK);
   heapProps.module = moduleID;
   heapProps.initial = EXAMPLE_COMMAND_HEAP_INITIAL_SIZE;
   heapProps.max = EXAMPLE_COMMAND_HEAP_MAXIMUM_SIZE;
   heapProps.creationTimeoutMS = VMK_TIMEOUT_NONBLOCKING;
   
   status = vmk_HeapCreate(&heapProps, &exCommandHeap);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog, "vmk_HeapCreate failed");
      goto error;
   }

   exCmdHeapInitialized = VMK_TRUE;

   for (i = 0; i < maxDevices; i++) {
      createdDevices[i] = NULL;
   }

   vmk_ListInit(&retryQueue);

   lockProps.moduleID = moduleID;
   lockProps.heapID = miscHeap;
   status = vmk_NameInitialize(&lockProps.name, EXAMPLE_LOCK_NAME("plugin"));
   VMK_ASSERT(status == VMK_OK);
   lockProps.type = VMK_SPINLOCK;
   lockProps.domain = lockDomain; 
   lockProps.rank = EXAMPLE_PLUGIN_LOCK_RANK;
   status = vmk_SpinlockCreate(&lockProps, &pluginLock);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog, "vmk_SpinlockCreate failed: %s",
                  vmk_StatusToString(status));
      goto error;
   }

   spInitialized = VMK_TRUE;

   status = vmk_BinarySemaCreate(&deviceSema, moduleID, EXAMPLE_NAME);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog, "vmk_BinarySemaCreate failed: %s",
                  vmk_StatusToString(status));
      goto error;
   }

   semaInitialized = VMK_TRUE;

   /* Get SCSI IO accounting servie id for CPU accounting */
   status = vmk_ServiceGetID(VMK_SERVICE_ACCT_NAME_SCSI, &ScsiIoAccountingId);

   if (status != VMK_OK) {
      vmk_Warning(pluginLog, "vmk_ServiceGetID failed: %s",
                  vmk_StatusToString(status));
      goto error;
   }

   /* Register the plugin with the framework */
   plugin = vmk_ScsiAllocatePlugin(miscHeap, EXAMPLE_NAME);
   if (plugin == NULL) {
      vmk_Warning(pluginLog, "vmk_ScsiAllocatePlugin failed");
      goto error;
   }

   plugin->scsiRevision = VMK_SCSI_REVISION;
   plugin->pluginRevision = EXAMPLE_PLUGIN_REVISION;
   plugin->productRevision = EXAMPLE_PRODUCT_REVISION;
   plugin->moduleID = moduleID;
   plugin->type = VMK_SCSI_PLUGIN_TYPE_MULTIPATHING;
   plugin->pluginIoctl            = ExamplePluginIoctl;
   plugin->u.mp.pathClaimBegin    = ExamplePathClaimBegin;
   plugin->u.mp.pathClaim         = ExampleClaimPath;
   plugin->u.mp.pathUnclaim       = ExampleUnclaimPath;
   plugin->u.mp.pathClaimEnd      = ExamplePathClaimEnd;
   plugin->u.mp.pathProbe         = ExampleProbePath;
   plugin->u.mp.pathSetState      = ExamplePathSetState;
   plugin->u.mp.pathGetDeviceName = ExamplePathGetDeviceName;
   plugin->u.mp.pathIssueCmd      = ExamplePathIssueCommandDirect;
   plugin->logState     = ExamplePluginStateLogger;

   status = vmk_ConfigParamOpen(VMK_CONFIG_GROUP_SCSI, "LogMPCmdErrors", 
                                &configLogMPCmdErrorsHandle);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog,
                  "vmk_ConfigParamOpen for LogMPCmdErrors failed: %s",
                  vmk_StatusToString(status));
      goto error;
   }

   configHandleInitialized = VMK_TRUE;

   status = vmk_ScsiRegisterPlugin(plugin);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog, "vmk_ScsiRegisterPlugin failed: %s",
                  vmk_StatusToString(status));
      goto error;
   }

   pluginRegistered = VMK_TRUE;

#if 0
   status = vmk_MgmtInit(moduleID,
                        miscHeap,
                        &mgmtSig,
                        NULL,
                        0, // Not using a cookie here
                        &mgmtHandle);
   if (status != VMK_OK) {
	vmk_WarningMessage("Anup: vmk_MgmtInit Failed\n");
        goto error;
   }
     
   vmk_WarningMessage("Anup: vmk_MgmtInit Success*********\n");
#endif

   /* Enable the plugin */
   status = ExampleStartPlugin();
   if (status == VMK_OK) {
      /* Anup: Calling Callback in userworld
      Callback(0);
	*/
#if 0
      if (VMK_OK != send_msg(NULL)) {
	vmk_WarningMessage("Anup: send_msg() failed\n");
	goto error;
      }
#endif
      /* Test for threapool */
      if (VMK_OK != rpc_tests()) {
	      vmk_WarningMessage("rpc_test failed");
	      goto error;
      }

      return 0;
   }

   vmk_Warning(pluginLog, "ExampleStartPlugin failed.");
error:
   if (pluginRegistered) {
      vmk_ScsiSetPluginState(plugin, VMK_SCSI_PLUGIN_STATE_DISABLED);
      vmk_ScsiUnregisterPlugin(plugin);
   }

   if (plugin != NULL) {
      vmk_ScsiFreePlugin(plugin);
   }

   if (semaInitialized) {
      vmk_SemaDestroy(&deviceSema);
   }

   if (spInitialized) {
      vmk_SpinlockDestroy(pluginLock);
   }

   if (tqInitialized) {
      vmk_TimerQueueDestroy(timerQueue);
   }

   if (exCmdHeapInitialized) {
      vmk_HeapDestroy(exCommandHeap);
   }

   if (spClassInitialized) {
      vmk_LockDomainDestroy(lockDomain);
   }

   if (logRegistered) {
      vmk_LogUnregister(pluginLog);
   }

   if (deviceArrayCreated) {
      vmk_HeapFree(miscHeap, createdDevices);
   }

   if (miscHeapInitialized) {
      vmk_HeapDestroy(miscHeap);
   }

   if (moduleRegistered) {
      vmk_ModuleUnregister(moduleID);
   }

   if (configHandleInitialized) {
      vmk_ConfigParamClose(configLogMPCmdErrorsHandle);
   }

   return 1;
}

VMK_VERSION_INFO("Version " VMK_REVISION_STRING(EXAMPLE_PLUGIN)
                                     ", Built on: " __DATE__);

/*
 ***********************************************************************
 * cleanup_module --                                              */ /**
 *
 * \brief Free all resources allocated to the module.
 *
 * This function is invoked when a user attempts to unload the
 * module(using vmkload_mod -u) and the module's reference count is nul.
 *
 * The SCSI framework guaranties that cleanup_module() isn't invoked 
 * (i.e. that the module cannot be unloaded) when a plugin->op() is in 
 * progress, and that no plugin->op() gets initiated once the module 
 * unload has been scheduled. In particular, cleanup_module() can't be 
 * invoked if a device is opened.
 *
 * However, the framework doesn't provide any synchronization regarding 
 * outgoing calls from the module to the framework or the core VMkernel 
 * API, or incoming callbacks (e.g. timer callback, IO completion, world 
 * initialization function, ...). The module must cancel all timer 
 * callbacks, drain all IOs, quiesce all module code paths and free all 
 * resources before cleanup_module() returns.
 *
 * \note This is one of the module's entry points
 *
 ***********************************************************************
 */
void
cleanup_module(void)
{
   VMK_ReturnStatus status;
   vmk_uint32 i;

   /* Anup 
   vmk_MgmtDestroy(mgmtHandle);
	*/

   VMK_ASSERT(vmk_ListIsEmpty(&retryQueue));

   for (i = 0; i < maxDevices; i++) {
      VMK_ASSERT(createdDevices[i] == NULL);
   }
   vmk_HeapFree(miscHeap, createdDevices);

   /* Stop the internal worlds */
   ExampleStopWorld(&retryWorldState);
 
   vmk_ScsiSetPluginState(plugin, VMK_SCSI_PLUGIN_STATE_DISABLED);

   status = vmk_ScsiUnregisterPlugin(plugin);
   if (status != VMK_OK) {
      vmk_Warning(pluginLog,
                  "Failed to unregister the plugin. The module will be "
                  "unloaded anyway.");
      return;
   }

   vmk_LogUnregister(pluginLog);

   vmk_ScsiFreePlugin(plugin);
   vmk_SemaDestroy(&deviceSema);
   vmk_SpinlockDestroy(pluginLock);
   vmk_LockDomainDestroy(lockDomain);

   vmk_TimerQueueDestroy(timerQueue);

   vmk_HeapDestroy(exCommandHeap);
   vmk_HeapDestroy(miscHeap);

   vmk_ModuleUnregister(moduleID);

   vmk_ConfigParamClose(configLogMPCmdErrorsHandle);

   return;
}

static inline _module_struct_init(module_global_t *module)
{
	module->module   = EXAMPLE_NAME;
	module->mod_id   = moduleID;
	module->heap_id  = miscHeap;
	module->lockd_id = lockDomain;
}

static inline VMK_ReturnStatus _pthread_lock_test_func(void *data)
{
	pthread_mutex_t *lock = data;
	int             i;

	for (i = 0; i < 100; i++) {
		pthread_mutex_lock(*lock);
		pthread_mutex_unlock(*lock);
	}

	return VMK_OK;
}

VMK_ReturnStatus pthread_mutex_test(void)
{
	const int       MAX = 4;
	module_global_t module;
	int             rc;
	pthread_mutex_t lock;
	int             i;
	pthread_t       threads[MAX];
	char            n[128];

	_module_struct_init(&module);

	memset(threads, 0, sizeof(threads));

	rc = pthread_mutex_init(&lock, EXAMPLE_NAME, &module);
	if (rc < 0) {
		return VMK_FAILURE;
	}

	vmk_WarningMessage("%s Running TEST1\n", __func__);
	for (i = 0; i < 10000; i++) {
		pthread_mutex_lock(lock);
		pthread_mutex_unlock(lock);
	}
	vmk_WarningMessage("%s TEST1: PASSED\n", __func__);

	vmk_WarningMessage("%s Running TEST2\n", __func__);
	for (i = 0; i < MAX; i++) {
		vmk_StringFormat(n, sizeof(n), NULL, "%s-%d", EXAMPLE_NAME, i);

		rc = pthread_create(&threads[i], n, &module,
				_pthread_lock_test_func, &lock);

		if (rc < 0) {
			goto error;
		}
	}

	for (i = 0; i < MAX; i++) {
		pthread_join(threads[i], NULL);
	}
	vmk_WarningMessage("%s TEST2: PASSED\n", __func__);

	pthread_mutex_destroy(lock);
	return VMK_OK;

error:
	for (i = 0; i < MAX; i++) {
		if (threads[i] == 0) {
			continue;
		}

		pthread_cancel(threads[i]);
	}

	pthread_mutex_destroy(lock);
	vmk_WarningMessage("%s A test failed.\n", __func__);
	return VMK_FAILURE;
}

VMK_ReturnStatus bufpool_test(void)
{
	const int        MAX = 200;
	module_global_t  module;
	bufpool_t        bp;
	int              rc;
	int              i;
	char             *b;
	char             *bufs[MAX + 10];
	VMK_ReturnStatus rs;

	_module_struct_init(&module);

	vmk_Memset(bufs, 0, sizeof(bufs));

	rc = bufpool_init(&bp, EXAMPLE_NAME, &module, 128, 100, MAX);
	if (rc < 0) {
		return VMK_FAILURE;
	}

	vmk_WarningMessage("bufpool_init done\n");
	for (i = 0; i < MAX; i++) {
		b  = NULL;
		rc = bufpool_get(&bp, &b, 1);
		if (b == NULL || rc < 0) {
			rs = VMK_FAILURE;
			goto error;
		}
		bufs[i] = b;
	}

	b  = NULL;
	rc = bufpool_get(&bp, &b, 1);
	if (b == NULL || rc < 0) {
		rs = VMK_OK;
	} else {
		bufpool_put(&bp, b);
		rs = VMK_FAILURE;
	}
error:
	for (i = 0; i < (sizeof(bufs) / sizeof(*bufs)); i++) {
		b = bufs[i];

		if (b == NULL) {
			continue;
		}

		bufpool_put(&bp, b);
	}

	bufpool_deinit(&bp);
	return rs;
}

void do_work(work_t *w, void *data)
{
	char *msg = (char *) data;
	assert(data != NULL);

	vmk_WorldSleep(1000);
	vmk_WarningMessage("%s", data);
	vmk_HeapFree(miscHeap, data);
}

VMK_ReturnStatus threadpool_test(void)
{
	module_global_t module;
	thread_pool_t   tp;
	char            *tmp;
	int             rc;
	int             i;
	work_t          *w;

	_module_struct_init(&module);

	rc = thread_pool_init(&tp, "test", &module, 10);
	if (rc < 0) {
		vmk_WarningMessage("thread_pool_init failed");
		return VMK_FAILURE;
	}

	for (i = 0; i < 1000; i++) {
		w          = new_work(&tp);
		w->data    = vmk_HeapAlloc(miscHeap, 100);
		w->work_fn = do_work;
		vmk_StringFormat(w->data, 100, NULL, "Number-%d", i);

		rc = schedule_work(&tp, w);
		assert(rc == 0);
	}

	vmk_WarningMessage("**** calling deinit ******\n");
	vmk_WarningMessage("**** calling deinit ******\n");
	thread_pool_deinit(&tp);
	vmk_WarningMessage("**** deinit DONE ******\n");
	return VMK_OK;
err:
	thread_pool_deinit(&tp);

	return VMK_FAILURE;
}

VMK_ReturnStatus run_tests(void *data)
{
	VMK_ReturnStatus rc;

	vmk_WarningMessage("Running pthred_mutex test\n");
	rc = pthread_mutex_test();
	if (rc != VMK_OK) {
		vmk_WarningMessage("pthread_mutex_test: FAILED\n");
		goto error;
	}
	vmk_WarningMessage("pthread_mutex_test: PASSED\n");

	vmk_WarningMessage("Running bufpool test\n");
	rc = bufpool_test();
	if (rc != VMK_OK) {
		vmk_WarningMessage("bufpool test: FAILED.\n");
		goto error;
	}
	vmk_WarningMessage("bufpool test: PASSED.\n");

	vmk_WarningMessage("Running ThreadPool test\n");
	rc = threadpool_test();
	if (rc != VMK_OK) {
		vmk_WarningMessage("ThreadPool test: FAILED.\n");
		goto error;
	}
	vmk_WarningMessage("ThreadPool test: PASSED.\n");

	return VMK_OK;

error:
	return VMK_FAILURE;
}

VMK_ReturnStatus rpc_tests(void)
{
	/* 1. start main work creating  thread
	   2. inside this thread, create thread pool
	   3. thread pool work function should sleep before work
	   4. 
	 */
	VMK_ReturnStatus status = VMK_FAILURE;
	vmk_WorldProps props;


	/* Create the world */
	props.name = EXAMPLE_NAME"-rpc-tests";
	props.moduleID = moduleID;
	props.startFunction = run_tests;
	props.data = NULL;
	props.schedClass = VMK_WORLD_SCHED_CLASS_DEFAULT;
	status = vmk_WorldCreate(&props, NULL);

	if (status != VMK_OK) {
		vmk_WarningMessage("threadpool world create failed\n");
	}

	return status;
}

#if 0
VMK_ReturnStatus
start_send_msg(void)
{
	VMK_ReturnStatus status = VMK_FAILURE;
	vmk_WorldProps worldProps;

	/* Create the world */
	worldProps.name = EXAMPLE_NAME"-send_msg";
	worldProps.moduleID = moduleID;
	worldProps.startFunction = send_msg;
	worldProps.data = NULL;
	worldProps.schedClass = VMK_WORLD_SCHED_CLASS_DEFAULT;
	status = vmk_WorldCreate(&worldProps, NULL);

	if (status == VMK_OK) {
		vmk_WarningMessage("start_send_msg world created succeed\n");
	} else {
		vmk_WarningMessage("start_send_msg world created failed\n");
	}
	
	return status;
}

#if 1
#define CLASS(p) ((*(unsigned char*)(p))>>6)
int
parseip(char *name, vmk_uint32 *ip)
{
        unsigned char addr[4];
        char *p;
        int i, x;

        p = name;
        for(i=0; i<4 && *p; i++){
                x = vmk_Strtoul(p, &p, 0);
                if(x < 0 || x >= 256)
                        return -1;
                if(*p != '.' && *p != 0)
                        return -1;
                if(*p == '.')
                        p++;
                addr[i] = x;
        }

        switch(CLASS(addr)){
        case 0:
        case 1:
                if(i == 3){
                        addr[3] = addr[2];
                        addr[2] = addr[1];
                        addr[1] = 0;
                }else if(i == 2){
                        addr[3] = addr[1];
                        addr[2] = 0;
                        addr[1] = 0;
                }else if(i != 4)
                        return -1;
                break;
        case 2:
                if(i == 3){
                        addr[3] = addr[2];
                        addr[2] = 0;
                }else if(i != 4)
                        return -1;
                break;
        }
        *ip = *(vmk_uint32*)addr;
        return 0;
}
#endif

static inline int strlen(const char *str)
{
	int i;
	for (i = 0; str[i]; i++);
	return i;
}

char *get_buf(void)
{
	char *buf;

	buf = vmk_HeapAlign(alignedHeap, 4096, 4096);
	if (buf == NULL) {
		vmk_WarningMessage("vmk_HeapAlign failed\n");
		goto err;
	}

	if ((unsigned long)buf & 4095 != 0) {
		vmk_WarningMessage("Unaligned buffer.\n");
		goto err;
	}

	return buf;
err:
	return NULL;
}

void free_buf(char *buf)
{
	vmk_HeapFree(alignedHeap, buf);
}

static VMK_ReturnStatus
send_msg(void * data)
{
	vmk_Socket sock;
	int len = 0;
	VMK_ReturnStatus rc;
   	vmk_HeapCreateProps heapProps;
   	vmk_Bool alignedHeapInitialized = VMK_FALSE;
	vmk_Bool socket_created = VMK_FALSE;
	//char buf[4096] = {0};
	char *buf = NULL;
	struct vmk_SocketIPAddressAddr ip;
	vmk_SocketIPAddress sa, client_sa;
	unsigned long long i;
	unsigned long long part_msgs;
	unsigned long long complt_msgs;
	int size;
	int offset;
	int max_size;

	heapProps.type = VMK_HEAP_TYPE_SIMPLE;
	rc = vmk_NameInitialize(&heapProps.name, EXAMPLE_NAME "-aligned");
	VMK_ASSERT(rc  == VMK_OK);
	heapProps.module = moduleID;
	heapProps.initial = EXAMPLE_MISC_HEAP_INITIAL_SIZE;
	heapProps.max = EXAMPLE_MISC_HEAP_MAXIMUM_SIZE;
	heapProps.creationTimeoutMS = VMK_TIMEOUT_NONBLOCKING;
	rc  = vmk_HeapCreate(&heapProps, &alignedHeap);
	if (rc != VMK_OK) {
		vmk_WarningMessage("vmk_HeapCreate failed for alignedHeap");
		goto err;
	}
	vmk_WarningMessage("vmk_HeapCreate succeed for alignedHeap");

	alignedHeapInitialized = VMK_TRUE;

	if (VMK_OK != vmk_SocketCreate(VMK_SOCKET_AF_INET, VMK_SOCKET_SOCK_STREAM,
		             0, &sock)) {
		vmk_WarningMessage("Anup: vmk_SocketCreate failed\n");
		rc = VMK_FAILURE;
		goto err;
	}

	socket_created = VMK_TRUE;

	vmk_WarningMessage("Anup: vmk_SocketCreate succeed\n");
	vmk_Memset(&sa, 0, sizeof(vmk_SocketIPAddress));
	vmk_Memset(&client_sa, 0, sizeof(vmk_SocketIPAddress));

	if (VMK_OK != vmk_SocketStringToAddr(VMK_SOCKET_AF_INET, "10.10.10.114",
				sizeof("10.10.10.114"), (vmk_SocketAddress *)&sa)) {

		vmk_WarningMessage("Anup: vmk_SocketStringToAddr failed\n");
		rc = VMK_FAILURE;
		goto err;
	}

	vmk_WarningMessage("Anup: vmk_SocketStringToAddr succeed\n");

	sa.sin_port   = 40000;
	sa.sin_len    = sizeof(sa);
	sa.sin_family = VMK_SOCKET_AF_INET;

	if (VMK_OK != vmk_SocketStringToAddr(VMK_SOCKET_AF_INET, "10.10.10.100",
				sizeof("10.10.10.100"), (vmk_SocketAddress *)&client_sa)) {

		vmk_WarningMessage("Anup: vmk_SocketStringToAddr failed for client\n");
		rc = VMK_FAILURE;
		goto err;
	}

	vmk_WarningMessage("Anup: vmk_SocketStringToAddr succeed for client\n");

	client_sa.sin_port   = 40000;
	client_sa.sin_len    = sizeof(client_sa);
	client_sa.sin_family = VMK_SOCKET_AF_INET;

	if (VMK_OK != (rc = vmk_SocketBind(sock, (vmk_SocketAddress*)&client_sa, 
			sizeof(vmk_SocketAddress)))) {
		vmk_WarningMessage("Anup: vmk_Socketbind failed");
		rc = VMK_FAILURE;
		goto err;
	}

	vmk_WarningMessage("Anup: vmk_Socketbind succeed for client\n");

	vmk_WarningMessage("Anup: Calling vmk_SocketConnect");

	if (VMK_OK != (rc = vmk_SocketConnect(sock, (vmk_SocketAddress *)&sa,
					sizeof(vmk_SocketAddress)))) {
		vmk_WarningMessage("Anup: vmk_SocketConnect failed");
		rc = VMK_FAILURE;
		goto err;
	}

	vmk_WarningMessage("Anup: vmk_SocketConnect succeed");

#if 0
	vmk_Strncpy(buf, "Hello World", sizeof(buf));
	rc = vmk_SocketSendTo(sock, VMK_SOCKET_MSG_DONTWAIT, NULL, buf, strlen(buf)+1, &len);
	if (VMK_OK != rc) {
		vmk_WarningMessage("vmk_SocketSendTo failed");
		goto err;
	}

	len = 0;
	vmk_WorldSleep(1000000);
#endif
	max_size = 0;
	i      = 0;
	//size   = sizeof(buf);
	size = 4096;
	offset = 0;
	part_msgs = 0;
	complt_msgs = 0;

	buf = get_buf();
	if (buf == NULL) {
		rc = VMK_FAILURE;
		goto err;
	}

	vmk_Memset(buf, 1, 4096);

	while (1) {

		if (size == 0) {
			vmk_WarningMessage("size is zero. this should not happen.\n");
			break;
		}

		//rc = vmk_SocketRecvFrom(sock, 0, NULL, NULL, buf + offset, size, &len);
		rc = vmk_SocketSendTo(sock, 0, NULL,  buf + offset, size, &len);

		//if (len == sizeof(buf))
		if (len == 4096)
			complt_msgs++;

		if (len > max_size) {
			max_size = len;
		}

		if (rc != VMK_OK || len == 0) {
			vmk_WarningMessage("vmk_SocketSendTo failed");
			vmk_WarningMessage("len = %d, rc = %d #msgs = %llu"
					   "max_size = %d complt_msgs = %llu part_msgs=%llu\n",
					   len, rc, i, max_size, complt_msgs, part_msgs);
			rc = VMK_FAILURE;
			goto err;
		} else if (len != size) {
			size   = size - len;
			offset += len;
			part_msgs++;
			continue;
		}

		//if (size < sizeof(buf))
		if (size < 4096)
			part_msgs++;

		//size   = sizeof(buf);
		size   = 4096;
		offset = 0;
		i++;
	}

	rc = VMK_OK;
err:
	if (buf) {
		free_buf(buf);
	}

	if (alignedHeapInitialized) {
		vmk_HeapDestroy(alignedHeap);
	}

	if (socket_created) {
		vmk_SocketClose(sock);
	}
	
	vmk_WorldExit(rc);
	//return rc;
}

#if 0 //Anup
void
Callback(vmk_TimerCookie unusedData)
{
        static vmk_uint64 evtCount = 0;
        static vmk_uint64 failCount = 0;
	int rc;
        statisticsType statParm;
        evtCount++;
        failCount+=5;

        vmk_WarningMessage("Anup: Callback function started ===>\n");

        /*
           * Here we're just simulating ever-increasing failures every time
           * the kernel callback runs.
           */
        statParm.ioFailures = failCount;
        statParm.linkSpeed = 1000;
        vmk_Strncpy((char *)statParm.modelString,
                        "Acme Model",
                        MGMT_PARM_STRLEN);
repeat:
        /*
           * This will schedule the callback to fire in user-space, if a user-space
           * application is listening. Otherwise it is dropped (with status VMK_NOT_FOUND).
           */
        rc = vmk_MgmtCallbackInvoke(mgmtHandle,
                        VMK_MGMT_NO_INSTANCE_ID,
                        TEST_CB_TO_USER,
                        &evtCount,
                        &statParm);
	if (rc == VMK_NOT_FOUND) {
		vmk_WarningMessage("Anup: Repeating Callback\n");
		vmk_WorldSleep(1000000);
		goto repeat;
	}

        vmk_WarningMessage("Anup: Callback function ended <===\n");
}
#endif
#endif
