/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The generic structure marshaller: converts a whole AmigaOS structure
          to or from its native counterpart by walking a generated field table,
          so no structure needs conversion code written for it.
*/

#include <exec/types.h>
#include <proto/exec.h>
#include <string.h>

#include "emu68k_layouts.h"

/* A guest scalar is big-endian and as wide as the guest's field; a native one
 * is host-endian and may be wider (a guest LONG against a native IPTR). So the
 * value is read as a number and written as a number, never copied as bytes -
 * copying is only ever right for a field that HAS no byte order, which the
 * table marks separately. */

static ULONG native_read(const UBYTE *p, unsigned w)
{
    switch (w)
    {
    case 1:  return *(const UBYTE *)p;
    case 2:  return *(const UWORD *)(const void *)p;
    case 4:  return *(const ULONG *)(const void *)p;
    default: return (ULONG)*(const UQUAD *)(const void *)p;   /* narrowed */
    }
}

static void native_write(UBYTE *p, unsigned w, ULONG v)
{
    switch (w)
    {
    case 1:  *(UBYTE *)p = (UBYTE)v; break;
    case 2:  *(UWORD *)(void *)p = (UWORD)v; break;
    case 4:  *(ULONG *)(void *)p = v; break;
    default: *(UQUAD *)(void *)p = (UQUAD)v; break;
    }
}

static ULONG guest_read(const UBYTE *p, unsigned w)
{
    ULONG v = 0;
    unsigned i;
    for (i = 0; i < w; i++)                    /* big-endian, widest byte first */
        v = (v << 8) | p[i];
    return v;
}

static void guest_write(UBYTE *p, unsigned w, ULONG v)
{
    unsigned i;
    for (i = 0; i < w; i++)
        p[w - 1 - i] = (UBYTE)(v >> (8 * i));
}

/* native -> guest: what the program reads back after a call filled something. */
void emu68k_to_guest(APTR guest0, ULONG gbase, const void *native,
                     const struct EmuField *f, int n)
{
    UBYTE *g = (UBYTE *)guest0 + gbase;
    const UBYTE *nat = native;
    int i;

    for (i = 0; i < n; i++, f++)
    {
        if (f->kind == EMU_F_BYTES)
            CopyMem((APTR)(nat + f->n_off), g + f->g_off, f->g_w);
        else
            guest_write(g + f->g_off, f->g_w,
                        native_read(nat + f->n_off, f->n_w));
    }
}

/* guest -> native: the settings a program filled in before calling. */
void emu68k_from_guest(APTR guest0, ULONG gbase, void *native,
                       const struct EmuField *f, int n)
{
    const UBYTE *g = (const UBYTE *)guest0 + gbase;
    UBYTE *nat = native;
    int i;

    for (i = 0; i < n; i++, f++)
    {
        if (f->kind == EMU_F_BYTES)
            CopyMem((APTR)(g + f->g_off), nat + f->n_off, f->g_w);
        else
            native_write(nat + f->n_off, f->n_w,
                         guest_read(g + f->g_off, f->g_w));
    }
}
