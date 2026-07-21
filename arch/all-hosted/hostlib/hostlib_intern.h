#ifndef HOSTLIB_INTERN_H
#define HOSTLIB_INTERN_H

#include <exec/nodes.h>
#include <exec/semaphores.h>

#include "hostinterface.h"

/*
 * Windows is a very harsh environment.
 * It requires us to Forbid() in order to call itself.
 *
 * Darwin needs the same, for the same underlying reason: the host process
 * has its own thread world (AppKit main thread, libdispatch workers) taking
 * host-libc locks -- environ (getenv/setenv), locale, malloc arenas. With
 * the semaphore lock a guest task can be PREEMPTED mid-host-call (now that
 * preemption ticks are delivered reliably) while holding such a lock; the
 * suspended task's saved context keeps the lock, the cocoa main thread
 * blocks on it, and any guest task that then dispatch_syncs to the main
 * thread (cm_pump_events) completes a three-way deadlock: whole app frozen
 * at 0% CPU, un-signalable (observed 2026-07-21, environ lock via
 * emul-handler localtime->tzset->getenv under a stat storm). Forbid()
 * makes host calls run-to-completion. This costs nothing on the 1-cpu
 * guest: while the AROS host thread is inside a host call no other guest
 * task can execute anyway -- suspending the caller mid-call is the ONLY
 * thing preemption could do here, and it is never a useful thing.
 */
#if defined(HOST_OS_mingw32) || defined(HOST_OS_darwin)
#define USE_FORBID_LOCK
#endif

struct HostLibBase 
{
    struct Node hlb_Node;
    struct HostInterface *HostIFace;
#ifndef USE_FORBID_LOCK
    struct SignalSemaphore HostSem;
#endif
};

#if defined(USE_FORBID_LOCK)
#define HOSTLIB_LOCK()   Forbid()
#define HOSTLIB_UNLOCK() Permit()
#else
#define HOSTLIB_LOCK()   ObtainSemaphore(&HostLibBase->HostSem)
#define HOSTLIB_UNLOCK() ReleaseSemaphore(&HostLibBase->HostSem)
#endif

#endif
