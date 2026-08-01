/*
    Copyright (C) 1995-2017, The AROS Development Team. All rights reserved.

    Desc: Initialize the interface to the "hardware".
*/

#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <aros/asmcall.h>
#include <aros/atomic.h>
#include <aros/symbolsets.h>
#include <hardware/intbits.h>
#include <proto/exec.h>
#include <proto/hostlib.h>
#include <utility/tagitem.h>
#include <libraries/debug.h>
#include <proto/debug.h>

#define timeval sys_timeval

#define __AROS_KERNEL__

#include "hostinterface.h"
#include "kernel_base.h"
#include "kernel_debug.h"
#include "kernel_globals.h"
#include "kernel_intr.h"
#include "kernel_intern.h"
#include "kernel_interrupts.h"
#include "kernel_scheduler.h"
#include "kernel_unix.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#undef timeval

#define D(x)
#define DSC(x)

#ifdef SIGCORE_NEED_SA_SIGINFO
#define SETHANDLER(sa, h)                       \
    sa.sa_sigaction = h ## _gate;       \
    sa.sa_flags |= SA_SIGINFO
#else
#define SETHANDLER(sa, h)                       \
    sa.sa_handler = h ## _gate;
#endif

/*
 * Resolve a code address to "module symbol + offset" via debug.library and
 * append it to the current bug() line. The bootstrap hands the kernel a
 * KRN_DebugInfo module list, so debug.library has every kickstart module with
 * its ELF symbol tables registered. DecodeLocationA() is explicitly safe in
 * supervisor/crash context: when KrnIsSuper() it skips its semaphore and only
 * walks the (read-only) module list. debug is a LIBRARY (not a resource), so we
 * find its base by name in SysBase->LibList -- a read-only list walk, avoiding
 * OpenLibrary() (which may Wait/alloc and is unsafe in a trap handler). If the
 * library or address is unknown we print nothing extra and the raw address
 * still stands on its own.
 */
static void krnSymbolize(IPTR addr)
{
    static struct Library *DebugBase = NULL;
    char *modname = NULL, *segname = NULL, *symname = NULL;
    void *segaddr = NULL, *symaddr = NULL;
    unsigned int segnum = 0;
    struct TagItem tags[] =
    {
        { DL_ModuleName,    (IPTR)&modname },
        { DL_SegmentNumber, (IPTR)&segnum  },
        { DL_SegmentName,   (IPTR)&segname },
        { DL_SegmentStart,  (IPTR)&segaddr },
        { DL_SymbolName,    (IPTR)&symname },
        { DL_SymbolStart,   (IPTR)&symaddr },
        { TAG_DONE }
    };

    if (!DebugBase)
        DebugBase = (struct Library *)FindName(&SysBase->LibList, "debug.library");
    if (!DebugBase)
        return;

    if (!DecodeLocationA((APTR)addr, tags) || !modname)
        return;

    if (symaddr)
        bug("  %s %s + 0x%x", modname, symname ? symname : "(no symbol)",
            (unsigned int)((IPTR)addr - (IPTR)symaddr));
    else
        bug("  %s seg %d (%s) + 0x%x", modname, (int)segnum,
            segname ? segname : "(unnamed)",
            (unsigned int)((IPTR)addr - (IPTR)segaddr));
}

/*
 * Stop the host process. Used when a CPU fault is unrecoverable: returning
 * from the signal handler would just re-execute the faulting instruction and
 * trap again -- the endless identical-dump loop. _exit() ends the macOS
 * process cleanly so the window closes after a single guru.
 */
static void krnHaltHost(struct PlatformData *pd, int code)
{
    if (pd && pd->iface && pd->iface->_exit)
        pd->iface->_exit(code);
}

