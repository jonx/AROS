/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library WaitSelect (LVO 21).

    PLACEHOLDER for the [N1] bring-up. The real timer-poll WaitSelect (spec
    §R-WAITSELECT composed with §R-DARWIN-WAKE: register the fd_sets with the kqueue
    pump, then Delay()-poll pump_drain + the *sigmask bits, rebuild the output
    fd_sets from a non-blocking re-probe, rewrite *sigmask, clear fds on a signal)
    lands once the round-trip ([N1]: socket/connect/send/recv) is verified through
    the real library — those use the blocking ops directly and do not need select().
*/

#include <proto/exec.h>
#include <sys/errno.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"

/* fd_set / struct timeval appear here only as opaque pointer params in the stub
   signature. Forward-declare fd_set rather than pulling sys/net_types.h, whose
   `long fd_mask` collides with the libc fd_set reachable in this module build. The
   real WaitSelect (Layer C) includes the AmiTCP fd_set and dereferences it. */
typedef struct fd_set fd_set;

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
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds;
    (void)timeout; (void)sigmask;
    SetError(EINVAL, taskBase);     /* TODO: Layer C — real timer-poll WaitSelect */
    return -1;
    AROS_LIBFUNC_EXIT
}
