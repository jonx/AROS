/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Internal helpers for the host-passthrough bsdsocket.library.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/hostlib.h>
#include <dos/dos.h>
#include <sys/errno.h>
#include <string.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"
#include "socket_intern.h"

/* proto/hostlib.h's stubs use HostLibBase; the HostLib_*-calling helpers here
   (IntCloseSocket, PollFd) all have `gb` (the global base) in scope. */
#define HostLibBase (gb->hostlib)

void SetError(int error, struct TaskBase *tb)
{
    switch (tb->errnoSize)
    {
    case 8: *(UQUAD *)tb->errnoPtr = (UQUAD)error; break;
    case 4: *(ULONG *)tb->errnoPtr = (ULONG)error; break;
    case 2: *(UWORD *)tb->errnoPtr = (UWORD)error; break;
    case 1: *(UBYTE *)tb->errnoPtr = (UBYTE)error; break;
    default: break;
    }
}

ULONG SetDTableSize(ULONG size, struct TaskBase *tb)
{
    struct Socket **old = tb->dTable;
    struct Socket **table;
    ULONG oldsize = tb->dTableSize * sizeof(struct Socket *);
    ULONG newsize = size * sizeof(struct Socket *);

    if (size < tb->dTableSize)      /* FIXME: shrink not supported (mingw32 parity) */
        return EMFILE;

    table = AllocPooled(tb->pool, newsize);
    if (!table)
        return ENOMEM;

    memset(table, 0, newsize);
    if (old)
        CopyMem(old, table, oldsize);

    tb->dTable = table;
    tb->dTableSize = size;

    if (old)
        FreePooled(tb->pool, old, oldsize);

    return 0;
}

int GetFreeFD(struct TaskBase *tb)
{
    ULONG i;
    for (i = 0; i < tb->dTableSize; i++)
        if (!tb->dTable[i])
            return (int)i;

    SetError(EMFILE, tb);
    return -1;
}

struct Socket *GetSocket(int s, struct TaskBase *tb)
{
    struct Socket *sd;

    if (s < 0 || (ULONG)s >= tb->dTableSize)
    {
        SetError(EBADF, tb);
        return NULL;
    }

    sd = tb->dTable[s];
    if (!sd)
        SetError(EBADF, tb);

    return sd;
}

struct Socket *IntCloseSocket(int s, struct TaskBase *tb)
{
    struct Socket *sd = GetSocket(s, tb);

    if (sd)
    {
        struct bsdsocketBase *gb = tb->glob;

        HostLib_Lock();
        gb->pump->pump_unregister(sd->s, tb->psig);   /* drop from kqueue */
        gb->sys->close(sd->s);
        HostLib_Unlock();
    }

    return sd;
}

int PollFd(struct TaskBase *tb, int hostfd, unsigned want, ULONG sigmask,
           LONG timeout_ms, ULONG *hitmask)
{
    struct bsdsocketBase *gb = tb->glob;
    APTR DOSBase = gb->DOSBase;
    ULONG waitsigs = sigmask | tb->sigintr;
    LONG elapsed = 0;
    int rc = 0;

    HostLib_Lock();
    gb->pump->pump_register(hostfd, want, tb->psig);
    HostLib_Unlock();

    for (;;)
    {
        struct PumpReady rd[8];
        unsigned readybits = 0;
        ULONG sig;
        int n, i;

        /* (1) a pending wait-signal wins immediately (autodoc break semantics). */
        sig = SetSignal(0, waitsigs) & waitsigs;
        if (sig)
        {
            if (hitmask) *hitmask = sig;
            rc = -1;
            break;
        }

        /* (2) the pump's readiness stash for this task's PumpSig. */
        HostLib_Lock();
        n = gb->pump->pump_drain(tb->psig, rd, 8);
        HostLib_Unlock();
        for (i = 0; i < n; i++)
            if (rd[i].fd == hostfd)
                readybits |= rd[i].ready;
        if (readybits & want)
        {
            rc = (int)(readybits & want);
            break;
        }

        /* (3) timeout (a negative timeout means block until ready/signalled). */
        if (timeout_ms >= 0 && elapsed >= timeout_ms)
        {
            rc = 0;
            break;
        }

        /* (4) park one timer tick — the safe darwin wake path (R-DARWIN-WAKE).
               Delay(1) = 1/TICKS_PER_SEC s (~20ms). */
        Delay(1);
        elapsed += 20;          /* Delay(1) = 1 tick = 1/50s ~= 20ms */
    }

    HostLib_Lock();
    gb->pump->pump_unregister(hostfd, tb->psig);
    HostLib_Unlock();
    return rc;
}
