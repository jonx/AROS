/*
    Copyright (C) 1995-2019, The AROS Development Team. All rights reserved.
*/

#include <stdarg.h>
#include <stdio.h>

/* These macros are defined in both UNIX and AROS headers. Get rid of warnings. */
#undef __pure
#undef __const
#undef __pure2
#undef __deprecated

#include <aros/config.h>
#include <aros/kernel.h>
#include <exec/lists.h>
#include <exec/resident.h>
#include <dos/bptr.h>

#include "hostinterface.h"

#include "hostlib.h"
#include "shutdown.h"

#if defined(HOST_OS_darwin)
typedef void *host_pthread_t;
extern host_pthread_t pthread_self(void);

static volatile int HostLockState;
static volatile uintptr_t HostLockOwner;
static volatile unsigned int HostLockDepth;

static void Host_Lock(void)
{
    uintptr_t self = (uintptr_t)pthread_self();

    if (HostLockOwner == self)
    {
        HostLockDepth++;
        return;
    }

    while (__sync_lock_test_and_set(&HostLockState, 1))
        ;

    HostLockOwner = self;
    __sync_synchronize();
    HostLockDepth = 1;
}

static void Host_Unlock(void)
{
    uintptr_t self = (uintptr_t)pthread_self();

    if (HostLockOwner != self || HostLockDepth == 0)
        return;

    if (--HostLockDepth == 0)
    {
        HostLockOwner = 0;
        __sync_synchronize();
        __sync_lock_release(&HostLockState);
    }
}
#else
static void Host_Lock(void)
{
}

static void Host_Unlock(void)
{
}
#endif

#if AROS_MODULES_DEBUG
/* gdb hooks from which it obtains modules list */

/* This is needed in order to bring in definition of struct segment */
#include "../../../rom/debug/debug_intern.h"

APTR AbsExecBase = NULL;
struct segment *seg = NULL;
struct Resident *res = NULL;
struct MinList *Debug_ModList = NULL;
#endif

/*
 * Some helpful functions that link us to the underlying host OS.
 * Without them we would not be able to estabilish any interaction with it.
 */
static struct HostInterface _HostIFace =
{
    AROS_ARCHITECTURE,
    HOSTINTERFACE_VERSION,

    Host_HostLib_Open,
    Host_HostLib_Close,
    Host_HostLib_GetPointer,
    KPutC,
    Host_HostLib_GetTime,
    Host_Lock,
    Host_Unlock,
#if AROS_MODULES_DEBUG
    &Debug_ModList,
#else
    NULL,
#endif
};

void *HostIFace = &_HostIFace;