static void core_TrapHandler(int sig, regs_t *regs)
{
    static volatile int in_trap   = 0;
    static int          loop_sig  = 0;
    static IPTR         loop_pc   = 0;
    static struct Task *loop_task = NULL;
    struct KernelBase *KernelBase = getKernelBase();
    struct PlatformData *pd = KernelBase->kb_PlatformData;
    const struct SignalTranslation *s;
    short amigaTrap;
    struct AROSCPUContext ctx;
    IPTR pc;
    int fatal;

    SUPERVISOR_ENTER;

    pc = PC(regs);      /* faulting instruction, captured at entry */
    fatal = (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE);
#ifdef SIGSTKFLT
    fatal = fatal || (sig == SIGSTKFLT);
#endif

    /*
     * Loop breaker. The exec trap path (core_Trap -> tc_TrapCode, an inline in
     * kernel_intr.h) hands the fault to a guru/alert routine that ultimately
     * re-executes the SAME faulting instruction -- so the identical fault
     * re-enters this handler forever. Each pass the PC is redirected into the
     * alert routine (pc != PC(regs) below), yet the *faulting* PC never changes.
     * Keyed on that faulting PC, detect the re-entry and stop the host after the
     * first dump instead of spamming. A real recovery would not re-fault at the
     * same PC, so this never fires on forward progress.
     */
    /* The task is part of the key: under the opt-in containment policy a
     * contained (removed) task's successor CAN legitimately fault at the same
     * pc (e.g. the same crashing program run twice); only the SAME task
     * re-faulting at the same instruction is a no-progress loop. */
    if (fatal && sig == loop_sig && pc == loop_pc
        && (SysBase ? (struct Task *)SysBase->ThisTask : NULL) == loop_task)
    {
        bug("[KRN] Trap re-faulting at pc=%p (signal %d) -- unrecoverable; halting host.\n",
            (APTR)(IPTR)pc, sig);
        krnHaltHost(pd, 20);    /* _exit(): does not return */
        SUPERVISOR_LEAVE;       /* only reached if the host _exit was unavailable */
        return;
    }
    loop_sig  = sig;
    loop_pc   = pc;
    loop_task = SysBase ? (struct Task *)SysBase->ThisTask : NULL;

    /*
     * Re-entered while already printing a crash: a second fault hit inside the
     * handler itself (e.g. the backtrace or symbolizer walked into bad memory).
     * Don't recurse -- stop the host process now.
     */
    if (in_trap)
    {
        bug("[KRN] Nested trap (signal %d) during crash handling -- halting host.\n", sig);
        krnHaltHost(pd, 20);
        SUPERVISOR_LEAVE;
        return;
    }
    in_trap = 1;

    /* Just for completeness */
    krnRunIRQHandlers(KernelBase, sig);

    bug("[KRN] Trap signal %d [h2], SysBase %p, KernelBase %p\n", sig, SysBase, KernelBase);
    PRINT_SC(regs);

#ifdef FAULTADDR
    /* The faulting data address + GPRs -- the bad pointer is in one of these, and
     * the fault address says exactly what was touched (NULL? wild? just past a
     * buffer?). Essential for tracking down an out-of-bounds without a sanitizer. */
    {
        int _r;
        bug("[KRN] fault addr=%p ESR=%08x\n", (APTR)(IPTR)FAULTADDR(regs), (unsigned)ESR(regs));
        for (_r = 0; _r < 28; _r += 4)
            bug("[KRN] x%-2d=%p x%-2d=%p x%-2d=%p x%-2d=%p\n",
                _r,   (APTR)(IPTR)Xn(regs, _r),     _r+1, (APTR)(IPTR)Xn(regs, _r+1),
                _r+2, (APTR)(IPTR)Xn(regs, _r+2),   _r+3, (APTR)(IPTR)Xn(regs, _r+3));
        bug("[KRN] x28=%p fp =%p lr =%p sp =%p\n",
            (APTR)(IPTR)Xn(regs, 28), (APTR)(IPTR)FP(regs),
            (APTR)(IPTR)LR(regs), (APTR)(IPTR)SP(regs));
    }
#endif

    /*
     * Call/jump through a NULL (or near-NULL) function pointer: the CPU is
     * executing at (near) address 0, so there is no code at the faulting PC and a
     * bare "pc=0" is opaque. Say so explicitly, and point at the caller -- LR
     * holds the return address of the bad call, i.e. the code that made it.
     */
    if (PC(regs) < 0x1000)
    {
        bug("[KRN] *** Call through a NULL pointer: PC=%p has no code; caller LR=%p",
            (APTR)(IPTR)PC(regs), (APTR)(IPTR)LR(regs));
        krnSymbolize(LR(regs));
        bug("\n");
    }

    /*
     * Stack backtrace (frame-pointer chain). Inlined and self-contained -- no
     * library calls -- so it is safe in a post-crash trap context (calling the
     * kernel's own KrnPrintBacktrace LVO here can re-fault and hang). Modules are
     * built -fno-omit-frame-pointer, so the AAPCS64 frame record at x29 is
     * [saved_fp, return_addr]. Map the printed addresses against the module bases
     * shown by sysdebug=InitResident ("InitResident begin <romtag> (<module>)").
     */
    {
        IPTR *fp = (IPTR *)(IPTR)FP(regs);
        struct Task *t = SysBase ? (struct Task *)SysBase->ThisTask : NULL;
        IPTR stk_lo = 0, stk_hi = 0;
        ULONG i;

        /*
         * A crashed program's frame chain is not to be trusted: one bad link
         * and the walk dereferences a wild pointer, which faults inside the
         * crash handler and takes the whole host down (the nested-trap halt
         * above) instead of reporting the crash. Bound the walk by the
         * faulting task's own stack when that is known, and never step more
         * than one large stack's worth in a single frame.
         */
        if (t && t->tc_SPLower && t->tc_SPUpper > t->tc_SPLower)
        {
            stk_lo = (IPTR)t->tc_SPLower;
            stk_hi = (IPTR)t->tc_SPUpper;
        }

        bug("[KRN] Backtrace (innermost first): pc=%p", (APTR)(IPTR)PC(regs));
        krnSymbolize(PC(regs));
        bug("\n");
        for (i = 0; i < 24 && fp; i++)
        {
            IPTR saved_fp, ret;

            if ((IPTR)fp & 0xF)
                break;
            if (stk_hi && ((IPTR)fp < stk_lo || (IPTR)fp + 2 * sizeof(IPTR) > stk_hi))
                break;

            saved_fp = fp[0];
            ret      = fp[1];
            /* A return address is 4-byte aligned and inside the 47-bit user
             * range; anything else is data the walk has strayed into, and
             * handing it to the symbolizer faults. */
            if (ret < 0x1000 || (ret & 3) || ret >= ((IPTR)1 << 47))
                break;
            bug("[KRN]   <- %p", (APTR)ret);
            krnSymbolize(ret);
            bug("\n");
            if (saved_fp <= (IPTR)fp || (saved_fp & 0xF)
                || saved_fp >= ((IPTR)1 << 47))
                break;
            if (!stk_hi && saved_fp - (IPTR)fp > (16UL << 20))
                break;
            fp = (IPTR *)saved_fp;
        }
    }

    /* Translate UNIX trap number to CPU and exec trap numbers */
    for (s = sigs; s->sig != -1; s++)
    {
        if (sig == s->sig)
            break;
    }

    /*
     * Trap handler expects struct ExceptionContext, so we have to convert regs_t to it.
     * But first initialize all context area to zero, this is important since it may include
     * pointers to FPU state buffers.
     * TODO: FPU state also can be interesting for debuggers, we need to prepare space for it
     * too. Needs to be enclosed in some macros.
     */
    memset(&ctx, 0, sizeof(ctx));
    SAVEREGS(&ctx, regs);
    /* pc (the faulting PC) was captured at handler entry, above. */

    amigaTrap = s->AmigaTrap;
    if (s->CPUTrap != -1)
    {
        if (krnRunExceptionHandlers(KernelBase, s->CPUTrap, &ctx))
            /* Do not call exec trap handler */
            amigaTrap = -1;
    }

    /*
     * Call exec trap handler if needed.
     * Note that it may return, this means that the it has
     * fixed the problem somehow and we may safely continue.
     */
    if (amigaTrap != -1)
        core_Trap(amigaTrap, &ctx);

    /* Trap handler(s) have possibly modified the context, so
       we convert it back before returning */
    RESTOREREGS(&ctx, regs);

    /*
     * The program counter may have been redirected by the exec trap path (into
     * a guru/alert subroutine). Fix up the stack for the redirected entry:
     * - x86_64: the ABI expects (%rsp + 8) % 16 == 0 at a function entry (as
     *   if a return address was just pushed), so push a fake slot.
     * - aarch64 (and others): SP must stay 16-byte ALIGNED at all times; the
     *   hardware faults any SP-relative access otherwise (SP-alignment
     *   SIGBUS, ESR EC 0x26). Do NOT subtract 8 here -- that misalignment is
     *   exactly what used to kill Exec_CrashHandler before it could run, so
     *   every guru turned into a re-fault halt. Just defensively re-align.
     * If this fault is really a non-progressing loop, the loop breaker at the
     * top of this handler stops the host on the next (identical) re-entry.
     */
    if (pc != PC(regs))
    {
#ifdef __x86_64__
        if ((SP(regs) & 0xf) == 0x0) SP(regs) -= 8;
#else
        if (SP(regs) & 0xf) SP(regs) &= ~(IPTR)0xf;
#endif
    }

    in_trap = 0;
    SUPERVISOR_LEAVE;
}

