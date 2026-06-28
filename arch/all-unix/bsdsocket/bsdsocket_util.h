/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Internal helpers for the host-passthrough bsdsocket.library.
*/
#ifndef BSDSOCKET_UTIL_H
#define BSDSOCKET_UTIL_H

#include "bsdsocket_intern.h"

/* Host (Darwin) errno literals we branch on directly (macOS <sys/errno.h>). The
   AmiTCP values apps see come from errno_xlate.c; these are the raw host numbers. */
#define HOST_EWOULDBLOCK 35
#define HOST_EAGAIN      35
#define HOST_EINPROGRESS 36

/* Bracket one host socket call with the hostlib lock and capture the host errno in
   the SAME locked region (host errno is thread-local to the single underlying
   thread and shared across AROS tasks, so it must be read before another task can
   make a host call — the lock guarantees that). RET = result lvalue; CALL = the
   gb->sys->fn(...) expression; FAILED = expr true when the call failed; HE = errno
   lvalue (0 if not failed). */
#define HOSTSOCK(gb, RET, CALL, FAILED, HE) do {   \
    HostLib_Lock();                                \
    (RET) = (CALL);                                \
    (HE)  = (FAILED) ? *((gb)->sys->__error()) : 0;\
    HostLib_Unlock();                              \
} while (0)

void SetError(int aros_errno, struct TaskBase *tb);
ULONG SetDTableSize(ULONG size, struct TaskBase *tb);
int GetFreeFD(struct TaskBase *tb);
struct Socket *GetSocket(int s, struct TaskBase *tb);
struct Socket *IntCloseSocket(int s, struct TaskBase *tb);

/* Timer-poll park (spec R-DARWIN-WAKE): host-thread Signal is unsafe on darwin, so
   register hostfd with the kqueue pump and poll on dos Delay() ticks until it is
   ready, a (sigmask|sigintr) signal arrives, or timeout_ms elapses (-1 = forever).
   Returns: >0 the PS_WANT_* bits ready; 0 timeout; -1 interrupted by a signal (the
   fired bits written to *hitmask). The caller re-issues the non-blocking op as the
   source of truth (R-W3). */
int PollFd(struct TaskBase *tb, int hostfd, unsigned want, ULONG sigmask,
           LONG timeout_ms, ULONG *hitmask);

#endif /* BSDSOCKET_UTIL_H */
