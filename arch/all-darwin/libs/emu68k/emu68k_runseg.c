/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Emu68k_RunSeg - run a 68k hunk program (raw image in the launch
          context) through the host execution service, as the calling
          process: output streams to the process console, CTRL-C kills at
          the next safe point, quanta keep the system scheduling.
*/

#include <aros/libcall.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "emu68k_intern.h"
#include LC_LIBDEFS_FILE

#include <aros/debug.h>

void Emu68k_OSCallEndRun(APTR guest0);
int Emu68k_OSCall(const char *libname, int lvo, APTR regs, APTR guest0,
                  APTR user, char *err, ULONG errlen);

/* dispatcher roundtrips per quantum: small enough that CTRL-C and other tasks
 * stay responsive, large enough that the lock traffic is noise */
#define EMU68K_QUANTUM 4096

struct emu68k_sinkctx
{
    APTR dosbase;
    BPTR out;
};

/* Called by the host engine (same task context) with program output bytes. */
static void emu68k_sink(const char *buf, long len, void *user)
{
    struct emu68k_sinkctx *sc = user;
    APTR DOSBase = sc->dosbase;
    if (sc->out && len > 0)
        Write(sc->out, (APTR)buf, len);
}

AROS_LH2(LONG, Emu68k_RunSeg,
         AROS_LHA(struct Emu68kLaunchCtx *, ctx,    A0),
         AROS_LHA(LONG *,                   result, A1),
         struct Emu68kBase *, Emu68kBase, 5, Emu68k)
{
    AROS_LIBFUNC_INIT

    APTR DOSBase;
    struct Process *me;
    APTR saved_winptr;
    struct emu68k_sinkctx sc;
    struct Emu68kOSCallCtx osctx;
    emu68k_run_h run;
    char err[256];
    unsigned int d0 = 0;
    ULONG argslen;
    int rc;
    LONG ran = DOSFALSE;

    if (!ctx || ctx->elc_Version < 2 || !ctx->elc_Image || !ctx->elc_ImageSize)
        return DOSFALSE;
    if (!Emu68kBase->host_ok)
        return DOSFALSE;

    DOSBase = OpenLibrary("dos.library", 36);
    if (!DOSBase)
        return DOSFALSE;

    /* A 68k program's dos calls are performed by THIS process, so a path it
     * cannot resolve raises the usual AmigaDOS "please insert volume"
     * requester - and a guest that is being driven headlessly, or from a
     * script, then blocks inside a native call forever, somewhere the run's
     * own wall-clock guard cannot reach. -1 is the standard way to say "fail
     * the call instead of asking"; the guest sees an ordinary error, which is
     * what it is equipped to handle. Restored before returning. */
    me = (struct Process *)FindTask(NULL);
    saved_winptr = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;

    sc.dosbase = DOSBase;
    sc.out     = Output();

    /* the host service appends the AmigaDOS trailing newline itself */
    argslen = ctx->elc_ArgSize;
    if (argslen && ctx->elc_Args && ctx->elc_Args[argslen - 1] == '\n')
        argslen--;

    /* [T2] AUTO routing: ask the static scan first. A program that plainly
     * drives the Amiga hardware cannot be served by translation, so say so
     * instead of running it into a fault. Anything else runs, with the runtime
     * guard as the authority. */
    err[0] = 0;
    if (ctx->elc_Mode == 0 && Emu68kBase->host.scan_image &&
        Emu68kBase->host.scan_image(ctx->elc_Image, ctx->elc_ImageSize,
                                    err, sizeof err))
    {
        if (sc.out)
        {
            FPuts(sc.out, "This program needs a full Amiga emulator: ");
            FPuts(sc.out, err);
            FPuts(sc.out, "\n");
        }
        bug("[emu68k.library] \"%s\": routed FULL (%s)\n",
            ctx->elc_Name ? (const char *)ctx->elc_Name : "", err);
        *result = RETURN_FAIL;
        me->pr_WindowPtr = saved_winptr;
        CloseLibrary(DOSBase);
        return DOSTRUE;               /* handled: a routing decision, not a decline */
    }

