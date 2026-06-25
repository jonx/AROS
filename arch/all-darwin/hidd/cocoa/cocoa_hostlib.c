/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Load cocoametal.dylib via hostlib.resource and resolve the cm_* ABI.
*/

#define DEBUG 1

#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/hostlib.h>

#include "cocoa_intern.h"

/* The frozen symbol list (INTERFACE.md §1a, v2). HostLib_GetInterface resolves
 * these in order into struct CMInterface (same order = the contract). */
static const char *const cm_symbols[] =
{
    "cm_open",
    "cm_close",
    "cm_upload_rect",
    "cm_present",
    "cm_set_effect",
    "cm_pump_events",
    "cm_readback",
    "cm_target_size",
    "cm_render_effect_readback",
    "cm_abi_version",
    "cm_set_option",
    "cm_get_option",
    "cm_open_settings",
    NULL
};

#define HostLibBase (xsd->hostlib)

BOOL cocoa_hostlib_init(struct cocoahidd *xsd)
{
    ULONG errcount = 0;
    int abi;

    xsd->hostlib = OpenResource("hostlib.resource");
    if (!xsd->hostlib)
        return FALSE;

    /* Disable() across dlopen so that any Metal / libdispatch / Foundation
     * threads the dylib's initialisers spawn inherit a BLOCKED scheduler-signal
     * mask. Disable() does sigprocmask(SIG_BLOCK, <interrupt signals incl.
     * SIGALRM>) on the AROS thread; new threads inherit that mask. Otherwise a
     * host thread can catch SIGALRM and run the AROS scheduler on a non-AROS
     * thread -- cpu_Dispatch jumps to a saved AROS task context on the wrong
     * thread => intermittent wedge or SIGILL. (pthread_sigmask isn't linkable
     * from an AROS module; Disable() reaches the same host call via the kernel.) */
    Disable();
    xsd->cmHandle = HostLib_Open(CM_DYLIB_NAME, NULL);
    Enable();

    if (!xsd->cmHandle)
    {
        D(bug("[Cocoa] HostLib_Open(%s) failed\n", CM_DYLIB_NAME));
        return FALSE;
    }

    xsd->cm = (struct CMInterface *)HostLib_GetInterface(xsd->cmHandle,
                                        (char **)cm_symbols, &errcount);
    if ((!xsd->cm) || errcount)
    {
        D(bug("[Cocoa] HostLib_GetInterface: %u symbols unresolved\n", errcount));
        cocoa_hostlib_expunge(xsd);
        return FALSE;
    }

    /* Refuse a stale/mismatched dylib (INTERFACE.md §7). */
    HostLib_Lock();
    abi = xsd->cm->cm_abi_version();
    AROS_HOST_BARRIER
    HostLib_Unlock();

    if (abi != CM_ABI_VERSION)
    {
        D(bug("[Cocoa] ABI mismatch: dylib reports %d, expected %d\n", abi, CM_ABI_VERSION));
        cocoa_hostlib_expunge(xsd);
        return FALSE;
    }

    D(bug("[Cocoa] cocoametal.dylib loaded, ABI v%d OK\n", abi));
    return TRUE;
}

void cocoa_hostlib_expunge(struct cocoahidd *xsd)
{
    if (xsd->cm)
    {
        HostLib_DropInterface((APTR *)xsd->cm);
        xsd->cm = NULL;
    }
    if (xsd->cmHandle)
    {
        HostLib_Close(xsd->cmHandle, NULL);
        xsd->cmHandle = NULL;
    }
}