/* T-TICKPROBE: diagnostic counters for interrupt delivery. The preemption
 * model depends on the ITIMER_REAL SIGALRM tick reaching AROS's host thread;
 * on darwin a process-directed signal can land on any thread and the guard
 * below drops off-thread ones on the theory that "the next tick will hit the
 * right thread". These counters measure whether that theory holds under a
 * CPU-bound guest task (see ClockTest / UPSTREAM-NOTES item 38). Plain
 * volatile increments: async-signal-safe enough for a diagnostic ratio. */
static volatile unsigned long core_irq_handled;    /* ran on AROS's thread */
static volatile unsigned long core_irq_dropped;    /* landed elsewhere, dropped */

#ifdef SIGINFO
/*
 * Out-of-band guest diagnostics. Delivered by a host signal (SIGINFO) that the
 * bootstrap main thread blocks, so it converges on AROS's scheduler thread. Its
 * job is to answer "what is every task doing, and where is it stuck?" WITHOUT
 * needing a working shell -- essential when the input chain itself is
 * deadlocked. Read-only: it walks the task lists and symbolizes each task's
 * saved frame; it never mutates exec state. Safe in the wedged case (nothing is
 * running to race the walk); best-effort otherwise. Triggered by
 * `aros-ctl tasks`.
 */
