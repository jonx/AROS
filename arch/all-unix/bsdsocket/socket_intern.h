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

#define SOF_NBIO 0x0001     /* non-blocking (always set on darwin) */

#endif /* SOCKET_INTERN_H */
