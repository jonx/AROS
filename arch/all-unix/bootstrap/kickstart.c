/*
    Copyright (C) 1995-2014, The AROS Development Team. All rights reserved.
*/

#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__APPLE__)
/*
 * Host pthread/signal API declared by hand. <pthread.h>/<signal.h> in the
 * bootstrap's include path resolve to AROS's own (pthread.library etc.), not
 * the host's, so we cannot include them here. These prototypes/constants match
 * the macOS/arm64 ABI: pthread_t is a pointer, pthread_attr_t is a 64-byte
 * opaque blob, sigset_t is a 32-bit mask with bit (signo-1) set per signal.
 */
typedef void          *host_pthread_t;
typedef unsigned int   host_sigset_t;
extern int pthread_create(host_pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(host_pthread_t, void **);
extern int pthread_attr_init(void *);
extern int pthread_attr_setstacksize(void *, unsigned long);
extern int pthread_attr_destroy(void *);
extern int pthread_sigmask(int, const host_sigset_t *, host_sigset_t *);
#define HOST_SIG_BLOCK    1
#define HOST_SIG_UNBLOCK  2
#define HOST_SIGALRM     14
#define HOST_SIGIO       23
#define HOST_SIGVTALRM   26
#define HOST_SIGUSR1     30
#define HOST_SIGUSR2     31
#if defined(__APPLE__)
#define HOST_SIGINFO     29    /* BSD/darwin: the out-of-band task-dump signal */
#endif
static host_sigset_t aros_signal_mask(void)
{
    host_sigset_t m = (1u << (HOST_SIGALRM   - 1)) | (1u << (HOST_SIGVTALRM - 1)) |
                      (1u << (HOST_SIGIO     - 1)) | (1u << (HOST_SIGUSR1   - 1)) |
                      (1u << (HOST_SIGUSR2   - 1));
#if defined(HOST_SIGINFO)
    m |= (1u << (HOST_SIGINFO - 1));
#endif
    return m;
}

/* CoreFoundation run loop, declared by hand (link -framework CoreFoundation).
 * The windowed shell pumps this on the main thread so cocoametal's NSWindow --
 * created on the main thread via the cm_* main-thread hop -- is serviced and the
 * main dispatch queue (which those hops use) runs. cocoametal itself owns all
 * NSApplication/window setup; the shell only has to keep the run loop turning. */
typedef const void *host_CFStringRef;
extern host_CFStringRef kCFRunLoopDefaultMode;
extern int CFRunLoopRunInMode(host_CFStringRef mode, double seconds,
                              unsigned char returnAfterSourceHandled);
#endif

/* These macros are defined in both UNIX and AROS headers. Get rid of warnings. */
#undef __pure
#undef __const
#undef __pure2
#undef __deprecated

#include <aros/kernel.h>
#include <runtime.h>

#include "kickstart.h"
#include "platform.h"

#define D(x)

/*
 * This is the UNIX-hosted kicker. Theory of operation:
 * We want to run the loaded code multiple times, every time as a new process (in order
 * to drop all open file descriptors etc).
 * We have already loaded our kickstart and allocated RAM. Now we use fork() to mark
 * the point where we are started.
 * AROS is executed inside child process. The parent just sits and waits for the return
 * code.
 * When AROS shuts down, it sets exit status in order to indicate a reason. There are
 * three status codes:
 * 1. Shutdown
 * 2. Cold reboot.
 * 3. Warm reboot.
 * We pick up this code and see what we need to do. Warm reboot means just creating a
 * new AROS process using the same kickstart and RAM image. When the RAM is made shared,
 * this will effectively keep KickTags etc.
 * Cold reboot is the same as before, re-running everything from scratch.
 * Shutdown is just plain exit.
 */
#if defined(__APPLE__)
/*
 * The "engine" entry: run AROS's kernel. Which thread it runs ON is the host
 * shell's choice -- the main thread (default no-fork) or a dedicated thread
 * (AROS_DARWIN_THREADED, the model the Cocoa-app shell + a future libAROS use,
 * so the host keeps the main thread for AppKit/Metal). AROS's scheduler/IO
 * signals are masked on the host's threads so process-directed delivery (e.g.
 * ITIMER_REAL's SIGALRM) can only land on AROS's thread, never a
 * libdispatch/Metal worker.
 */
struct aros_engine_ctx { kernel_entry_fun_t addr; struct TagItem *msg; int ret; volatile int done; };

static void *aros_engine_thread(void *p)
{
    struct aros_engine_ctx *c = p;
    host_sigset_t mask = aros_signal_mask();
    /* This is AROS's thread: accept AROS's signals here (the host blocked them
     * on its threads, so process-directed delivery converges on us). */
    pthread_sigmask(HOST_SIG_UNBLOCK, &mask, NULL);
    fprintf(stderr, "[Bootstrap] AROS running on dedicated thread...\n");
    Host_PreBoot();
    c->ret = c->addr(c->msg, AROS_BOOT_MAGIC);
    c->done = 1;
    return NULL;
}
#endif

int kick(kernel_entry_fun_t addr, struct TagItem *msg)
{
    int i;

#if defined(__APPLE__)
    /*
     * Opt-in via the AROS_DARWIN_THREADED env var (set by the windowed host
     * shell): run AROS on a dedicated thread while THIS (main) thread stays free
     * for the shell to own (AppKit/Metal need the main thread). The main thread
     * blocks AROS's scheduler signals so they are delivered only to AROS's
     * thread -- the containment that lets AROS and Cocoa/Metal coexist in one
     * process. No fork() => Metal's XPC services work; warm/cold reboot is
     * unavailable. Default (env var unset) stays the known-good fork boot below.
     */
    if (getenv("AROS_DARWIN_THREADED"))
    {
        static struct aros_engine_ctx ctx;
        host_pthread_t t;
        unsigned char attr[64];                 /* opaque pthread_attr_t storage */
        host_sigset_t mask = aros_signal_mask();

        pthread_sigmask(HOST_SIG_BLOCK, &mask, NULL);  /* host thread: hands AROS's signals to AROS */

        ctx.addr = addr; ctx.msg = msg; ctx.ret = -1;
        pthread_attr_init(attr);
        pthread_attr_setstacksize(attr, 32UL * 1024 * 1024);
        if (pthread_create(&t, attr, aros_engine_thread, &ctx) != 0)
        {
            DisplayError("Failed to start AROS thread!");
            return -1;
        }
        pthread_attr_destroy(attr);

        /*
         * Host shell: this (main) thread now drives the host run loop instead of
         * blocking in a join. cocoametal opens its NSWindow on THIS thread (the
         * cm_* main-thread hop dispatches onto the main run loop), so we must
         * keep pumping it for the window to appear, draw and stay responsive --
         * and to service the main dispatch queue the hops use. Pumping is also
         * harmless headless (no window => nothing to service). Loop until AROS
         * exits; if AROS runs forever (a live desktop), so do we.
         */
        while (!ctx.done)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, 1);
        pthread_join(t, NULL);
        return ctx.ret;
    }
#endif

#if defined(__APPLE__) && defined(AROS_DARWIN_NOFORK)
    /*
     * Opt-in (build with -DAROS_DARWIN_NOFORK): run AROS in the bootstrap's own
     * process rather than a fork()ed child. fork() without a following exec()
     * leaves the Mach bootstrap/XPC ports unusable, so Metal's runtime shader
     * compiler is unreachable ("Unable to reach MTLCompilerService ... No such
     * process"). Running in the main process keeps Metal working -- BUT loading
     * the Cocoa/Metal frameworks alongside the AROS-hosted kernel in one process
     * currently faults (SIGBUS) once a display registers. Left here as a known,
     * gated path; the preferred fix is a precompiled Metal shader host-side so
     * the fork()ed child needs no MTLCompilerService.
     * Trade-off: warm/cold reboot is also unavailable in this mode.
     */
    fprintf(stderr, "[Bootstrap] Entering kernel at %p (darwin: no-fork)...\n", addr);
    Host_PreBoot();
    return addr(msg, AROS_BOOT_MAGIC);
#else
    do
    {
        pid_t child = fork();

        switch (child)
        {
        case -1:
            DisplayError("Failed to run kickstart!");
            return -1;

        case 0:
            fprintf(stderr, "[Bootstrap] Entering kernel at %p...\n", addr);
            Host_PreBoot();
            i = addr(msg, AROS_BOOT_MAGIC);
            exit(i);
        }

        /* Wait until AROS process exits */
        waitpid(child, &i, 0);

        if (!WIFEXITED(i))
        {
            D(fprintf(stderr, "AROS process died with error\n"));
            return -1;
        }

        D(fprintf(stderr, "AROS exited with status 0x%08X\n", WEXITSTATUS(i)));

        /* ColdReboot() returns 0x8F */
    } while (WEXITSTATUS(i) == 0x8F);

    if (WEXITSTATUS(i) == 0x81)
    {
        /*
         * Perform cold boot if requested.
         * Before rebooting, we clean up. Otherwise execvp()'ed process will
         * inherit what we allocated here, then again... This will cause memory leak.
         */
        Host_FreeMem();
        Host_ColdBoot();
    }

    return WEXITSTATUS(i);
#endif
}
