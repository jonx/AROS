#ifndef EMU68K_INTERN_H
#define EMU68K_INTERN_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: emu68k.library internals - the host execution-service binding.
*/

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <libraries/emu68k.h>

#define EMU68K_DYLIB_NAME "libemu68k.dylib"

/* Guest-visible file handles. MUST match the host service (emu68k_host.c):
 * a dos handle is a BPTR the program may dereference, so each one is backed by
 * a real guest structure and what crosses is MKBADDR of its address. */
#define EMU68K_GUEST_FH_BASE  0x00212000UL
#define EMU68K_GUEST_FH_SLOT  64UL
#define EMU68K_GUEST_FH_MAX   32UL

/* the host service surface (hosted/emu68k/emu68k_host.h on the graft side) */
typedef void *emu68k_run_h;
typedef void (*emu68k_sink_fn)(const char *buf, long len, void *user);

#define EMU68K_RC_DONE   0
#define EMU68K_RC_YIELD  1
#define EMU68K_RC_KILLED 2
#define EMU68K_RC_ERROR  (-1)
#define EMU68K_RC_HARDWARE 3

struct Emu68kHostIf
{
    emu68k_run_h (*run_new)(const void *image, unsigned long imagelen,
                            const char *args, unsigned long argslen,
                            emu68k_sink_fn sink, void *sink_user,
                            char *err, unsigned errlen);
    int  (*run_quantum)(emu68k_run_h r, unsigned long max_roundtrips,
                        unsigned int *exit_d0, char *err, unsigned errlen);
    void (*run_kill)(emu68k_run_h r);
    void (*run_free)(emu68k_run_h r);
    const char *(*version)(void);
    void (*run_set_name)(emu68k_run_h r, const char *name);
    int  (*scan_image)(const void *image, unsigned long imagelen,
                       char *detail, unsigned detaillen);
    void (*set_oscall)(int (*fn)(const char *libname, int lvo, APTR regs,
                                 APTR guest0, APTR user, char *err, ULONG errlen),
                       APTR user);
    APTR (*run_guest0)(emu68k_run_h r);
    ULONG (*run_guest_alloc)(emu68k_run_h r, unsigned long size);
    int (*run_call_hook)(emu68k_run_h r, unsigned long entry,
                         unsigned long hook, unsigned long object,
                         unsigned long message, unsigned int *result,
                         char *err, unsigned errlen);
};

struct Emu68kOSCallCtx
{
    APTR dosbase;
    emu68k_run_h run;
    ULONG (*guest_alloc)(emu68k_run_h r, unsigned long size);
    int (*call_hook)(emu68k_run_h r, unsigned long entry,
                     unsigned long hook, unsigned long object,
                     unsigned long message, unsigned int *result,
                     char *err, unsigned errlen);
};

struct Emu68kBase
{
    struct Library          lib;
    struct SignalSemaphore  runlock;    /* ONE translated run at a time: hosted
                                         * AROS tasks share a single host thread
                                         * and the engine's active-instance model
                                         * is documented single-runner; quanta
                                         * interleave through this lock */
    APTR                    hostlibBase;
    APTR                    dylib;
    struct Emu68kHostIf     host;       /* resolved symbols (all or nothing)   */
    LONG                    host_ok;
};

#endif /* EMU68K_INTERN_H */
