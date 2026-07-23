/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Per-socket descriptor for the host-passthrough bsdsocket.library.
*/
#ifndef SOCKET_INTERN_H
#define SOCKET_INTERN_H

#include <exec/nodes.h>
#include <exec/types.h>

struct Socket
{
    struct MinNode n;       /* link in bsdsocketBase->socks  */
    int            s;       /* host (libSystem) fd           */
    ULONG          flags;   /* SOF_*                         */
};

#define SOF_NBIO      0x0001     /* host fd is O_NONBLOCK (always set on darwin)  */
#define SOF_USER_NBIO 0x0002     /* caller asked for non-blocking (FIONBIO true)  */

/* FIONBIO request code for IoctlSocket. The host fd is always O_NONBLOCK; this
   flag only decides whether the library parks on a would-block or reports it. */
#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif

#endif /* SOCKET_INTERN_H */
