/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The generic structure marshaller: converts a whole AmigaOS structure
          to or from its native counterpart by walking a generated field table,
          so no structure needs conversion code written for it.
*/

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/memory.h>
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

/* Installed by the run owner (emu68k_oscall.c); the allocations live until
 * the run tears down, because the callee RETAINS what a followed conversion
 * builds - a gadget class keeps the label structure it was given and reads
 * it again at render time, long after the call returned. */
APTR (*emu68k_persist_alloc)(ULONG size) = NULL;

APTR emu68k_scratch_alloc(ULONG size, char *err, ULONG errlen)
{
    APTR scratch = size ? AllocMem(size, MEMF_ANY) : NULL;

    if (!scratch && size && err && errlen)
        snprintf(err, errlen, "native bridge scratch allocation of %lu bytes failed",
                 (unsigned long)size);
    return scratch;
}

void emu68k_scratch_free(APTR scratch, ULONG size)
{
    if (scratch)
        FreeMem(scratch, size);
}

static ULONG guest_be32(APTR guest0, ULONG addr)
{
    const UBYTE *p = (const UBYTE *)guest0 + addr;
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static UWORD guest_be16(APTR guest0, ULONG addr)
{
    const UBYTE *p = (const UBYTE *)guest0 + addr;
    return ((UWORD)p[0] << 8) | (UWORD)p[1];
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

/* A LoadRGB32 stream is a sequence of count/first headers followed by three
 * ULONGs per colour, terminated by a zero header. The guest ULONGs are
 * big-endian, so even though both ABIs call them ULONG the stream cannot be
 * passed through. This helper is shared by direct pointer arguments and RGB32
 * values nested inside a policy-compiled taglist. */
LONG emu68k_rgb32_to_native(APTR guest0, ULONG guest_table,
                            ULONG *native_table, ULONG capacity,
                            const char *what, char *err, ULONG errlen)
{
    ULONG count, remaining = 0;

    if (!guest_table || !native_table || !capacity)
        return guest_table ? -1 : 0;
    for (count = 0; count < capacity; count++)
    {
        ULONG value;
        if (emu68k_require_guest_range(guest_table + count * 4, 4,
                                       what, err, errlen) < 0)
            return -1;
        value = guest_be32(guest0, guest_table + count * 4);
        native_table[count] = value;
        if (remaining)
            remaining--;
        else if (!value)
            return 0;
        else
        {
            ULONG colors = value >> 16;
            if (!colors || colors > (capacity - count - 1) / 3)
                break;
            remaining = colors * 3;
        }
    }
    if (err && errlen)
        snprintf(err, errlen, "%s RGB32 stream is malformed or unterminated",
                 what ? what : "guest");
    return -1;
}

/* Which descriptor a domain has for one tag value, or NULL: the lookup the
 * single-attribute crossings (GetAttr) use to decide whether an attribute is
 * a scalar they may serve. */
const struct EmuTagDesc *emu68k_tag_lookup(const struct EmuTagDomain *domain,
                                           ULONG tag)
{
    UWORD i;

    if (!domain) return NULL;
    for (i = 0; i < domain->count; i++)
        if (domain->tags[i].tag == tag)
            return &domain->tags[i];
    return NULL;
}

/* Convert one structure AND everything its followed pointer fields reference.
 * Each node is a run-lifetime allocation, because the callee RETAINS what a
 * followed conversion builds - a gadget class keeps the label structure it
 * was given and reads it again at render time, long after the call returned.
 * The depth bound is what terminates a cyclic or runaway chain, and hitting
 * it is a refusal by name, never a silently shortened list. */
static APTR emu68k_deep_convert(APTR guest0, ULONG gaddr,
                                const struct EmuStructDesc *all,
                                const struct EmuStructDesc *sd,
                                int depth, const char *what,
                                char *err, ULONG errlen)
{
    UBYTE *node;
    UWORD i;

    if (depth <= 0)
    {
        if (err && errlen)
            snprintf(err, errlen, "%s: %s chain deeper than the marshaller "
                     "follows", what, sd->name);
        return NULL;
    }
    if (emu68k_require_guest_range(gaddr, sd->guest_size, what, err, errlen) < 0)
        return NULL;
    if (!emu68k_persist_alloc)
    {
        if (err && errlen)
            snprintf(err, errlen, "%s: no run-lifetime allocator for %s",
                     what, sd->name);
        return NULL;
    }
    node = emu68k_persist_alloc(sd->native_size);
    if (!node)
    {
        if (err && errlen)
            snprintf(err, errlen, "%s: no memory for a native %s",
                     what, sd->name);
        return NULL;
    }
    memset(node, 0, sd->native_size);
    emu68k_from_guest(guest0, gaddr, node, sd->fields, sd->nfields);
    for (i = 0; i < sd->nfollow; i++)
    {
        const struct EmuFollow *f = &sd->follow[i];
        ULONG gp = guest_be32(guest0, gaddr + f->g_off);
        APTR sub;

        if (!gp)
            continue;
        sub = emu68k_deep_convert(guest0, gp, all, &all[f->ref],
                                  depth - 1, what, err, errlen);
        if (!sub)
            return NULL;
        *(APTR *)(void *)(node + f->n_off) = sub;
    }
    return node;
}

/* Expose the descriptor-driven walk to generated direct arguments as well as
 * TagItem values.  The generated layout table remains the only description of
 * what may be followed, and the same range/depth checks apply. */
APTR emu68k_struct_graph_to_native(APTR guest0, ULONG guest_addr,
                                    const struct EmuStructDesc *descs,
                                    UWORD desc_index, const char *what,
                                    char *err, ULONG errlen)
{
    if (!descs)
    {
        if (err && errlen)
            snprintf(err, errlen, "%s: no structure descriptors", what);
        return NULL;
    }
    return emu68k_deep_convert(guest0, guest_addr, descs,
                               &descs[desc_index], 33, what, err, errlen);
}

/* GadTools listviews retain a struct List and walk the Node headers and their
 * ln_Name strings during later renders.  A classic List is circular through
 * its embedded tail sentinel, so the ordinary acyclic structure-graph walker
 * is deliberately the wrong representation.  Validate that exact invariant,
 * then rebuild one retained native allocation containing the List, Nodes and
 * copied strings. */
static struct List *guest_node_list_to_native(APTR guest0, ULONG guest_list,
                                              ULONG max_nodes,
                                              const char *what,
                                              char *err, ULONG errlen)
{
    ULONG tail = guest_list + M68K_List_lh_Tail;
    ULONG walk, count = 0, names_size = 0;
    UQUAD total;
    UBYTE *storage, *names;
    struct List *list;
    struct Node *nodes;

    if (emu68k_require_guest_range(guest_list, M68K_List_SIZEOF,
                                   what, err, errlen) < 0)
        return NULL;
    walk = guest_be32(guest0, guest_list + M68K_List_lh_Head);
    while (walk != tail)
    {
        ULONG name, n = 0;

        if (!walk || count >= max_nodes ||
            emu68k_require_guest_range(walk, M68K_Node_SIZEOF,
                                       what, err, errlen) < 0)
            goto malformed;
        name = guest_be32(guest0, walk + M68K_Node_ln_Name);
        if (name)
        {
            while (n < 65536)
            {
                if (emu68k_require_guest_range(name + n, 1,
                                               what, err, errlen) < 0)
                    return NULL;
                n++;
                if (!((const UBYTE *)guest0)[name + n - 1])
                    break;
            }
            if (n == 65536 || names_size > 1024 * 1024 - n)
                goto malformed;
            names_size += n;
        }
        count++;
        walk = guest_be32(guest0, walk + M68K_Node_ln_Succ);
    }

    total = (UQUAD)sizeof(struct List) +
            (UQUAD)count * sizeof(struct Node) + names_size;
    if (total > 1024 * 1024 || !emu68k_persist_alloc)
    {
        if (err && errlen)
            snprintf(err, errlen, "%s has no retained-list allocator or is too large",
                     what);
        return NULL;
    }
    storage = emu68k_persist_alloc((ULONG)total);
    if (!storage)
    {
        if (err && errlen)
            snprintf(err, errlen, "no memory for retained %s", what);
        return NULL;
    }
    memset(storage, 0, (ULONG)total);
    list = (struct List *)(void *)storage;
    nodes = (struct Node *)(void *)(storage + sizeof(struct List));
    names = storage + sizeof(struct List) + count * sizeof(struct Node);
    NewList(list);

    walk = guest_be32(guest0, guest_list + M68K_List_lh_Head);
    for (ULONG i = 0; i < count; i++)
    {
        ULONG name = guest_be32(guest0, walk + M68K_Node_ln_Name);
        nodes[i].ln_Type = ((const UBYTE *)guest0)[walk + M68K_Node_ln_Type];
        nodes[i].ln_Pri = (BYTE)((const UBYTE *)guest0)[walk + M68K_Node_ln_Pri];
        if (name)
        {
            ULONG n = 0;
            do { names[n] = ((const UBYTE *)guest0)[name + n]; } while (names[n++]);
            nodes[i].ln_Name = (STRPTR)names;
            names += n;
        }
        AddTail(list, &nodes[i]);
        walk = guest_be32(guest0, walk + M68K_Node_ln_Succ);
    }
    return list;

malformed:
    if (err && errlen)
        snprintf(err, errlen, "%s is malformed, cyclic, unterminated, or exceeds %lu nodes",
                 what, (unsigned long)max_nodes);
    return NULL;
}

/* MX and Cycle gadgets retain a NULL-terminated STRPTR array.  Rebuild both
 * pointer widths and the pointed-to strings in one run-lifetime allocation. */
static STRPTR *guest_cstr_array_to_native(APTR guest0, ULONG guest_array,
                                          ULONG max_count, const char *what,
                                          char *err, ULONG errlen)
{
    ULONG count = 0, strings_size = 0;
    UQUAD total;
    STRPTR *array;
    UBYTE *strings;

    while (count < max_count)
    {
        ULONG gp, n = 0;
        if (emu68k_require_guest_range(guest_array + count * 4, 4,
                                       what, err, errlen) < 0)
            return NULL;
        gp = guest_be32(guest0, guest_array + count * 4);
        if (!gp)
            break;
        while (n < 65536)
        {
            if (emu68k_require_guest_range(gp + n, 1, what, err, errlen) < 0)
                return NULL;
            n++;
            if (!((const UBYTE *)guest0)[gp + n - 1])
                break;
        }
        if (n == 65536 || strings_size > 1024 * 1024 - n)
            goto malformed;
        strings_size += n;
        count++;
    }
    if (count == max_count)
        goto malformed;

    total = (UQUAD)(count + 1) * sizeof(STRPTR) + strings_size;
    if (total > 1024 * 1024 || !emu68k_persist_alloc)
    {
        if (err && errlen)
            snprintf(err, errlen, "%s has no retained-array allocator or is too large",
                     what);
        return NULL;
    }
    array = emu68k_persist_alloc((ULONG)total);
    if (!array)
    {
        if (err && errlen)
            snprintf(err, errlen, "no memory for retained %s", what);
        return NULL;
    }
    memset(array, 0, (ULONG)total);
    strings = (UBYTE *)(void *)(array + count + 1);
    for (ULONG i = 0; i < count; i++)
    {
        ULONG gp = guest_be32(guest0, guest_array + i * 4);
        ULONG n = 0;
        do { strings[n] = ((const UBYTE *)guest0)[gp + n]; } while (strings[n++]);
        array[i] = (STRPTR)strings;
        strings += n;
    }
    return array;

malformed:
    if (err && errlen)
        snprintf(err, errlen, "%s is unterminated or exceeds %lu strings",
                 what, (unsigned long)max_count);
    return NULL;
}

/* Convert a guest's packed, big-endian 8-byte TagItems into native 16-byte
 * TagItems. TAG_MORE is flattened; IGNORE/SKIP are interpreted while walking.
 * The semantic policy determines ti_Data's type. Unknown/refused tags fail the
 * crossing with their domain and name instead of being guessed as scalars. */
static LONG tags_to_native(APTR guest0, ULONG guest_tags,
                           const struct EmuTagDomain *domain,
                           struct TagItem *native_tags, ULONG capacity,
                           APTR scratch, ULONG scratch_size,
                           BOOL omit_unknown,
                           char *err, ULONG errlen)
{
    ULONG p = guest_tags, out = 0, steps = 0, used = 0;

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
            /* A guest-defined BOOPSI subclass sees its original guest taglist
             * in the callback.  When it subsequently calls its native system
             * superclass, private subclass tags are unknown by definition and
             * must be omitted; all public/system crossings keep the strict
             * fail-closed behaviour below. */
            if (omit_unknown)
            {
                p += 8;
                continue;
            }
            if (err && errlen)
                snprintf(err, errlen, "capability gap: unknown tag %08lx "
                         "value %08lx in %s", (unsigned long)tag,
                         (unsigned long)data, domain->name);
            return -1;
        }
        if (desc->kind == EMU_TAG_REFUSE)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: tag %s in %s needs object policy",
                         desc->name, domain->name);
            return -1;
        }
        if (desc->kind == EMU_TAG_NULL && data)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: tag %s in %s needs pointer policy",
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
        if (desc->kind == EMU_TAG_OBJECT ||
            desc->kind == EMU_TAG_OBJECT_OR_FFFF)
        {
            APTR object;
            if (desc->kind == EMU_TAG_OBJECT_OR_FFFF && data == 0xffffffffUL)
            {
                native_tags[out].ti_Data = (IPTR)-1;
                out++; p += 8;
                continue;
            }
            if (desc->mirror)
            {
                /* An adoptable type: the value may be a structure the program
                 * (or a guest-side library) built itself. The adopt path
                 * resolves an issued token at the head anyway, so this only
                 * widens what the tag accepts. */
                if (!data && !desc->object_nullable)
                {
                    if (err && errlen)
                        snprintf(err, errlen, "capability gap: NULL %s object",
                                 desc->name);
                    return -1;
                }
                if (emu68k_object_adopt_guest(guest0, data, desc->object_type,
                                              desc->name, desc->mirror,
                                              &object, err, errlen) < 0)
                    return -1;
            }
            else if (emu68k_object_from_guest(guest0, data, desc->object_type,
                                              desc->object_nullable, desc->name,
                                              &object, err, errlen) < 0)
                return -1;
            native_tags[out].ti_Data = (IPTR)object;
        }
        else if (desc->kind == EMU_TAG_OUT_U32)
        {
            ULONG need = (sizeof(struct EmuTagOutSlot) + 7u) & ~7u;
            struct EmuTagOutSlot *slot;

            if (used + need > scratch_size || !scratch)
            {
                if (err && errlen)
                    snprintf(err, errlen, "tag %s has no output scratch in %s",
                             desc->name, domain->name);
                return -1;
            }
            if (emu68k_require_guest_range(data, 4, desc->name,
                                           err, errlen) < 0)
                return -1;
            slot = (struct EmuTagOutSlot *)((UBYTE *)scratch + used);
            used += need;
            slot->value = (IPTR)(ULONG)emu68k_scalar_from_guest(guest0, data, 4);
            slot->guest_addr = data;
            native_tags[out].ti_Data = (IPTR)&slot->value;
        }
        else if ((desc->kind == EMU_TAG_STRUCT ||
                  desc->kind == EMU_TAG_STRUCT_INOUT) && desc->sdesc1)
        {
            /* A structure with followed pointer fields: rebuilt whole,
             * run-lifetime, because the callee retains it. */
            APTR node;

            if (!data)
            {
                native_tags[out].ti_Data = 0;
                out++; p += 8;
                continue;
            }
            node = emu68k_deep_convert(guest0, data, desc->sdescs,
                                       &desc->sdescs[desc->sdesc1 - 1],
                                       33, desc->name, err, errlen);
            if (!node)
                return -1;
            native_tags[out].ti_Data = (IPTR)node;
        }
        else if (desc->kind == EMU_TAG_STRUCT ||
                 desc->kind == EMU_TAG_STRUCT_INOUT)
        {
            /* The value is a guest pointer to a structure, so the callee is
             * given a native one built from it, living in the caller's scratch
             * for exactly the length of the call. */
            ULONG need = (desc->native_size + 7u) & ~7u;
            UBYTE *slot;

            if (!data)
            {
                native_tags[out].ti_Data = 0;
                out++; p += 8;
                continue;
            }
            if (used + need > scratch_size || !scratch)
            {
                if (err && errlen)
                    snprintf(err, errlen, "tag %s has no room to rebuild %s",
                             desc->name, domain->name);
                return -1;
            }
            if (emu68k_require_guest_range(data, desc->guest_size,
                                           desc->name, err, errlen) < 0)
                return -1;
            slot = (UBYTE *)scratch + used;
            used += need;
            memset(slot, 0, desc->native_size);
            emu68k_from_guest(guest0, data, slot, desc->fields, desc->nfields);
            native_tags[out].ti_Data = (IPTR)slot;
        }
        else if (desc->kind == EMU_TAG_U16_FFFF)
        {
            ULONG count, need = ((ULONG)desc->guest_size * sizeof(UWORD) + 7u) & ~7u;
            UWORD *slot;

            if (!data)
            {
                native_tags[out].ti_Data = 0;
                out++; p += 8;
                continue;
            }
            if (used + need > scratch_size || !scratch)
            {
                if (err && errlen)
                    snprintf(err, errlen, "tag %s has no room to rebuild UWORD array",
                             desc->name);
                return -1;
            }
            slot = (UWORD *)((UBYTE *)scratch + used);
            used += need;
            for (count = 0; count < desc->guest_size; count++)
            {
                if (emu68k_require_guest_range(data + count * 2, 2,
                                               desc->name, err, errlen) < 0)
                    return -1;
                slot[count] = guest_be16(guest0, data + count * 2);
                if (slot[count] == (UWORD)~0)
                    break;
            }
            if (count == desc->guest_size)
            {
                if (err && errlen)
                    snprintf(err, errlen, "tag %s UWORD array has no terminator",
                             desc->name);
                return -1;
            }
            native_tags[out].ti_Data = (IPTR)slot;
        }
        else if (desc->kind == EMU_TAG_RGB32)
        {
            ULONG need = ((ULONG)desc->guest_size * sizeof(ULONG) + 7u) & ~7u;
            ULONG *slot;

            if (!data)
            {
                native_tags[out].ti_Data = 0;
                out++; p += 8;
                continue;
            }
            if (used + need > scratch_size || !scratch)
            {
                if (err && errlen)
                    snprintf(err, errlen, "tag %s has no room to rebuild RGB32 stream",
                             desc->name);
                return -1;
            }
            slot = (ULONG *)((UBYTE *)scratch + used);
            used += need;
            if (emu68k_rgb32_to_native(guest0, data, slot, desc->guest_size,
                                       desc->name, err, errlen) < 0)
                return -1;
            native_tags[out].ti_Data = (IPTR)slot;
        }
        else if (desc->kind == EMU_TAG_NODELIST)
        {
            struct List *list;

            if (!data || data == 0xffffffffUL)
                native_tags[out].ti_Data = data ? (IPTR)-1 : 0;
            else
            {
                list = guest_node_list_to_native(guest0, data,
                                                 desc->guest_size,
                                                 desc->name, err, errlen);
                if (!list)
                    return -1;
                native_tags[out].ti_Data = (IPTR)list;
            }
        }
        else if (desc->kind == EMU_TAG_CSTR_ARRAY)
        {
            STRPTR *array;

            if (!data)
                native_tags[out].ti_Data = 0;
            else
            {
                array = guest_cstr_array_to_native(guest0, data,
                                                  desc->guest_size,
                                                  desc->name, err, errlen);
                if (!array)
                    return -1;
                native_tags[out].ti_Data = (IPTR)array;
            }
        }
        else if (desc->kind == EMU_TAG_CSTR)
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