static const char *core_TaskState(UBYTE s)
{
    switch (s)
    {
    case TS_INVALID:  return "INVALID";
    case TS_ADDED:    return "ADDED";
    case TS_RUN:      return "RUN";
    case TS_READY:    return "READY";
    case TS_WAIT:     return "WAIT";
    case TS_EXCEPT:   return "EXCEPT";
    case TS_REMOVED:  return "REMOVED";
    default:          return "?";
    }
}

/* TRUE if [p, p+len) is fully dereferenceable: inside ONE of exec's managed
 * RAM regions. (Module .data/.bss lives outside these mmaps and is missed --
 * acceptable for a best-effort diagnostic.) Every pointer this dump follows
 * is a guess about a task it did not stop; when a guess fails this check the
 * dump must skip it, never fault -- a diagnostic that kills the task it is
 * inspecting is worse than no diagnostic. */
static BOOL core_DiagValidRange(APTR p, size_t len)
{
    struct Node *n;
    if (!p || ((IPTR)p & 3))
        return FALSE;
    for (n = SysBase->MemList.lh_Head; n->ln_Succ; n = n->ln_Succ)
    {
        struct MemHeader *mh = (struct MemHeader *)n;
        if (p >= (APTR)mh->mh_Lower && (APTR)((UBYTE *)p + len) <= mh->mh_Upper)
            return TRUE;
    }
    return FALSE;
}

static BOOL core_DiagValidPtr(APTR p)
{
    return core_DiagValidRange(p, sizeof(IPTR));
}

/* Print cand if it is a valid NT_SIGNALSEM node not yet in seen[].
 * tag+num form the location label: ("x", 19) => "x19", ("sp+", 0x40) => "sp+64". */
#define DIAG_SEM_SEEN_MAX 16
static void core_DiagSemReport(const char *tag, unsigned long num,
                               struct SignalSemaphore *ss,
                               struct SignalSemaphore **seen, int *nseen)
{
    struct Task *owner;
    const char *sname, *oname;
    int j;

    if (!core_DiagValidRange(ss, sizeof(*ss)))
        return;
    if (ss->ss_Link.ln_Type != NT_SIGNALSEM)
        return;
    for (j = 0; j < *nseen; j++)
        if (seen[j] == ss)
            return;
    if (*nseen < DIAG_SEM_SEEN_MAX)
        seen[(*nseen)++] = ss;

