/*
    Copyright (C) 1995-2017, The AROS Development Team. All rights reserved.

    Desc: Initialize the interface to the "hardware".
*/

#include <exec/interrupts.h>
#include <exec/execbase.h>
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
    static volatile int in_trap  = 0;
    static int          loop_sig = 0;
    static IPTR         loop_pc  = 0;
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
    if (fatal && sig == loop_sig && pc == loop_pc)
    {
        bug("[KRN] Trap re-faulting at pc=%p (signal %d) -- unrecoverable; halting host.\n",
            (APTR)(IPTR)pc, sig);
        krnHaltHost(pd, 20);    /* _exit(): does not return */
        SUPERVISOR_LEAVE;       /* only reached if the host _exit was unavailable */
        return;
    }
    loop_sig = sig;
    loop_pc  = pc;

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
        ULONG i;
        bug("[KRN] Backtrace (innermost first): pc=%p", (APTR)(IPTR)PC(regs));
        krnSymbolize(PC(regs));
        bug("\n");
        for (i = 0; i < 24 && fp; i++)
        {
            IPTR saved_fp = fp[0];
            IPTR ret      = fp[1];
            if (!ret)
                break;
            bug("[KRN]   <- %p", (APTR)ret);
            krnSymbolize(ret);
            bug("\n");
            if (saved_fp <= (IPTR)fp || (saved_fp & 0xF))
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
     * The program counter may have been redirected by the exec trap path (into a
     * guru/alert subroutine). Align the stack as if a return address was passed,
     * so it stays 16-byte aligned -- necessary for x86_64, harmless elsewhere.
     * If this fault is really a non-progressing loop, the loop breaker at the top
     * of this handler stops the host on the next (identical) re-entry.
     */
    if (pc != PC(regs))
        if ((SP(regs) & 0xf) == 0x0) SP(regs) -= 8;

    in_trap = 0;
    SUPERVISOR_LEAVE;
}

static void core_IRQ(int sig, regs_t *sc)
{
    struct KernelBase *KernelBase = getKernelBase();

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
#ifdef HOST_OS_android
    "sigwait",
#else
    "sigemptyset",
    "sigfillset",
    "sigaddset",
    "sigdelset",
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
