/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: emu68k.library init/expunge - bind the host 68k execution service
          via hostlib.resource. Mirrors arch/all-darwin/libs/gpufx.
*/

#include <exec/types.h>
#include <aros/libcall.h>
#include <aros/symbolsets.h>

#include <proto/exec.h>
#include <proto/hostlib.h>

#include "emu68k_intern.h"
#include LC_LIBDEFS_FILE

#include <aros/debug.h>

static const char *const emu68k_syms[] =
{
    "emu68k_run_new",
    "emu68k_run_quantum",
    "emu68k_run_kill",
    "emu68k_run_free",
    "emu68k_version",
    "emu68k_run_set_name",
    "emu68k_scan_image",
    "emu68k_set_oscall",
    "emu68k_run_guest0",
    "emu68k_run_guest_alloc",
    NULL
};

#define HostLibBase LIBBASE->hostlibBase

static int Emu68k_InitLib(LIBBASETYPEPTR LIBBASE)
{
    ULONG errcount = 0;
    void *syms[10] = { 0 };
    int i;

    InitSemaphore(&LIBBASE->runlock);
    LIBBASE->host_ok = FALSE;

    LIBBASE->hostlibBase = OpenResource("hostlib.resource");
    if (!LIBBASE->hostlibBase)
    {
        D(bug("[emu68k.library] no hostlib.resource\n"));
        return TRUE;                     /* library opens; RunSeg declines */
    }

    /* Disable() across dlopen so any host threads inherit a blocked
       scheduler-signal mask (same reasoning as the cocoa/gpufx bindings). */
    Disable();
    LIBBASE->dylib = HostLib_Open(EMU68K_DYLIB_NAME, NULL);
    Enable();
    if (!LIBBASE->dylib)
    {
        D(bug("[emu68k.library] HostLib_Open(%s) failed\n", EMU68K_DYLIB_NAME));
        return TRUE;
    }

    for (i = 0; emu68k_syms[i]; i++)
    {
        syms[i] = HostLib_GetPointer(LIBBASE->dylib, emu68k_syms[i], NULL);
        if (!syms[i]) errcount++;
    }
    if (errcount)
    {
        D(bug("[emu68k.library] %lu host symbols missing\n", (unsigned long)errcount));
        HostLib_Close(LIBBASE->dylib, NULL);
        LIBBASE->dylib = NULL;
        return TRUE;
    }

    LIBBASE->host.run_new      = syms[0];
    LIBBASE->host.run_quantum  = syms[1];
    LIBBASE->host.run_kill     = syms[2];
    LIBBASE->host.run_free     = syms[3];
    LIBBASE->host.version      = syms[4];
    LIBBASE->host.run_set_name = syms[5];
    LIBBASE->host.scan_image   = syms[6];
    LIBBASE->host.set_oscall   = syms[7];
    LIBBASE->host.run_guest0   = syms[8];
    LIBBASE->host.run_guest_alloc = syms[9];
    LIBBASE->host_ok = TRUE;

    D(bug("[emu68k.library] bound: %s\n", LIBBASE->host.version()));
    return TRUE;
}

static int Emu68k_ExpungeLib(LIBBASETYPEPTR LIBBASE)
{
    if (LIBBASE->dylib && LIBBASE->hostlibBase)
        HostLib_Close(LIBBASE->dylib, NULL);
    LIBBASE->dylib = NULL;
    LIBBASE->host_ok = FALSE;
    return TRUE;
}

ADD2INITLIB(Emu68k_InitLib, 0)
ADD2EXPUNGELIB(Emu68k_ExpungeLib, 0)
