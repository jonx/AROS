/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The generic structure marshaller: converts a whole AmigaOS structure
          to or from its native counterpart by walking a generated field table,
          so no structure needs conversion code written for it.
*/

#include <exec/types.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <string.h>
#include <stdio.h>

#include "emu68k_layouts.h"
#include "emu68k_gen.h"

/* The host service reserves guest addresses 0..32 MiB and maps ordinary RAM
 * from $210000 upward, with the classic hardware windows punched back out.
 * A generated crossing must validate before dereferencing: adding guest0 to an
 * arbitrary 32-bit tag value is not validation. These constants are the same
 * ABI contract already shared for guest file-handle slots. */
#define EMU_GUEST_RAM_LO     0x00210000UL
#define EMU_GUEST_RAM_HI     0x02000000UL
#define EMU_GUEST_CIA_LO     0x00BFD000UL
#define EMU_GUEST_CIA_HI     0x00BFF000UL
#define EMU_GUEST_CUSTOM_LO  0x00DFF000UL
#define EMU_GUEST_CUSTOM_HI  0x00E00000UL

static int guest_range_ok(ULONG addr, ULONG len)
{
    ULONG end;

    if (addr < EMU_GUEST_RAM_LO || addr >= EMU_GUEST_RAM_HI)
        return 0;
    if (len > EMU_GUEST_RAM_HI - addr)
        return 0;
    end = addr + len;
    if (addr < EMU_GUEST_CIA_HI && end > EMU_GUEST_CIA_LO)
        return 0;
    if (addr < EMU_GUEST_CUSTOM_HI && end > EMU_GUEST_CUSTOM_LO)
        return 0;
    return 1;
}

LONG emu68k_require_guest_range(ULONG guest_addr, ULONG length,
                                const char *what, char *err, ULONG errlen)
{
    if (guest_range_ok(guest_addr, length))
        return 0;
    if (err && errlen)
        snprintf(err, errlen, "%s at %08lx+%lu is outside guest memory",
                 what ? what : "guest object", (unsigned long)guest_addr,
                 (unsigned long)length);
    return -1;
}

static ULONG guest_be32(APTR guest0, ULONG addr)
{
    const UBYTE *p = (const UBYTE *)guest0 + addr;
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static const struct EmuTagDesc *tag_desc(const struct EmuTagDomain *domain,
                                         ULONG tag)
{
    UWORD i;
    for (i = 0; i < domain->count; i++)
        if (domain->tags[i].tag == tag)
            return &domain->tags[i];
    return NULL;
}

/* Convert a guest's packed, big-endian 8-byte TagItems into native 16-byte
 * TagItems. TAG_MORE is flattened; IGNORE/SKIP are interpreted while walking.
 * The semantic policy determines ti_Data's type. Unknown/refused tags fail the
 * crossing with their domain and name instead of being guessed as scalars. */
LONG emu68k_tags_to_native(APTR guest0, ULONG guest_tags,
                           const struct EmuTagDomain *domain,
                           struct TagItem *native_tags, ULONG capacity,
                           char *err, ULONG errlen)
{
    ULONG p = guest_tags, out = 0, steps = 0;

    if (!domain || !native_tags || capacity < 1)
        return -1;
    if (!p)
    {
        native_tags[0].ti_Tag = TAG_DONE;
        native_tags[0].ti_Data = 0;
        return 0;
    }

    while (++steps <= 4096)
    {
        ULONG tag, data;
        const struct EmuTagDesc *desc;

        if (!guest_range_ok(p, 8))
        {
            if (err && errlen)
                snprintf(err, errlen, "taglist %08lx is outside guest memory",
                         (unsigned long)p);
            return -1;
        }
        tag = guest_be32(guest0, p);
        data = guest_be32(guest0, p + 4);

        if (tag == TAG_DONE)
            break;
        if (tag == TAG_IGNORE)
        {
            p += 8;
            continue;
        }
        if (tag == TAG_MORE)
        {
            p = data;
            continue;
        }
        if (tag == TAG_SKIP)
        {
            if (data > (EMU_GUEST_RAM_HI / 8) - 1)
            {
                if (err && errlen)
                    snprintf(err, errlen, "TAG_SKIP overflow in %s", domain->name);
                return -1;
            }
            p += (data + 1) * 8;
            continue;
        }

        desc = tag_desc(domain, tag);
        if (!desc)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: unknown tag %08lx in %s",
                         (unsigned long)tag, domain->name);
            return -1;
        }
        if (desc->kind == EMU_TAG_REFUSE)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: tag %s in %s needs object policy",
                         desc->name, domain->name);
            return -1;
        }
        if (out + 1 >= capacity)
        {
            if (err && errlen)
                snprintf(err, errlen, "taglist in %s exceeds compiled limit %lu",
                         domain->name, (unsigned long)(capacity - 1));
            return -1;
        }

        native_tags[out].ti_Tag = desc->tag;
        if (desc->kind == EMU_TAG_CSTR)
        {
            ULONG n;
            if (!data)
                native_tags[out].ti_Data = 0;
            else
            {
                for (n = 0; n < 65536; n++)
                {
                    if (!guest_range_ok(data + n, 1))
                    {
                        if (err && errlen)
                            snprintf(err, errlen, "tag %s string is outside guest memory",
                                     desc->name);
                        return -1;
                    }
                    if (!((const UBYTE *)guest0)[data + n])
                        break;
                }
                if (n == 65536)
                {
                    if (err && errlen)
                        snprintf(err, errlen, "tag %s string is not terminated",
                                 desc->name);
                    return -1;
                }
                native_tags[out].ti_Data = (IPTR)((UBYTE *)guest0 + data);
            }
        }
        else
            native_tags[out].ti_Data = (IPTR)data;
        out++;
        p += 8;
    }

    if (steps > 4096)
    {
        if (err && errlen)
            snprintf(err, errlen, "taglist cycle or excessive controls in %s",
                     domain->name);
        return -1;
    }
    native_tags[out].ti_Tag = TAG_DONE;
    native_tags[out].ti_Data = 0;
    return (LONG)out;
}

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

