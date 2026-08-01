#ifndef DOS_EMU68K_H
#define DOS_EMU68K_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: 68k hunk seglist launch routing (non-m68k targets)
*/

#include <dos/dos.h>
#include <dos/dosextens.h>

/* The launch context the execution boundaries hand to the 68k router:
 * RunCommand() for CLI commands, DosEntry() for created processes. The
 * record is versioned so later fields can be appended without breaking
 * either side. */
#define EMU68K_CTX_VERSION 1

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
};

/* Route a 68k hunk seglist launch. Returns DOSTRUE if the router ran the
 * program (its result in *result_out); DOSFALSE if it declined, in which
 * case the caller keeps its existing not-executable behavior. */
LONG Emu68k_RouteSegList(struct Emu68kLaunchCtx *ctx, LONG *result_out,
                         struct DosLibrary *DOSBase);

#endif /* DOS_EMU68K_H */
