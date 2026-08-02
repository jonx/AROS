/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The OS-call bridge: a library call a 68k program makes that the host
          engine cannot serve itself arrives here, and is performed as a REAL
          native AROS library call.

          This is the model-2 boundary: the 68k program lives in a big-endian
          guest arena and calls native little-endian AROS libraries, with every
          value crossing the boundary converted according to its declared type.
          Guest pointers become host pointers by adding the guest base; the
          structures behind them are NOT converted, so only functions whose
          arguments this file understands are offered.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "emu68k_intern.h"
#include "emu68k_gen.h"
#include LC_LIBDEFS_FILE

#include <aros/debug.h>
#include <string.h>

/* dos.library vector indices (negative offset / 6) */
#define DOS_LVO_OPEN     5    /* -30  */
#define DOS_LVO_CLOSE    6    /* -36  */
#define DOS_LVO_READ     7    /* -42  */
#define DOS_LVO_WRITE    8    /* -48  */
#define DOS_LVO_INPUT    9    /* -54  */
#define DOS_LVO_OUTPUT  10    /* -60  */
#define DOS_LVO_SEEK    11    /* -66  */
#define DOS_LVO_DELAY   33    /* -198 */
#define DOS_LVO_WAITFORCHAR   34   /* -204 */
#define DOS_LVO_ISINTERACTIVE 36   /* -216 */
#define DOS_LVO_FLUSH         60   /* -360 */
#define DOS_LVO_LOCK          14   /* -84  */
#define DOS_LVO_UNLOCK        15   /* -90  */
#define DOS_LVO_DUPLOCK       16   /* -96  */
#define DOS_LVO_CREATEDIR     20   /* -120 */
#define DOS_LVO_CURRENTDIR    21   /* -126 */
#define DOS_LVO_IOERR   22    /* -132: what every failed dos call is followed by */
#define DOS_LVO_GETPROGRAMNAME 96   /* -576 */
#define DOS_LVO_GETVAR        151   /* -906 */
#define DOS_LVO_SETVAR        152   /* -912 */
#define DOS_LVO_PRINTFAULT 79 /* -474 */
#define DOS_LVO_SETIOERR   77 /* -462 */
#define ICON_LVO_FINDTOOLTYPE  16   /* -96  */

/* A guest pointer becomes a host pointer by adding the guest base. Only memory
 * INSIDE the guest arena may be handed to a native call this way. */
static APTR gptr(APTR guest0, ULONG addr)
{
    return addr ? (APTR)((UBYTE *)guest0 + addr) : NULL;
}

/* ---- OPAQUE HANDLES -------------------------------------------------------
 * A 68k register is 32 bits; a native AROS BPTR is a 64-bit pointer. A file
 * handle therefore CANNOT be handed to the program as itself - truncating it
 * produces a pointer that dereferences to nothing (which is exactly how this
 * first failed). So handles cross the boundary as TOKENS: the program carries
 * an opaque 32-bit value and this table maps it back to the real BPTR. The
 * program never dereferences it, which is what makes a token legitimate.
 * Small and fixed: a 68k program with more than this many open files is not
 * the case we are serving yet, and it fails cleanly rather than corrupting. */
#define EMU68K_MAX_HANDLES ((int)EMU68K_GUEST_FH_MAX)

static struct { BPTR bptr; } g_handles[EMU68K_MAX_HANDLES];

/* A handle crosses as a BPTR of a real guest structure, not as an opaque tag:
 * a program may dereference its handle, and BADDR of a tag lands nowhere. Slot
 * i lives at EMU68K_GUEST_FH_BASE + i*SLOT, and the guest gets that >> 2. */
static ULONG handle_slot_bptr(int i)
{
    return (ULONG)((EMU68K_GUEST_FH_BASE + (ULONG)i * EMU68K_GUEST_FH_SLOT) >> 2);
}

static ULONG handle_token(BPTR b)
{
    int i;
    if (!b) return 0;
    for (i = 0; i < EMU68K_MAX_HANDLES; i++)
        if (g_handles[i].bptr == b) return handle_slot_bptr(i);
    for (i = 0; i < EMU68K_MAX_HANDLES; i++)
        if (!g_handles[i].bptr)
        {
            g_handles[i].bptr = b;
            return handle_slot_bptr(i);
        }
    return 0;                                    /* table full: NULL, cleanly  */
}

