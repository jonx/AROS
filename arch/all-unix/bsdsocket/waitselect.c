/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library WaitSelect (LVO 21) — the AmiTCP "select() plus an Amiga
          Wait() signal mask" primitive, realised with the darwin timer-poll park
          (spec R-WAITSELECT composed with R-DARWIN-WAKE): register the requested
          fds with the kqueue pump, then Delay()-poll pump_drain + the *sigmask
          signals; on wake rebuild the output fd_sets from the pump's readiness,
          rewrite *sigmask, and (on a signal) clear the fd_sets. No host-thread
          Signal — the wake is a timer tick, the pump's stash is the truth.

    fd_set: AmiTCP's <sys/net_types.h> uses `long fd_mask`, which collides with the
    libc fd_set forced into this module's build (see git history). The contract is
    binary — long fds_bits[howmany(FD_SETSIZE=64, 64)] = long fds_bits[1] — so we
    mirror that layout locally and cast, rather than include net_types.h. Same for
    the BSD struct timeval ({long tv_sec; long tv_usec;}).
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/hostlib.h>
#include <sys/errno.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"
#include "socket_intern.h"

#define HostLibBase (gb->hostlib)

/* fd_set appears in the signature as an opaque pointer (we cast to ws_fdset, the
   binary-compatible local mirror); forward-declare it rather than pull net_types.h. */
typedef struct fd_set fd_set;

/* fd_set / timeval mirrors (AmiTCP layout; FD_SETSIZE = 64, NFDBITS = 64). */
#define WS_NFDBITS  (int)(sizeof(long) * 8)
struct ws_fdset { long fds_bits[(64 + 64 - 1) / 64]; };   /* one 64-bit word */
struct ws_timeval { long tv_sec; long tv_usec; };

#define WS_ISSET(n, p) (((struct ws_fdset *)(p))->fds_bits[(n) / WS_NFDBITS] &  (1L << ((n) % WS_NFDBITS)))
#define WS_SET(n, p)   (((struct ws_fdset *)(p))->fds_bits[(n) / WS_NFDBITS] |= (1L << ((n) % WS_NFDBITS)))
#define WS_ZERO(p)     do { if (p) ((struct ws_fdset *)(p))->fds_bits[0] = 0; } while (0)

#define WS_MAXFD 64

AROS_LH6(int, WaitSelect,
    AROS_LHA(int, nfds,                   D0),
    AROS_LHA(fd_set *, readfds,           A0),
    AROS_LHA(fd_set *, writefds,          A1),
    AROS_LHA(fd_set *, exceptfds,         A2),
    AROS_LHA(struct timeval *, timeout,   A3),
    AROS_LHA(ULONG *, sigmask,            D1),
    struct TaskBase *, taskBase, 21, BSDSocket)
{
    AROS_LIBFUNC_INIT

    struct bsdsocketBase *gb = taskBase->glob;
    APTR DOSBase = gb->DOSBase;
    struct { int fd; int hostfd; unsigned want; } reg[WS_MAXFD];
    int nreg = 0, nf, fd, count = 0, j;
    ULONG waitsigs, fired = 0;
    LONG timeout_ms = -1, elapsed = 0;

    nf = nfds;
    if (nf > WS_MAXFD) nf = WS_MAXFD;
    if (nf < 0)        nf = 0;

    /* (1) register every requested fd with the kqueue pump. */
    for (fd = 0; fd < nf; fd++)
    {
        unsigned want = 0;
        struct Socket *sd;

        if (readfds  && WS_ISSET(fd, readfds))  want |= PS_WANT_READ;
        if (writefds && WS_ISSET(fd, writefds)) want |= PS_WANT_WRITE;
        /* exceptfds (OOB) is not separately monitored on this port; cleared below. */
        if (!want)
            continue;

        sd = ((ULONG)fd < taskBase->dTableSize) ? taskBase->dTable[fd] : NULL;
        if (!sd)
        {
            SetError(EBADF, taskBase);
            /* unwind any prior registrations */
            for (j = 0; j < nreg; j++)
            {
                HostLib_Lock();
                gb->pump->pump_unregister(reg[j].hostfd, taskBase->psig);
                HostLib_Unlock();
            }
            return -1;
        }

        reg[nreg].fd = fd; reg[nreg].hostfd = sd->s; reg[nreg].want = want;
        HostLib_Lock();
        gb->pump->pump_register(sd->s, want, taskBase->psig);
        HostLib_Unlock();
        nreg++;
    }

    /* (2) output sets are rebuilt from readiness — clear them now. */
    WS_ZERO(readfds);
    WS_ZERO(writefds);
    WS_ZERO(exceptfds);

    waitsigs = (sigmask ? *sigmask : 0) | taskBase->sigintr;
    if (timeout)
    {
        struct ws_timeval *tv = (struct ws_timeval *)timeout;
        timeout_ms = (LONG)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    }

    /* (3) timer-poll: signals win immediately; else drain the pump; else tick. */
    for (;;)
    {
        ULONG sig = SetSignal(0, waitsigs) & waitsigs;
        struct PumpReady rd[16];
        int n, i;

        if (sig) { fired = sig; break; }

        HostLib_Lock();
        n = gb->pump->pump_drain(taskBase->psig, rd, 16);
        HostLib_Unlock();
        for (i = 0; i < n; i++)
        {
            for (j = 0; j < nreg; j++)
            {
                if (reg[j].hostfd == rd[i].fd)
                {
                    unsigned r = rd[i].ready & reg[j].want;
                    if ((r & PS_WANT_READ)  && readfds)  { WS_SET(reg[j].fd, readfds);  count++; }
                    if ((r & PS_WANT_WRITE) && writefds) { WS_SET(reg[j].fd, writefds); count++; }
                    break;
                }
            }
        }
        if (count > 0)
            break;
        if (timeout_ms >= 0 && elapsed >= timeout_ms)
            break;                              /* timeout: count stays 0 */

        Delay(1);                               /* ~20ms tick (R-DARWIN-WAKE) */
        elapsed += 20;
    }

    /* (4) unregister everything. */
    for (j = 0; j < nreg; j++)
    {
        HostLib_Lock();
        gb->pump->pump_unregister(reg[j].hostfd, taskBase->psig);
        HostLib_Unlock();
    }

    /* (5) result, per the AmiTCP autodoc. */
    if (fired)
    {
        /* a *sigmask / break signal preempted with no fd ready: clear fds, return 0,
           and report only the caller's bits that fired (not sigintr). */
        WS_ZERO(readfds);
        WS_ZERO(writefds);
        WS_ZERO(exceptfds);
        if (sigmask)
            *sigmask &= fired;
        return 0;
    }

    if (sigmask)
        *sigmask = 0;                           /* woke by fd readiness or timeout */
    return count;

    AROS_LIBFUNC_EXIT
}