    err[0] = 0;
    run = Emu68kBase->host.run_new(ctx->elc_Image, ctx->elc_ImageSize,
                                   (const char *)ctx->elc_Args, argslen,
                                   emu68k_sink, &sc, err, sizeof err);
    if (!run)
    {
        /* Say so. A silent load failure is indistinguishable from a program
         * that ran and printed nothing, which is exactly how a loader bug hid
         * behind a row of "quiet" programs in the corpus sweeps. */
        bug("[emu68k.library] load failed for \"%s\": %s\n",
            ctx->elc_Name ? (const char *)ctx->elc_Name : "", err);
        if (sc.out)
        {
            FPuts(sc.out, "emu68k: cannot load this 68k program: ");
            FPuts(sc.out, err);
            FPuts(sc.out, "\n");
        }
        *result = RETURN_FAIL;
        me->pr_WindowPtr = saved_winptr;
        CloseLibrary(DOSBase);
        return DOSTRUE;                  /* handled: reported, not silently declined */
    }
    if (ctx->elc_Name)
        Emu68kBase->host.run_set_name(run, (const char *)ctx->elc_Name);

    /* [T3] Library calls the engine cannot serve itself come back to us. The
     * context carries DOSBase plus the current run's guest allocator, which is
     * how native-created readable façades receive real guest addresses. */
    osctx.dosbase = DOSBase;
    osctx.run = run;
    osctx.guest_alloc = Emu68kBase->host.run_guest_alloc;
    osctx.call_hook = Emu68kBase->host.run_call_hook;
    if (Emu68kBase->host.set_oscall)
        Emu68kBase->host.set_oscall(Emu68k_OSCall, &osctx);

    D(bug("[emu68k.library] run \"%s\" origin=%lu args=%lub\n",
          ctx->elc_Name ? (const char *)ctx->elc_Name : "",
          (unsigned long)ctx->elc_Origin, (unsigned long)argslen));

    for (;;)
    {
        /* one runner at a time; quanta interleave through the lock */
        ObtainSemaphore(&Emu68kBase->runlock);
        rc = Emu68kBase->host.run_quantum(run, EMU68K_QUANTUM, &d0,
                                          err, sizeof err);
        ReleaseSemaphore(&Emu68kBase->runlock);

        if (rc == EMU68K_RC_YIELD)
        {
            if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            {
                SetSignal(0, SIGBREAKF_CTRL_C);
                Emu68kBase->host.run_kill(run);
            }
            continue;
        }
        break;
    }

    switch (rc)
    {
    case EMU68K_RC_DONE:
        /* a shell-meaningful return code, exactly as the host CLI clamps it
         * (the full D0 is a 32-bit value; rc semantics are 0..255) */
        *result = (LONG)(d0 & 0xFF);
        ran = DOSTRUE;
        break;
    case EMU68K_RC_KILLED:
        if (sc.out)
            FPuts(sc.out, "***Break\n");
        *result = RETURN_FAIL;
        ran = DOSTRUE;
        break;
    case EMU68K_RC_HARDWARE:
        /* the runtime guard: it wanted the hardware after all (an address the
         * static scan could not see). Same answer, now with the exact register. */
        bug("[emu68k.library] \"%s\": hardware event: %s\n",
            ctx->elc_Name ? (const char *)ctx->elc_Name : "", err);
        if (sc.out)
        {
            FPuts(sc.out, "This program needs a full Amiga emulator: it ");
            FPuts(sc.out, err);
            FPuts(sc.out, "\n");
        }
        *result = RETURN_FAIL;
        ran = DOSTRUE;
        break;
    default:
        /* it ran and died: a real result, not a decline */
        bug("[emu68k.library] \"%s\": %s\n",
            ctx->elc_Name ? (const char *)ctx->elc_Name : "", err);
        if (sc.out)
        {
            FPuts(sc.out, "emu68k: ");
            FPuts(sc.out, err);
            FPuts(sc.out, "\n");
        }
        *result = RETURN_FAIL;
        ran = DOSTRUE;
        break;
    }

    /* Drop the bridge's per-run state (file handles, any directory scan the
     * program left open) BEFORE the arena goes away: it is keyed on the run's
     * own base, and that address becomes reusable the moment the run is freed. */
    if (Emu68kBase->host.run_guest0)
        Emu68k_OSCallEndRun(Emu68kBase->host.run_guest0(run));
    Emu68kBase->host.run_free(run);
    me->pr_WindowPtr = saved_winptr;
        CloseLibrary(DOSBase);
    return ran;

    AROS_LIBFUNC_EXIT
}