static int handle_index(ULONG token)
{
    ULONG addr = (ULONG)token << 2;              /* BADDR, the guest's view    */
    ULONG off;
    if (addr < EMU68K_GUEST_FH_BASE) return -1;
    off = addr - EMU68K_GUEST_FH_BASE;
    if (off % EMU68K_GUEST_FH_SLOT) return -1;
    off /= EMU68K_GUEST_FH_SLOT;
    return (off < EMU68K_GUEST_FH_MAX) ? (int)off : -1;
}

static BPTR handle_bptr(ULONG token)
{
    int i = handle_index(token);
    return (i < 0) ? BNULL : g_handles[i].bptr;
}

static void handle_release(ULONG token)
{
    int i = handle_index(token);
    if (i >= 0) g_handles[i].bptr = BNULL;
}

/* ---- THE GENERATED TABLE --------------------------------------------------
 * One entry per library the generator covers. The base is resolved on first
 * use and cached: dos and exec are already open, the rest are opened only if a
 * guest actually calls them, so covering intuition and graphics costs a
 * console program nothing. A library that will not open is remembered as
 * absent rather than retried on every call. */
enum { GENBASE_DOS, GENBASE_EXEC, GENBASE_OPEN };

struct EmuGenLib
{
    const char *name;
    int       (*fn)(int lvo, struct Emu68kRegs *r, APTR guest0, APTR base);
    UBYTE       kind;
    UBYTE       tried;
    APTR        base;
};

static struct EmuGenLib g_genlibs[] =
{
    { "dos.library",         emu68k_gen_dos,         GENBASE_DOS,  0, NULL },
    { "exec.library",        emu68k_gen_exec,        GENBASE_EXEC, 0, NULL },
    { "utility.library",        emu68k_gen_utility,        GENBASE_OPEN, 0, NULL },
    { "intuition.library",      emu68k_gen_intuition,      GENBASE_OPEN, 0, NULL },
    { "graphics.library",       emu68k_gen_graphics,       GENBASE_OPEN, 0, NULL },
    { "layers.library",         emu68k_gen_layers,         GENBASE_OPEN, 0, NULL },
    { "gadtools.library",       emu68k_gen_gadtools,       GENBASE_OPEN, 0, NULL },
    { "asl.library",            emu68k_gen_asl,            GENBASE_OPEN, 0, NULL },
    { "icon.library",           emu68k_gen_icon,           GENBASE_OPEN, 0, NULL },
    { "iffparse.library",       emu68k_gen_iffparse,       GENBASE_OPEN, 0, NULL },
    { "commodities.library",    emu68k_gen_commodities,    GENBASE_OPEN, 0, NULL },
    { "diskfont.library",       emu68k_gen_diskfont,       GENBASE_OPEN, 0, NULL },
    { "locale.library",         emu68k_gen_locale,         GENBASE_OPEN, 0, NULL },
    { "keymap.library",         emu68k_gen_keymap,         GENBASE_OPEN, 0, NULL },
    { "datatypes.library",      emu68k_gen_datatypes,      GENBASE_OPEN, 0, NULL },
    { "expansion.library",      emu68k_gen_expansion,      GENBASE_OPEN, 0, NULL },
    { "cybergraphics.library",  emu68k_gen_cybergraphics,  GENBASE_OPEN, 0, NULL },
    { "mathffp.library",        emu68k_gen_mathffp,        GENBASE_OPEN, 0, NULL },
    { "mathieeesingbas.library", emu68k_gen_mathieeesingbas, GENBASE_OPEN, 0, NULL },
    { "mathieeedoubbas.library", emu68k_gen_mathieeedoubbas, GENBASE_OPEN, 0, NULL },
};

