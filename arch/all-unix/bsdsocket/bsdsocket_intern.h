/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Internal definitions for the host-passthrough bsdsocket.library
          (aarch64-darwin / hosted-unix). Global base + per-task base.
*/
#ifndef BSDSOCKET_INTERN_H
#define BSDSOCKET_INTERN_H

#include <aros/debug.h>
#include <aros/libcall.h>
#include <exec/avl.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <utility/tagitem.h>

#include "bsdsocket_host.h"

struct Socket;
struct TaskBase;

/* The global library base — one per system. Holds the host-library handles and
   the resolved interfaces. bsdsocket.library is unusual: OpenLibrary() returns a
   PER-TASK TaskBase (below), not this; this is reached via taskBase->glob. */
struct bsdsocketBase
{
    struct Library lib;            /* Standard header                     */
    APTR hostlib;                  /* hostlib.resource base (the #define
                                      HostLibBase points here; the field is
                                      lowercased to avoid clashing with it) */
    APTR DOSBase;                  /* dos.library — for Delay() poll-park  */
    APTR libc;                     /* libSystem.dylib handle              */
    APTR pumphandle;               /* libbsdsockhost.dylib handle         */
    struct HostSockIFace *sys;     /* libSystem socket fns                */
    struct HostPumpIFace *pump;    /* our kqueue readiness pump           */
    struct AVLNode *tasks;         /* TaskBase tree keyed by Task         */
    struct MinList socks;          /* all open Sockets (debug/cleanup)    */
    struct SignalSemaphore lock;   /* guards tasks + socks                */
};

struct TaskNode
{
    struct AVLNode node;
    struct Task *task;
    struct TaskBase *self;
};

/* The per-task base. Each task doing socket I/O OpenLibrary()s its own; the fd
   table, errno and signal masks are per-task (AmiTCP contract). */
struct TaskBase
{
    struct Library lib;            /* Standard header                      */
    struct TaskNode n;             /* AVL link + lookup                    */
    struct bsdsocketBase *glob;    /* the global SocketBase                */
    APTR pool;                     /* per-task memory pool                 */
    void *errnoPtr;                /* errno storage (redirectable)         */
    int errnoSize;
    ULONG errnoVal;                /* default errno storage                */
    ULONG sigintr;                 /* SIGBREAKF_CTRL_C by default          */
    ULONG sigio;
    ULONG sigurg;
    ULONG dTableSize;              /* descriptor table size                */
    struct Socket **dTable;        /* AROS fd -> Socket                    */
    APTR psig;                     /* PumpSig* (ps_create_cb) wake target  */
};

#endif /* BSDSOCKET_INTERN_H */
