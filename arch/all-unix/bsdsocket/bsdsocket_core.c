/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library socket data path (host-passthrough, darwin/unix). Each
          op marshals to the libSystem call; a would-block result becomes a
          timer-poll park (PollFd, spec R-DARWIN-WAKE) then a re-issue (the
          non-blocking syscall is the source of truth, R-W3). No host-thread Signal.
*/

#include <proto/exec.h>
#include <proto/hostlib.h>
#include <sys/errno.h>
#include <sys/socket.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"
#include "socket_intern.h"
#include "errno_xlate.h"

/* proto/hostlib.h's stubs use HostLibBase; every LVO body here has `gb` in scope. */
#define HostLibBase (gb->hostlib)

/* ---- socket (LVO 5) ----------------------------------------------------- */
AROS_LH3(int, socket,
    AROS_LHA(int, domain,   D0),
    AROS_LHA(int, type,     D1),
    AROS_LHA(int, protocol, D2),
    struct TaskBase *, taskBase, 5, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd;
    int s, hostfd, he = 0;

    s = GetFreeFD(taskBase);
    if (s == -1)
        return -1;

    sd = AllocPooled(taskBase->pool, sizeof(struct Socket));
    if (!sd) { SetError(ENOMEM, taskBase); return -1; }

    /* hs_socket = host socket() + O_NONBLOCK (the varargs fcntl lives in the dylib). */
    HostLib_Lock();
    hostfd = gb->pump->hs_socket(domain, type, protocol);
    he = (hostfd < 0) ? *(gb->sys->__error()) : 0;
    HostLib_Unlock();

    if (hostfd < 0)
    {
        FreePooled(taskBase->pool, sd, sizeof(struct Socket));
        SetError(bsdsock_errno_h2a(he), taskBase);
        return -1;
    }

    sd->s = hostfd;
    sd->flags = SOF_NBIO;

    ObtainSemaphore(&gb->lock);
    AddTail((struct List *)&gb->socks, (struct Node *)sd);
    ReleaseSemaphore(&gb->lock);

    taskBase->dTable[s] = sd;
    return s;
    AROS_LIBFUNC_EXIT
}