static int gen_dispatch(const char *libname, int lvo, struct Emu68kRegs *r,
                        APTR guest0, APTR DOSBase)
{
    unsigned i;

    for (i = 0; i < sizeof(g_genlibs) / sizeof(g_genlibs[0]); i++)
    {
        struct EmuGenLib *g = &g_genlibs[i];

        if (strcmp(libname, g->name) != 0)
            continue;

        if (!g->tried)
        {
            g->tried = 1;
            switch (g->kind)
            {
            case GENBASE_DOS:  g->base = DOSBase;                    break;
            case GENBASE_EXEC: g->base = SysBase;                    break;
            default:           g->base = OpenLibrary(g->name, 0);    break;
            }
        }
        if (!g->base)
            return 1;
        return g->fn(lvo, r, guest0, g->base);
    }
    return 1;
}

/* Released at expunge: the bases the table opened on demand. */
void Emu68k_OSCallCleanup(void)
{
    unsigned i;

    for (i = 0; i < sizeof(g_genlibs) / sizeof(g_genlibs[0]); i++)
    {
        if (g_genlibs[i].kind == GENBASE_OPEN && g_genlibs[i].base)
            CloseLibrary(g_genlibs[i].base);
        g_genlibs[i].base  = NULL;
        g_genlibs[i].tried = 0;
    }
}