/* How many elements of a field fit in the caller's `limit` bytes of the GUEST
 * structure. Several AmigaOS calls take the number of bytes the caller is
 * willing to have filled (GetPrefs is the one this was written for), and the
 * limit is a hard bound: an element that would cross it is not written at all,
 * rather than clipped to a fraction of a value. An array fills as far as it
 * reaches, which is what a byte-copying Amiga does. */
static ULONG fits(const struct EmuField *f, ULONG limit)
{
    ULONG room, n;

    if (limit <= f->g_off)
        return 0;
    room = limit - f->g_off;
    n = room / f->g_w;
    return n < f->count ? n : f->count;
}

/* native -> guest: what the program reads back after a call filled something. */
void emu68k_to_guest_sized(APTR guest0, ULONG gbase, const void *native,
                           const struct EmuField *f, int n, ULONG limit)
{
    UBYTE *g = (UBYTE *)guest0 + gbase;
    const UBYTE *nat = native;
    int i;
    ULONG e, count;

    for (i = 0; i < n; i++, f++)
    {
        count = fits(f, limit);
        if (f->kind == EMU_F_BYTES)
            CopyMem((APTR)(nat + f->n_off), g + f->g_off, count);
        else
            for (e = 0; e < count; e++)
                guest_write(g + f->g_off + e * f->g_w, f->g_w,
                            native_read(nat + f->n_off + e * f->n_w, f->n_w));
    }
}

void emu68k_to_guest(APTR guest0, ULONG gbase, const void *native,
                     const struct EmuField *f, int n)
{
    emu68k_to_guest_sized(guest0, gbase, native, f, n, EMU_NO_LIMIT);
}

/* guest -> native: the settings a program filled in before calling. */
void emu68k_from_guest_sized(APTR guest0, ULONG gbase, void *native,
                             const struct EmuField *f, int n, ULONG limit)
{
    const UBYTE *g = (const UBYTE *)guest0 + gbase;
    UBYTE *nat = native;
    int i;
    ULONG e, count;

    for (i = 0; i < n; i++, f++)
    {
        count = fits(f, limit);
        if (f->kind == EMU_F_BYTES)
            CopyMem((APTR)(g + f->g_off), nat + f->n_off, count);
        else
            for (e = 0; e < count; e++)
                native_write(nat + f->n_off + e * f->n_w, f->n_w,
                             guest_read(g + f->g_off + e * f->g_w, f->g_w));
    }
}

void emu68k_from_guest(APTR guest0, ULONG gbase, void *native,
                       const struct EmuField *f, int n)
{
    emu68k_from_guest_sized(guest0, gbase, native, f, n, EMU_NO_LIMIT);
}
