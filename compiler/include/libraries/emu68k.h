#ifndef LIBRARIES_EMU68K_H
#define LIBRARIES_EMU68K_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: 68k execution service - the launch context handed to
          emu68k.library by the DOS entry boundaries.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#define EMU68KNAME "emu68k.library"

/* Version 2: adds the raw image bytes (the DOS loader stashes the source
 * file alongside the registered seglist; the seglist's own payload is not
 * interpretable on hosts with no 32-bit-addressable memory). */
#define EMU68K_CTX_VERSION 2

#define EMU68K_LAUNCH_CLI  1   /* RunCommand: a shell command               */
#define EMU68K_LAUNCH_PROC 2   /* DosEntry: CreateNewProc/Workbench process */

struct Emu68kLaunchCtx
{
    ULONG           elc_Version;   /* EMU68K_CTX_VERSION                    */
    ULONG           elc_Origin;    /* EMU68K_LAUNCH_...                     */
    BPTR            elc_SegList;   /* the tracked 68k hunk seglist          */
    CONST_STRPTR    elc_Name;      /* best-known program name (may be NULL) */
    CONST_STRPTR    elc_Args;      /* argument string (may be NULL)         */
    ULONG           elc_ArgSize;
    struct Process *elc_Process;
    ULONG           elc_Mode;      /* 0 = automatic (route decided later)   */
    /* v2 */
    CONST_APTR      elc_Image;     /* the raw hunk-file bytes (may be NULL) */
    ULONG           elc_ImageSize;
};

#endif /* LIBRARIES_EMU68K_H */