int Emu68k_OSCall(const char *libname, int lvo, APTR regs, APTR guest0,
                  APTR user, char *err, ULONG errlen)
{
    struct Emu68kRegs *r = regs;
    APTR DOSBase = user;

    if (strcmp(libname, "dos.library") == 0)
    {
        switch (lvo)
        {
        case DOS_LVO_IOERR:      /* IoErr() -> the last error code               */
            r->d[0] = (ULONG)IoErr();
            return 0;

        case DOS_LVO_SETIOERR:   /* SetIoErr(LONG D1) -> old                     */
            r->d[0] = (ULONG)SetIoErr((LONG)r->d[1]);
            return 0;

        case DOS_LVO_PRINTFAULT: /* PrintFault(LONG code D1, STRPTR hdr D2)      */
            r->d[0] = (ULONG)PrintFault((LONG)r->d[1],
                                        (CONST_STRPTR)gptr(guest0, r->d[2]));
            return 0;

        case DOS_LVO_OUTPUT:
            r->d[0] = handle_token(Output());
            return 0;

        case DOS_LVO_INPUT:
            r->d[0] = handle_token(Input());
            return 0;

        case DOS_LVO_WRITE:      /* Write(BPTR file D1, APTR buf D2, LONG len D3) */
            r->d[0] = (ULONG)Write(handle_bptr(r->d[1]),
                                   gptr(guest0, r->d[2]), (LONG)r->d[3]);
            return 0;

        case DOS_LVO_READ:       /* Read(BPTR file D1, APTR buf D2, LONG len D3)  */
            r->d[0] = (ULONG)Read(handle_bptr(r->d[1]),
                                  gptr(guest0, r->d[2]), (LONG)r->d[3]);
            return 0;

        case DOS_LVO_OPEN:       /* Open(STRPTR name D1, LONG mode D2) -> BPTR    */
            r->d[0] = handle_token(Open((CONST_STRPTR)gptr(guest0, r->d[1]),
                                        (LONG)r->d[2]));
            return 0;

        case DOS_LVO_CLOSE:      /* Close(BPTR file D1)                           */
            r->d[0] = (ULONG)Close(handle_bptr(r->d[1]));
            handle_release(r->d[1]);
            return 0;

        case DOS_LVO_SEEK:       /* Seek(BPTR D1, LONG pos D2, LONG mode D3)      */
            r->d[0] = (ULONG)Seek(handle_bptr(r->d[1]), (LONG)r->d[2],
                                  (LONG)r->d[3]);
            return 0;

        case DOS_LVO_DELAY:      /* Delay(LONG ticks D1)                          */
            Delay((LONG)r->d[1]);
            return 0;

        /* Both take a file handle, so they are hand-written rather than derived:
         * the BPTR has to go through the token table before it means anything
         * native. A program asks IsInteractive to decide whether it is talking
         * to a terminal, which is the first thing an archiver does. */
        case DOS_LVO_ISINTERACTIVE:   /* IsInteractive(BPTR file D1)              */
            r->d[0] = (ULONG)IsInteractive(handle_bptr(r->d[1]));
            return 0;

        case DOS_LVO_WAITFORCHAR:     /* WaitForChar(BPTR file D1, LONG tmo D2)   */
            r->d[0] = (ULONG)WaitForChar(handle_bptr(r->d[1]), (LONG)r->d[2]);
            return 0;

        case DOS_LVO_FLUSH:           /* Flush(BPTR file D1)                      */
            r->d[0] = (ULONG)Flush(handle_bptr(r->d[1]));
            return 0;

        /* A lock is a BPTR like a file handle, so it crosses through the same
         * table. What a program may NOT be handed is the native BPTR itself:
         * it is 64-bit and a 68k register is not. */
        case DOS_LVO_LOCK:            /* Lock(STRPTR name D1, LONG mode D2)       */
            r->d[0] = handle_token(Lock((CONST_STRPTR)gptr(guest0, r->d[1]),
                                        (LONG)r->d[2]));
            return 0;

        case DOS_LVO_UNLOCK:          /* UnLock(BPTR lock D1)                     */
            UnLock(handle_bptr(r->d[1]));
            handle_release(r->d[1]);
            return 0;

        case DOS_LVO_DUPLOCK:         /* DupLock(BPTR lock D1)                    */
            r->d[0] = handle_token(DupLock(handle_bptr(r->d[1])));
            return 0;

        case DOS_LVO_CREATEDIR:       /* CreateDir(STRPTR name D1)                */
            r->d[0] = handle_token(CreateDir((CONST_STRPTR)gptr(guest0, r->d[1])));
            return 0;

        case DOS_LVO_CURRENTDIR:      /* CurrentDir(BPTR lock D1) -> the old one  */
            r->d[0] = handle_token(CurrentDir(handle_bptr(r->d[1])));
            return 0;

        case DOS_LVO_GETVAR:
            /* GetVar(name D1, buffer D2, size D3, flags D4) -> length.
             * The buffer is the guest's, so the native call fills guest memory
             * directly - the value is bytes, not a structure, which is why this
             * one can be bridged rather than reimplemented. */
        {
            LONG n = GetVar((CONST_STRPTR)gptr(guest0, r->d[1]),
                            (STRPTR)gptr(guest0, r->d[2]),
                            (LONG)r->d[3], (LONG)r->d[4]);
            r->d[0] = (ULONG)n;
            return 0;
        }

        case DOS_LVO_SETVAR:
            r->d[0] = (ULONG)SetVar((CONST_STRPTR)gptr(guest0, r->d[1]),
                                    (CONST_STRPTR)gptr(guest0, r->d[2]),
                                    (LONG)r->d[3], (LONG)r->d[4]);
            return 0;

        case DOS_LVO_GETPROGRAMNAME:  /* GetProgramName(buf D1, len D2)          */
        {
            STRPTR buf = gptr(guest0, r->d[1]);
            LONG   len = (LONG)r->d[2];
            if (buf && len > 0)
            {
                /* the guest's own name, into the guest's own buffer */
                if (!GetProgramName(buf, len)) buf[0] = '\0';
                r->d[0] = DOSTRUE;
            }
            else r->d[0] = DOSFALSE;
            return 0;
        }
        }
    }

    if (strcmp(libname, "icon.library") == 0)
    {
        /* FindToolType on a program launched from the Shell has no tool types
         * to find: NULL is the correct, expected answer, and programs are
         * written to handle it. */
        if (lvo == ICON_LVO_FINDTOOLTYPE) { r->d[0] = 0; return 0; }
    }

    /* Nothing above claimed it, so try the GENERATED table: the crossings that
     * follow entirely from the vector's declared prototype and register map.
     * Hand-written cases run first and win, so a crossing that needs judgement
     * is never silently replaced by a derived one. */
    if (gen_dispatch(libname, lvo, r, guest0, DOSBase) == 0)
        return 0;

    /* Anything else is a capability gap, reported by name so the ledger says
     * exactly what to implement next. Never a guess. */
    if (err && errlen)
        snprintf(err, errlen, "%s LVO %d", libname, lvo);
    return 1;
}
