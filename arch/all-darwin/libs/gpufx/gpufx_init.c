/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: gpufx.library init/expunge -- load cocoametal.dylib via
          hostlib.resource and resolve the cm_gpu_* compute section. Mirrors
          arch/all-darwin/hidd/cocoa/cocoa_hostlib.c.
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <aros/libcall.h>
#include <aros/symbolsets.h>

#include <proto/exec.h>
#include <proto/hostlib.h>

#include "gpufx_intern.h"
#include LC_LIBDEFS_FILE

#include <aros/debug.h>

APTR                       gfxfx_hostlib;   /* hostlib.resource (HostLibBase) */
static APTR                gfxfx_cmhandle;  /* the opened cocoametal.dylib    */
struct GfxFxCMInterface    *gfxfx_gpu;      /* NULL => CPU fallback only       */

/* Resolved in this order into struct GfxFxCMInterface (the contract). */
static const char *const gfxfx_gpu_syms[] =
{
    "cm_gpu_open",
    "cm_gpu_abi",
    "cm_gpu_scale",
    "cm_gpu_convert_yuv420",
    NULL
};

#define HostLibBase gfxfx_hostlib

/*
    Bind the GPU path. Never fails the library: a missing host / dylib / ABI
    mismatch just leaves gfxfx_gpu == NULL and every op runs the CPU fallback.
*/
static int GfxFx_InitLib(LIBBASETYPEPTR LIBBASE)
{
    ULONG errcount = 0;
    int abi;

    gfxfx_hostlib = OpenResource("hostlib.resource");
    if (!gfxfx_hostlib)
    {
        D(bug("[gpufx] no hostlib.resource -- CPU fallback only\n"));
        return TRUE;
    }

    /* Disable() across dlopen so any Metal/libdispatch/Foundation threads the
       dylib spawns inherit a BLOCKED scheduler-signal mask -- otherwise a host
       thread can catch SIGALRM and run the AROS scheduler on a non-AROS thread
       (wedge/SIGILL). Same reasoning as cocoa_hostlib_init. The dylib is
       normally already resident (the display driver opened it); HostLib_Open
       then just returns the existing handle. */
    Disable();
    gfxfx_cmhandle = HostLib_Open(GPUFX_CM_DYLIB_NAME, NULL);
    Enable();

    if (!gfxfx_cmhandle)
    {
        D(bug("[gpufx] HostLib_Open(%s) failed -- CPU fallback only\n",
              GPUFX_CM_DYLIB_NAME));
        return TRUE;
    }

    gfxfx_gpu = (struct GfxFxCMInterface *)HostLib_GetInterface(
                    gfxfx_cmhandle, (char **)gfxfx_gpu_syms, &errcount);
    if ((!gfxfx_gpu) || errcount)
    {
        D(bug("[gpufx] %u cm_gpu_* symbols unresolved -- CPU fallback only\n",
              errcount));
        gfxfx_gpu = NULL;
        return TRUE;
    }

    /* Refuse a stale/mismatched compute section. */
    abi = gfxfx_gpu->cm_gpu_abi();
    AROS_HOST_BARRIER
    if (abi != GPUFX_CM_ABI)
    {
        D(bug("[gpufx] cm_gpu ABI mismatch: dylib %d, expected %d -- CPU fallback\n",
              abi, GPUFX_CM_ABI));
        gfxfx_gpu = NULL;
        return TRUE;
    }

    /* Bring the compute section up (adopts the display's shared device). */
    if (gfxfx_gpu->cm_gpu_open() != 0)
    {
        AROS_HOST_BARRIER
        D(bug("[gpufx] cm_gpu_open failed -- CPU fallback only\n"));
        gfxfx_gpu = NULL;
        return TRUE;
    }
    AROS_HOST_BARRIER

    D(bug("[gpufx] cocoametal.dylib GPU section bound, ABI v%d\n", abi));
    return TRUE;
}

static int GfxFx_ExpungeLib(LIBBASETYPEPTR LIBBASE)
{
    if (gfxfx_cmhandle)
    {
        HostLib_Close(gfxfx_cmhandle, NULL);
        gfxfx_cmhandle = NULL;
    }
    gfxfx_gpu = NULL;
    return TRUE;
}

ADD2INITLIB(GfxFx_InitLib, 0);
ADD2EXPUNGELIB(GfxFx_ExpungeLib, 0);
