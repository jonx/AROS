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
