/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library Open/Close — the per-task SocketBase (TaskBase). Each
          task gets its own base: its own dTable, errno and signal masks, and its
          own kqueue-pump wake target (PumpSig). Ported from the host-neutral
          mingw32 BSDSocket_OpenLib/CloseLib.
*/

#include <aros/asmcall.h>
#include <aros/symbolsets.h>
#include <proto/exec.h>
#include <proto/hostlib.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"
#include "socket_intern.h"

/* proto/hostlib.h's stubs use HostLibBase as the resource base; both LVO funcs
   here have `SocketBase` (the global base) in scope. */
#define HostLibBase (SocketBase->hostlib)

extern APTR BSDSocket_FuncTable[];

#define DEFAULT_DTABLESIZE 64       /* AmiTCP FD_SETSIZE */

/* The kqueue pump (host thread) calls this on readiness. On darwin we DO NOT
   Signal from the host thread (R-DARWIN-WAKE) — the AROS side polls pump_drain on a
   timer tick — so this is a deliberate no-op; the readiness is already stashed for
   pump_drain before this runs. Runs on the host pump thread: must touch no AROS
   state (it doesn't). */
static void bsd_pump_wake(APTR cookie)
{
    (void)cookie;
}

static AROS_UFH2(LONG, TaskKeyCompare,
                 AROS_UFHA(const struct AVLNode *, td, A0),
                 AROS_UFHA(AVLKey, key, A1))
{
    AROS_USERFUNC_INIT
    struct Task *id = ((struct TaskNode *)td)->task;
    if (id == (struct Task *)key) return 0;
    else if (id < (struct Task *)key) return -1;
    else return 1;
    AROS_USERFUNC_EXIT
}

static AROS_UFH2(LONG, TaskNodeCompare,
                 AROS_UFHA(const struct AVLNode *, td1, A0),
                 AROS_UFHA(const struct AVLNode *, td2, A1))
{
    AROS_USERFUNC_INIT
    IPTR t1 = (IPTR)((struct TaskNode *)td1)->task;
    IPTR t2 = (IPTR)((struct TaskNode *)td2)->task;
    if (t1 == t2) return 0;
    else if (t1 < t2) return -1;
    else return 1;
    AROS_USERFUNC_EXIT
}

AROS_LH1(struct TaskBase *, BSDSocket_OpenLib,
         AROS_LHA(ULONG, version, D0),
         struct bsdsocketBase *, SocketBase, 1, BSDSocket)
{
    AROS_LIBFUNC_INIT

    struct TaskBase *tb;
    struct Task *task = FindTask(NULL);
    struct TaskNode *tn;

    ObtainSemaphore(&SocketBase->lock);
    tn = (struct TaskNode *)AVL_FindNode(SocketBase->tasks, task, TaskKeyCompare);

    if (tn)
    {
        tb = tn->self;
    }
    else
    {
        APTR pool = CreatePool(MEMF_ANY, 2048, 1024);
        if (!pool)
        {
            ReleaseSemaphore(&SocketBase->lock);
            return NULL;
        }

        tb = (struct TaskBase *)MakeLibrary(BSDSocket_FuncTable, NULL, NULL,
                                            sizeof(struct TaskBase), NULL);
        if (!tb)
        {
            DeletePool(pool);
            ReleaseSemaphore(&SocketBase->lock);
            return NULL;
        }

        tb->lib.lib_Node.ln_Name = SocketBase->lib.lib_Node.ln_Name;
        tb->lib.lib_Node.ln_Type = NT_LIBRARY;
        tb->lib.lib_Node.ln_Pri  = SocketBase->lib.lib_Node.ln_Pri;
        tb->lib.lib_Flags        = LIBF_CHANGED;
        tb->lib.lib_Version      = SocketBase->lib.lib_Version;
        tb->lib.lib_Revision     = SocketBase->lib.lib_Revision;
        tb->lib.lib_IdString     = SocketBase->lib.lib_IdString;
        SumLibrary(&tb->lib);

        tb->n.task    = task;
        tb->n.self    = tb;
        tb->glob      = SocketBase;
        tb->pool      = pool;
        tb->errnoPtr  = &tb->errnoVal;
        tb->errnoSize = sizeof(tb->errnoVal);
        tb->sigintr   = SIGBREAKF_CTRL_C;
        tb->dTable    = NULL;
        tb->dTableSize = 0;

        SetDTableSize(DEFAULT_DTABLESIZE, tb);

        /* This task's kqueue-pump wake target (R-DARWIN-WAKE: cb is a no-op; the
           park polls pump_drain). */
        HostLib_Lock();
        tb->psig = SocketBase->pump->ps_create_cb(bsd_pump_wake, tb);
        HostLib_Unlock();
        if (!tb->psig)
        {
            DeletePool(pool);
            ReleaseSemaphore(&SocketBase->lock);
            return NULL;
        }

        AVL_AddNode(&SocketBase->tasks, &tb->n.node, TaskNodeCompare);
    }

    tb->lib.lib_OpenCnt++;
    SocketBase->lib.lib_OpenCnt++;
    ReleaseSemaphore(&SocketBase->lock);

    return tb;

    AROS_LIBFUNC_EXIT
}

AROS_LH0(BPTR, BSDSocket_CloseLib,
         struct TaskBase *, tb, 2, BSDSocket)
{
    AROS_LIBFUNC_INIT

    struct bsdsocketBase *SocketBase = tb->glob;

    ObtainSemaphore(&SocketBase->lock);

    tb->lib.lib_OpenCnt--;
    SocketBase->lib.lib_OpenCnt--;

    if (!tb->lib.lib_OpenCnt)
    {
        APTR addr;
        ULONG i;

        for (i = 0; i < tb->dTableSize; i++)
            if (tb->dTable[i])
                IntCloseSocket((int)i, tb);

        if (tb->psig)
        {
            HostLib_Lock();
            SocketBase->pump->ps_destroy(tb->psig);
            HostLib_Unlock();
        }

        AVL_RemNodeByAddress(&SocketBase->tasks, &tb->n.node);
        DeletePool(tb->pool);

        addr = (APTR)((IPTR)tb - tb->lib.lib_NegSize);
        FreeMem(addr, tb->lib.lib_NegSize + tb->lib.lib_PosSize);
    }

    ReleaseSemaphore(&SocketBase->lock);
    return (BPTR)NULL;

    AROS_LIBFUNC_EXIT
}