/* ---- bind (LVO 6) ------------------------------------------------------- */
AROS_LH3(int, bind,
    AROS_LHA(int, s,                  D0),
    AROS_LHA(struct sockaddr *, name, A0),
    AROS_LHA(int, namelen,            D1),
    struct TaskBase *, taskBase, 6, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he;
    if (!sd) return -1;
    HOSTSOCK(gb, ret, gb->sys->bind(sd->s, name, (unsigned)namelen), ret < 0, he);
    if (ret < 0) { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

/* ---- listen (LVO 7) ----------------------------------------------------- */
AROS_LH2(int, listen,
    AROS_LHA(int, s,       D0),
    AROS_LHA(int, backlog, D1),
    struct TaskBase *, taskBase, 7, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he;
    if (!sd) return -1;
    HOSTSOCK(gb, ret, gb->sys->listen(sd->s, backlog), ret < 0, he);
    if (ret < 0) { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

/* ---- accept (LVO 8) ----------------------------------------------------- */
AROS_LH3(int, accept,
    AROS_LHA(int, s,                  D0),
    AROS_LHA(struct sockaddr *, addr, A0),
    AROS_LHA(int *, addrlen,          A1),
    struct TaskBase *, taskBase, 8, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    struct Socket *nsd;
    int newfd, he, ns;
    if (!sd) return -1;

    for (;;)
    {
        HOSTSOCK(gb, newfd,
                 gb->sys->accept(sd->s, addr, (unsigned int *)addrlen),
                 newfd < 0, he);
        if (newfd >= 0)
            break;
        if (he != HOST_EWOULDBLOCK && he != HOST_EAGAIN)
        { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
        {
            ULONG hit = 0;
            if (PollFd(taskBase, sd->s, PS_WANT_READ, 0, -1, &hit) < 0)
            { SetError(EINTR, taskBase); return -1; }
        }
    }

    /* allocate an AROS fd + Socket for the accepted connection (non-blocking). */
    ns = GetFreeFD(taskBase);
    if (ns == -1) { HostLib_Lock(); gb->sys->close(newfd); HostLib_Unlock(); return -1; }
    nsd = AllocPooled(taskBase->pool, sizeof(struct Socket));
    if (!nsd) { HostLib_Lock(); gb->sys->close(newfd); HostLib_Unlock(); SetError(ENOMEM, taskBase); return -1; }

    HostLib_Lock();
    gb->pump->hs_set_nonblock(newfd);
    HostLib_Unlock();

    nsd->s = newfd;
    nsd->flags = SOF_NBIO;
    ObtainSemaphore(&gb->lock);
    AddTail((struct List *)&gb->socks, (struct Node *)nsd);
    ReleaseSemaphore(&gb->lock);
    taskBase->dTable[ns] = nsd;
    return ns;
    AROS_LIBFUNC_EXIT
}

/* ---- connect (LVO 9) ---------------------------------------------------- */
AROS_LH3(int, connect,
    AROS_LHA(int, s,                  D0),
    AROS_LHA(struct sockaddr *, name, A0),
    AROS_LHA(int, namelen,            D1),
    struct TaskBase *, taskBase, 9, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he, soerr;
    unsigned int slen;
    if (!sd) return -1;

    HOSTSOCK(gb, ret, gb->sys->connect(sd->s, name, (unsigned)namelen), ret < 0, he);
    if (ret == 0)
        return 0;
    if (he != HOST_EINPROGRESS && he != HOST_EWOULDBLOCK)
    { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }

    /* async connect: park on write-readiness, then read SO_ERROR (the result). */
    {
        ULONG hit = 0;
        if (PollFd(taskBase, sd->s, PS_WANT_WRITE, 0, -1, &hit) < 0)
        { SetError(EINTR, taskBase); return -1; }
    }
    soerr = 0; slen = sizeof(soerr);
    HOSTSOCK(gb, ret, gb->sys->getsockopt(sd->s, SOL_SOCKET, SO_ERROR, &soerr, &slen), ret < 0, he);
    if (ret < 0)   { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    if (soerr != 0){ SetError(bsdsock_errno_h2a(soerr), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

/* ---- send (LVO 11) ------------------------------------------------------ */
AROS_LH4(int, send,
    AROS_LHA(int, s,            D0),
    AROS_LHA(const void *, msg, A0),
    AROS_LHA(int, len,          D1),
    AROS_LHA(int, flags,        D2),
    struct TaskBase *, taskBase, 11, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    long ret; int he;
    if (!sd) return -1;
    for (;;)
    {
        HOSTSOCK(gb, ret, gb->sys->send(sd->s, msg, (unsigned long)len, flags), ret < 0, he);
        if (ret >= 0) return (int)ret;          /* one logical send; may be partial */
        if (he != HOST_EWOULDBLOCK && he != HOST_EAGAIN)
        { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
        {
            ULONG hit = 0;
            if (PollFd(taskBase, sd->s, PS_WANT_WRITE, 0, -1, &hit) < 0)
            { SetError(EINTR, taskBase); return -1; }
        }
    }
    AROS_LIBFUNC_EXIT
}

/* ---- sendto (LVO 10) ---------------------------------------------------- */
AROS_LH6(int, sendto,
    AROS_LHA(int, s,                       D0),
    AROS_LHA(const void *, msg,            A0),
    AROS_LHA(int, len,                     D1),
    AROS_LHA(int, flags,                   D2),
    AROS_LHA(const struct sockaddr *, to,  A1),
    AROS_LHA(int, tolen,                   D3),
    struct TaskBase *, taskBase, 10, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    long ret; int he;
    if (!sd) return -1;
    for (;;)
    {
        HOSTSOCK(gb, ret,
                 gb->sys->sendto(sd->s, msg, (unsigned long)len, flags, to, (unsigned)tolen),
                 ret < 0, he);
        if (ret >= 0) return (int)ret;
        if (he != HOST_EWOULDBLOCK && he != HOST_EAGAIN)
        { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
        {
            ULONG hit = 0;
            if (PollFd(taskBase, sd->s, PS_WANT_WRITE, 0, -1, &hit) < 0)
            { SetError(EINTR, taskBase); return -1; }
        }
    }
    AROS_LIBFUNC_EXIT
}

/* ---- recv (LVO 13) ------------------------------------------------------ */
AROS_LH4(int, recv,
    AROS_LHA(int, s,       D0),
    AROS_LHA(void *, buf,  A0),
    AROS_LHA(int, len,     D1),
    AROS_LHA(int, flags,   D2),
    struct TaskBase *, taskBase, 13, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    long ret; int he;
    if (!sd) return -1;
    for (;;)
    {
        HOSTSOCK(gb, ret, gb->sys->recv(sd->s, buf, (unsigned long)len, flags), ret < 0, he);
        if (ret >= 0) return (int)ret;          /* 0 = peer closed */
        if (he != HOST_EWOULDBLOCK && he != HOST_EAGAIN)
        { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
        {
            ULONG hit = 0;
            if (PollFd(taskBase, sd->s, PS_WANT_READ, 0, -1, &hit) < 0)
            { SetError(EINTR, taskBase); return -1; }
        }
    }
    AROS_LIBFUNC_EXIT
}

/* ---- recvfrom (LVO 12) -------------------------------------------------- */
AROS_LH6(int, recvfrom,
    AROS_LHA(int, s,                    D0),
    AROS_LHA(void *, buf,               A0),
    AROS_LHA(int, len,                  D1),
    AROS_LHA(int, flags,                D2),
    AROS_LHA(struct sockaddr *, from,   A1),
    AROS_LHA(int *, fromlen,            A2),
    struct TaskBase *, taskBase, 12, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    long ret; int he;
    if (!sd) return -1;
    for (;;)
    {
        HOSTSOCK(gb, ret,
                 gb->sys->recvfrom(sd->s, buf, (unsigned long)len, flags,
                                   from, (unsigned int *)fromlen),
                 ret < 0, he);
        if (ret >= 0) return (int)ret;
        if (he != HOST_EWOULDBLOCK && he != HOST_EAGAIN)
        { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
        {
            ULONG hit = 0;
            if (PollFd(taskBase, sd->s, PS_WANT_READ, 0, -1, &hit) < 0)
            { SetError(EINTR, taskBase); return -1; }
        }
    }
    AROS_LIBFUNC_EXIT
}

/* ---- shutdown (LVO 14) -------------------------------------------------- */
AROS_LH2(int, shutdown,
    AROS_LHA(int, s,   D0),
    AROS_LHA(int, how, D1),
    struct TaskBase *, taskBase, 14, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = GetSocket(s, taskBase);
    int ret, he;
    if (!sd) return -1;
    HOSTSOCK(gb, ret, gb->sys->shutdown(sd->s, how), ret < 0, he);
    if (ret < 0) { SetError(bsdsock_errno_h2a(he), taskBase); return -1; }
    return 0;
    AROS_LIBFUNC_EXIT
}

/* ---- CloseSocket (LVO 20) ----------------------------------------------- */
AROS_LH1(int, CloseSocket,
    AROS_LHA(int, s, D0),
    struct TaskBase *, taskBase, 20, BSDSocket)
{
    AROS_LIBFUNC_INIT
    struct bsdsocketBase *gb = taskBase->glob;
    struct Socket *sd = IntCloseSocket(s, taskBase);   /* pump_unregister + close */
    if (sd)
    {
        ObtainSemaphore(&gb->lock);
        Remove((struct Node *)sd);
        ReleaseSemaphore(&gb->lock);
        FreePooled(taskBase->pool, sd, sizeof(struct Socket));
        taskBase->dTable[s] = NULL;
        return 0;
    }
    return -1;
    AROS_LIBFUNC_EXIT
}
