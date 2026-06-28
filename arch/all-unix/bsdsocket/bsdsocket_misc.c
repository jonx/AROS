/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library per-task accessors: Errno, SetErrnoPtr, getdtablesize,
          SetSocketSignals.
*/

#include <proto/exec.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"

AROS_LH0(LONG, Errno,
    struct TaskBase *, taskBase, 27, BSDSocket)
{
    AROS_LIBFUNC_INIT
    switch (taskBase->errnoSize)
    {
    case 8: return (LONG)*(UQUAD *)taskBase->errnoPtr;
    case 4: return (LONG)*(ULONG *)taskBase->errnoPtr;
    case 2: return (LONG)*(UWORD *)taskBase->errnoPtr;
    case 1: return (LONG)*(UBYTE *)taskBase->errnoPtr;
    }
    return 0;
    AROS_LIBFUNC_EXIT
}

AROS_LH2(void, SetErrnoPtr,
    AROS_LHA(void *, ptr, A0),
    AROS_LHA(int,    size, D0),
    struct TaskBase *, taskBase, 28, BSDSocket)
{
    AROS_LIBFUNC_INIT
    taskBase->errnoPtr  = ptr;
    taskBase->errnoSize = size;
    AROS_LIBFUNC_EXIT
}

AROS_LH0(int, getdtablesize,
    struct TaskBase *, taskBase, 23, BSDSocket)
{
    AROS_LIBFUNC_INIT
    return (int)taskBase->dTableSize;
    AROS_LIBFUNC_EXIT
}

AROS_LH3(void, SetSocketSignals,
    AROS_LHA(ULONG, intrmask, D0),
    AROS_LHA(ULONG, iomask,   D1),
    AROS_LHA(ULONG, urgmask,  D2),
    struct TaskBase *, taskBase, 22, BSDSocket)
{
    AROS_LIBFUNC_INIT
    taskBase->sigintr = intrmask;
    taskBase->sigio   = iomask;
    taskBase->sigurg  = urgmask;
    AROS_LIBFUNC_EXIT
}
