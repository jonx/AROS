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
#include LC_LIBDEFS_FILE

#include <aros/debug.h>
#include <string.h>

/* The leading, stable part of the engine's 68k register file. The engine owns
 * the full struct; only these two arrays are contractual here. */
struct Emu68kRegs
{
    ULONG d[8];
    ULONG a[8];
};

/* dos.library vector indices (negative offset / 6) */
#define DOS_LVO_OPEN     5    /* -30  */
#define DOS_LVO_CLOSE    6    /* -36  */
#define DOS_LVO_READ     7    /* -42  */
#define DOS_LVO_WRITE    8    /* -48  */
#define DOS_LVO_INPUT    9    /* -54  */
#define DOS_LVO_OUTPUT  10    /* -60  */
#define DOS_LVO_SEEK    11    /* -66  */
#define DOS_LVO_DELAY   33    /* -198 */

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
#define EMU68K_MAX_HANDLES 32
#define EMU68K_HANDLE_TAG  0x48000000UL          /* 'H' in the top byte        */

static struct { BPTR bptr; } g_handles[EMU68K_MAX_HANDLES];

static ULONG handle_token(BPTR b)
{
    int i;
    if (!b) return 0;
    for (i = 0; i < EMU68K_MAX_HANDLES; i++)
        if (g_handles[i].bptr == b) return EMU68K_HANDLE_TAG | (ULONG)i;
    for (i = 0; i < EMU68K_MAX_HANDLES; i++)
        if (!g_handles[i].bptr)
        {
            g_handles[i].bptr = b;
            return EMU68K_HANDLE_TAG | (ULONG)i;
        }
    return 0;                                    /* table full: NULL, cleanly  */
}

static BPTR handle_bptr(ULONG token)
{
    ULONG idx = token & 0x00FFFFFFUL;
    if ((token & 0xFF000000UL) != EMU68K_HANDLE_TAG) return BNULL;
    if (idx >= EMU68K_MAX_HANDLES) return BNULL;
    return g_handles[idx].bptr;
}

static void handle_release(ULONG token)
{
    ULONG idx = token & 0x00FFFFFFUL;
    if ((token & 0xFF000000UL) == EMU68K_HANDLE_TAG && idx < EMU68K_MAX_HANDLES)
        g_handles[idx].bptr = BNULL;
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
        }
    }

    /* Anything else is a capability gap, reported by name so the ledger says
     * exactly what to implement next. Never a guess. */
    if (err && errlen)
        snprintf(err, errlen, "%s LVO %d", libname, lvo);
    return 1;
}
