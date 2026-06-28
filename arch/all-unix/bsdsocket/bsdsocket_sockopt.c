/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library socket-option / name ops (direct host passthrough).
          NOTE: option level/name constants are passed through to libSystem as-is;
          SOL_SOCKET and the common SO_ and IPPROTO_ options are BSD-identical on
          macOS. A per-option translation table is a follow-up if a non-identity
          option surfaces (TODO).
*/

#include <proto/exec.h>
#include <proto/hostlib.h>
#include <sys/errno.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"
#include "socket_intern.h"
#include "errno_xlate.h"

/* proto/hostlib.h's stubs use HostLibBase; the HostLib_*-calling bodies here have
   `gb` in scope (IoctlSocket makes no host call, so its lack of `gb` is fine). */
#define HostLibBase (gb->hostlib)

AROS_LH5(int, setsockopt,
    AROS_LHA(int, s,        D0),
    AROS_LHA(int, level,    D1),
    AROS_LHA(int, optname,  D2),
    AROS_LHA(void *, optval, A0),
    AROS_LHA(int, optlen,   D3),
    struct TaskBase *, taskBase, 15, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he;
    if (!sd) return -1;
    HOSTSOCK(gb, ret, gb->sys->setsockopt(sd->s, level, optname, optval, (unsigned)optlen), ret < 0, he);
    if (ret < 0) { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

AROS_LH5(int, getsockopt,
    AROS_LHA(int, s,        D0),
    AROS_LHA(int, level,    D1),
    AROS_LHA(int, optname,  D2),
    AROS_LHA(void *, optval, A0),
    AROS_LHA(void *, optlen, A1),
    struct TaskBase *, taskBase, 16, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he;
    if (!sd) return -1;
    HOSTSOCK(gb, ret, gb->sys->getsockopt(sd->s, level, optname, optval, (unsigned int *)optlen), ret < 0, he);
    if (ret < 0) { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

AROS_LH3(int, getsockname,
    AROS_LHA(int, s,                  D0),
    AROS_LHA(struct sockaddr *, name, A0),
    AROS_LHA(int *, namelen,          A1),
    struct TaskBase *, taskBase, 17, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he;
    if (!sd) return -1;
    HOSTSOCK(gb, ret, gb->sys->getsockname(sd->s, name, (unsigned int *)namelen), ret < 0, he);
    if (ret < 0) { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

AROS_LH3(int, getpeername,
    AROS_LHA(int, s,                  D0),
    AROS_LHA(struct sockaddr *, name, A0),
    AROS_LHA(int *, namelen,          A1),
    struct TaskBase *, taskBase, 18, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he;
    if (!sd) return -1;
    HOSTSOCK(gb, ret, gb->sys->getpeername(sd->s, name, (unsigned int *)namelen), ret < 0, he);
    if (ret < 0) { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

/* IoctlSocket — our sockets are always O_NONBLOCK, and the library hides blocking
   behind the timer-poll park, so FIONBIO is a no-op success. FIONREAD and other
   request codes are a TODO (would need a varargs-safe host ioctl helper). */
AROS_LH3(int, IoctlSocket,
    AROS_LHA(int, s,                   D0),
    AROS_LHA(unsigned long, request,   D1),
    AROS_LHA(char *, argp,             A0),
    struct TaskBase *, taskBase, 19, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct Socket *sd = GetSocket(s, taskBase);
    (void)request; (void)argp;
    if (!sd) return -1;
    return 0;
    AROS_LIBFUNC_EXIT
}
