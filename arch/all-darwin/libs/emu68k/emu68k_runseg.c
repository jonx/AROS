/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Emu68k_RunSeg - run a 68k hunk program (raw image in the launch
          context) through the host execution service, as the calling
          process: output streams to the process console, CTRL-C kills at
          the next safe point, quanta keep the system scheduling.
*/

#include <aros/libcall.h>
#include <aros/asmcall.h>
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "emu68k_intern.h"
#include LC_LIBDEFS_FILE

#include <aros/debug.h>

void Emu68k_OSCallEndRun(APTR guest0);
void Emu68k_OSCallPreopen(void);
int Emu68k_OSCall(const char *libname, int lvo, APTR regs, APTR guest0,
                  APTR user, char *err, ULONG errlen);

/* dispatcher roundtrips per quantum: small enough that CTRL-C and other tasks
 * stay responsive, large enough that the lock traffic is noise */
#define EMU68K_QUANTUM 4096

/* The stack a run gets.
 *
 * A bridge call is not a shallow one: the chain is this process's stack, plus
 * the engine's frames, plus the entire native implementation of whatever vector
 * the 68k program called - and for a drawing vector that is intuition into
 * graphics into layers into the display driver into the host. A shell's default
 * stack is around 40K and does not cover it, and nothing says so, because AROS
 * stacks have no guard page: the overflow quietly writes through whatever lies
 * below and the machine stops being able to do anything some time later. The
 * caller cannot be expected to know how deep a guest's library calls will go,
 * so this library brings the stack itself. */
#define EMU68K_STACK_SIZE (512 * 1024)

/* Run the guest to completion. Called through NewStackSwap, so every frame
 * below this one - the engine, the bridge, and the native library it calls -
 * is on the stack allocated for the run. StackSwap() is the older spelling and
 * is documented as unreliable on hosted builds with stack checking, which this
 * is: it swapped the bookkeeping without moving the machine anywhere useful. */
static AROS_UFH4(int, emu68k_run_to_completion,
                 AROS_UFHA(struct Emu68kBase *, Emu68kBase, A0),
                 AROS_UFHA(emu68k_run_h, run, A1),
                 AROS_UFHA(unsigned int *, d0p, A2),
                 AROS_UFHA(char *, err, A3))
{
    AROS_USERFUNC_INIT
    struct Task *me = FindTask(NULL);
    APTR saved_lower = me->tc_SPLower;
    APTR saved_upper = me->tc_SPUpper;
    int rc;

    /* The engine executes guest code with SP translated into the run's arena.
     * The scheduler's stack probe would see that SP outside the task's bounds
     * and suspend the task for good, mid-run, holding whatever native locks
     * the bridge had taken. Widen the bounds to cover the arena for the
     * duration of the run; native frames still live on the swapped stack. */
    if (Emu68kBase->host.run_guest0)
    {
        APTR g0 = Emu68kBase->host.run_guest0(run);
        if (g0 && g0 < me->tc_SPLower)
            me->tc_SPLower = g0;
    }
    me->tc_SPUpper = (APTR)~(IPTR)0;

    for (;;)
    {
        /* one runner at a time; quanta interleave through the lock */
        ObtainSemaphore(&Emu68kBase->runlock);
        rc = Emu68kBase->host.run_quantum(run, EMU68K_QUANTUM, d0p,
                                          err, 256);
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
        me->tc_SPLower = saved_lower;
        me->tc_SPUpper = saved_upper;
        return rc;
    }

    AROS_USERFUNC_EXIT
}

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

    /* Open the libraries the bridge crosses into BEFORE the guest starts.
     *
     * A library that is not already resident has to be loaded from disk, and
     * that load runs on whoever asked. Inside a bridge call the asker is the
     * engine's callback rather than this process going about its business, and
     * the load does not survive it: a library nothing else had opened yet
     * failed there while opening perfectly from a shell in the same boot.
     * Here we are an ordinary process, so the load is an ordinary one, and
     * from then on every bridge call only bumps a reference. */
    Emu68k_OSCallPreopen();

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
    osctx.device_base = Emu68kBase->host.run_device_base;
    osctx.call_hook = Emu68kBase->host.run_call_hook;
    if (Emu68kBase->host.set_oscall)
        Emu68kBase->host.set_oscall(Emu68k_OSCall, &osctx);

    D(bug("[emu68k.library] run \"%s\" origin=%lu args=%lub\n",
          ctx->elc_Name ? (const char *)ctx->elc_Name : "",
          (unsigned long)ctx->elc_Origin, (unsigned long)argslen));

    {
        struct StackSwapStruct sss;
        struct StackSwapArgs ssa;
        APTR stackmem = AllocMem(EMU68K_STACK_SIZE, MEMF_ANY);

        ssa.Args[0] = (IPTR)Emu68kBase;
        ssa.Args[1] = (IPTR)run;
        ssa.Args[2] = (IPTR)&d0;
        ssa.Args[3] = (IPTR)err;

        if (stackmem)
        {
            sss.stk_Lower   = stackmem;
            sss.stk_Upper   = (IPTR)stackmem + EMU68K_STACK_SIZE;
            sss.stk_Pointer = (APTR)sss.stk_Upper;
            rc = (int)NewStackSwap(&sss, emu68k_run_to_completion, &ssa);
            FreeMem(stackmem, EMU68K_STACK_SIZE);
        }
        else
        {
            /* No memory for a stack of our own is not a reason not to run;
             * it is a reason to say so, because what follows may be the
             * silent overflow this exists to prevent. */
            bug("[emu68k.library] no memory for a %luK run stack; running on "
                "the caller's\n", (unsigned long)(EMU68K_STACK_SIZE / 1024));
            rc = AROS_UFC4(int, emu68k_run_to_completion,
                           AROS_UFCA(struct Emu68kBase *, Emu68kBase, A0),
                           AROS_UFCA(emu68k_run_h, run, A1),
                           AROS_UFCA(unsigned int *, &d0, A2),
                           AROS_UFCA(char *, err, A3));
        }
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