    sname = (ss->ss_Link.ln_Name && core_DiagValidPtr(ss->ss_Link.ln_Name))
            ? ss->ss_Link.ln_Name : "(unnamed)";
    owner = ss->ss_Owner;
    oname = (owner && core_DiagValidRange(owner, sizeof(struct Task))
             && owner->tc_Node.ln_Name && core_DiagValidPtr(owner->tc_Node.ln_Name))
            ? owner->tc_Node.ln_Name : (owner ? "(?)" : "none/shared");

    bug("[KRN-DIAG]     %s%lu -> semaphore %p '%s' owner=%p '%s' nest=%d queue=%d\n",
        tag, num, ss, sname, owner, oname,
        (int)ss->ss_NestCount, (int)ss->ss_QueueCount);
}

/*
 * Semaphore forensics for a blocked task. InternalObtainSemaphore keeps the
 * SignalSemaphore pointer live across its Wait() -- sometimes in a callee-saved
 * register (x19-x28), but the compiler may spill it to the stack instead. So
 * scan both: the saved callee-saved registers, then the blocked task's stack
 * from its saved SP up (bounded by tc_SPUpper and a scan cap). Report every
 * distinct word that points at a valid NT_SIGNALSEM node, with its owner --
 * that names the other side of a deadlock.
 */
static void core_DiagSemCandidates(struct Task *t, struct ExceptionContext *regs)
{
    struct SignalSemaphore *seen[DIAG_SEM_SEEN_MAX];
    int nseen = 0;
    int i;

    /* regs is a guess (a saved frame that may never have been filled in, or
     * may be mid-write while this SIGINFO runs). Verify the whole structure
     * is readable before touching any field. */
    if (!core_DiagValidRange(regs, sizeof(*regs)))
    {
        bug("[KRN-DIAG]     (sem scan skipped: reg frame %p unreadable)\n", regs);
        return;
    }

    for (i = 19; i <= 28; i++)
        core_DiagSemReport("x", i, (struct SignalSemaphore *)(IPTR)regs->x[i],
                           seen, &nseen);

    if (regs->sp && !(regs->sp & 7)
        && (APTR)regs->sp >= t->tc_SPLower && (APTR)regs->sp < t->tc_SPUpper)
    {
        IPTR *sp  = (IPTR *)regs->sp;
        IPTR *top = (IPTR *)t->tc_SPUpper;
        if (top - sp > 2048)  /* cap the scan at 16 KB above SP */
            top = sp + 2048;
        /* tc_SPLower/Upper came from the (already-validated) Task, but the
         * window itself must still be mapped exec RAM. */
        if (!core_DiagValidRange(sp, (size_t)((UBYTE *)top - (UBYTE *)sp)))
        {
            bug("[KRN-DIAG]     (sem scan skipped: stack window %p..%p unreadable)\n",
                sp, top);
            return;
        }
        for (; sp < top; sp++)
            core_DiagSemReport("sp+", (unsigned long)((IPTR)sp - regs->sp),
                               (struct SignalSemaphore *)*sp, seen, &nseen);
    }
}

static void core_DiagBacktrace(IPTR pc, IPTR fp)
{
    ULONG i;
    if (pc)
    {
        bug("[KRN-DIAG]     pc=%p", (APTR)pc);
        krnSymbolize(pc);
        bug("\n");
    }
    for (i = 0; i < 16 && fp; i++)
    {
        IPTR saved_fp, ret;
        /* Both words of the frame record must be readable. The first fp can
         * legitimately point outside exec RAM (e.g. the boot task runs on the
         * host stack) -- report and stop rather than dereference. */
        if ((fp & 0xF) || !core_DiagValidRange((APTR)fp, 2 * sizeof(IPTR)))
        {
            bug("[KRN-DIAG]     <- (fp %p unreadable; backtrace unavailable)\n",
                (APTR)fp);
            break;
        }
        saved_fp = ((IPTR *)fp)[0];
        ret      = ((IPTR *)fp)[1];
        if (!ret)
            break;
        bug("[KRN-DIAG]     <- %p", (APTR)ret);
        krnSymbolize(ret);
        bug("\n");
        if (saved_fp <= fp)
            break;
        fp = saved_fp;
    }
}

