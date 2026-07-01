#ifndef AROS_HOSTBIND_H
#define AROS_HOSTBIND_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: HostBind -- tiny convenience layer over hostlib.resource for the common
          "tap a function out of the host" pattern on hosted AROS.

    Every hosted bridge (battclock time, bsdsocket, arc4random, the Cocoa/X11/SDL
    HIDDs, ...) opens a host library and resolves symbols through hostlib.resource,
    and each one hand-rolls the same OpenResource + HostLib_Open + resolve + cleanup
    dance. These two static-inline helpers collapse that to one call.

    Two shapes:
      (a) borrow a symbol from the host C library:   HostBind_LibcSym("arc4random_buf")
      (b) open a named host dylib + bind a symbol table into a struct of function
          pointers:                                  HostBind_Interface(names, syms, &n)

    Both return NULL cleanly when there is no host (native build) or the symbol is
    missing, so a caller can fall back. See docs/features/host-bridge (aros-aarch64)
    and hosted/hostbind (the worked sample).

    Header-only on purpose: no new module to build or link. Each translation unit that
    uses (a) opens the host libc once and caches the handle for its own symbol lookups.
*/

#include <proto/exec.h>
#include <proto/hostlib.h>

/* Host C library names to probe, most-specific first. libSystem is darwin; the two
   libc.so forms cover the Linux-hosted flavours. On a native build none of these
   open and the helpers return NULL. */
#ifndef HOSTBIND_LIBC_NAMES
#  define HOSTBIND_LIBC_NAMES { "libSystem.dylib", "libc.so.6", "libc.so", (const char *)0 }
#endif

/*
    HostBind_LibcSym -- resolve `symbol` from the host C library.

    Returns the function pointer, or NULL if there is no host or the symbol is not
    exported. The host libc handle is opened once per translation unit and cached.
*/
static inline APTR HostBind_LibcSym(const char *symbol)
{
    static APTR cached = (APTR)-1;                 /* -1: not tried; NULL: no host */
    static const char *names[] = HOSTBIND_LIBC_NAMES;
    APTR HostLibBase;

    if (!symbol)
        return NULL;

    HostLibBase = OpenResource((STRPTR)"hostlib.resource");
    if (!HostLibBase)
        return NULL;                               /* native build: nothing to bind */

    if (cached == (APTR)-1)
    {
        int i;
        cached = NULL;
        for (i = 0; names[i]; i++)
        {
            APTR lib = HostLib_Open(names[i], (char **)0);
            if (lib) { cached = lib; break; }
        }
    }
    if (!cached)
        return NULL;

    return HostLib_GetPointer(cached, symbol, (char **)0);
}

/*
    HostBind_Interface -- open the first host dylib in `libnames` that loads and
    exports every name in `symbols`, and return its function-pointer interface (cast
    it to your own struct of matching pointers, in symbol order).

    `libnames` and `symbols` are NULL-terminated arrays. Returns NULL if no candidate
    library resolves the whole table (or there is no host). *unresolved, if non-NULL,
    gets the missing-symbol count from the last attempt (0 on success).
*/
static inline APTR HostBind_Interface(const char **libnames, const char **symbols,
                                      ULONG *unresolved)
{
    APTR HostLibBase;
    int i;

    if (unresolved)
        *unresolved = 0;
    if (!libnames || !symbols)
        return NULL;

    HostLibBase = OpenResource((STRPTR)"hostlib.resource");
    if (!HostLibBase)
        return NULL;

    for (i = 0; libnames[i]; i++)
    {
        APTR lib = HostLib_Open(libnames[i], (char **)0);
        ULONG unres = 0;
        APTR iface;

        if (!lib)
            continue;

        iface = HostLib_GetInterface(lib, (const char **)symbols, &unres);
        if (iface && unres == 0)
            return iface;                          /* got the whole table; keep lib open */

        if (unresolved)
            *unresolved = unres;
        if (iface)
            HostLib_DropInterface(iface);
        HostLib_Close(lib, (char **)0);
    }

    return NULL;
}

#endif /* AROS_HOSTBIND_H */