LONG emu68k_tags_to_native(APTR guest0, ULONG guest_tags,
                           const struct EmuTagDomain *domain,
                           struct TagItem *native_tags, ULONG capacity,
                           APTR scratch, ULONG scratch_size,
                           char *err, ULONG errlen)
{
    return tags_to_native(guest0, guest_tags, domain, native_tags, capacity,
                          scratch, scratch_size, FALSE, err, errlen);
}

LONG emu68k_tags_to_native_known(APTR guest0, ULONG guest_tags,
                                 const struct EmuTagDomain *domain,
                                 struct TagItem *native_tags, ULONG capacity,
                                 APTR scratch, ULONG scratch_size,
                                 char *err, ULONG errlen)
{
    return tags_to_native(guest0, guest_tags, domain, native_tags, capacity,
                          scratch, scratch_size, TRUE, err, errlen);
}

/* Copy proven scalar output attributes back after the native call. Slots are
 * initialized from guest memory before the call, so an unsupported GetAttr
 * that leaves its destination untouched also leaves the guest value intact. */
LONG emu68k_tags_to_guest(APTR guest0, struct TagItem *native_tags,
                          ULONG count, const struct EmuTagDomain *domain,
                          char *err, ULONG errlen)
{
    ULONG i;
    if (!domain || (!native_tags && count))
        return -1;
    for (i = 0; i < count; i++)
    {
        const struct EmuTagDesc *desc = tag_desc(domain, native_tags[i].ti_Tag);
        if (!desc)
        {
            if (err && errlen)
                snprintf(err, errlen, "native tag %08lx escaped domain %s",
                         (unsigned long)native_tags[i].ti_Tag, domain->name);
            return -1;
        }
        if (desc->kind == EMU_TAG_OUT_U32)
        {
            struct EmuTagOutSlot *slot =
                (struct EmuTagOutSlot *)(IPTR)native_tags[i].ti_Data;
            if (!slot)
            {
                if (err && errlen)
                    snprintf(err, errlen, "tag %s lost its output slot",
                             desc->name);
                return -1;
            }
            emu68k_scalar_to_guest(guest0, slot->guest_addr, 4,
                                   (UQUAD)(ULONG)slot->value);
        }
    }
    return 0;
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
        /* A native address has no guest form, so a rebased pointer is not
         * written back: what the guest put there is still what it means. */
        if (f->kind == EMU_F_GUESTPTR)
            continue;
        if (f->kind == EMU_F_BPTR)
        {
            /* A native BPTR is 64-bit and means nothing in the arena. Give
             * the guest the same 32-bit token it would get from a call that
             * returned this lock, so it can pass it straight back. */
            for (e = 0; e < count; e++)
            {
                BPTR b = *(BPTR *)(void *)(nat + f->n_off + e * f->n_w);
                guest_write(g + f->g_off + e * f->g_w, f->g_w,
                            b ? emu68k_handle_token(guest0, b) : 0);
            }
            continue;
        }
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
            {
                ULONG v = guest_read(g + f->g_off + e * f->g_w, f->g_w);
                if (f->kind == EMU_F_GUESTPTR)
                    *(APTR *)(void *)(nat + f->n_off + e * f->n_w) =
                        v ? (APTR)((UBYTE *)guest0 + v) : NULL;
                else if (f->kind == EMU_F_BPTR)
                    *(BPTR *)(void *)(nat + f->n_off + e * f->n_w) =
                        v ? emu68k_handle_bptr(guest0, v) : BNULL;
                else
                    native_write(nat + f->n_off + e * f->n_w, f->n_w, v);
            }
    }
}

void emu68k_from_guest(APTR guest0, ULONG gbase, void *native,
                       const struct EmuField *f, int n)
{
    emu68k_from_guest_sized(guest0, gbase, native, f, n, EMU_NO_LIMIT);
}