static void core_DiagTask(struct Task *t, regs_t *live)
{
    if (!core_DiagValidRange(t, sizeof(struct Task)))
    {
        bug("[KRN-DIAG] task %p (unreadable; skipped)\n", t);
        return;
    }

    bug("[KRN-DIAG] task %p '%s' state=%s pri=%d sigWait=%08x sigRecvd=%08x\n",
        t, (t->tc_Node.ln_Name && core_DiagValidPtr(t->tc_Node.ln_Name))
           ? t->tc_Node.ln_Name : "(unnamed)",
        core_TaskState(t->tc_State), (int)(BYTE)t->tc_Node.ln_Pri,
        (unsigned)t->tc_SigWait, (unsigned)t->tc_SigRecvd);

    if (live)
    {
        /* the running task: use the live signal frame */
        core_DiagBacktrace(PC(live), (IPTR)FP(live));
    }
    else if ((t->tc_Flags & TF_ETASK)
             && core_DiagValidRange(t->tc_UnionETask.tc_ETask,
                                    sizeof(struct ETask))
             && core_DiagValidRange(t->tc_UnionETask.tc_ETask->et_RegFrame,
                                    sizeof(struct AROSCPUContext)))
    {
        struct AROSCPUContext *ctx = t->tc_UnionETask.tc_ETask->et_RegFrame;
        core_DiagBacktrace((IPTR)ctx->regs.pc, (IPTR)ctx->regs.fp);
        core_DiagSemCandidates(t, &ctx->regs);
    }
    else if (!live)
    {
        bug("[KRN-DIAG]     (no readable saved frame)\n");
    }
}

static void core_DiagHandler(int sig, regs_t *regs)
{
    struct Task *cur = SysBase->ThisTask;
    struct Task *t;

    bug("[KRN-DIAG] ===== task dump (signal %d) =====\n", sig);
    bug("[KRN-DIAG] IDNestCnt=%d TDNestCnt=%d SysFlags=%04x\n",
        (int)SysBase->IDNestCnt, (int)SysBase->TDNestCnt, (unsigned)SysBase->SysFlags);
    bug("[KRN-DIAG] irq ticks: handled-on-aros-thread=%lu dropped-off-thread=%lu\n",
        core_irq_handled, core_irq_dropped);

    if (cur)
    {
        bug("[KRN-DIAG] -- current --\n");
        core_DiagTask(cur, regs);
    }

    /* SIGINFO is async: a list can be mid-Remove/AddTail right now. Validate
     * each node before reading its ln_Succ, and bound the walk in case a
     * torn link forms a cycle. */
    bug("[KRN-DIAG] -- ready --\n");
    {
        int guard = 0;
        for (t = (struct Task *)SysBase->TaskReady.lh_Head;
             core_DiagValidRange(t, sizeof(struct Task)) && t->tc_Node.ln_Succ
             && guard++ < 256;
             t = (struct Task *)t->tc_Node.ln_Succ)
            core_DiagTask(t, NULL);
    }

    bug("[KRN-DIAG] -- waiting --\n");
    {
        int guard = 0;
        for (t = (struct Task *)SysBase->TaskWait.lh_Head;
             core_DiagValidRange(t, sizeof(struct Task)) && t->tc_Node.ln_Succ
             && guard++ < 256;
             t = (struct Task *)t->tc_Node.ln_Succ)
            core_DiagTask(t, NULL);
    }

    bug("[KRN-DIAG] ===== end task dump =====\n");
}
#endif /* SIGINFO */

static void core_IRQ(int sig, regs_t *sc)
{
    struct KernelBase *KernelBase = getKernelBase();

#ifdef HOST_OS_darwin
    /* If this process-directed signal landed on a host thread that is NOT
     * AROS's (a libdispatch/Metal/AppKit worker), we must not run the
     * scheduler here: touching the global SupervisorCount off AROS's thread
     * races the real AROS task ("ObtainSemaphore called in supervisor
     * mode!!! -> Privilege violation").
     *
     * But DROPPING the tick outright broke preemption entirely: under a
     * CPU-bound guest task, darwin delivered essentially every ITIMER_REAL
     * SIGALRM to some other thread (measured: 0 ticks reached AROS's thread
     * in 10s while ~237/s landed elsewhere -- T-TICKPROBE, UPSTREAM-NOTES
     * item 38), so a task that never blocks was never preempted and wedged
     * the whole guest (ClockTest 'pure'/'clock' hang; every "Feraille
     * freezes" report). FORWARD the signal thread-directed instead:
     * pthread_kill is async-signal-safe (POSIX), a pending classic signal
     * coalesces (no storm), and if the AROS thread has it masked (guest
     * Disable()) it stays pending and lands on the next Enable(). */
    {
        struct PlatformData *pd = KernelBase->kb_PlatformData;
        if (pd->aros_host_thread && pd->iface->pthread_self
            && pd->iface->pthread_self() != pd->aros_host_thread)
        {
            core_irq_dropped++;   /* T-TICKPROBE: now counts forwards */
            if (pd->iface->pthread_kill)
                pd->iface->pthread_kill(pd->aros_host_thread, sig);
            return;
        }
    }
#endif
    core_irq_handled++;

    SUPERVISOR_ENTER;

    /* Just additional protection - what if there's more than 32 signals? */
    if (sig < IRQ_COUNT)
        krnRunIRQHandlers(KernelBase, sig);

    if (UKB(KernelBase)->SupervisorCount == 1)
        core_ExitInterrupt(sc);

    SUPERVISOR_LEAVE;
}

