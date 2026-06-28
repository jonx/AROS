/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library — LVO entries not yet implemented on the host-passthrough
          darwin/unix port. They keep the vector table complete (genmodule links by
          name) and fail safely. Layers after the [N1] round-trip fill these in:
          the resolver (gethostbyname and the getserv/getproto/getnet families, via
          a host helper thread), the inet helpers (pure, easy), ObtainSocket and
          ReleaseSocket (cross-task fd hand-off), Dup2Socket, SocketBaseTagList
          (SBTC tag config), sendmsg/recvmsg, GetSocketEvents.
*/

#include <proto/exec.h>
#include <netdb.h>
#include <sys/socket.h>

#include "bsdsocket_intern.h"

AROS_LH4(LONG, ObtainSocket,
    AROS_LHA(LONG, id, D0), AROS_LHA(LONG, domain, D1),
    AROS_LHA(LONG, type, D2), AROS_LHA(LONG, protocol, D3),
    struct TaskBase *, taskBase, 24, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; return -1; AROS_LIBFUNC_EXIT }

AROS_LH2(LONG, ReleaseSocket,
    AROS_LHA(LONG, sd, D0), AROS_LHA(LONG, id, D1),
    struct TaskBase *, taskBase, 25, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; return -1; AROS_LIBFUNC_EXIT }

AROS_LH2(LONG, ReleaseCopyOfSocket,
    AROS_LHA(LONG, sd, D0), AROS_LHA(LONG, id, D1),
    struct TaskBase *, taskBase, 26, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; return -1; AROS_LIBFUNC_EXIT }

AROS_LH1(char *, Inet_NtoA,
    AROS_LHA(unsigned long, in, D0),
    struct TaskBase *, taskBase, 29, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)in; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH1(unsigned long, inet_addr,
    AROS_LHA(const char *, cp, A0),
    struct TaskBase *, taskBase, 30, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)cp; return (unsigned long)-1; AROS_LIBFUNC_EXIT }

AROS_LH1(unsigned long, Inet_LnaOf,
    AROS_LHA(unsigned long, in, D0),
    struct TaskBase *, taskBase, 31, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)in; return 0; AROS_LIBFUNC_EXIT }

AROS_LH1(unsigned long, Inet_NetOf,
    AROS_LHA(unsigned long, in, D0),
    struct TaskBase *, taskBase, 32, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)in; return 0; AROS_LIBFUNC_EXIT }

AROS_LH2(unsigned long, Inet_MakeAddr,
    AROS_LHA(int, net, D0), AROS_LHA(int, lna, D1),
    struct TaskBase *, taskBase, 33, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)net; (void)lna; return 0; AROS_LIBFUNC_EXIT }

AROS_LH1(unsigned long, inet_network,
    AROS_LHA(const char *, cp, A0),
    struct TaskBase *, taskBase, 34, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)cp; return (unsigned long)-1; AROS_LIBFUNC_EXIT }

AROS_LH1(struct hostent *, gethostbyname,
    AROS_LHA(char *, name, A0),
    struct TaskBase *, taskBase, 35, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)name; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH3(struct hostent *, gethostbyaddr,
    AROS_LHA(char *, addr, A0), AROS_LHA(int, len, D0), AROS_LHA(int, type, D1),
    struct TaskBase *, taskBase, 36, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)addr; (void)len; (void)type; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH1(struct netent *, getnetbyname,
    AROS_LHA(char *, name, A0),
    struct TaskBase *, taskBase, 37, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)name; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH2(struct netent *, getnetbyaddr,
    AROS_LHA(long, net, D0), AROS_LHA(int, type, D1),
    struct TaskBase *, taskBase, 38, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)net; (void)type; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH2(struct servent *, getservbyname,
    AROS_LHA(char *, name, A0), AROS_LHA(char *, proto, A1),
    struct TaskBase *, taskBase, 39, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)name; (void)proto; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH2(struct servent *, getservbyport,
    AROS_LHA(int, port, D0), AROS_LHA(char *, proto, A0),
    struct TaskBase *, taskBase, 40, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)port; (void)proto; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH1(struct protoent *, getprotobyname,
    AROS_LHA(char *, name, A0),
    struct TaskBase *, taskBase, 41, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)name; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH1(struct protoent *, getprotobynumber,
    AROS_LHA(int, proto, D0),
    struct TaskBase *, taskBase, 42, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)proto; return NULL; AROS_LIBFUNC_EXIT }

AROS_LH3(void, vsyslog,
    AROS_LHA(int, level, D0), AROS_LHA(const char *, format, A0), AROS_LHA(LONG *, args, A1),
    struct TaskBase *, taskBase, 43, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)level; (void)format; (void)args; AROS_LIBFUNC_EXIT }

AROS_LH2(int, Dup2Socket,
    AROS_LHA(int, fd1, D0), AROS_LHA(int, fd2, D1),
    struct TaskBase *, taskBase, 44, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)fd1; (void)fd2; return -1; AROS_LIBFUNC_EXIT }

AROS_LH3(int, sendmsg,
    AROS_LHA(int, s, D0), AROS_LHA(const struct msghdr *, msg, A0), AROS_LHA(int, flags, D1),
    struct TaskBase *, taskBase, 45, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)s; (void)msg; (void)flags; return -1; AROS_LIBFUNC_EXIT }

AROS_LH3(int, recvmsg,
    AROS_LHA(int, s, D0), AROS_LHA(struct msghdr *, msg, A0), AROS_LHA(int, flags, D1),
    struct TaskBase *, taskBase, 46, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)s; (void)msg; (void)flags; return -1; AROS_LIBFUNC_EXIT }

AROS_LH2(int, gethostname,
    AROS_LHA(char *, name, A0), AROS_LHA(int, namelen, D0),
    struct TaskBase *, taskBase, 47, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)name; (void)namelen; return -1; AROS_LIBFUNC_EXIT }

AROS_LH0(long, gethostid,
    struct TaskBase *, taskBase, 48, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; return 0; AROS_LIBFUNC_EXIT }

AROS_LH1(ULONG, SocketBaseTagList,
    AROS_LHA(struct TagItem *, tagList, A0),
    struct TaskBase *, taskBase, 49, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)tagList; return 0; AROS_LIBFUNC_EXIT }   /* TODO: SBTC_* */

AROS_LH1(LONG, GetSocketEvents,
    AROS_LHA(ULONG *, eventsp, A0),
    struct TaskBase *, taskBase, 50, BSDSocket)
{ AROS_LIBFUNC_INIT (void)taskBase; (void)eventsp; return -1; AROS_LIBFUNC_EXIT }
