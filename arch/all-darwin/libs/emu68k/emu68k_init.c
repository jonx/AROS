/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: emu68k.library init/expunge - bind the host 68k execution service
          via hostlib.resource. Mirrors arch/all-darwin/libs/gpufx.
*/

#include <exec/types.h>
#include <aros/libcall.h>
#include <aros/symbolsets.h>
#include <libraries/debug.h>

#include <proto/exec.h>
#include <proto/hostlib.h>
#include <proto/debug.h>

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
    "emu68k_run_call_hook",
    "emu68k_run_device_base",
    "emu68k_run_set_mouse_buttons",
    "emu68k_run_progdir",
    NULL
};

#define HostLibBase LIBBASE->hostlibBase

/* Resolve a host code address to "module symbol + offset" for the host-side
 * crash report. Same contract as the kernel's own trap symbolizer: DebugBase
 * by list walk (no OpenLibrary in a crash path), DecodeLocationA is read-only.
 * Writes "" when the address is not in any registered module. */
static void emu68k_symbolize(unsigned long long addr, char *out, unsigned outlen)
{
    static struct Library *DebugBase = NULL;
    char *modname = NULL, *symname = NULL;
    void *symaddr = NULL, *segaddr = NULL;
    unsigned int segnum = 0;
    struct TagItem tags[] =
    {
        { DL_ModuleName,    (IPTR)&modname },
        { DL_SegmentNumber, (IPTR)&segnum  },
        { DL_SegmentStart,  (IPTR)&segaddr },
        { DL_SymbolName,    (IPTR)&symname },
        { DL_SymbolStart,   (IPTR)&symaddr },
        { TAG_DONE }
    };

    if (out && outlen) out[0] = 0;
    if (!out || !outlen) return;
    if (!DebugBase)
        DebugBase = (struct Library *)FindName(&SysBase->LibList, "debug.library");
    if (!DebugBase) return;
    if (!DecodeLocationA((APTR)(IPTR)addr, tags) || !modname) return;
    if (symaddr)
        snprintf(out, outlen, "%s %s + 0x%lx", modname,
                 symname ? symname : "(no symbol)",
                 (unsigned long)((IPTR)addr - (IPTR)symaddr));
    else
        snprintf(out, outlen, "%s seg %u + 0x%lx", modname, segnum,
                 (unsigned long)((IPTR)addr - (IPTR)segaddr));
}

static int Emu68k_InitLib(LIBBASETYPEPTR LIBBASE)
{
    ULONG errcount = 0;
    void *syms[16] = { 0 };
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
    LIBBASE->host.run_call_hook = syms[10];
    LIBBASE->host.run_device_base = syms[11];
    LIBBASE->host.run_set_mouse_buttons = syms[12];
    LIBBASE->host.run_progdir = syms[13];
    LIBBASE->host_ok = TRUE;

    {   /* optional: an older dylib without the symbolizer hook still binds */
        void (*set_sym)(void (*)(unsigned long long, char *, unsigned)) =
            HostLib_GetPointer(LIBBASE->dylib, "emu68k_set_symbolizer", NULL);
        if (set_sym)
            set_sym(emu68k_symbolize);
    }

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