/*
 * This is from sigcore.h - it brings in the definition of the
 * systems initial signal handler, which simply calls
 * sighandler(int signum, regs_t sigcontext)
*/
GLOBAL_SIGNAL_INIT(core_TrapHandler)
GLOBAL_SIGNAL_INIT(core_SysCall)
GLOBAL_SIGNAL_INIT(core_IRQ)
#ifdef SIGINFO
GLOBAL_SIGNAL_INIT(core_DiagHandler)
#endif

/* libc functions that we use */
static const char *kernel_functions[] =
{
    "raise",
    "sigprocmask",
    "sigsuspend",
    "sigaction",
    "mprotect",
    "read",
    "fcntl",
    "mmap",
    "munmap",
#ifdef HOST_OS_linux
    "__errno_location",
#else
#ifdef HOST_OS_android
    "__errno",
#else
    "__error",
#endif
#endif
    "_exit",
#ifdef HOST_OS_darwin
    "sys_icache_invalidate",
#endif
#ifdef HOST_OS_android
    "sigwait",
#else
    "sigemptyset",
    "sigfillset",
    "sigaddset",
    "sigdelset",
#endif
#ifdef HOST_OS_darwin
    "pthread_self",
    "pthread_kill",
#endif
    NULL
};

/*
 * Our post-SINGLETASK initialization code.
 * At this point we are starting up interrupt subsystem.
 * We have hostlib.resource and can use it in order to pick up all needed host OS functions.
 */
