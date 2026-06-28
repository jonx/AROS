/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library init/cleanup — resolve libSystem (BSD sockets) and
          libbsdsockhost.dylib (the kqueue readiness pump) via hostlib.resource.
*/

#include <aros/symbolsets.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/hostlib.h>

#include "bsdsocket_intern.h"

/* Symbol order MUST match struct HostSockIFace (bsdsocket_host.h). */
static const char *const sys_symbols[] =
{
    "socket", "bind", "listen", "accept", "connect", "send", "recv", "sendto",
    "recvfrom", "shutdown", "setsockopt", "getsockopt", "getsockname",
    "getpeername", "close", "__error",
    NULL
};

/* Symbol order MUST match struct HostPumpIFace (bsdsocket_host.h). */
static const char *const pump_symbols[] =
{
    "pump_start", "pump_stop", "pump_register", "pump_unregister", "pump_drain",
    "pump_wake_kqueue", "ps_create_cb", "ps_destroy", "hs_set_nonblock", "hs_socket",
    NULL
};

#define HostLibBase (SocketBase->hostlib)

static int bsdsocket_Init(struct bsdsocketBase *SocketBase)
{
    ULONG errcount = 0;

    SocketBase->hostlib = OpenResource("hostlib.resource");
    if (!SocketBase->hostlib)
        return FALSE;

    SocketBase->DOSBase = OpenLibrary("dos.library", 0);
    if (!SocketBase->DOSBase)
        return FALSE;

    InitSemaphore(&SocketBase->lock);
    NEWLIST((struct List *)&SocketBase->socks);
    SocketBase->tasks = NULL;

    /* dlopen the host libs under Disable() so any thread the pump spawns inherits a
       BLOCKED scheduler-signal (SIGALRM) mask — otherwise a host thread could catch
       SIGALRM and run the AROS scheduler on a non-AROS thread. Same discipline as
       arch/all-darwin/hidd/cocoa/cocoa_hostlib.c. */
    Disable();
    SocketBase->libc = HostLib_Open("libSystem.dylib", NULL);
    SocketBase->pumphandle = HostLib_Open("libbsdsockhost.dylib", NULL);
    Enable();

    if (!SocketBase->libc || !SocketBase->pumphandle)
    {
        D(bug("[bsdsocket] HostLib_Open failed (libc=%p pump=%p)\n",
              SocketBase->libc, SocketBase->pumphandle));
        return FALSE;
    }

    SocketBase->sys = (struct HostSockIFace *)
        HostLib_GetInterface(SocketBase->libc, (char **)sys_symbols, &errcount);
    if (!SocketBase->sys || errcount)
    {
        D(bug("[bsdsocket] libSystem: %u symbols unresolved\n", errcount));
        return FALSE;
    }

    errcount = 0;
    SocketBase->pump = (struct HostPumpIFace *)
        HostLib_GetInterface(SocketBase->pumphandle, (char **)pump_symbols, &errcount);
    if (!SocketBase->pump || errcount)
    {
        D(bug("[bsdsocket] libbsdsockhost: %u symbols unresolved\n", errcount));
        return FALSE;
    }

    /* Start the kqueue pump thread (under Disable for the SIGALRM-mask reason). */
    Disable();
    HostLib_Lock();
    errcount = (ULONG)SocketBase->pump->pump_start();
    HostLib_Unlock();
    Enable();
    if (errcount != 0)
    {
        D(bug("[bsdsocket] pump_start failed\n"));
        return FALSE;
    }

    D(bug("[bsdsocket] init OK (libSystem + kqueue pump up)\n"));
    return TRUE;
}

static int bsdsocket_Cleanup(struct bsdsocketBase *SocketBase)
{
    if (!SocketBase->hostlib)
        return TRUE;

    if (SocketBase->pump)
    {
        HostLib_Lock();
        SocketBase->pump->pump_stop();
        HostLib_Unlock();
    }

    if (SocketBase->sys)
        HostLib_DropInterface((APTR *)SocketBase->sys);
    if (SocketBase->pump)
        HostLib_DropInterface((APTR *)SocketBase->pump);
    if (SocketBase->libc)
        HostLib_Close(SocketBase->libc, NULL);
    if (SocketBase->pumphandle)
        HostLib_Close(SocketBase->pumphandle, NULL);
    if (SocketBase->DOSBase)
        CloseLibrary(SocketBase->DOSBase);

    return TRUE;
}

ADD2INITLIB(bsdsocket_Init, 0);
ADD2EXPUNGELIB(bsdsocket_Cleanup, 0);