int core_Start(void *libc)
{
    struct KernelBase *KernelBase = getKernelBase();
    struct PlatformData *pd = KernelBase->kb_PlatformData;
    APTR HostLibBase;
    struct sigaction sa = {};
    const struct SignalTranslation *s;
    sigset_t tmp_mask;
    ULONG r;

    /* We have hostlib.resource. Obtain the complete set of needed functions. */
    HostLibBase = OpenResource("hostlib.resource");
    if (!HostLibBase)
    {
        krnPanic(KernelBase, "Failed to open hostlib.resource");
        return FALSE;
    }

    pd->iface = (struct KernelInterface *)HostLib_GetInterface(libc, kernel_functions, &r);
    if (!pd->iface)
    {
        krnPanic(KernelBase, "Failed to allocate host-side libc interface");
        return FALSE;
    }

    if (r)
    {
        krnPanic(KernelBase, "Failed to resove %u functions from host libc", r);
        return FALSE;
    }

    /* Cache errno pointer, for context switching */
    pd->errnoPtr = pd->iface->__error();
    AROS_HOST_BARRIER

#ifdef HOST_OS_darwin
    /* Remember which host thread is AROS's. core_Start runs on it, so this is
     * the AROS scheduler thread. The VBlank tick (process-directed SIGALRM from
     * ITIMER_REAL) can be delivered to any host thread whose mask has it
     * unblocked -- including a libdispatch/Metal/AppKit worker that a Cocoa
     * call on AROS's thread spawned with the (unblocked) inherited mask.
     * core_IRQ uses this to drop such a tick instead of running the scheduler /
     * touching SupervisorCount off AROS's thread (the "ObtainSemaphore called
     * in supervisor mode" privilege-violation race). */
    pd->aros_host_thread = pd->iface->pthread_self();
    AROS_HOST_BARRIER
#endif

#if DEBUG
    /* Pass unhandled exceptions to the debugger, if present */
    SIGEMPTYSET(&pd->sig_int_mask);
#else
    /* We only want signal that we can handle at the moment */
    SIGFILLSET(&pd->sig_int_mask);
#endif
    SIGEMPTYSET(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    /*
     * These ones we consider as processor traps.
     * They are not be blocked by KrnCli()
     */
    SETHANDLER(sa, core_TrapHandler);
    for (s = sigs; s->sig != -1; s++)
    {
        pd->iface->sigaction(s->sig, &sa, NULL);
        AROS_HOST_BARRIER
        SIGDELSET(&pd->sig_int_mask, s->sig);
    }

    /* SIGUSRs are software interrupts, we also never block them */
    SIGDELSET(&pd->sig_int_mask, SIGUSR1);
    SIGDELSET(&pd->sig_int_mask, SIGUSR2);
    /* We want to be able to interrupt AROS using Ctrl-C in its console,
       so exclude SIGINT too. */
    SIGDELSET(&pd->sig_int_mask, SIGINT);

    /*
     * Any interrupt including software one must disable
     * all interrupts. Otherwise one interrupt may happen
     * between interrupt handler entry and supervisor count
     * increment. This can cause bad things in cpu_Dispatch()
     */
    sa.sa_mask = pd->sig_int_mask;

    /* Install interrupt handlers */
    SETHANDLER(sa, core_IRQ);
#if DEBUG
    /* Use VTALRM instead of ALRM during debugging, so
     * that stepping though code won't have to deal
     * with constant SIGALRM processing.
     *
     * NOTE: This will cause the AROS clock to march slower
     *       than the host clock in debug builds!
     */
    pd->iface->sigaction(SIGVTALRM, &sa, NULL);
    AROS_HOST_BARRIER
#else
    pd->iface->sigaction(SIGALRM, &sa, NULL);
    AROS_HOST_BARRIER
#endif
    pd->iface->sigaction(SIGIO  , &sa, NULL);
    AROS_HOST_BARRIER

    /* Software IRQs do not need to block themselves. Anyway we know when we send them. */
    sa.sa_flags |= SA_NODEFER;

    pd->iface->sigaction(SIGUSR2, &sa, NULL);
    AROS_HOST_BARRIER

    SETHANDLER(sa, core_SysCall);
    pd->iface->sigaction(SIGUSR1, &sa, NULL);
    AROS_HOST_BARRIER

#ifdef SIGINFO
    /* Out-of-band guest task dump (aros-ctl tasks). Like the SIGUSRs it must not
     * defer itself, and the bootstrap blocks it on the host threads so it
     * converges on AROS's thread. */
    SETHANDLER(sa, core_DiagHandler);
    pd->iface->sigaction(SIGINFO, &sa, NULL);
    AROS_HOST_BARRIER
    SIGDELSET(&pd->sig_int_mask, SIGINFO);
#endif

    /* We need to start up with disabled interrupts */
    pd->iface->sigprocmask(SIG_BLOCK, &pd->sig_int_mask, NULL);
    AROS_HOST_BARRIER

    /*
     * Explicitly make sure that SIGUSR1 and SIGUSR2 are enabled.
     * This effectively kicks DalvikVM's ass on Android and takes SIGUSR1 away
     * from its "signal catcher". On other platforms this at least should not harm.
     * I also added SIGUSR2, just in case.
     */
    SIGEMPTYSET(&tmp_mask);
    SIGADDSET(&tmp_mask, SIGUSR1);
    SIGADDSET(&tmp_mask, SIGUSR2);
    pd->iface->sigprocmask(SIG_UNBLOCK, &tmp_mask, NULL);
    AROS_HOST_BARRIER

    return TRUE;
}

/*
 * All syscalls are mapped to single SIGUSR1. We look at task's tc_State
 * to determine the needed action.
 * This is not inlined because actual SIGUSR1 number is host-specific, it can
 * differ between different UNIX variants, and even between different ports of the same
 * OS (e. g. Linux). We need host OS includes in order to get the proper definition, and
 * we include them only in arch-specific code.
 */
void unix_SysCall(unsigned char n, struct KernelBase *KernelBase)
{
    DSC(bug("[KRN] SysCall %d\n", n));

    KernelBase->kb_PlatformData->iface->raise(SIGUSR1);
    AROS_HOST_BARRIER
}
