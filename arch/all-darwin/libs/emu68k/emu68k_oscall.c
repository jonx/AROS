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
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <intuition/diattr.h>
#include <intuition/extensions.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/cybergraphics.h>

#include "emu68k_intern.h"
#include "emu68k_gen.h"
#include "emu68k_lvos.h"
#include "emu68k_layouts.h"
#include LC_LIBDEFS_FILE

#include <aros/debug.h>

#include "emu68k_guest_offsets.h"
#include <aros/asmcall.h>
#include <string.h>

#define ICON_LVO_FINDTOOLTYPE  16   /* -96  */
#define GFX_LVO_ALLOCRASTER    82   /* -492 */
#define GFX_LVO_FREERASTER     83   /* -498 */
#define GFX_LVO_ALLOCBITMAP   153   /* -918 */
#define GFX_LVO_FREEBITMAP    154   /* -924 */
#define WB_LVO_ADDAPPWINDOW     8   /* -48  */
#define WB_LVO_REMOVEAPPWINDOW  9   /* -54  */
#define WB_LVO_ADDAPPICON      10   /* -60  */
#define WB_LVO_REMOVEAPPICON   11   /* -66  */
#define WB_LVO_ADDAPPMENUITEM  12   /* -72  */
#define WB_LVO_REMOVEAPPMENUITEM 13 /* -78  */
#define WB_LVO_ADDAPPWINDOWDROPZONE 19    /* -114 */
#define WB_LVO_REMOVEAPPWINDOWDROPZONE 20 /* -120 */
#define GADTOOLS_LVO_GT_GETIMSG      12
#define GADTOOLS_LVO_GT_REPLYIMSG    13

/* TextFont facades reserve their public TextFontExtension and five classic
 * TagItems before the trailing 64-byte font name generated from policy. */
#define EMU68K_TEXTFONT_EXT_OFF       M68K_TextFont_SIZEOF
#define EMU68K_TEXTFONT_EXT_SIZE      24
#define EMU68K_TEXTFONT_EXT_TAGS_OFF  (EMU68K_TEXTFONT_EXT_OFF + \
                                       EMU68K_TEXTFONT_EXT_SIZE)
#define EMU68K_TEXTFONT_EXT_TAGS_MAX  5
#define EMU68K_TEXTFONT_NAME_OFF      116

/* A guest pointer becomes a host pointer by adding the guest base. Only memory
 * INSIDE the guest arena may be handed to a native call this way. */
static APTR gptr(APTR guest0, ULONG addr)
{
    return addr ? (APTR)((UBYTE *)guest0 + addr) : NULL;
}

static void gw32(APTR guest0, ULONG addr, ULONG v)
{
    UBYTE *p = (UBYTE *)guest0 + addr;
    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v;
}

static ULONG gr32(APTR guest0, ULONG addr)
{
    const UBYTE *p = (const UBYTE *)guest0 + addr;
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

/* These setters are deliberately mirrored instead of treating a guest-owned
 * RastPort as a native pointer.  The byte fields have the same scalar meaning
 * in both ABIs; the UWORD Flags field is kept big-endian in guest memory. */
#define EMU68K_RPF_NO_PENS (1U << 14)

static void guest_rastport_minterms(APTR guest0, ULONG rp)
{
    UBYTE *g = (UBYTE *)guest0;
    UBYTE fg = g[rp + M68K_RastPort_FgPen];
    UBYTE bg = g[rp + M68K_RastPort_BgPen];
    UBYTE mode = g[rp + M68K_RastPort_DrawMode];
    UBYTE min_a, min_b;
    ULONG i;

    if (mode & COMPLEMENT)
    {
        min_a = (mode & INVERSVID) ? 0x6a : 0x9a;
        for (i = 0; i < 8; i++)
            g[rp + M68K_RastPort_minterms + i] = min_a;
        return;
    }
    for (i = 0; i < 8; i++)
    {
        min_a = ((fg >> i) & 1) * 3;
        min_b = (mode & JAM2) ? ((bg >> i) & 1) * 3 : 2;
        min_a = (mode & INVERSVID)
            ? (UBYTE)((min_b << 2) | min_a)
            : (UBYTE)((min_a << 2) | min_b);
        g[rp + M68K_RastPort_minterms + i] = (min_a << 4) | 0x0a;
    }
}

static void guest_rastport_enable_pens(APTR guest0, ULONG rp)
{
    ULONG flags = emu68k_scalar_from_guest(guest0,
        rp + M68K_RastPort_Flags, 2);
    emu68k_scalar_to_guest(guest0, rp + M68K_RastPort_Flags, 2,
                           flags & ~EMU68K_RPF_NO_PENS);
}

static void guest_text_extent_from_native(APTR guest0, ULONG gp,
                                          const struct TextExtent *te)
{
    emu68k_scalar_to_guest(guest0, gp + M68K_TextExtent_te_Width, 2,
                           te->te_Width);
    emu68k_scalar_to_guest(guest0, gp + M68K_TextExtent_te_Height, 2,
                           te->te_Height);
    emu68k_scalar_to_guest(guest0, gp + M68K_TextExtent_te_Extent_MinX, 2,
                           te->te_Extent.MinX);
    emu68k_scalar_to_guest(guest0, gp + M68K_TextExtent_te_Extent_MinY, 2,
                           te->te_Extent.MinY);
    emu68k_scalar_to_guest(guest0, gp + M68K_TextExtent_te_Extent_MaxX, 2,
                           te->te_Extent.MaxX);
    emu68k_scalar_to_guest(guest0, gp + M68K_TextExtent_te_Extent_MaxY, 2,
                           te->te_Extent.MaxY);
}

static void guest_text_extent_to_native(APTR guest0, ULONG gp,
                                        struct TextExtent *te)
{
    te->te_Width = emu68k_scalar_from_guest(
        guest0, gp + M68K_TextExtent_te_Width, 2);
    te->te_Height = emu68k_scalar_from_guest(
        guest0, gp + M68K_TextExtent_te_Height, 2);
    te->te_Extent.MinX = emu68k_scalar_from_guest(
        guest0, gp + M68K_TextExtent_te_Extent_MinX, 2);
    te->te_Extent.MinY = emu68k_scalar_from_guest(
        guest0, gp + M68K_TextExtent_te_Extent_MinY, 2);
    te->te_Extent.MaxX = emu68k_scalar_from_guest(
        guest0, gp + M68K_TextExtent_te_Extent_MaxX, 2);
    te->te_Extent.MaxY = emu68k_scalar_from_guest(
        guest0, gp + M68K_TextExtent_te_Extent_MaxY, 2);
}

/* A guest string, only if the whole of it is inside the arena. Returns NULL
 * rather than a truncated or runaway string. */
static const char *guest_cstr(APTR guest0, ULONG addr, ULONG max)
{
    ULONG n;
    if (!addr)
        return NULL;
    for (n = 0; n < max; n++)
    {
        if (emu68k_require_guest_range(addr + n, 1, "string", NULL, 0) < 0)
            return NULL;
        if (!((const UBYTE *)guest0)[addr + n])
            return (const char *)guest0 + addr;
    }
    return NULL;
}

static int easy_append(char *out, ULONG room, ULONG *used,
                       const char *text, ULONG length)
{
    if (!out || !used || *used > room || length >= room - *used)
        return -1;
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = 0;
    return 0;
}

/* EasyRequestArgs receives a RawDoFmt-style stream in A3.  That stream is
 * 68k-sized and big-endian, so it cannot be handed to native Intuition.  Make
 * the final text while it is still in the guest ABI, then give Intuition a
 * format with no remaining arguments.  This is the common RawDoFmt surface
 * used by system requesters; unsupported/runaway input fails by name instead
 * of becoming a native varargs read. */
static LONG easy_text_from_guest(APTR guest0, ULONG format, ULONG args,
                                 char *out, ULONG room,
                                 char *err, ULONG errlen)
{
    const char *f = guest_cstr(guest0, format, 65536);
    ULONG fp = 0, ap = args, used = 0;

    if (!f || !out || room < 2)
        goto invalid;
    out[0] = 0;
    while (f[fp])
    {
        char spec[48], temp[1024];
        ULONG si = 0, value, bytes;
        char conv;
        BOOL is_long = FALSE;

        if (f[fp] != '%')
        {
            if (easy_append(out, room, &used, f + fp, 1) < 0) goto large;
            fp++;
            continue;
        }
        fp++;
        if (f[fp] == '%')
        {
            if (easy_append(out, room, &used, "%", 1) < 0) goto large;
            fp++;
            continue;
        }

        spec[si++] = '%';
        while (strchr("-+ #0", f[fp]))
        {
            if (si + 2 >= sizeof spec) goto invalid;
            spec[si++] = f[fp++];
        }
        while (f[fp] >= '0' && f[fp] <= '9')
        {
            if (si + 2 >= sizeof spec) goto invalid;
            spec[si++] = f[fp++];
        }
        if (f[fp] == '.')
        {
            if (si + 2 >= sizeof spec) goto invalid;
            spec[si++] = f[fp++];
            while (f[fp] >= '0' && f[fp] <= '9')
            {
                if (si + 2 >= sizeof spec) goto invalid;
                spec[si++] = f[fp++];
            }
        }
        if (f[fp] == 'l') { is_long = TRUE; fp++; }
        conv = f[fp++];
        if (!conv) goto invalid;

        if (conv == 's' || conv == 'b')
        {
            const char *s;
            char bstr[256];
            ULONG p, n;
            if (!ap || emu68k_require_guest_range(ap, 4,
                    "EasyRequestArgs string argument", err, errlen) < 0)
                return -1;
            p = gr32(guest0, ap); ap += 4;
            if (conv == 's')
            {
                s = guest_cstr(guest0, p, 65536);
                if (!s) goto invalid;
            }
            else
            {
                p <<= 2;
                if (emu68k_require_guest_range(p, 1,
                        "EasyRequestArgs BSTR", err, errlen) < 0)
                    return -1;
                n = *((UBYTE *)guest0 + p++);
                if (emu68k_require_guest_range(p, n,
                        "EasyRequestArgs BSTR bytes", err, errlen) < 0)
                    return -1;
                memcpy(bstr, (UBYTE *)guest0 + p, n);
                bstr[n] = 0;
                s = bstr;
            }
            spec[si++] = 's'; spec[si] = 0;
            if (snprintf(temp, sizeof temp, spec, s) < 0) goto invalid;
        }
        else if (conv == 'c')
        {
            if (!ap || emu68k_require_guest_range(ap, 2,
                    "EasyRequestArgs character argument", err, errlen) < 0)
                return -1;
            value = (ULONG)emu68k_scalar_from_guest(guest0, ap, 2); ap += 2;
            spec[si++] = 'c'; spec[si] = 0;
            if (snprintf(temp, sizeof temp, spec, (int)(UBYTE)value) < 0)
                goto invalid;
        }
        else if (strchr("diuxX", conv))
        {
            bytes = is_long ? 4 : 2;
            if (!ap || emu68k_require_guest_range(ap, bytes,
                    "EasyRequestArgs numeric argument", err, errlen) < 0)
                return -1;
            value = (ULONG)emu68k_scalar_from_guest(guest0, ap, (UBYTE)bytes);
            ap += bytes;
            spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0;
            if (conv == 'd' || conv == 'i')
            {
                LONG signed_value = is_long ? (LONG)value : (WORD)value;
                if (snprintf(temp, sizeof temp, spec, (long)signed_value) < 0)
                    goto invalid;
            }
            else if (snprintf(temp, sizeof temp, spec,
                              (unsigned long)value) < 0)
                goto invalid;
        }
        else
        {
            temp[0] = conv; temp[1] = 0;
        }
        if (easy_append(out, room, &used, temp, strlen(temp)) < 0) goto large;
    }
    return 0;

large:
    if (err && errlen)
        snprintf(err, errlen, "EasyRequestArgs formatted text exceeds %lu bytes",
                 (unsigned long)(room - 1));
    return -1;
invalid:
    if (err && errlen)
        snprintf(err, errlen, "EasyRequestArgs has an invalid EasyStruct string or RAWARG stream");
    return -1;
}

UQUAD emu68k_scalar_from_guest(APTR guest0, ULONG addr, UBYTE width)
{
    const UBYTE *p = (const UBYTE *)guest0 + addr;
    UQUAD value = 0;
    UBYTE i;
    for (i = 0; i < width; i++) value = (value << 8) | p[i];
    return value;
}

void emu68k_scalar_to_guest(APTR guest0, ULONG addr, UBYTE width, UQUAD value)
{
    UBYTE *p = (UBYTE *)guest0 + addr;
    while (width)
    {
        p[--width] = (UBYTE)value;
        value >>= 8;
    }
}

/* A string a guest library reads out of a facade. Always terminated within
 * the reserved room, and a NULL source leaves an empty string rather than
 * whatever the previous occupant of that memory was. */
void emu68k_cstr_to_guest(APTR guest0, ULONG addr, const char *s, ULONG room)
{
    UBYTE *p = (UBYTE *)guest0 + addr;
    ULONG i = 0;

    if (!room) return;
    if (s)
        for (; i < room - 1 && s[i]; i++)
            p[i] = (UBYTE)s[i];
    p[i] = 0;
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
#define EMU68K_MAX_SCANS   4
/* A classic GUI hands over a whole gadget family at once and takes a RastPort
 * per render, so a real program needs far more live objects than a test does. */
#define EMU68K_MAX_OBJECTS 1024
#define EMU68K_MAX_TOMBSTONES 4096
#define EMU68K_MAX_BITMAPS 64
#define EMU68K_MAX_RUNS    4
#define EMU68K_OBJECT_TOKEN_BASE 0xE6800000UL

/* A slot holds either an object the BRIDGE issued (a native object the guest
 * knows by token) or a MIRROR of a structure the PROGRAM allocated. The two
 * cannot be confused: only a mirror's token is a readable guest address, so
 * anything that walks or rewrites guest memory has to know which it has. */
#define EMU68K_OBJ_GUEST_OWNED  0x0001   /* a mirror of the program's memory  */
#define EMU68K_OBJ_ADOPT_FRESH  0x0002   /* created by the adoption in flight */
#define EMU68K_OBJ_GADGET_VIEW  0x0004   /* facade carries a converted Gadget */
#define EMU68K_OBJ_GADGET_PROP  0x0008   /* mirror owns appended PropInfo      */
#define EMU68K_OBJ_GADGET_STRING 0x0010  /* mirror owns StringInfo and buffers */
#define EMU68K_OBJ_GUEST_CLASS  0x0020   /* per-run public BOOPSI shadow       */
#define EMU68K_OBJ_BORROWED     0x0040   /* native owner outlives this borrow  */

struct Emu68kObject
{
    APTR native;
    APTR base;
    EmuObjectCleanup cleanup;
    ULONG token;
    ULONG refs;
    UWORD type;
    UWORD flags;
    /* A guest-created BOOPSI class remains callable after NewObjectA returns:
     * Intuition invokes gadget methods later while processing input.  The
     * temporary per-crossing bridge nests on top of this run-lifetime one. */
    struct Emu68kBoopsiBridge persistent_boopsi;
};

/* ---- PER-RUN STATE --------------------------------------------------------
 * Handles and directory scans belong to ONE guest program, not to the library:
 * two 68k programs running at once each have their own file handles, and a
 * token is only meaningful against the run that issued it. Keeping these in
 * file-scope tables worked only while a single guest ran at a time, and would
 * have started handing one program another's files the moment that stopped
 * being true. `guest0` is the run's own arena base, so it identifies the run
 * uniquely and is already passed to every call.
 *
 * Library bases deliberately stay shared below: an open library is a refcounted
 * OS resource, not guest state, and reopening it per run would be waste. */
#define EMU68K_MAX_IMSG 64
#define EXEC_LVO_REPLYMSG 63
#define EXEC_LVO_OPENDEVICE  74
#define EXEC_LVO_CLOSEDEVICE 75
#define EMU68K_MAX_DEV 8

#define INT_LVO_MODIFYIDCMP 25
#define EXEC_LVO_EVENT_PUMP 9001 /* private typed native-event broker pump */
#define EMU68K_MAX_IDCMP  16

struct Emu68kRunState
{
    APTR  guest0;                                    /* NULL = a free slot     */
    emu68k_run_h run;
    ULONG (*guest_alloc)(emu68k_run_h r, unsigned long size);
    int (*call_hook)(emu68k_run_h r, unsigned long entry,
                     unsigned long hook, unsigned long object,
                     unsigned long message, unsigned int *result,
                     char *err, unsigned errlen);
    struct { BPTR bptr; } handles[EMU68K_MAX_HANDLES];
    struct { ULONG guest; struct AnchorPath *nap; } scans[EMU68K_MAX_SCANS];
    struct { ULONG guest; } bitmaps[EMU68K_MAX_BITMAPS];
    struct Emu68kObject objects[EMU68K_MAX_OBJECTS];
    /* Facades are allocated inside the same readable arena as program-owned
     * structures.  Once released, their addresses must never be re-adopted as
     * if the program had allocated them itself.  The arena allocator is
     * monotonic for a run, so a bounded retired-address set is sufficient;
     * exhaustion makes later adoption fail closed. */
    ULONG object_tombstones[EMU68K_MAX_TOMBSTONES];
    ULONG object_tombstone_count;
    BOOL object_tombstones_full;
    /* Intuition's own messages, paired with the guest copies handed out, so a
     * reply reaches the message Intuition is waiting to get back. */
    struct { APTR native; APTR original; ULONG guest; UBYTE allocated; }
        imsg[EMU68K_MAX_IMSG];
    /* Which guest port a window's IDCMP is delivered to. Recorded when the
     * program says so, never guessed: several windows commonly share one.
     * synth_classes replays an event edge the program provably raced against
     * (native activation completes whole quanta before the guest's
     * ModifyIDCMP can enable delivery); active_seen bounds it to one per
     * activation epoch. */
    struct { APTR window; ULONG guest_port; ULONG synth_classes;
             UBYTE active_seen; } idcmp[EMU68K_MAX_IDCMP];
    /* The program directory currently applied to this process on behalf of a
     * guest context, and the process's own one to restore at end of run. */
    char  progdir_applied[256];
    BPTR  progdir_lock;
    BPTR  progdir_saved;
    BOOL  progdir_active;
    /* Native endian-converted sprite words retained by SetPointer until the
     * matching ClearPointer, replacement, or run teardown. */
    struct { APTR window; UWORD *words; } pointer_shadow[EMU68K_MAX_IDCMP];
    struct { APTR native; ULONG guest; } monitors[8];
    APTR pubscreen_list;
    ULONG pubscreen_guest;
    struct { APTR native; ULONG guest; } cmodes[8];
    struct {
        APTR handle;
        APTR native_base;
        ULONG guest_handle;
        ULONG guest_buffer;
        ULONG size;
    } cyberlock[16];
    /* Devices opened for the guest. The native IORequest is OURS: the guest's
     * is big-endian and 32-bit and can never be handed to a device. */
    struct { struct IORequest *req; ULONG guest_req; char name[32]; } dev[EMU68K_MAX_DEV];
    ULONG (*device_base)(emu68k_run_h r, const char *name);
    void (*set_mouse_buttons)(emu68k_run_h r, unsigned int buttons);
    ULONG mouse_buttons;
    ULONG next_object;
    /* Deep-marshalled structures the callee retains (a class keeps the label
     * it was given); freed when the run ends, after the objects holding them
     * are disposed. */
    struct Emu68kPersistHdr *persist_head;
};

struct Emu68kPersistHdr
{
    struct Emu68kPersistHdr *next;
    IPTR pad;                       /* keep the payload 16-aligned */
};

static struct Emu68kRunState g_runs[EMU68K_MAX_RUNS];

/* The run whose crossing is currently executing. The service is driven by
 * one scheduler thread, so a plain static is the whole story. */
static struct Emu68kRunState *g_persist_rs;

static APTR emu68k_persist_from_run(ULONG size)
{
    struct Emu68kPersistHdr *h;

    if (!g_persist_rs)
        return NULL;
    h = AllocVec(sizeof *h + size, MEMF_CLEAR);
    if (!h)
        return NULL;
    h->next = g_persist_rs->persist_head;
    g_persist_rs->persist_head = h;
    return h + 1;
}

static CONST_STRPTR retained_guest_title(struct Emu68kRunState *rs,
                                         APTR guest0, ULONG address,
                                         char *err, ULONG errlen)
{
    const char *source;
    char *copy;
    ULONG length;
    struct Emu68kRunState *previous;

    if (!address) return NULL;
    if (address == 0xffffffffUL)
        return (CONST_STRPTR)(IPTR)-1;
    source = guest_cstr(guest0, address, 65536);
    if (!source)
    {
        if (err && errlen)
            snprintf(err, errlen, "SetWindowTitles string is outside guest memory or unterminated");
        return NULL;
    }
    length = (ULONG)strlen(source) + 1;
    previous = g_persist_rs;
    g_persist_rs = rs;
    copy = emu68k_persist_from_run(length);
    g_persist_rs = previous;
    if (!copy)
    {
        if (err && errlen) snprintf(err, errlen, "SetWindowTitles string shadow allocation failed");
        return NULL;
    }
    memcpy(copy, source, length);
    return copy;
}

static struct Emu68kRunState *run_state(APTR guest0)
{
    int i, free_slot = -1;

    for (i = 0; i < EMU68K_MAX_RUNS; i++)
    {
        if (g_runs[i].guest0 == guest0) return &g_runs[i];
        if (!g_runs[i].guest0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;                  /* too many live guests   */
    memset(&g_runs[free_slot], 0, sizeof g_runs[free_slot]);
    g_runs[free_slot].guest0 = guest0;
    return &g_runs[free_slot];
}

AROS_UFH3(static IPTR, emu68k_native_hook_entry,
          AROS_UFHA(struct Hook *, hook, A0),
          AROS_UFHA(APTR, object, A2),
          AROS_UFHA(APTR, message, A1))
{
    AROS_USERFUNC_INIT

    struct Emu68kHookBridge *bridge = hook ? hook->h_Data : NULL;
    struct Emu68kRunState *rs = bridge ? bridge->state : NULL;
    unsigned int result = 0;

    if (!bridge || !rs || !rs->call_hook)
    {
        if (bridge)
        {
            bridge->failed = TRUE;
            snprintf(bridge->error, sizeof bridge->error,
                     "Hook callback has no active 68k run");
        }
        return 0;
    }
    if (rs->call_hook(rs->run, bridge->entry, bridge->guest_hook,
                      (ULONG)(IPTR)object, (ULONG)(IPTR)message, &result,
                      bridge->error, sizeof bridge->error) != 0)
    {
        bridge->failed = TRUE;
        return 0;
    }
    return (IPTR)result;

    AROS_USERFUNC_EXIT
}

LONG emu68k_hook_prepare(APTR guest0, ULONG guest_hook,
                         struct Emu68kHookBridge *bridge,
                         char *err, ULONG errlen)
{
    ULONG entry;

    if (!bridge || emu68k_require_guest_range(guest_hook, M68K_Hook_SIZEOF,
                                               "Hook", err, errlen) < 0)
        return -1;
    entry = gr32(guest0, guest_hook + M68K_Hook_h_Entry);
    if (emu68k_require_guest_range(entry, 2, "Hook entry", err, errlen) < 0)
        return -1;
    memset(bridge, 0, sizeof *bridge);
    bridge->native.h_Entry = (APTR)emu68k_native_hook_entry;
    bridge->native.h_Data = bridge;
    bridge->state = run_state(guest0);
    bridge->guest_hook = guest_hook;
    bridge->entry = entry;
    return bridge->state ? 0 : -1;
}

LONG emu68k_hook_finish(const struct Emu68kHookBridge *bridge,
                        char *err, ULONG errlen)
{
    if (!bridge || !bridge->failed) return 0;
    if (err && errlen)
        snprintf(err, errlen, "68k Hook callback failed: %s", bridge->error);
    return -1;
}

static struct Emu68kObject *object_by_token(struct Emu68kRunState *rs,
                                            ULONG token);
static struct Emu68kObject *object_by_native(struct Emu68kRunState *rs,
                                             APTR native);
static BOOL class_roots_in(Class *cl, const char *id);

/* Turn the classic planar BitMap a program built in its arena into a
 * short-lived native view.  The plane storage is not copied: after exact
 * range validation the native software-bitmap wrapper may read or write the
 * guest planes directly for the duration of the call. */
static int guest_bitmap_view(struct Emu68kRunState *rs, APTR guest0,
                             ULONG gp, struct BitMap *shadow,
                             struct BitMap **native, const char *what,
                             char *err, ULONG errlen)
{
    struct Emu68kObject *o = object_by_token(rs, gp);
    ULONG bytesperrow, rows, depth, size, i;

    if (!gp || (o && o->type != EMU_OBJ_BitMap))
        return -1;
    if (o && !(o->flags & EMU68K_OBJ_GUEST_OWNED))
    {
        *native = (struct BitMap *)o->native;
        return 0;
    }
    if (emu68k_require_guest_range(gp, M68K_BitMap_SIZEOF,
                                   what, err, errlen) < 0)
        return -1;

    bytesperrow = emu68k_scalar_from_guest(
        guest0, gp + M68K_BitMap_BytesPerRow, 2);
    rows = emu68k_scalar_from_guest(guest0, gp + M68K_BitMap_Rows, 2);
    depth = *((UBYTE *)guest0 + gp + M68K_BitMap_Depth);
    if (!bytesperrow || !rows || !depth || depth > 8 ||
        (UQUAD)bytesperrow * rows > 0xffffffffUL)
    {
        if (err && errlen)
            snprintf(err, errlen, "%s has invalid planar geometry %lux%lu "
                     "depth %lu", what, (unsigned long)bytesperrow,
                     (unsigned long)rows, (unsigned long)depth);
        return -1;
    }

    memset(shadow, 0, sizeof *shadow);
    shadow->BytesPerRow = bytesperrow;
    shadow->Rows = rows;
    shadow->Flags = *((UBYTE *)guest0 + gp + M68K_BitMap_Flags);
    shadow->Depth = depth;
    size = bytesperrow * rows;
    for (i = 0; i < depth; i++)
    {
        ULONG plane = gr32(guest0, gp + M68K_BitMap_Planes + i * 4);
        /* Classic planar bitmaps may name an absent/all-zero plane with NULL
         * and an all-one plane with (PLANEPTR)-1.  Neither is a guest pointer
         * that should be rebased. */
        if (!plane || plane == 0xffffffffUL)
            shadow->Planes[i] = (PLANEPTR)(IPTR)plane;
        else
        {
            if (emu68k_require_guest_range(plane, size, "BitMap plane",
                                           err, errlen) < 0)
                return -1;
            shadow->Planes[i] = gptr(guest0, plane);
        }
    }
    *native = shadow;
    return 0;
}

/* BitMap.Planes is a pointer array, so the generated scalar layout correctly
 * leaves it untouched.  A guest-owned bitmap mirror nevertheless has to make
 * those planes usable by native graphics.library: validate each classic
 * pointer and rebase it for the duration of every crossing.  The actual plane
 * storage remains in guest memory, so drawing is immediately visible on both
 * sides and no byte copy is necessary. */
static LONG bitmap_planes_validate(APTR guest0, ULONG gp,
                                   char *err, ULONG errlen)
{
    ULONG bytesperrow, rows, depth, size, i;

    bytesperrow = emu68k_scalar_from_guest(
        guest0, gp + M68K_BitMap_BytesPerRow, 2);
    rows = emu68k_scalar_from_guest(
        guest0, gp + M68K_BitMap_Rows, 2);
    depth = *((UBYTE *)guest0 + gp + M68K_BitMap_Depth);
    if (depth > 8 || (UQUAD)bytesperrow * rows > 0xffffffffUL)
    {
        if (err && errlen)
            snprintf(err, errlen, "BitMap %08lx has invalid planar geometry "
                     "%lux%lu depth %lu", (unsigned long)gp,
                     (unsigned long)bytesperrow, (unsigned long)rows,
                     (unsigned long)depth);
        return -1;
    }
    size = bytesperrow * rows;
    for (i = 0; i < depth; i++)
    {
        ULONG plane = gr32(guest0, gp + M68K_BitMap_Planes + i * 4);
        if (plane && plane != 0xffffffffUL &&
            (!size || emu68k_require_guest_range(
                plane, size, "BitMap plane", err, errlen) < 0))
            return -1;
    }
    return 0;
}

static void bitmap_planes_from_guest(APTR guest0, ULONG gp,
                                     struct BitMap *native)
{
    ULONG depth = *((UBYTE *)guest0 + gp + M68K_BitMap_Depth);
    ULONG i;

    for (i = 0; i < 8; i++) native->Planes[i] = NULL;
    if (depth > 8) depth = 8;
    for (i = 0; i < depth; i++)
    {
        ULONG plane = gr32(guest0, gp + M68K_BitMap_Planes + i * 4);
        native->Planes[i] = (!plane || plane == 0xffffffffUL)
                          ? (PLANEPTR)(IPTR)plane : gptr(guest0, plane);
    }
}

static LONG bitmap_planes_to_guest(APTR guest0, ULONG gp,
                                   const struct BitMap *native,
                                   char *err, ULONG errlen)
{
    ULONG depth = *((UBYTE *)guest0 + gp + M68K_BitMap_Depth);
    ULONG i;

    if (depth > 8) depth = 8;
    for (i = 0; i < depth; i++)
    {
        ULONG plane = gr32(guest0, gp + M68K_BitMap_Planes + i * 4);
        PLANEPTR expected = (!plane || plane == 0xffffffffUL)
                          ? (PLANEPTR)(IPTR)plane : gptr(guest0, plane);
        if (native->Planes[i] != expected)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: graphics.library "
                         "replaced plane %lu in guest BitMap %08lx",
                         (unsigned long)i, (unsigned long)gp);
            return -1;
        }
    }
    return 0;
}

/* A program-owned RastPort is not an object token, but drawing into its
 * program-owned planar BitMap is still representable.  Build only the native
 * state graphics.library consumes for rendering; never reinterpret the guest
 * structure or its private pointer fields in place. */
static int guest_render_rastport(struct Emu68kRunState *rs, APTR guest0,
                                 ULONG gp, struct RastPort *shadow,
                                 struct BitMap *bmshadow,
                                 struct RastPort **native,
                                 char *err, ULONG errlen)
{
    struct Emu68kObject *o = object_by_token(rs, gp);
    struct Emu68kObject *layero;
    struct BitMap *bitmap;
    ULONG bitmap_gp, layer_gp;
    UBYTE *g = (UBYTE *)guest0;

    if (!gp || (o && o->type != EMU_OBJ_RastPort))
        return -1;
    if (o)
    {
        *native = (struct RastPort *)o->native;
        return 0;
    }
    if (emu68k_require_guest_range(gp, M68K_RastPort_SIZEOF,
                                   "render RastPort", err, errlen) < 0)
        return -1;
    bitmap_gp = gr32(guest0, gp + M68K_RastPort_BitMap);
    if (guest_bitmap_view(rs, guest0, bitmap_gp, bmshadow, &bitmap,
                          "render RastPort BitMap", err, errlen) < 0)
        return -1;

    InitRastPort(shadow);
    shadow->BitMap = bitmap;
    shadow->Mask = g[gp + M68K_RastPort_Mask];
    shadow->FgPen = g[gp + M68K_RastPort_FgPen];
    shadow->BgPen = g[gp + M68K_RastPort_BgPen];
    shadow->AOlPen = g[gp + M68K_RastPort_AOlPen];
    shadow->DrawMode = g[gp + M68K_RastPort_DrawMode];
    shadow->Flags = emu68k_scalar_from_guest(
        guest0, gp + M68K_RastPort_Flags, 2);
    shadow->LinePtrn = emu68k_scalar_from_guest(
        guest0, gp + M68K_RastPort_LinePtrn, 2);
    memcpy(shadow->minterms, g + gp + M68K_RastPort_minterms,
           sizeof shadow->minterms);

    layer_gp = gr32(guest0, gp + M68K_RastPort_Layer);
    if (layer_gp)
    {
        layero = object_by_token(rs, layer_gp);
        if (!layero || layero->type != EMU_OBJ_Layer)
        {
            if (err && errlen)
                snprintf(err, errlen, "render RastPort has unknown Layer %08lx",
                         (unsigned long)layer_gp);
            return -1;
        }
        shadow->Layer = (struct Layer *)layero->native;
    }
    *native = shadow;
    return 0;
}

static int guest_text_rastport(struct Emu68kRunState *rs, APTR guest0,
                               ULONG rp, struct RastPort *shadow,
                               struct RastPort **native,
                               char *err, ULONG errlen)
{
    struct Emu68kObject *rpo = object_by_token(rs, rp);
    struct Emu68kObject *fonto;
    ULONG fontp;
    UBYTE *g = (UBYTE *)guest0;

    if (!rp || (rpo && rpo->type != EMU_OBJ_RastPort))
        return -1;
    if (rpo)
    {
        *native = (struct RastPort *)rpo->native;
        return 0;
    }
    if (emu68k_require_guest_range(rp, M68K_RastPort_SIZEOF,
                                   "text RastPort", err, errlen) < 0)
        return -1;
    fontp = emu68k_scalar_from_guest(guest0, rp + M68K_RastPort_Font, 4);
    fonto = object_by_token(rs, fontp);
    if (!fonto || fonto->type != EMU_OBJ_TextFont)
    {
        if (err && errlen)
            snprintf(err, errlen, "text RastPort has unknown TextFont %08lx",
                     (unsigned long)fontp);
        return -1;
    }

    memset(shadow, 0, sizeof *shadow);
    shadow->Font = (struct TextFont *)fonto->native;
    shadow->Mask = g[rp + M68K_RastPort_Mask];
    shadow->FgPen = g[rp + M68K_RastPort_FgPen];
    shadow->BgPen = g[rp + M68K_RastPort_BgPen];
    shadow->DrawMode = g[rp + M68K_RastPort_DrawMode];
    shadow->AlgoStyle = g[rp + M68K_RastPort_AlgoStyle];
    shadow->TxFlags = g[rp + M68K_RastPort_TxFlags];
    shadow->TxHeight = emu68k_scalar_from_guest(
        guest0, rp + M68K_RastPort_TxHeight, 2);
    shadow->TxWidth = emu68k_scalar_from_guest(
        guest0, rp + M68K_RastPort_TxWidth, 2);
    shadow->TxBaseline = emu68k_scalar_from_guest(
        guest0, rp + M68K_RastPort_TxBaseline, 2);
    shadow->TxSpacing = (WORD)emu68k_scalar_from_guest(
        guest0, rp + M68K_RastPort_TxSpacing, 2);
    *native = shadow;
    return 0;
}

AROS_UFH3(static IPTR, emu68k_native_boopsi_entry,
          AROS_UFHA(Class *, cl, A0),
          AROS_UFHA(Object *, object, A2),
          AROS_UFHA(Msg, message, A1))
{
    AROS_USERFUNC_INIT

    struct Emu68kBoopsiBridge *bridge = cl ? cl->cl_Dispatcher.h_Data : NULL;
    struct Emu68kRunState *rs = bridge ? bridge->state : NULL;
    unsigned int result = 0;
    ULONG guest_message, guest_object, method, guest_storage_size = 4;
    const struct EmuTagDesc *attr_desc = NULL;
    UBYTE *p;

    if (!bridge || !rs || !rs->call_hook || !rs->guest_alloc)
    {
        if (bridge)
        {
            bridge->failed = TRUE;
            snprintf(bridge->error, sizeof bridge->error,
                     "BOOPSI callback has no active 68k run");
        }
        return 0;
    }
    if ((APTR)object == bridge->native_class)
        guest_object = bridge->guest_class;   /* OM_NEW convention: o is the cl */
    else
    {
        /* A method on an instance: the guest dispatcher must see the guest
         * facade this run issued for it, never the native pointer. */
        struct Emu68kObject *o = object_by_native(rs, (APTR)object);
        if (!o)
        {
            bridge->failed = TRUE;
            snprintf(bridge->error, sizeof bridge->error,
                     "BOOPSI callback object needs a guest facade");
            return 0;
        }
        guest_object = o->token;
    }
    method = message ? *(const ULONG *)(const void *)message : 0;
    bug("[emu68k/boopsi] dispatch method=%lx object=%p guest_object=%lx\n",
        (unsigned long)method, object, (unsigned long)guest_object);
    /* OM_NEW carries a real opSet: the guest dispatcher parses attributes
     * itself, so ops_AttrList is the caller's ORIGINAL guest taglist, not a
     * conversion. OM_GET carries a real opGet with a guest storage slot,
     * copied back after the dispatcher answers. Anything else still crosses
     * as the method ID alone and a dispatcher that needs more will fail
     * visibly rather than read garbage. */
    if (method == OM_GET && message)
    {
        const struct opGet *og = (const struct opGet *)message;
        attr_desc = emu68k_tag_lookup(emu68k_domain_intuition_new_object,
                                      (ULONG)og->opg_AttrID);
        if (attr_desc && attr_desc->kind == EMU_TAG_STRUCT_INOUT)
            guest_storage_size = attr_desc->guest_size;
    }
    guest_message = rs->guest_alloc(rs->run,
                                    method == OM_NEW ? 12 :
                                    method == OM_GET ? 12 + guest_storage_size : 4);
    if (!guest_message)
    {
        bridge->failed = TRUE;
        snprintf(bridge->error, sizeof bridge->error,
                 "guest memory exhausted for BOOPSI message");
        return 0;
    }
    p = (UBYTE *)rs->guest0 + guest_message;
    p[0] = (UBYTE)(method >> 24); p[1] = (UBYTE)(method >> 16);
    p[2] = (UBYTE)(method >> 8);  p[3] = (UBYTE)method;
    if (method == OM_NEW)
    {
        ULONG t = bridge->guest_tags;
        p[4] = (UBYTE)(t >> 24); p[5] = (UBYTE)(t >> 16);
        p[6] = (UBYTE)(t >> 8);  p[7] = (UBYTE)t;
        p[8] = p[9] = p[10] = p[11] = 0;              /* ops_GInfo = NULL */
    }
    else if (method == OM_GET)
    {
        const struct opGet *og = (const struct opGet *)message;
        ULONG a = (ULONG)og->opg_AttrID;
        ULONG s = guest_message + 12;                 /* the guest storage slot */
        p[4] = (UBYTE)(a >> 24); p[5] = (UBYTE)(a >> 16);
        p[6] = (UBYTE)(a >> 8);  p[7] = (UBYTE)a;
        p[8] = (UBYTE)(s >> 24); p[9] = (UBYTE)(s >> 16);
        p[10] = (UBYTE)(s >> 8); p[11] = (UBYTE)s;
        memset(p + 12, 0, guest_storage_size);
    }
    if (rs->call_hook(rs->run, bridge->entry, bridge->guest_class,
                      guest_object, guest_message, &result,
                      bridge->error, sizeof bridge->error) != 0)
    {
        bridge->failed = TRUE;
        bug("[emu68k/boopsi] dispatch method=%lx FAILED: %s\n",
            (unsigned long)method, bridge->error);
        return 0;
    }
    bug("[emu68k/boopsi] dispatch method=%lx -> %lx\n",
        (unsigned long)method, (unsigned long)result);
    if (method == OM_GET && message)
    {
        /* Scalar attributes retain their guest meaning.  Reviewed flat
         * structure attributes are rebuilt field-by-field into the native
         * output buffer the caller supplied. */
        struct opGet *og = (struct opGet *)message;
        if (og->opg_Storage)
        {
            if (attr_desc && attr_desc->kind == EMU_TAG_STRUCT_INOUT)
                emu68k_from_guest(rs->guest0, guest_message + 12,
                                  og->opg_Storage, attr_desc->fields,
                                  attr_desc->nfields);
            else
                *og->opg_Storage = (IPTR)(ULONG)((p[12] << 24) |
                                                 (p[13] << 16) |
                                                 (p[14] << 8) | p[15]);
        }
    }
    if (method == OM_NEW && result)
    {
        /* The guest dispatcher answers OM_NEW with the guest FACADE of the
         * object its super call made; native intuition needs the native
         * object behind it. Anything else is not an object this run knows. */
        struct Emu68kObject *o = object_by_token(rs, result);
        if (!o)
        {
            bridge->failed = TRUE;
            snprintf(bridge->error, sizeof bridge->error,
                     "guest OM_NEW returned %08lx, which is not an object "
                     "this run issued", (unsigned long)result);
            return 0;
        }
        return (IPTR)o->native;
    }
    return (IPTR)result;

    AROS_USERFUNC_EXIT
}

LONG emu68k_boopsi_prepare(APTR guest0, ULONG guest_class, APTR native_class,
                           ULONG guest_tags, struct Emu68kBoopsiBridge *bridge,
                           char *err, ULONG errlen)
{
    Class *cl = native_class;
    struct Emu68kRunState *rs;
    ULONG entry;

    if (!bridge) return -1;
    /* No class pointer means the class was named by string instead, which is
     * the ordinary way to make an object of one of the system's own classes.
     * There is no guest dispatcher behind it, so there is nothing to bridge:
     * an empty bridge that emu68k_boopsi_finish leaves alone. */
    if (!guest_class || !cl)
    {
        memset(bridge, 0, sizeof *bridge);
        return 0;
    }
    if (emu68k_require_guest_range(guest_class, M68K_IClass_SIZEOF,
                                   "BOOPSI Class", err, errlen) < 0)
        return -1;
    entry = gr32(guest0, guest_class + M68K_IClass_cl_Dispatcher_h_Entry);
    if (emu68k_require_guest_range(entry, 2, "BOOPSI dispatcher", err, errlen) < 0)
        return -1;
    rs = run_state(guest0);
    if (!rs)
    {
        if (err && errlen)
            snprintf(err, errlen, "BOOPSI callback has no active 68k run");
        return -1;
    }
    memset(bridge, 0, sizeof *bridge);
    bridge->saved_dispatcher = cl->cl_Dispatcher;
    bridge->native_class = cl;
    bridge->state = rs;
    bridge->guest_class = guest_class;
    bridge->guest_tags = guest_tags;
    bridge->entry = entry;
    cl->cl_Dispatcher.h_Entry = (APTR)emu68k_native_boopsi_entry;
    cl->cl_Dispatcher.h_Data = bridge;
    return 0;
}

LONG emu68k_boopsi_finish(struct Emu68kBoopsiBridge *bridge,
                          char *err, ULONG errlen)
{
    Class *cl;
    struct Emu68kObject *co;

    if (!bridge) return 0;
    cl = bridge->native_class;
    if (cl) cl->cl_Dispatcher = bridge->saved_dispatcher;
    if (bridge->failed)
    {
        if (err && errlen)
            snprintf(err, errlen, "68k BOOPSI callback failed: %s",
                     bridge->error);
        return -1;
    }
    /* NewObjectA is only the first call into a guest class. Native Intuition
     * later dispatches input/render methods without another library crossing,
     * so restoring a NULL native dispatcher here turns the first click into a
     * jump through address zero. Keep one stable bridge in the class's object
     * record; a later crossing temporarily replaces it and restores it again. */
    co = object_by_token(bridge->state, bridge->guest_class);
    if (cl && co && co->native == cl && co->type == EMU_OBJ_Class &&
        !(cl->cl_Dispatcher.h_Entry == (APTR)emu68k_native_boopsi_entry &&
          cl->cl_Dispatcher.h_Data == &co->persistent_boopsi))
    {
        memset(&co->persistent_boopsi, 0, sizeof co->persistent_boopsi);
        co->persistent_boopsi.saved_dispatcher = bridge->saved_dispatcher;
        co->persistent_boopsi.native_class = cl;
        co->persistent_boopsi.state = bridge->state;
        co->persistent_boopsi.guest_class = bridge->guest_class;
        co->persistent_boopsi.entry = bridge->entry;
        cl->cl_Dispatcher.h_Entry = (APTR)emu68k_native_boopsi_entry;
        cl->cl_Dispatcher.h_Data = &co->persistent_boopsi;
    }
    return 0;
}

/* Prepare the same temporary dispatcher bridge when a method is invoked on
 * an existing object of a guest-created class.  System/native classes have no
 * guest dispatcher entry in their facade and intentionally produce an empty
 * bridge. */
LONG emu68k_boopsi_prepare_object(APTR guest0, APTR native_object,
                                  ULONG guest_tags,
                                  struct Emu68kBoopsiBridge *bridge,
                                  char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    struct Emu68kObject *co;
    Class *cl;
    ULONG entry;

    if (!bridge) return -1;
    if (!native_object || !rs)
    {
        memset(bridge, 0, sizeof *bridge);
        return native_object ? -1 : 0;
    }
    cl = OCLASS((Object *)native_object);
    co = object_by_native(rs, cl);
    if (!co || co->type != EMU_OBJ_Class ||
        emu68k_require_guest_range(co->token, M68K_IClass_SIZEOF,
                                   "BOOPSI Class", NULL, 0) < 0)
    {
        memset(bridge, 0, sizeof *bridge);
        return 0;
    }
    entry = gr32(guest0, co->token + M68K_IClass_cl_Dispatcher_h_Entry);
    if (emu68k_require_guest_range(entry, 2, "BOOPSI dispatcher",
                                   NULL, 0) < 0)
    {
        memset(bridge, 0, sizeof *bridge);
        return 0;
    }
    return emu68k_boopsi_prepare(guest0, co->token, cl, guest_tags,
                                  bridge, err, errlen);
}

/* A handle crosses as a BPTR of a real guest structure, not as an opaque tag:
 * a program may dereference its handle, and BADDR of a tag lands nowhere. Slot
 * i lives at EMU68K_GUEST_FH_BASE + i*SLOT, and the guest gets that >> 2. */
static ULONG handle_slot_bptr(int i)
{
    return (ULONG)((EMU68K_GUEST_FH_BASE + (ULONG)i * EMU68K_GUEST_FH_SLOT) >> 2);
}

static ULONG handle_token(struct Emu68kRunState *rs, BPTR b)
{
    int i;
    if (!b || !rs) return 0;
    for (i = 0; i < EMU68K_MAX_HANDLES; i++)
        if (rs->handles[i].bptr == b) return handle_slot_bptr(i);
    for (i = 0; i < EMU68K_MAX_HANDLES; i++)
        if (!rs->handles[i].bptr)
        {
            rs->handles[i].bptr = b;
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

static BPTR handle_bptr(struct Emu68kRunState *rs, ULONG token)
{
    int i = handle_index(token);
    return (i < 0 || !rs) ? BNULL : rs->handles[i].bptr;
}

/* A BPTR argument, resolved or REFUSED.
 *
 * handle_bptr answers BNULL for a token this run never issued, and BNULL is a
 * legitimate argument - "no lock" means the current directory - so the native
 * call went ahead and dereferenced nothing. A token the program made up, or one
 * it has already unlocked, has to be named instead: it is the same mistake the
 * typed object table refuses by name, on the one table that was still silent. */
LONG emu68k_handle_require(APTR guest0, ULONG token, const char *what,
                           BPTR *out, char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    if (out) *out = BNULL;
    if (!token) return 0;                       /* the program said "none"     */
    if (handle_index(token) < 0 || !rs || !handle_bptr(rs, token))
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: stale or unknown %s handle "
                     "%08lx", what ? what : "BPTR", (unsigned long)token);
        return -1;
    }
    if (out) *out = handle_bptr(rs, token);
    return 0;
}

/* The generated crossings need the handle table too (a BPTR argument or
 * result); this file owns it, so it exports the two ends. */
BPTR emu68k_handle_bptr(APTR guest0, ULONG token)
{
    return handle_bptr(run_state(guest0), token);
}

ULONG emu68k_handle_token(APTR guest0, BPTR b)
{
    return handle_token(run_state(guest0), b);
}

static void handle_release(struct Emu68kRunState *rs, ULONG token)
{
    int i = handle_index(token);
    if (i >= 0 && rs) rs->handles[i].bptr = BNULL;
}

/* ---- TYPED NATIVE OBJECTS -------------------------------------------------
 * Catalogs, Locales, Windows and similar OS-owned pointers do not fit in a
 * 68k register and must never be dereferenced as native structures by the
 * guest. They cross as typed per-run tokens. Repeated opens of the same native
 * object preserve pointer identity while refs records how many matching close
 * calls are required. A cleanup thunk generated in the owning library's C
 * file closes anything a terminating guest forgot to release. */
static struct Emu68kObject *object_by_token(struct Emu68kRunState *rs,
                                            ULONG token)
{
    int i;
    if (!rs || !token) return NULL;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
        if (rs->objects[i].native && rs->objects[i].token == token)
            return &rs->objects[i];
    return NULL;
}

static BOOL object_token_retired(struct Emu68kRunState *rs, ULONG token)
{
    ULONG i;
    if (!rs || !token) return FALSE;
    for (i = 0; i < rs->object_tombstone_count; i++)
        if (rs->object_tombstones[i] == token) return TRUE;
    return FALSE;
}

static void object_retire(struct Emu68kRunState *rs,
                          const struct Emu68kObject *object)
{
    ULONG token;
    if (!rs || !object || !object->native ||
        (object->flags & EMU68K_OBJ_GUEST_OWNED))
        return;
    token = object->token;
    /* Opaque E680 tokens already cannot pass as guest memory.  Tombstones are
     * specifically for readable facade/alias addresses, preserving capacity
     * for the ambiguity they close. */
    if (!token || emu68k_require_guest_range(token, 1, "retired object",
                                              NULL, 0) < 0 ||
        object_token_retired(rs, token))
        return;
    if (rs->object_tombstone_count < EMU68K_MAX_TOMBSTONES)
        rs->object_tombstones[rs->object_tombstone_count++] = token;
    else
        rs->object_tombstones_full = TRUE;
}

static ULONG object_new_token(struct Emu68kRunState *rs)
{
    ULONG token;
    int tries;

    for (tries = 0; tries < 0xffff; tries++)
    {
        rs->next_object = (rs->next_object + 1) & 0xffff;
        if (!rs->next_object) rs->next_object = 1;
        token = EMU68K_OBJECT_TOKEN_BASE | rs->next_object;
        if (!object_by_token(rs, token)) return token;
    }
    return 0;
}

/* ---- GUEST-OWNED OBJECTS ---------------------------------------------------
 *
 * Classic Intuition code allocates its own Gadget list and hands it to
 * AddGList; the library then keeps, renders and hit-tests those structures for
 * as long as the window lives. Nothing issued a token, so the object table had
 * nothing to resolve and the crossing failed closed - correctly, but the
 * program is doing something completely ordinary.
 *
 * The guest structure cannot be passed through: it is big-endian with 32-bit
 * pointers and a different layout. So the run ADOPTS it - one native mirror per
 * guest structure, registered under the guest address so identity survives,
 * converted in before every call and back out after it.
 *
 * Rules the mirrors obey, each of which is a way this can be wrong:
 *
 *  - The whole structure is validated on EVERY crossing, not once. A program
 *    that sets GadgetRender after the mirror exists must be refused, not
 *    silently rendered blank.
 *  - A linked family is adopted whole and walked with a bound; exceeding it
 *    means truncation or a cycle, and either is a named gap, never a quiet
 *    short list.
 *  - Adoption is all or nothing. A family that fails on its third node leaves
 *    no mirrors behind for the first two.
 *  - Copyback walks the NATIVE chain, because the library relinks it. The
 *    guest link is written as the guest address of the next mirror, so the
 *    program reads its own addresses back, never a native pointer.
 *  - A chain may not mix mirrors with bridge-issued objects: their tokens mean
 *    different things and only one of the two is guest memory.
 */
static struct Emu68kObject *object_slot(struct Emu68kRunState *rs)
{
    int i;
    if (!rs) return NULL;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
        if (!rs->objects[i].native) return &rs->objects[i];
    return NULL;
}

static struct Emu68kObject *object_by_native(struct Emu68kRunState *rs,
                                             APTR native)
{
    int i;
    if (!rs || !native) return NULL;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
        if (rs->objects[i].native == native) return &rs->objects[i];
    return NULL;
}

static void idcmp_bind_window(struct Emu68kRunState *rs, APTR guest0,
                              ULONG window_token)
{
    struct Emu68kObject *w = object_by_token(rs, window_token);
    ULONG port;
    int i, free_slot = -1;
    if (!w || w->type != EMU_OBJ_Window) return;
    port = gr32(guest0, w->token + M68K_Window_UserPort);
    for (i = 0; i < EMU68K_MAX_IDCMP; i++)
    {
        if (rs->idcmp[i].window == w->native)
        { rs->idcmp[i].guest_port = port; return; }
        if (!rs->idcmp[i].window && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0)
    {
        rs->idcmp[free_slot].window = w->native;
        rs->idcmp[free_slot].guest_port = port;
    }
}

/* Guest AddTail plus the signal a native PutMsg would set. */
static void idcmp_queue_guest(APTR guest0, ULONG guest_port, ULONG bit,
                              ULONG guest_msg)
{
    ULONG list = guest_port + M68K_MsgPort_mp_MsgList_lh_Head;
    ULONG tailpred = gr32(guest0, list + M68K_List_lh_TailPred);
    ULONG task;

    gw32(guest0, guest_msg, list + M68K_List_lh_Tail);
    gw32(guest0, guest_msg + 4, tailpred);
    gw32(guest0, tailpred, guest_msg);
    gw32(guest0, list + M68K_List_lh_TailPred, guest_msg);
    task = gr32(guest0, guest_port + M68K_MsgPort_mp_SigTask);
    if (task && bit < 32)
        gw32(guest0, task + M68K_Task_tc_SigRecvd,
             gr32(guest0, task + M68K_Task_tc_SigRecvd) | (1u << bit));
}

static void idcmp_unbind_window(struct Emu68kRunState *rs, ULONG window_token)
{
    struct Emu68kObject *w = object_by_token(rs, window_token);
    int i;
    if (!w || w->type != EMU_OBJ_Window) return;
    for (i = 0; i < EMU68K_MAX_IDCMP; i++)
        if (rs->idcmp[i].window == w->native)
            memset(&rs->idcmp[i], 0, sizeof rs->idcmp[i]);
}

/* IntuiMessage has two application-visible pointer identities that the
 * generated scalar layout deliberately cannot guess.  Resolve them through
 * this run's typed-object table: windows returned by OpenWindow and gadgets
 * created/adopted by Intuition/GadTools already have an exact guest facade or
 * mirror token there.  Unknown native pointers remain zero; truncating one
 * into the 32-bit arena would turn a harmless missing field into corruption.
 *
 * Run this again after GT_FilterIMsg: GadTools may replace the message view,
 * but the application must still see its own Window/Gadget identities. */
static void intui_message_to_guest(struct Emu68kRunState *rs, APTR guest0,
                                   ULONG guest_msg,
                                   const struct IntuiMessage *native)
{
    struct Emu68kObject *o;
    struct Emu68kObject *layer;

    memset((UBYTE *)guest0 + guest_msg, 0, M68K_IntuiMessage_SIZEOF);
    emu68k_to_guest(guest0, guest_msg, native, emu_fields_IntuiMessage,
                    EMU_NFIELDS(emu_fields_IntuiMessage));

    o = object_by_native(rs, native ? (APTR)native->IDCMPWindow : NULL);
    if (o)
    {
        /* A Window and its Layer are retained native objects, not immutable
         * return values.  Intuition changes their public scalar state while
         * producing IDCMP messages (LAYERREFRESH is the important example).
         * Refresh those facade fields at the delivery boundary, before the
         * guest can inspect IDCMPWindow->WLayer directly.  Pointer fields are
         * deliberately absent from the flat layout, so their typed tokens
         * already installed in the facades remain intact. */
        if (o->type == EMU_OBJ_Window)
        {
            struct Window *window = (struct Window *)o->native;

            emu68k_to_guest(guest0, o->token, window,
                            emu_fields_Window, EMU_NFIELDS(emu_fields_Window));
            layer = object_by_native(rs, window->WLayer);
            if (layer && layer->type == EMU_OBJ_Layer)
                emu68k_to_guest(guest0, layer->token, window->WLayer,
                                emu_fields_Layer,
                                EMU_NFIELDS(emu_fields_Layer));
        }
        gw32(guest0, guest_msg + M68K_IntuiMessage_IDCMPWindow, o->token);
    }

    o = object_by_native(rs, native ? native->IAddress : NULL);
    if (o)
        gw32(guest0, guest_msg + M68K_IntuiMessage_IAddress, o->token);
}

static void emu68k_mirror_cleanup(APTR base, APTR object)
{
    (void)base;
    FreeVec(object);
}

static LONG object_adopt_guest_impl(APTR guest0, ULONG addr, UWORD type,
                                    const char *type_name,
                                    const struct EmuMirror *m, APTR *native,
                                    BOOL commit, char *err, ULONG errlen);

/* How much of the guest structure this crossing covers. A variant flag (a
 * Gadget is a shorter structure unless GFLG_EXTENDED is set) decides between
 * the base layout and the extended one; converting the long form over a short
 * allocation would read past the program's memory. */
static ULONG mirror_guest_size(APTR guest0, ULONG addr, const struct EmuMirror *m)
{
    ULONG flags;
    if (m->flag_off < 0 || !m->base_size) return m->m68k_size;
    flags = (ULONG)emu68k_scalar_from_guest(guest0, addr + (ULONG)m->flag_off, 2);
    return (flags & m->flag_mask) ? m->m68k_size : m->base_size;
}

/* ExtendFont stores a native TextFontExtension in tf_Message.mn_ReplyPort.
 * That field is public ABI: classic programs dereference it and set Flags0.
 * Carry the complete fixed header plus the bounded tag list in the reserved
 * tail of every TextFont facade, and synchronize the mutable flag bytes before
 * the native font APIs see the object again. */
static LONG textfont_extension_from_guest(struct Emu68kRunState *rs,
                                          APTR guest0, ULONG token,
                                          char *err, ULONG errlen)
{
    struct Emu68kObject *o;
    struct TextFont *font;
    struct TextFontExtension *tfe;
    ULONG ext;

    if (!token) return 0;
    o = object_by_token(rs, token);
    if (!o || o->type != EMU_OBJ_TextFont) return 0;
    font = o->native;
    tfe = (struct TextFontExtension *)font->tf_Extension;
    ext = gr32(guest0, token + M68K_TextFont_tf_Message_mn_ReplyPort);
    if (!tfe)
    {
        if (!ext) return 0;
        if (err && errlen)
            snprintf(err, errlen, "capability gap: TextFont %08lx acquired "
                     "a guest extension the native font does not own",
                     (unsigned long)token);
        return -1;
    }
    if (ext != token + EMU68K_TEXTFONT_EXT_OFF)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: TextFont %08lx changed its "
                     "extension pointer", (unsigned long)token);
        return -1;
    }
    tfe->tfe_Flags0 = ((UBYTE *)guest0)[ext + 2];
    tfe->tfe_Flags1 = ((UBYTE *)guest0)[ext + 3];
    return 0;
}

static LONG textfont_extension_to_guest(struct Emu68kRunState *rs,
                                        APTR guest0, ULONG token,
                                        char *err, ULONG errlen)
{
    struct Emu68kObject *o;
    struct TextFont *font;
    struct TextFontExtension *tfe;
    ULONG ext = token + EMU68K_TEXTFONT_EXT_OFF;
    ULONG tags = token + EMU68K_TEXTFONT_EXT_TAGS_OFF;
    ULONG orig = 0;
    int i, ended = 0;

    if (!token) return 0;
    o = object_by_token(rs, token);
    if (!o || o->type != EMU_OBJ_TextFont)
    {
        if (err && errlen)
            snprintf(err, errlen, "TextFont facade %08lx is not live",
                     (unsigned long)token);
        return -1;
    }
    font = o->native;
    tfe = (struct TextFontExtension *)font->tf_Extension;
    ((UBYTE *)guest0)[token + M68K_TextFont_tf_Style] = font->tf_Style;
    if (!tfe)
    {
        gw32(guest0, token + M68K_TextFont_tf_Message_mn_ReplyPort, 0);
        memset((UBYTE *)guest0 + ext, 0,
               EMU68K_TEXTFONT_NAME_OFF - EMU68K_TEXTFONT_EXT_OFF);
        return 0;
    }

    if (tfe->tfe_OrigReplyPort)
    {
        struct Emu68kObject *po = object_by_native(rs, tfe->tfe_OrigReplyPort);
        if (!po || po->type != EMU_OBJ_MsgPort)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: TextFont %08lx has an "
                         "extension reply port with no guest form",
                         (unsigned long)token);
            return -1;
        }
        orig = po->token;
    }
    if (tfe->tfe_OFontPatchS || tfe->tfe_OFontPatchK)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: TextFont %08lx extension "
                     "uses outline-font patch arrays", (unsigned long)token);
        return -1;
    }

    memset((UBYTE *)guest0 + ext, 0,
           EMU68K_TEXTFONT_NAME_OFF - EMU68K_TEXTFONT_EXT_OFF);
    emu68k_scalar_to_guest(guest0, ext, 2, tfe->tfe_MatchWord);
    ((UBYTE *)guest0)[ext + 2] = tfe->tfe_Flags0;
    ((UBYTE *)guest0)[ext + 3] = tfe->tfe_Flags1;
    gw32(guest0, ext + 4, token);
    gw32(guest0, ext + 8, orig);
    if (tfe->tfe_Tags)
    {
        for (i = 0; i < EMU68K_TEXTFONT_EXT_TAGS_MAX; i++)
        {
            ULONG tag = tfe->tfe_Tags[i].ti_Tag;
            IPTR data = tfe->tfe_Tags[i].ti_Data;
            if (tag == TAG_MORE)
            {
                if (err && errlen)
                    snprintf(err, errlen, "capability gap: TextFont %08lx "
                             "extension retains a native TAG_MORE chain",
                             (unsigned long)token);
                return -1;
            }
            gw32(guest0, tags + (ULONG)i * 8, tag);
            gw32(guest0, tags + (ULONG)i * 8 + 4, (ULONG)data);
            if (tag == TAG_DONE) { ended = 1; break; }
        }
        if (!ended)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: TextFont %08lx "
                         "extension exceeds %d tags", (unsigned long)token,
                         EMU68K_TEXTFONT_EXT_TAGS_MAX);
            return -1;
        }
        gw32(guest0, ext + 12, tags);
    }
    gw32(guest0, token + M68K_TextFont_tf_Message_mn_ReplyPort, ext);
    return 0;
}

static LONG border_xy_validate(APTR guest0, ULONG border,
                               char *err, ULONG errlen)
{
    BYTE count = ((UBYTE *)guest0)[border + M68K_Border_Count];
    ULONG xy = gr32(guest0, border + M68K_Border_XY);

    if (count < 0)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: Border %08lx has negative "
                     "coordinate count %ld", (unsigned long)border,
                     (long)count);
        return -1;
    }
    if (count && (!xy || emu68k_require_guest_range(
            xy, (ULONG)count * 4, "Border XY coordinates", err, errlen) < 0))
        return -1;
    return 0;
}

static ULONG border_alloc_size(APTR guest0, ULONG border,
                               const struct EmuMirror *m)
{
    UBYTE count = ((UBYTE *)guest0)[border + M68K_Border_Count];
    ULONG head = (m->native_size + sizeof(IPTR) - 1) & ~(sizeof(IPTR) - 1);
    return head + (ULONG)count * 2 * sizeof(WORD);
}

static void border_xy_from_guest(APTR guest0, ULONG border,
                                 struct Border *native,
                                 const struct EmuMirror *m)
{
    UBYTE count = ((UBYTE *)guest0)[border + M68K_Border_Count];
    ULONG xy = gr32(guest0, border + M68K_Border_XY), i;
    ULONG head = (m->native_size + sizeof(IPTR) - 1) & ~(sizeof(IPTR) - 1);

    native->XY = count ? (WORD *)((UBYTE *)native + head) : NULL;
    for (i = 0; i < (ULONG)count * 2; i++)
        native->XY[i] = (WORD)emu68k_scalar_from_guest(
            guest0, xy + i * 2, 2);
}

static LONG border_xy_to_guest(APTR guest0, ULONG border,
                               struct Border *native,
                               const struct EmuMirror *m,
                               char *err, ULONG errlen)
{
    UBYTE count = ((UBYTE *)guest0)[border + M68K_Border_Count];
    ULONG xy = gr32(guest0, border + M68K_Border_XY), i;
    ULONG head = (m->native_size + sizeof(IPTR) - 1) & ~(sizeof(IPTR) - 1);
    WORD *expected = count ? (WORD *)((UBYTE *)native + head) : NULL;

    if (native->XY != expected)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: Intuition replaced Border "
                     "%08lx XY coordinates", (unsigned long)border);
        return -1;
    }
    for (i = 0; i < (ULONG)count * 2; i++)
        emu68k_scalar_to_guest(guest0, xy + i * 2, 2,
                               (UWORD)native->XY[i]);
    return 0;
}

/* A classic Image owns planar UWORD data immediately reachable through its
 * ImageData pointer.  Native Intuition retains and walks both that data and
 * NextImage, so neither may be a guest address in the native structure. */
static LONG image_data_words(APTR guest0, ULONG image, ULONG *words,
                             char *err, ULONG errlen)
{
    LONG width = (WORD)emu68k_scalar_from_guest(
        guest0, image + M68K_Image_Width, 2);
    LONG height = (WORD)emu68k_scalar_from_guest(
        guest0, image + M68K_Image_Height, 2);
    LONG depth = (WORD)emu68k_scalar_from_guest(
        guest0, image + M68K_Image_Depth, 2);
    UBYTE pick = ((UBYTE *)guest0)[image + M68K_Image_PlanePick];
    ULONG planes = 0, rowwords, total;
    UBYTE bits;

    if (words) *words = 0;
    if (width < 0 || height < 0 || depth < 0 || depth > 8)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: Image %08lx has unsupported "
                     "dimensions %ldx%ld depth %ld (custom/BOOPSI images need "
                     "a method bridge)", (unsigned long)image,
                     (long)width, (long)height, (long)depth);
        return -1;
    }
    for (bits = pick; bits; bits >>= 1) planes += bits & 1;
    if (planes > (ULONG)depth) planes = (ULONG)depth;
    rowwords = ((ULONG)width + 15) >> 4;
    if (rowwords && (ULONG)height > 0x00800000UL / rowwords)
        goto too_large;
    total = rowwords * (ULONG)height;
    if (planes && total > 0x00800000UL / planes)
        goto too_large;
    total *= planes;              /* at most 16 MiB of guest image data */
    if (words) *words = total;
    return 0;

too_large:
    if (err && errlen)
        snprintf(err, errlen, "capability gap: Image %08lx data exceeds 16 MiB",
                 (unsigned long)image);
    return -1;
}

static LONG image_data_validate(APTR guest0, ULONG image,
                                char *err, ULONG errlen)
{
    ULONG words, data = gr32(guest0, image + M68K_Image_ImageData);
    if (image_data_words(guest0, image, &words, err, errlen) < 0)
        return -1;
    if (words && (!data || emu68k_require_guest_range(
            data, words * 2, "Image planar data", err, errlen) < 0))
        return -1;
    return 0;
}

static ULONG image_alloc_size(APTR guest0, ULONG image,
                              const struct EmuMirror *m)
{
    ULONG words = 0;
    ULONG head = (m->native_size + sizeof(IPTR) - 1) & ~(sizeof(IPTR) - 1);
    (void)image_data_words(guest0, image, &words, NULL, 0);
    return head + words * sizeof(UWORD);
}

static void image_data_from_guest(APTR guest0, ULONG image,
                                  struct Image *native,
                                  const struct EmuMirror *m)
{
    ULONG words = 0, i;
    ULONG data = gr32(guest0, image + M68K_Image_ImageData);
    ULONG head = (m->native_size + sizeof(IPTR) - 1) & ~(sizeof(IPTR) - 1);
    (void)image_data_words(guest0, image, &words, NULL, 0);
    native->ImageData = words ? (UWORD *)((UBYTE *)native + head) : NULL;
    for (i = 0; i < words; i++)
        native->ImageData[i] = (UWORD)emu68k_scalar_from_guest(
            guest0, data + i * 2, 2);
}

static LONG image_data_to_guest(APTR guest0, ULONG image,
                                struct Image *native,
                                const struct EmuMirror *m,
                                char *err, ULONG errlen)
{
    ULONG words = 0, i;
    ULONG data = gr32(guest0, image + M68K_Image_ImageData);
    ULONG head = (m->native_size + sizeof(IPTR) - 1) & ~(sizeof(IPTR) - 1);
    UWORD *expected;

    if (image_data_words(guest0, image, &words, err, errlen) < 0)
        return -1;
    expected = words ? (UWORD *)((UBYTE *)native + head) : NULL;
    if (native->ImageData != expected)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: Intuition replaced Image "
                     "%08lx planar data", (unsigned long)image);
        return -1;
    }
    for (i = 0; i < words; i++)
        emu68k_scalar_to_guest(guest0, data + i * 2, 2,
                               native->ImageData[i]);
    return 0;
}

static ULONG image_native_words(const struct Image *image)
{
    ULONG planes = 0, rowwords;
    UBYTE bits;
    LONG depth = image->Depth;
    if (image->Width < 0 || image->Height < 0 || depth < 0 || depth > 8)
        return (ULONG)~0U;
    for (bits = image->PlanePick; bits; bits >>= 1) planes += bits & 1;
    if (planes > (ULONG)depth) planes = (ULONG)depth;
    rowwords = ((ULONG)image->Width + 15) >> 4;
    return rowwords * (ULONG)image->Height * planes;
}

static LONG image_family_validate(struct Emu68kRunState *rs, APTR guest0,
                                  ULONG root, char *err, ULONG errlen)
{
    ULONG walk, count;
    for (walk = root, count = 0; walk; count++)
    {
        struct Emu68kObject *o = object_by_token(rs, walk);
        ULONG words = 0;
        if (count >= emu68k_mirror_Image->limit)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: the Image family at "
                         "%08lx exceeds %lu members or contains a cycle",
                         (unsigned long)root,
                         (unsigned long)emu68k_mirror_Image->limit);
            return -1;
        }
        if (o && (o->type != EMU_OBJ_Image ||
                  !(o->flags & EMU68K_OBJ_GUEST_OWNED)))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Image %08lx is already "
                         "a different object", (unsigned long)walk);
            return -1;
        }
        if (emu68k_require_guest_range(walk, M68K_Image_SIZEOF,
                                       "Image", err, errlen) < 0 ||
            image_data_validate(guest0, walk, err, errlen) < 0)
            return -1;
        (void)image_data_words(guest0, walk, &words, NULL, 0);
        if (o && image_native_words((struct Image *)o->native) != words)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Image %08lx changed "
                         "planar data size after its mirror was created",
                         (unsigned long)walk);
            return -1;
        }
        walk = gr32(guest0, walk + M68K_Image_NextImage);
    }
    return 0;
}

/* The generated BitMap mirror is library-local.  A guest-owned RastPort can
 * point at the same classic structure, so keep the identical descriptor here
 * for its nested adoption instead of exposing one generated translation unit
 * as the owner of a cross-library type. */
static const struct EmuMirror emu68k_nested_bitmap_mirror =
{
    emu_fields_BitMap, EMU_NFIELDS(emu_fields_BitMap),
    sizeof(struct BitMap), M68K_BitMap_SIZEOF,
    0, -1, 0, -1, -1, 1
};

static LONG rastport_refs_validate(struct Emu68kRunState *rs, APTR guest0,
                                   ULONG rp, char *err, ULONG errlen)
{
    ULONG layer = gr32(guest0, rp + M68K_RastPort_Layer);
    ULONG bitmap = gr32(guest0, rp + M68K_RastPort_BitMap);
    ULONG font = gr32(guest0, rp + M68K_RastPort_Font);
    struct Emu68kObject *o;
    struct BitMap shadow, *native;

    if (layer)
    {
        o = object_by_token(rs, layer);
        if (!o || o->type != EMU_OBJ_Layer)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: RastPort %08lx has "
                         "unknown Layer %08lx", (unsigned long)rp,
                         (unsigned long)layer);
            return -1;
        }
    }
    if (bitmap && guest_bitmap_view(rs, guest0, bitmap, &shadow, &native,
                                    "RastPort BitMap", err, errlen) < 0)
        return -1;
    if (font)
    {
        o = object_by_token(rs, font);
        if (!o || o->type != EMU_OBJ_TextFont)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: RastPort %08lx has "
                         "unknown TextFont %08lx", (unsigned long)rp,
                         (unsigned long)font);
            return -1;
        }
    }
    return 0;
}

static LONG rastport_refs_from_guest(APTR guest0, ULONG rp,
                                     struct RastPort *native,
                                     char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    ULONG layer = gr32(guest0, rp + M68K_RastPort_Layer);
    ULONG bitmap = gr32(guest0, rp + M68K_RastPort_BitMap);
    ULONG font = gr32(guest0, rp + M68K_RastPort_Font);
    struct Emu68kObject *o;
    APTR object = NULL;

    native->Layer = NULL;
    native->BitMap = NULL;
    native->Font = NULL;
    if (layer)
    {
        o = object_by_token(rs, layer);
        if (!o || o->type != EMU_OBJ_Layer) return -1;
        native->Layer = (struct Layer *)o->native;
    }
    if (bitmap)
    {
        if (object_adopt_guest_impl(guest0, bitmap, EMU_OBJ_BitMap,
                "BitMap", &emu68k_nested_bitmap_mirror, &object, FALSE,
                err, errlen) < 0)
            return -1;
        native->BitMap = (struct BitMap *)object;
    }
    if (font)
    {
        o = object_by_token(rs, font);
        if (!o || o->type != EMU_OBJ_TextFont) return -1;
        native->Font = (struct TextFont *)o->native;
    }
    return 0;
}

static LONG rastport_refs_to_guest(APTR guest0, ULONG rp,
                                   struct RastPort *native,
                                   char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    ULONG layer = gr32(guest0, rp + M68K_RastPort_Layer);
    ULONG bitmap = gr32(guest0, rp + M68K_RastPort_BitMap);
    ULONG font = gr32(guest0, rp + M68K_RastPort_Font);
    struct Emu68kObject *o;

    o = object_by_native(rs, native->Layer);
    if ((native->Layer && (!o || o->token != layer)) ||
        (!native->Layer && layer))
        goto replaced;
    o = object_by_native(rs, native->BitMap);
    if ((native->BitMap && (!o || o->token != bitmap)) ||
        (!native->BitMap && bitmap))
        goto replaced;
    if (bitmap && emu68k_object_sync_guest(
            guest0, bitmap, EMU_OBJ_BitMap, "BitMap",
            &emu68k_nested_bitmap_mirror, err, errlen) < 0)
        return -1;
    o = object_by_native(rs, native->Font);
    if ((native->Font && (!o || o->token != font)) ||
        (!native->Font && font))
        goto replaced;
    return 0;

replaced:
    if (err && errlen)
        snprintf(err, errlen, "capability gap: graphics.library replaced a "
                 "Layer, BitMap or TextFont pointer in guest RastPort %08lx",
                 (unsigned long)rp);
    return -1;
}

static LONG border_family_validate(struct Emu68kRunState *rs, APTR guest0,
                                   ULONG root, char *err, ULONG errlen)
{
    ULONG walk, count;
    for (walk = root, count = 0; walk; count++)
    {
        struct Emu68kObject *o = object_by_token(rs, walk);
        BYTE points;
        if (count >= emu68k_mirror_Border->limit)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: the Border family at "
                         "%08lx exceeds %lu members or contains a cycle",
                         (unsigned long)root,
                         (unsigned long)emu68k_mirror_Border->limit);
            return -1;
        }
        if (o && (o->type != EMU_OBJ_Border ||
                  !(o->flags & EMU68K_OBJ_GUEST_OWNED)))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Border %08lx is already "
                         "a different object", (unsigned long)walk);
            return -1;
        }
        if (emu68k_require_guest_range(walk, M68K_Border_SIZEOF,
                                       "Border", err, errlen) < 0 ||
            border_xy_validate(guest0, walk, err, errlen) < 0)
            return -1;
        points = ((UBYTE *)guest0)[walk + M68K_Border_Count];
        if (o && ((struct Border *)o->native)->Count != points)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Border %08lx changed "
                         "coordinate count after its mirror was created",
                         (unsigned long)walk);
            return -1;
        }
        walk = gr32(guest0, walk + M68K_Border_NextBorder);
    }
    return 0;
}

static LONG gadget_render_validate(struct Emu68kRunState *rs, APTR guest0,
                                   ULONG gadget, char *err, ULONG errlen)
{
    ULONG flags = emu68k_scalar_from_guest(
        guest0, gadget + M68K_Gadget_Flags, 2);
    ULONG render = gr32(guest0, gadget + M68K_Gadget_GadgetRender);
    ULONG select = gr32(guest0, gadget + M68K_Gadget_SelectRender);

    if ((render || select) && (flags & GFLG_GADGIMAGE))
    {
        if ((render && image_family_validate(rs, guest0, render,
                                             err, errlen) < 0) ||
            (select && image_family_validate(rs, guest0, select,
                                             err, errlen) < 0))
            return -1;
        return 0;
    }
    if ((render && border_family_validate(rs, guest0, render, err, errlen) < 0) ||
        (select && border_family_validate(rs, guest0, select, err, errlen) < 0))
        return -1;
    return 0;
}

static LONG intuitext_family_validate(APTR guest0, ULONG root,
                                      const char *what,
                                      char *err, ULONG errlen)
{
    ULONG walk, count;

    for (walk = root, count = 0; walk; count++)
    {
        ULONG font, text;
        if (count >= 32)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: the IntuiText family "
                         "at %08lx exceeds 32 members or contains a cycle",
                         (unsigned long)root);
            return -1;
        }
        if (emu68k_require_guest_range(walk, M68K_IntuiText_SIZEOF,
                                       what, err, errlen) < 0)
            return -1;
        font = gr32(guest0, walk + M68K_IntuiText_ITextFont);
        text = gr32(guest0, walk + M68K_IntuiText_IText);
        if (font)
        {
            ULONG name;
            if (emu68k_require_guest_range(font, M68K_TextAttr_SIZEOF,
                                           "IntuiText TextAttr", err,
                                           errlen) < 0)
                return -1;
            name = gr32(guest0, font + M68K_TextAttr_ta_Name);
            if (name && !guest_cstr(guest0, name, 65536))
            {
                if (err && errlen)
                    snprintf(err, errlen, "capability gap: IntuiText at %08lx "
                             "has an unterminated font name",
                             (unsigned long)walk);
                return -1;
            }
        }
        if (text && !guest_cstr(guest0, text, 65536))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: IntuiText at %08lx has "
                         "an unterminated label", (unsigned long)walk);
            return -1;
        }
        walk = gr32(guest0, walk + M68K_IntuiText_NextText);
    }
    return 0;
}

static LONG gadget_text_validate(APTR guest0, ULONG gadget,
                                 char *err, ULONG errlen)
{
    return intuitext_family_validate(
        guest0, gr32(guest0, gadget + M68K_Gadget_GadgetText),
        "Gadget IntuiText", err, errlen);
}

static LONG gadget_render_from_guest(APTR guest0, ULONG gadget,
                                     struct Gadget *native,
                                     char *err, ULONG errlen)
{
    ULONG render = gr32(guest0, gadget + M68K_Gadget_GadgetRender);
    ULONG select = gr32(guest0, gadget + M68K_Gadget_SelectRender);
    ULONG flags = emu68k_scalar_from_guest(
        guest0, gadget + M68K_Gadget_Flags, 2);
    APTR nr = NULL, ns = NULL;
    UWORD type = (flags & GFLG_GADGIMAGE) ? EMU_OBJ_Image : EMU_OBJ_Border;
    const char *name = (flags & GFLG_GADGIMAGE) ? "Image" : "Border";
    const struct EmuMirror *mirror = (flags & GFLG_GADGIMAGE)
                                   ? emu68k_mirror_Image
                                   : emu68k_mirror_Border;

    if (object_adopt_guest_impl(guest0, render, type, name,
            mirror, &nr, FALSE, err, errlen) < 0 ||
        object_adopt_guest_impl(guest0, select, type, name,
            mirror, &ns, FALSE, err, errlen) < 0)
        return -1;
    native->GadgetRender = nr;
    native->SelectRender = ns;
    return 0;
}

static LONG gadget_text_from_guest(APTR guest0, ULONG gadget,
                                   struct Gadget *native,
                                   char *err, ULONG errlen)
{
    ULONG text = gr32(guest0, gadget + M68K_Gadget_GadgetText);

    native->GadgetText = NULL;
    if (!text) return 0;
    native->GadgetText = emu68k_struct_graph_to_native(
        guest0, text, emu_sdescs, EMU_SDESC_IntuiText,
        "GadgetText", err, errlen);
    return native->GadgetText ? 0 : -1;
}

#define EMU68K_PROPINFO_SIZEOF  22
#define EMU68K_STRINGINFO_SIZEOF 36
#define EMU68K_STRING_Buffer       0
#define EMU68K_STRING_UndoBuffer   4
#define EMU68K_STRING_BufferPos    8
#define EMU68K_STRING_MaxChars    10
#define EMU68K_STRING_DispPos     12
#define EMU68K_STRING_UndoPos     14
#define EMU68K_STRING_NumChars    16
#define EMU68K_STRING_DispCount   18
#define EMU68K_STRING_CLeft       20
#define EMU68K_STRING_CTop        22
#define EMU68K_STRING_Extension   24
#define EMU68K_STRING_LongInt     28
#define EMU68K_STRING_AltKeyMap   32

static LONG gadget_special_validate(APTR guest0, ULONG addr,
                                    const struct EmuMirror *m,
                                    char *err, ULONG errlen)
{
    ULONG special, gadget_type;

    if (m != emu68k_mirror_Gadget)
        return 0;
    special = gr32(guest0, addr + M68K_Gadget_SpecialInfo);
    if (!special)
        return 0;
    gadget_type = emu68k_scalar_from_guest(
        guest0, addr + M68K_Gadget_GadgetType, 2) & GTYP_GTYPEMASK;
    if (gadget_type == GTYP_PROPGADGET)
        return emu68k_require_guest_range(special, EMU68K_PROPINFO_SIZEOF,
                                          "Gadget PropInfo", err, errlen);
    if (gadget_type == GTYP_STRGADGET)
    {
        ULONG buffer, undo, maxchars;
        if (emu68k_require_guest_range(special, EMU68K_STRINGINFO_SIZEOF,
                                       "Gadget StringInfo", err, errlen) < 0)
            return -1;
        maxchars = emu68k_scalar_from_guest(
            guest0, special + EMU68K_STRING_MaxChars, 2);
        buffer = gr32(guest0, special + EMU68K_STRING_Buffer);
        undo = gr32(guest0, special + EMU68K_STRING_UndoBuffer);
        if (!maxchars || maxchars > 65535 || !buffer ||
            emu68k_require_guest_range(buffer, maxchars,
                                       "StringInfo Buffer", err, errlen) < 0 ||
            (undo && emu68k_require_guest_range(
                undo, maxchars, "StringInfo UndoBuffer", err, errlen) < 0))
            return -1;
        if (gr32(guest0, special + EMU68K_STRING_Extension) ||
            gr32(guest0, special + EMU68K_STRING_AltKeyMap))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: StringInfo at %08lx "
                         "uses a StringExtend or alternate KeyMap",
                         (unsigned long)special);
            return -1;
        }
        return 0;
    }
    if (err && errlen)
        snprintf(err, errlen, "capability gap: Gadget at %08lx has "
                 "SpecialInfo for unsupported gadget type %lu",
                 (unsigned long)addr, (unsigned long)gadget_type);
    return -1;
}

static ULONG gadget_special_alloc_size(APTR guest0, ULONG gadget,
                                       const struct EmuMirror *m)
{
    ULONG gp = gr32(guest0, gadget + M68K_Gadget_SpecialInfo);
    ULONG type = emu68k_scalar_from_guest(
        guest0, gadget + M68K_Gadget_GadgetType, 2) & GTYP_GTYPEMASK;
    ULONG size = m->native_size;

    if (!gp) return size;
    if (type == GTYP_PROPGADGET) return size + sizeof(struct PropInfo);
    if (type == GTYP_STRGADGET)
    {
        ULONG maxchars = emu68k_scalar_from_guest(
            guest0, gp + EMU68K_STRING_MaxChars, 2);
        ULONG undo = gr32(guest0, gp + EMU68K_STRING_UndoBuffer);
        return size + ((sizeof(struct StringInfo) + 7) & ~7UL) +
               maxchars * (undo ? 2 : 1);
    }
    return size;
}

static UWORD gadget_special_flag(APTR guest0, ULONG gadget)
{
    ULONG gp = gr32(guest0, gadget + M68K_Gadget_SpecialInfo);
    ULONG type;
    if (!gp) return 0;
    type = emu68k_scalar_from_guest(
        guest0, gadget + M68K_Gadget_GadgetType, 2) & GTYP_GTYPEMASK;
    if (type == GTYP_PROPGADGET) return EMU68K_OBJ_GADGET_PROP;
    if (type == GTYP_STRGADGET) return EMU68K_OBJ_GADGET_STRING;
    return 0;
}

static void gadget_special_from_guest(APTR guest0, ULONG gadget,
                                      struct Gadget *native,
                                      const struct EmuMirror *m)
{
    ULONG gp = gr32(guest0, gadget + M68K_Gadget_SpecialInfo);
    ULONG type = emu68k_scalar_from_guest(
        guest0, gadget + M68K_Gadget_GadgetType, 2) & GTYP_GTYPEMASK;

    if (!gp)
    {
        native->SpecialInfo = NULL;
        return;
    }
    if (type == GTYP_PROPGADGET)
    {
        struct PropInfo *pi = (struct PropInfo *)
            ((UBYTE *)native + m->native_size);
#define PROP_FROM_GUEST(field, off) \
    pi->field = emu68k_scalar_from_guest(guest0, gp + (off), 2)
    PROP_FROM_GUEST(Flags, 0);
    PROP_FROM_GUEST(HorizPot, 2);
    PROP_FROM_GUEST(VertPot, 4);
    PROP_FROM_GUEST(HorizBody, 6);
    PROP_FROM_GUEST(VertBody, 8);
    PROP_FROM_GUEST(CWidth, 10);
    PROP_FROM_GUEST(CHeight, 12);
    PROP_FROM_GUEST(HPotRes, 14);
    PROP_FROM_GUEST(VPotRes, 16);
    PROP_FROM_GUEST(LeftBorder, 18);
    PROP_FROM_GUEST(TopBorder, 20);
#undef PROP_FROM_GUEST
        native->SpecialInfo = pi;
    }
    else if (type == GTYP_STRGADGET)
    {
        struct StringInfo *si = (struct StringInfo *)
            ((UBYTE *)native + m->native_size);
        UBYTE *bytes = (UBYTE *)si + ((sizeof *si + 7) & ~7UL);
        ULONG maxchars = emu68k_scalar_from_guest(
            guest0, gp + EMU68K_STRING_MaxChars, 2);
        ULONG buffer = gr32(guest0, gp + EMU68K_STRING_Buffer);
        ULONG undo = gr32(guest0, gp + EMU68K_STRING_UndoBuffer);

        si->Buffer = bytes;
        memcpy(si->Buffer, (UBYTE *)guest0 + buffer, maxchars);
        bytes += maxchars;
        if (undo)
        {
            si->UndoBuffer = bytes;
            memcpy(si->UndoBuffer, (UBYTE *)guest0 + undo, maxchars);
        }
#define STRING_FROM_GUEST(field, off) \
    si->field = emu68k_scalar_from_guest(guest0, gp + (off), 2)
        STRING_FROM_GUEST(BufferPos, EMU68K_STRING_BufferPos);
        STRING_FROM_GUEST(MaxChars, EMU68K_STRING_MaxChars);
        STRING_FROM_GUEST(DispPos, EMU68K_STRING_DispPos);
        STRING_FROM_GUEST(UndoPos, EMU68K_STRING_UndoPos);
        STRING_FROM_GUEST(NumChars, EMU68K_STRING_NumChars);
        STRING_FROM_GUEST(DispCount, EMU68K_STRING_DispCount);
        STRING_FROM_GUEST(CLeft, EMU68K_STRING_CLeft);
        STRING_FROM_GUEST(CTop, EMU68K_STRING_CTop);
#undef STRING_FROM_GUEST
        si->Extension = NULL;
        si->LongInt = (LONG)gr32(guest0, gp + EMU68K_STRING_LongInt);
        si->AltKeyMap = NULL;
        native->SpecialInfo = si;
    }
}

static LONG gadget_special_to_guest(APTR guest0, ULONG gadget,
                                    struct Gadget *native,
                                    const struct EmuMirror *m,
                                    char *err, ULONG errlen)
{
    ULONG gp = gr32(guest0, gadget + M68K_Gadget_SpecialInfo);
    ULONG type = emu68k_scalar_from_guest(
        guest0, gadget + M68K_Gadget_GadgetType, 2) & GTYP_GTYPEMASK;

    if (!gp) return 0;
    if (native->SpecialInfo != (APTR)((UBYTE *)native + m->native_size))
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: Intuition replaced "
                     "Gadget %08lx SpecialInfo with an unknown native object",
                     (unsigned long)gadget);
        return -1;
    }
    if (type == GTYP_PROPGADGET)
    {
        struct PropInfo *pi = native->SpecialInfo;
#define PROP_TO_GUEST(field, off) \
    emu68k_scalar_to_guest(guest0, gp + (off), 2, pi->field)
    PROP_TO_GUEST(Flags, 0);
    PROP_TO_GUEST(HorizPot, 2);
    PROP_TO_GUEST(VertPot, 4);
    PROP_TO_GUEST(HorizBody, 6);
    PROP_TO_GUEST(VertBody, 8);
    PROP_TO_GUEST(CWidth, 10);
    PROP_TO_GUEST(CHeight, 12);
    PROP_TO_GUEST(HPotRes, 14);
    PROP_TO_GUEST(VPotRes, 16);
    PROP_TO_GUEST(LeftBorder, 18);
    PROP_TO_GUEST(TopBorder, 20);
#undef PROP_TO_GUEST
    }
    else if (type == GTYP_STRGADGET)
    {
        struct StringInfo *si = native->SpecialInfo;
        ULONG maxchars = emu68k_scalar_from_guest(
            guest0, gp + EMU68K_STRING_MaxChars, 2);
        ULONG buffer = gr32(guest0, gp + EMU68K_STRING_Buffer);
        ULONG undo = gr32(guest0, gp + EMU68K_STRING_UndoBuffer);
        UBYTE *expected = (UBYTE *)si + ((sizeof *si + 7) & ~7UL);

        if (si->Buffer != expected ||
            (undo && si->UndoBuffer != expected + maxchars))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Intuition replaced "
                         "StringInfo %08lx buffers", (unsigned long)gp);
            return -1;
        }
        memcpy((UBYTE *)guest0 + buffer, si->Buffer, maxchars);
        if (undo) memcpy((UBYTE *)guest0 + undo, si->UndoBuffer, maxchars);
#define STRING_TO_GUEST(field, off) \
    emu68k_scalar_to_guest(guest0, gp + (off), 2, si->field)
        STRING_TO_GUEST(BufferPos, EMU68K_STRING_BufferPos);
        STRING_TO_GUEST(MaxChars, EMU68K_STRING_MaxChars);
        STRING_TO_GUEST(DispPos, EMU68K_STRING_DispPos);
        STRING_TO_GUEST(UndoPos, EMU68K_STRING_UndoPos);
        STRING_TO_GUEST(NumChars, EMU68K_STRING_NumChars);
        STRING_TO_GUEST(DispCount, EMU68K_STRING_DispCount);
        STRING_TO_GUEST(CLeft, EMU68K_STRING_CLeft);
        STRING_TO_GUEST(CTop, EMU68K_STRING_CTop);
#undef STRING_TO_GUEST
        gw32(guest0, gp + EMU68K_STRING_LongInt, (ULONG)si->LongInt);
    }
    return 0;
}

/* Every byte of the guest structure this crossing does NOT carry must still be
 * zero. A render Image, a label, a SpecialInfo is a guest pointer with no
 * native meaning; dropping it quietly would draw nothing and blame nobody. */
static LONG mirror_check_cover(APTR guest0, ULONG addr, UWORD type,
                               const struct EmuMirror *m, ULONG span,
                               const char *type_name,
                               char *err, ULONG errlen)
{
    ULONG b;
    for (b = 0; b < span; b++)
    {
        int covered = (m->guest_link >= 0 && (LONG)b >= m->guest_link &&
                       (LONG)b < m->guest_link + 4);
        if (!covered && m == emu68k_mirror_Gadget &&
            b >= M68K_Gadget_SpecialInfo &&
            b < M68K_Gadget_SpecialInfo + 4 &&
            ((emu68k_scalar_from_guest(guest0,
                 addr + M68K_Gadget_GadgetType, 2) & GTYP_GTYPEMASK) ==
                GTYP_PROPGADGET ||
             (emu68k_scalar_from_guest(guest0,
                 addr + M68K_Gadget_GadgetType, 2) & GTYP_GTYPEMASK) ==
                GTYP_STRGADGET))
            covered = 1;
        if (!covered && m == emu68k_mirror_Gadget &&
            ((b >= M68K_Gadget_GadgetRender &&
              b < M68K_Gadget_GadgetRender + 4) ||
             (b >= M68K_Gadget_SelectRender &&
              b < M68K_Gadget_SelectRender + 4)))
            covered = 1;
        if (!covered && m == emu68k_mirror_Gadget &&
            b >= M68K_Gadget_GadgetText &&
            b < M68K_Gadget_GadgetText + 4)
            covered = 1;
        if (!covered && m == emu68k_mirror_Border &&
            b >= M68K_Border_XY && b < M68K_Border_XY + 4)
            covered = 1;
        if (!covered && m == emu68k_mirror_Image &&
            b >= M68K_Image_ImageData && b < M68K_Image_ImageData + 4)
            covered = 1;
        if (!covered && type == EMU_OBJ_RastPort &&
            ((b >= M68K_RastPort_Layer && b < M68K_RastPort_Layer + 4) ||
             (b >= M68K_RastPort_BitMap && b < M68K_RastPort_BitMap + 4) ||
             (b >= M68K_RastPort_Font && b < M68K_RastPort_Font + 4)))
            covered = 1;
        if (!covered && type == EMU_OBJ_Menu &&
            ((b >= M68K_Menu_MenuName && b < M68K_Menu_MenuName + 4) ||
             (b >= M68K_Menu_FirstItem && b < M68K_Menu_FirstItem + 4)))
            covered = 1;
        if (!covered && type == EMU_OBJ_MenuItem &&
            ((b >= M68K_MenuItem_ItemFill &&
              b < M68K_MenuItem_ItemFill + 4) ||
             (b >= M68K_MenuItem_SelectFill &&
              b < M68K_MenuItem_SelectFill + 4) ||
             (b >= M68K_MenuItem_SubItem &&
              b < M68K_MenuItem_SubItem + 4)))
            covered = 1;
        int fi;
        for (fi = 0; !covered && fi < m->field_count; fi++)
        {
            ULONG lo = m->fields[fi].g_off;
            ULONG hi = lo + (ULONG)m->fields[fi].g_w *
                            (m->fields[fi].count ? m->fields[fi].count : 1);
            covered = (b >= lo && b < hi);
        }
        if (covered || !((const UBYTE *)guest0)[addr + b]) continue;
        if (err && errlen)
        {
            if (m == emu68k_mirror_Gadget)
                snprintf(err, errlen, "capability gap: %s at %08lx sets byte %lu, "
                         "which this mirror cannot carry (flags=%04lx type=%04lx "
                         "render=%08lx select=%08lx text=%08lx special=%08lx)",
                         type_name, (unsigned long)addr, (unsigned long)b,
                         (unsigned long)emu68k_scalar_from_guest(guest0,
                             addr + M68K_Gadget_Flags, 2),
                         (unsigned long)emu68k_scalar_from_guest(guest0,
                             addr + M68K_Gadget_GadgetType, 2),
                         (unsigned long)gr32(guest0,
                             addr + M68K_Gadget_GadgetRender),
                         (unsigned long)gr32(guest0,
                             addr + M68K_Gadget_SelectRender),
                         (unsigned long)gr32(guest0,
                             addr + M68K_Gadget_GadgetText),
                         (unsigned long)gr32(guest0,
                             addr + M68K_Gadget_SpecialInfo));
            else
                snprintf(err, errlen, "capability gap: %s at %08lx sets byte %lu, "
                         "which this mirror cannot carry", type_name,
                         (unsigned long)addr, (unsigned long)b);
        }
        return -1;
    }
    return 0;
}

/* Classic applications commonly build their menu strip statically.  The
 * generated object crossing can mirror the two linked-list spines, but Menu
 * and MenuItem also form a retained tree through FirstItem/SubItem and carry
 * discriminated ItemFill pointers.  Validate the whole tree before creating
 * any mirror so a bad descendant cannot leave a half-adopted menu installed. */
static LONG menu_item_tree_validate(struct Emu68kRunState *rs, APTR guest0,
                                    ULONG root, ULONG *seen, ULONG *nseen,
                                    ULONG depth, char *err, ULONG errlen)
{
    ULONG walk;

    if (depth > 32)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: MenuItem tree at %08lx "
                     "exceeds 32 submenu levels", (unsigned long)root);
        return -1;
    }
    for (walk = root; walk; walk = gr32(guest0, walk + M68K_MenuItem_NextItem))
    {
        struct Emu68kObject *o;
        ULONG flags, fill, select, sub, i;

        if (*nseen >= 1024)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: MenuItem tree at %08lx "
                         "exceeds 1024 members", (unsigned long)root);
            return -1;
        }
        for (i = 0; i < *nseen; i++)
            if (seen[i] == walk)
            {
                if (err && errlen)
                    snprintf(err, errlen, "capability gap: MenuItem tree at "
                             "%08lx contains a cycle or shared branch",
                             (unsigned long)root);
                return -1;
            }
        seen[(*nseen)++] = walk;
        o = object_by_token(rs, walk);
        if (o && (o->type != EMU_OBJ_MenuItem ||
                  !(o->flags & EMU68K_OBJ_GUEST_OWNED)))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: MenuItem %08lx is "
                         "already a different object", (unsigned long)walk);
            return -1;
        }
        if (emu68k_require_guest_range(walk, M68K_MenuItem_SIZEOF,
                                       "MenuItem", err, errlen) < 0 ||
            mirror_check_cover(guest0, walk, EMU_OBJ_MenuItem,
                               emu68k_mirror_MenuItem,
                               M68K_MenuItem_SIZEOF, "MenuItem",
                               err, errlen) < 0)
            return -1;

        flags = emu68k_scalar_from_guest(
            guest0, walk + M68K_MenuItem_Flags, 2);
        fill = gr32(guest0, walk + M68K_MenuItem_ItemFill);
        select = gr32(guest0, walk + M68K_MenuItem_SelectFill);
        sub = gr32(guest0, walk + M68K_MenuItem_SubItem);
        if (flags & ITEMTEXT)
        {
            if ((fill && intuitext_family_validate(guest0, fill,
                    "MenuItem IntuiText", err, errlen) < 0) ||
                (select && intuitext_family_validate(guest0, select,
                    "MenuItem select IntuiText", err, errlen) < 0))
                return -1;
        }
        else
        {
            struct Emu68kObject *image;
            image = object_by_token(rs, fill);
            if (fill && !(image && image->type == EMU_OBJ_Image &&
                          !(image->flags & EMU68K_OBJ_GUEST_OWNED)) &&
                image_family_validate(rs, guest0, fill, err, errlen) < 0)
                return -1;
            image = object_by_token(rs, select);
            if (select && !(image && image->type == EMU_OBJ_Image &&
                            !(image->flags & EMU68K_OBJ_GUEST_OWNED)) &&
                image_family_validate(rs, guest0, select, err, errlen) < 0)
                return -1;
        }
        if (sub && menu_item_tree_validate(rs, guest0, sub, seen, nseen,
                                           depth + 1, err, errlen) < 0)
            return -1;
    }
    return 0;
}

static LONG menu_refs_validate(struct Emu68kRunState *rs, APTR guest0,
                               ULONG menu, char *err, ULONG errlen)
{
    ULONG name = gr32(guest0, menu + M68K_Menu_MenuName);
    ULONG first = gr32(guest0, menu + M68K_Menu_FirstItem);
    ULONG seen[1024], nseen = 0;

    if (name && !guest_cstr(guest0, name, 65536))
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: Menu at %08lx has an "
                     "unterminated name", (unsigned long)menu);
        return -1;
    }
    return menu_item_tree_validate(rs, guest0, first, seen, &nseen, 0,
                                   err, errlen);
}

static LONG menu_item_refs_from_guest(APTR guest0, ULONG item,
                                      struct MenuItem *native,
                                      char *err, ULONG errlen)
{
    ULONG flags = emu68k_scalar_from_guest(
        guest0, item + M68K_MenuItem_Flags, 2);
    ULONG fill = gr32(guest0, item + M68K_MenuItem_ItemFill);
    ULONG select = gr32(guest0, item + M68K_MenuItem_SelectFill);
    ULONG sub = gr32(guest0, item + M68K_MenuItem_SubItem);
    APTR nf = NULL, ns = NULL, nsub = NULL;

    if (flags & ITEMTEXT)
    {
        if (fill)
        {
            nf = emu68k_struct_graph_to_native(
                guest0, fill, emu_sdescs, EMU_SDESC_IntuiText,
                "MenuItem.ItemFill", err, errlen);
            if (!nf) return -1;
        }
        if (select)
        {
            ns = emu68k_struct_graph_to_native(
                guest0, select, emu_sdescs, EMU_SDESC_IntuiText,
                "MenuItem.SelectFill", err, errlen);
            if (!ns) return -1;
        }
    }
    else if (object_adopt_guest_impl(
                 guest0, fill, EMU_OBJ_Image, "Image",
                 emu68k_mirror_Image, &nf, FALSE, err, errlen) < 0 ||
             object_adopt_guest_impl(
                 guest0, select, EMU_OBJ_Image, "Image",
                 emu68k_mirror_Image, &ns, FALSE, err, errlen) < 0)
        return -1;
    if (object_adopt_guest_impl(
            guest0, sub, EMU_OBJ_MenuItem, "MenuItem",
            emu68k_mirror_MenuItem, &nsub, FALSE, err, errlen) < 0)
        return -1;
    native->ItemFill = nf;
    native->SelectFill = ns;
    native->SubItem = nsub;
    return 0;
}

static LONG menu_refs_from_guest(APTR guest0, ULONG menu,
                                 struct Menu *native,
                                 char *err, ULONG errlen)
{
    ULONG name = gr32(guest0, menu + M68K_Menu_MenuName);
    ULONG first = gr32(guest0, menu + M68K_Menu_FirstItem);
    APTR nfirst = NULL;

    if (object_adopt_guest_impl(
            guest0, first, EMU_OBJ_MenuItem, "MenuItem",
            emu68k_mirror_MenuItem, &nfirst, FALSE, err, errlen) < 0)
        return -1;
    native->MenuName = name ? gptr(guest0, name) : NULL;
    native->FirstItem = nfirst;
    return 0;
}

static LONG nested_guest_object_to_token(struct Emu68kRunState *rs,
                                         APTR native, UWORD type,
                                         const char *what, ULONG *token,
                                         char *err, ULONG errlen)
{
    struct Emu68kObject *o;
    if (token) *token = 0;
    if (!native) return 0;
    o = object_by_native(rs, native);
    if (!o || o->type != type)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: native %s pointer was "
                     "replaced with an object that has no guest identity",
                     what);
        return -1;
    }
    if (token) *token = o->token;
    return 0;
}

static LONG menu_item_refs_to_guest(struct Emu68kRunState *rs, APTR guest0,
                                    ULONG item, struct MenuItem *native,
                                    char *err, ULONG errlen)
{
    ULONG flags = emu68k_scalar_from_guest(
        guest0, item + M68K_MenuItem_Flags, 2);
    ULONG fill = gr32(guest0, item + M68K_MenuItem_ItemFill);
    ULONG select = gr32(guest0, item + M68K_MenuItem_SelectFill);
    ULONG sub = 0;

    if (flags & ITEMTEXT)
    {
        if ((!fill) != (!native->ItemFill) ||
            (!select) != (!native->SelectFill))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Intuition replaced a "
                         "MenuItem text fill pointer");
            return -1;
        }
    }
    else
    {
        ULONG native_fill = 0, native_select = 0;
        if (nested_guest_object_to_token(rs, native->ItemFill, EMU_OBJ_Image,
                                         "MenuItem ItemFill", &native_fill,
                                         err, errlen) < 0 ||
            nested_guest_object_to_token(rs, native->SelectFill, EMU_OBJ_Image,
                                         "MenuItem SelectFill", &native_select,
                                         err, errlen) < 0)
            return -1;
        if (native_fill != fill || native_select != select)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Intuition replaced a "
                         "MenuItem image fill pointer");
            return -1;
        }
        if ((fill && emu68k_object_sync_guest(
                guest0, fill, EMU_OBJ_Image, "Image",
                emu68k_mirror_Image, err, errlen) < 0) ||
            (select && emu68k_object_sync_guest(
                guest0, select, EMU_OBJ_Image, "Image",
                emu68k_mirror_Image, err, errlen) < 0))
            return -1;
    }
    if (nested_guest_object_to_token(rs, native->SubItem, EMU_OBJ_MenuItem,
                                     "MenuItem SubItem", &sub,
                                     err, errlen) < 0)
        return -1;
    gw32(guest0, item + M68K_MenuItem_SubItem, sub);
    if (sub && emu68k_object_sync_guest(
            guest0, sub, EMU_OBJ_MenuItem, "MenuItem",
            emu68k_mirror_MenuItem, err, errlen) < 0)
        return -1;
    return 0;
}

static LONG menu_refs_to_guest(struct Emu68kRunState *rs, APTR guest0,
                               ULONG menu, struct Menu *native,
                               char *err, ULONG errlen)
{
    ULONG first = 0;
    ULONG name = gr32(guest0, menu + M68K_Menu_MenuName);

    if ((name ? gptr(guest0, name) : NULL) != native->MenuName)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: Intuition replaced Menu "
                     "%08lx's name pointer", (unsigned long)menu);
        return -1;
    }
    if (nested_guest_object_to_token(rs, native->FirstItem,
                                     EMU_OBJ_MenuItem, "Menu FirstItem",
                                     &first, err, errlen) < 0)
        return -1;
    gw32(guest0, menu + M68K_Menu_FirstItem, first);
    if (first && emu68k_object_sync_guest(
            guest0, first, EMU_OBJ_MenuItem, "MenuItem",
            emu68k_mirror_MenuItem, err, errlen) < 0)
        return -1;
    return 0;
}

static void mirror_rollback(struct Emu68kRunState *rs)
{
    int i;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
        if (rs->objects[i].flags & EMU68K_OBJ_ADOPT_FRESH)
        {
            FreeVec(rs->objects[i].native);
            memset(&rs->objects[i], 0, sizeof rs->objects[i]);
        }
}

static void mirror_commit(struct Emu68kRunState *rs)
{
    int i;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
        rs->objects[i].flags &= (UWORD)~EMU68K_OBJ_ADOPT_FRESH;
}

static LONG object_adopt_guest_impl(APTR guest0, ULONG addr, UWORD type,
                                    const char *type_name,
                                    const struct EmuMirror *m, APTR *native,
                                    BOOL commit, char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    APTR head = NULL, prev = NULL;
    ULONG walk, count, needed = 0;

    if (native) *native = NULL;
    if (!addr) return 0;
    if (!rs)
    {
        if (err && errlen)
            snprintf(err, errlen, "no per-run state to adopt a %s", type_name);
        return -1;
    }

    /* Pass one validates the whole family and creates nothing, so a failure
     * anywhere leaves the table exactly as it was. */
    for (walk = addr, count = 0; walk; count++)
    {
        struct Emu68kObject *o = object_by_token(rs, walk);
        ULONG span;

        if (count >= m->limit)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: the %s family at %08lx "
                         "exceeds %lu members or contains a cycle", type_name,
                         (unsigned long)addr, (unsigned long)m->limit);
            return -1;
        }
        if (!o && (object_token_retired(rs, walk) ||
                   rs->object_tombstones_full))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: stale or unknown %s "
                         "object token %08lx", type_name,
                         (unsigned long)walk);
            return -1;
        }
        if (o && !(o->flags & EMU68K_OBJ_GUEST_OWNED))
        {
            /* At the head this is not adoption at all: the program is passing
             * back an object the bridge issued, which resolves normally. Deeper
             * in the chain it is a family that mixes the two, and their tokens
             * do not mean the same thing - only a mirror's is guest memory. */
            if (count == 0 &&
                (o->type == type ||
                 (type == EMU_OBJ_Gadget && o->type == EMU_OBJ_Object &&
                  ((o->flags & EMU68K_OBJ_GADGET_VIEW) ||
                   class_roots_in(OCLASS(o->native), "gadgetclass")))))
            {
                if (native) *native = o->native;
                return 0;
            }
            if (err && errlen)
                snprintf(err, errlen, "capability gap: the %s family at %08lx "
                         "reaches %08lx, an object this bridge issued",
                         type_name, (unsigned long)addr, (unsigned long)walk);
            return -1;
        }
        if (o && o->type != type)
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: %08lx is already a "
                         "different object than %s", (unsigned long)walk,
                         type_name);
            return -1;
        }
        if (emu68k_require_guest_range(walk, m->m68k_size, type_name,
                                       NULL, 0) < 0)
        {
            /* Not a live token and not guest memory either. Almost always a
             * token this run has already released, so say that rather than
             * describing the program's memory it never was. */
            if (err && errlen)
                snprintf(err, errlen, "capability gap: stale or unknown %s "
                         "object token %08lx", type_name, (unsigned long)walk);
            return -1;
        }
        span = mirror_guest_size(guest0, walk, m);
        if (gadget_special_validate(guest0, walk, m, err, errlen) < 0)
            return -1;
        if (m == emu68k_mirror_Gadget &&
            gadget_render_validate(rs, guest0, walk, err, errlen) < 0)
            return -1;
        if (m == emu68k_mirror_Gadget &&
            gadget_text_validate(guest0, walk, err, errlen) < 0)
            return -1;
        if (m == emu68k_mirror_Border &&
            border_xy_validate(guest0, walk, err, errlen) < 0)
            return -1;
        if (m == emu68k_mirror_Image &&
            image_data_validate(guest0, walk, err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_BitMap &&
            bitmap_planes_validate(guest0, walk, err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_RastPort &&
            rastport_refs_validate(rs, guest0, walk, err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_Menu &&
            menu_refs_validate(rs, guest0, walk, err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_MenuItem && count == 0)
        {
            ULONG menu_seen[1024], menu_nseen = 0;
            if (menu_item_tree_validate(rs, guest0, walk,
                                        menu_seen, &menu_nseen, 0,
                                        err, errlen) < 0)
                return -1;
        }
        if (o && m == emu68k_mirror_Border &&
            ((struct Border *)o->native)->Count !=
                (BYTE)((UBYTE *)guest0)[walk + M68K_Border_Count])
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Border %08lx changed "
                         "coordinate count after its mirror was created",
                         (unsigned long)walk);
            return -1;
        }
        if (o && m == emu68k_mirror_Image)
        {
            ULONG words = 0;
            (void)image_data_words(guest0, walk, &words, NULL, 0);
            if (image_native_words((struct Image *)o->native) != words)
            {
                if (err && errlen)
                    snprintf(err, errlen, "capability gap: Image %08lx changed "
                             "planar data size after its mirror was created",
                             (unsigned long)walk);
                return -1;
            }
        }
        if (o && m == emu68k_mirror_Gadget &&
            gadget_special_flag(guest0, walk) &&
            !(o->flags & gadget_special_flag(guest0, walk)))
        {
            if (err && errlen)
                snprintf(err, errlen, "capability gap: Gadget %08lx changed "
                         "SpecialInfo type after its native mirror was created",
                         (unsigned long)walk);
            return -1;
        }
        if (mirror_check_cover(guest0, walk, type, m, span, type_name,
                               err, errlen) < 0)
            return -1;
        if (!o) needed++;
        if (m->guest_link < 0) { count++; break; }
        walk = gr32(guest0, walk + m->guest_link);
    }

    if (needed)
    {
        ULONG free_slots = 0;
        int i;
        for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
            if (!rs->objects[i].native) free_slots++;
        if (free_slots < needed)
        {
            if (err && errlen)
                snprintf(err, errlen, "more live objects than this bridge "
                         "keeps: adopting this %s family needs %lu more",
                         type_name, (unsigned long)needed);
            return -1;
        }
    }

    /* Pass two creates and converts. Only allocation can still fail, and a
     * failure rolls back every mirror this call made. */
    for (walk = addr, count = 0; walk && count < m->limit; count++)
    {
        struct Emu68kObject *o = object_by_token(rs, walk);
        APTR mirror;

        if (o)
            mirror = o->native;
        else
        {
            ULONG alloc_size = (m == emu68k_mirror_Gadget)
                ? gadget_special_alloc_size(guest0, walk, m)
                : (m == emu68k_mirror_Border)
                    ? border_alloc_size(guest0, walk, m)
                    : (m == emu68k_mirror_Image)
                        ? image_alloc_size(guest0, walk, m)
                        : m->native_size;
            mirror = AllocVec(alloc_size, MEMF_CLEAR);
            if (!mirror)
            {
                mirror_rollback(rs);
                if (err && errlen)
                    snprintf(err, errlen, "out of memory mirroring a %s",
                             type_name);
                return -1;
            }
            o = object_slot(rs);
            o->native  = mirror;
            o->base    = NULL;
            o->cleanup = emu68k_mirror_cleanup;
            o->token   = walk;
            o->refs    = 1;
            o->type    = type;
            o->flags   = EMU68K_OBJ_GUEST_OWNED | EMU68K_OBJ_ADOPT_FRESH;
            if (m == emu68k_mirror_Gadget)
                o->flags |= gadget_special_flag(guest0, walk);
        }

        emu68k_from_guest_sized(guest0, walk, mirror, m->fields, m->field_count,
                                mirror_guest_size(guest0, walk, m));
        if (m == emu68k_mirror_Gadget)
            gadget_special_from_guest(guest0, walk,
                                      (struct Gadget *)mirror, m);
        if (m == emu68k_mirror_Border)
            border_xy_from_guest(guest0, walk,
                                 (struct Border *)mirror, m);
        if (m == emu68k_mirror_Image)
            image_data_from_guest(guest0, walk,
                                  (struct Image *)mirror, m);
        if (type == EMU_OBJ_BitMap)
            bitmap_planes_from_guest(guest0, walk,
                                     (struct BitMap *)mirror);
        if (type == EMU_OBJ_RastPort &&
            rastport_refs_from_guest(guest0, walk,
                                     (struct RastPort *)mirror,
                                     err, errlen) < 0)
        {
            mirror_rollback(rs);
            return -1;
        }
        if (type == EMU_OBJ_Menu &&
            menu_refs_from_guest(guest0, walk, (struct Menu *)mirror,
                                 err, errlen) < 0)
        {
            mirror_rollback(rs);
            return -1;
        }
        if (type == EMU_OBJ_MenuItem &&
            menu_item_refs_from_guest(guest0, walk,
                                      (struct MenuItem *)mirror,
                                      err, errlen) < 0)
        {
            mirror_rollback(rs);
            return -1;
        }
        if (m == emu68k_mirror_Gadget &&
            gadget_render_from_guest(guest0, walk,
                                     (struct Gadget *)mirror,
                                     err, errlen) < 0)
        {
            mirror_rollback(rs);
            return -1;
        }
        if (m == emu68k_mirror_Gadget &&
            gadget_text_from_guest(guest0, walk,
                                   (struct Gadget *)mirror,
                                   err, errlen) < 0)
        {
            mirror_rollback(rs);
            return -1;
        }
        if (m->native_link >= 0)
            *(APTR *)((UBYTE *)mirror + m->native_link) = NULL;
        if (prev && m->native_link >= 0)
            *(APTR *)((UBYTE *)prev + m->native_link) = mirror;
        if (!head) head = mirror;
        prev = mirror;

        if (m->guest_link < 0) break;
        walk = gr32(guest0, walk + m->guest_link);
    }

    if (commit) mirror_commit(rs);
    if (native) *native = head;
    return 0;
}

LONG emu68k_object_adopt_guest(APTR guest0, ULONG addr, UWORD type,
                               const char *type_name,
                               const struct EmuMirror *m,
                               APTR *native, char *err, ULONG errlen)
{
    return object_adopt_guest_impl(guest0, addr, type, type_name, m, native,
                                   TRUE, err, errlen);
}

/* Write the library's view back where the program can read it.
 *
 * The walk follows the NATIVE chain, not the guest one, because the library
 * relinks it: AddGList splices the program's list into the window's, so the
 * last node's successor is decided by the library, not by what the guest wrote.
 * Each link is written back as the next mirror's guest address; a successor
 * with no guest form is a gap worth naming rather than a zero that claims the
 * list ended. */
LONG emu68k_object_sync_guest(APTR guest0, ULONG addr, UWORD type,
                              const char *type_name, const struct EmuMirror *m,
                              char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    struct Emu68kObject *o;
    APTR node;
    ULONG count;

    if (!rs || !addr) return 0;
    o = object_by_token(rs, addr);
    if (!o || o->type != type || !(o->flags & EMU68K_OBJ_GUEST_OWNED))
        return 0;

    for (node = o->native, count = 0; node && count < m->limit; count++)
    {
        struct Emu68kObject *self = object_by_native(rs, node);
        APTR next = (m->native_link >= 0)
                  ? *(APTR *)((UBYTE *)node + m->native_link) : NULL;
        ULONG next_token = 0;

        if (!self || !(self->flags & EMU68K_OBJ_GUEST_OWNED)) return 0;
        if (next)
        {
            struct Emu68kObject *no = object_by_native(rs, next);
            if (!no || !(no->flags & EMU68K_OBJ_GUEST_OWNED))
            {
                if (err && errlen)
                    snprintf(err, errlen, "capability gap: the library linked "
                             "%s %08lx to an object with no guest form",
                             type_name, (unsigned long)self->token);
                return -1;
            }
            next_token = no->token;
        }
        emu68k_to_guest_sized(guest0, self->token, node, m->fields,
                              m->field_count,
                              mirror_guest_size(guest0, self->token, m));
        if (m == emu68k_mirror_Gadget &&
            gadget_special_to_guest(guest0, self->token,
                                    (struct Gadget *)node, m,
                                    err, errlen) < 0)
            return -1;
        if (m == emu68k_mirror_Border &&
            border_xy_to_guest(guest0, self->token,
                               (struct Border *)node, m,
                               err, errlen) < 0)
            return -1;
        if (m == emu68k_mirror_Image &&
            image_data_to_guest(guest0, self->token,
                                (struct Image *)node, m,
                                err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_BitMap &&
            bitmap_planes_to_guest(guest0, self->token,
                                   (struct BitMap *)node,
                                   err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_RastPort &&
            rastport_refs_to_guest(guest0, self->token,
                                   (struct RastPort *)node,
                                   err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_Menu &&
            menu_refs_to_guest(rs, guest0, self->token,
                               (struct Menu *)node, err, errlen) < 0)
            return -1;
        if (type == EMU_OBJ_MenuItem &&
            menu_item_refs_to_guest(rs, guest0, self->token,
                                    (struct MenuItem *)node,
                                    err, errlen) < 0)
            return -1;
        if (m->guest_link >= 0)
            emu68k_scalar_to_guest(guest0, self->token + m->guest_link, 4,
                                   next_token);
        node = next;
    }
    return 0;
}

/* ---- A FONT THE PROGRAM LOADED ITSELF -------------------------------------
 *
 * Software of the period ships its own bitmap fonts and loads them with
 * LoadSeg rather than through diskfont, then hands graphics the TextFont
 * inside the segment it just loaded.  That structure is guest memory, and
 * tf_CharData, tf_CharLoc, tf_CharSpace and tf_CharKern hold guest addresses,
 * so no native library can use it as it stands.
 *
 * A font differs from the mirrored structures above in the way that matters:
 * the library renders from it and never writes to it.  So it is adopted ONCE
 * into a native TextFont built over copies of its glyph tables, registered
 * under its guest address so the program reads its own pointer back out of
 * rp->Font, and released with the run.  Nothing is copied back, because
 * nothing on the native side changes it.
 *
 * The raster is bytes and crosses as bytes; CharLoc, CharSpace and CharKern
 * are big-endian tables and are converted element by element. */
static APTR gen_base_for(const char *libname, APTR DOSBase);

static void emu_object_cleanup_guest_textfont(APTR base, APTR object)
{
    struct GfxBase *GfxBase = (struct GfxBase *)base;
    if (GfxBase && object)
        StripFont((struct TextFont *)object);
    FreeVec(object);
}

static struct Emu68kObject *textfont_adopt_guest(APTR guest0, ULONG addr,
                                                 char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    struct Emu68kObject *o;
    struct TextFont *font;
    UBYTE *block;
    ULONG lo, hi, chars, modulo, ysize;
    ULONG gdata, gloc, gspace, gkern, gname;
    ULONG databytes, locbytes, spanbytes, namelen = 0;
    ULONG head, off_loc, off_data, off_space, off_kern, off_name, total, i;

    if (!rs)
    {
        if (err && errlen)
            snprintf(err, errlen, "no per-run state to adopt a TextFont");
        return NULL;
    }
    if (emu68k_require_guest_range(addr, M68K_TextFont_SIZEOF, "TextFont",
                                   err, errlen) < 0)
        return NULL;

    ysize  = (ULONG)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_YSize, 2);
    modulo = (ULONG)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_Modulo, 2);
    lo     = (ULONG)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_LoChar, 1);
    hi     = (ULONG)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_HiChar, 1);
    gdata  = gr32(guest0, addr + M68K_TextFont_tf_CharData);
    gloc   = gr32(guest0, addr + M68K_TextFont_tf_CharLoc);
    gspace = gr32(guest0, addr + M68K_TextFont_tf_CharSpace);
    gkern  = gr32(guest0, addr + M68K_TextFont_tf_CharKern);
    gname  = gr32(guest0, addr + M68K_TextFont_tf_Message_mn_Node_ln_Name);

    if (hi < lo || !ysize || !modulo || !gdata || !gloc)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: %08lx is not a usable "
                     "TextFont (YSize %lu, Modulo %lu, chars %lu..%lu)",
                     (unsigned long)addr, (unsigned long)ysize,
                     (unsigned long)modulo, (unsigned long)lo,
                     (unsigned long)hi);
        return NULL;
    }
    chars     = hi - lo + 2;             /* plus the trailing default glyph */
    databytes = modulo * ysize;
    locbytes  = chars * 4;
    spanbytes = chars * 2;
    if (emu68k_require_guest_range(gdata, databytes, "TextFont glyph data",
                                   err, errlen) < 0 ||
        emu68k_require_guest_range(gloc, locbytes, "TextFont CharLoc",
                                   err, errlen) < 0 ||
        (gspace && emu68k_require_guest_range(gspace, spanbytes,
                                   "TextFont CharSpace", err, errlen) < 0) ||
        (gkern && emu68k_require_guest_range(gkern, spanbytes,
                                   "TextFont CharKern", err, errlen) < 0))
        return NULL;

    o = object_slot(rs);
    if (!o)
    {
        if (err && errlen)
            snprintf(err, errlen, "more live objects than this bridge keeps: "
                     "adopting a TextFont needs one more");
        return NULL;
    }

    if (gname && emu68k_require_guest_range(gname, 1, "TextFont name",
                                            NULL, 0) >= 0)
        while (namelen < 64 &&
               emu68k_scalar_from_guest(guest0, gname + namelen, 1))
            namelen++;

    /* One allocation, the font first, so releasing it is a single FreeVec. */
    head      = (sizeof(struct TextFont) + 7u) & ~7u;
    off_loc   = head;
    off_data  = off_loc + locbytes;
    off_space = off_data + ((databytes + 1u) & ~1u);
    off_kern  = off_space + spanbytes;
    off_name  = off_kern + spanbytes;
    total     = off_name + namelen + 1u;

    block = AllocVec(total, MEMF_CLEAR | MEMF_PUBLIC);
    if (!block)
    {
        if (err && errlen)
            snprintf(err, errlen, "no memory for a %lu-byte native copy of the "
                     "program's font", (unsigned long)total);
        return NULL;
    }
    font = (struct TextFont *)(void *)block;

    font->tf_Message.mn_Node.ln_Type = NT_FONT;
    for (i = 0; i < namelen; i++)
        block[off_name + i] = (UBYTE)emu68k_scalar_from_guest(guest0, gname + i, 1);
    font->tf_Message.mn_Node.ln_Name = namelen ? (char *)(block + off_name) : NULL;
    font->tf_YSize     = (UWORD)ysize;
    font->tf_Style     = (UBYTE)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_Style, 1);
    font->tf_Flags     = (UBYTE)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_Flags, 1);
    font->tf_XSize     = (UWORD)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_XSize, 2);
    font->tf_Baseline  = (UWORD)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_Baseline, 2);
    font->tf_BoldSmear = (UWORD)emu68k_scalar_from_guest(guest0, addr + M68K_TextFont_tf_BoldSmear, 2);
    font->tf_Accessors = 1;
    font->tf_LoChar    = (UBYTE)lo;
    font->tf_HiChar    = (UBYTE)hi;
    font->tf_Modulo    = (UWORD)modulo;

    CopyMem(gptr(guest0, gdata), block + off_data, databytes);
    font->tf_CharData = block + off_data;
    for (i = 0; i < chars; i++)
        ((ULONG *)(void *)(block + off_loc))[i] = gr32(guest0, gloc + i * 4);
    font->tf_CharLoc = block + off_loc;
    if (gspace)
    {
        for (i = 0; i < chars; i++)
            ((UWORD *)(void *)(block + off_space))[i] =
                (UWORD)emu68k_scalar_from_guest(guest0, gspace + i * 2, 2);
        font->tf_CharSpace = block + off_space;
    }
    if (gkern)
    {
        for (i = 0; i < chars; i++)
            ((UWORD *)(void *)(block + off_kern))[i] =
                (UWORD)emu68k_scalar_from_guest(guest0, gkern + i * 2, 2);
        font->tf_CharKern = block + off_kern;
    }

    o->token   = addr;
    o->native  = font;
    o->base    = gen_base_for("graphics.library", NULL);
    o->cleanup = emu_object_cleanup_guest_textfont;
    o->refs    = 1;
    o->type    = EMU_OBJ_TextFont;
    o->flags   = EMU68K_OBJ_GUEST_OWNED;
    return o;
}

/* Resolve a TextFont argument: a font this bridge issued, or one the program
 * loaded itself, adopted on first sight. */
static struct Emu68kObject *textfont_resolve(APTR guest0, ULONG addr,
                                             char *err, ULONG errlen)
{
    struct Emu68kObject *o = object_by_token(run_state(guest0), addr);
    if (o) return o->type == EMU_OBJ_TextFont ? o : NULL;
    return textfont_adopt_guest(guest0, addr, err, errlen);
}

LONG emu68k_object_from_guest(APTR guest0, ULONG token, UWORD type,
                              BOOL nullable, const char *type_name,
                              APTR *native, char *err, ULONG errlen)
{
    struct Emu68kObject *o;

    if (native) *native = NULL;
    if (!token)
    {
        if (nullable) return 0;
        if (err && errlen)
            snprintf(err, errlen, "capability gap: NULL %s object",
                     type_name ? type_name : "native");
        return -1;
    }
    o = object_by_token(run_state(guest0), token);
    if (!o && type == EMU_OBJ_TextFont)
        o = textfont_adopt_guest(guest0, token, NULL, 0);
    if (!o)
    {
        if (err && errlen)
        {
            /* Say WHICH of the two it is. A token this run once issued and has
               since released is a lifetime bug on our side; an address that was
               never a token is a structure the program built itself, which is a
               different question entirely and needs a different answer. */
            BOOL owned = emu68k_require_guest_range(token, 4, "object",
                                                   NULL, 0) >= 0;
            snprintf(err, errlen,
                     "capability gap: stale or unknown %s object token %08lx%s",
                     type_name ? type_name : "native", (unsigned long)token,
                     owned ? " - memory the program owns, not a token this "
                             "run issued" : "");
        }
        return -1;
    }
    if (o->type != type)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: wrong object type for %s token %08lx",
                     type_name ? type_name : "native", (unsigned long)token);
        return -1;
    }
    if (native) *native = o->native;
    if (type == EMU_OBJ_Object)
        bug("[emu68k/boopsi] resolve token %08lx -> native %p\n",
            (unsigned long)token, o->native);
    return 0;
}

LONG emu68k_object_to_guest(APTR guest0, APTR native, UWORD type,
                            APTR base, EmuObjectCleanup cleanup,
                            const char *type_name, ULONG *token,
                            char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    int i, free_slot = -1;

    if (token) *token = 0;
    if (!native) return 0;
    if (!rs)
    {
        if (cleanup) cleanup(base, native);
        if (err && errlen)
            snprintf(err, errlen, "no per-run state for %s object",
                     type_name ? type_name : "native");
        return -1;
    }
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
    {
        struct Emu68kObject *o = &rs->objects[i];
        if (o->native == native && o->type == type)
        {
            o->refs++;
            if (token) *token = o->token;
            return 0;
        }
        if (!o->native && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0)
    {
        if (cleanup) cleanup(base, native);
        if (err && errlen)
            snprintf(err, errlen, "more live %s objects than this bridge keeps",
                     type_name ? type_name : "native");
        return -1;
    }
    rs->objects[free_slot].token = object_new_token(rs);
    if (!rs->objects[free_slot].token)
    {
        if (cleanup) cleanup(base, native);
        if (err && errlen)
            snprintf(err, errlen, "object token space exhausted");
        return -1;
    }
    rs->objects[free_slot].native = native;
    rs->objects[free_slot].base = base;
    rs->objects[free_slot].cleanup = cleanup;
    rs->objects[free_slot].refs = 1;
    rs->objects[free_slot].type = type;
    if (!cleanup) rs->objects[free_slot].flags |= EMU68K_OBJ_BORROWED;
    if (token) *token = rs->objects[free_slot].token;
    return 0;
}

/* A returned native string cannot be truncated into D0.  Copy it into the
 * run's guest arena and return that 32-bit address.  max_count is a reviewed
 * bound: unterminated native data fails closed instead of walking host memory. */
LONG emu68k_cstr_result_to_guest(APTR guest0, const char *s, ULONG max_count,
                                 ULONG *guest, char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    ULONG length = 0, address;

    if (guest) *guest = 0;
    if (!s) return 0;
    if (!rs || !rs->guest_alloc)
    {
        if (err && errlen) snprintf(err, errlen, "no guest allocator for string result");
        return -1;
    }
    while (length < max_count && s[length]) length++;
    if (length == max_count)
    {
        if (err && errlen) snprintf(err, errlen, "native string result exceeds %lu bytes",
                                    (unsigned long)max_count);
        return -1;
    }
    address = rs->guest_alloc(rs->run, length + 1);
    if (!address)
    {
        if (err && errlen) snprintf(err, errlen, "guest memory exhausted for string result");
        return -1;
    }
    memcpy((UBYTE *)guest0 + address, s, length + 1);
    if (guest) *guest = address;
    return 0;
}

LONG emu68k_host_ptr_to_guest(APTR guest0, APTR native, ULONG *guest,
                              const char *what, char *err, ULONG errlen)
{
    UQUAD delta;
    if (guest) *guest = 0;
    if (!native) return 0;
    if ((IPTR)native < (IPTR)guest0)
        goto outside;
    delta = (UQUAD)(IPTR)native - (UQUAD)(IPTR)guest0;
    if (delta > 0xffffffffUL ||
        emu68k_require_guest_range((ULONG)delta, 1, what, NULL, 0) < 0)
    {
outside:
        if (err && errlen)
            snprintf(err, errlen, "%s returned a pointer outside guest memory",
                     what ? what : "native call");
        return -1;
    }
    if (guest) *guest = (ULONG)delta;
    return 0;
}

LONG emu68k_object_to_guest_facade(APTR guest0, APTR native, UWORD type,
                                   APTR base, EmuObjectCleanup cleanup,
                                   const char *type_name, ULONG facade_size,
                                   const struct EmuField *fields, int field_count,
                                   ULONG *token, char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    int i, free_slot = -1;
    ULONG facade;

    if (token) *token = 0;
    if (!native) return 0;
    if (!rs || !rs->guest_alloc)
    {
        if (cleanup) cleanup(base, native);
        if (err && errlen)
            snprintf(err, errlen, "no guest allocator for %s facade",
                     type_name ? type_name : "native");
        return -1;
    }
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
    {
        struct Emu68kObject *o = &rs->objects[i];
        if (o->native == native && o->type == type)
        {
            o->refs++;
            /* Preserve generated nested tokens already installed in this
             * facade. A facade reached through another object (Window.WScreen
             * is the first case) refreshes scalar fields here but does not
             * recursively rebuild the child's nested object graph. Clearing
             * the whole record would silently erase valid ViewPort/RastPort
             * aliases. Newly allocated facades are still zero-initialized
             * below before their first conversion. */
            emu68k_to_guest(guest0, o->token, native, fields, field_count);
            if (token) *token = o->token;
            return 0;
        }
        if (!o->native && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0)
    {
        if (cleanup) cleanup(base, native);
        if (err && errlen)
            snprintf(err, errlen, "more live %s facades than this bridge keeps",
                     type_name ? type_name : "native");
        return -1;
    }
    facade = rs->guest_alloc(rs->run, facade_size);
    if (!facade)
    {
        if (cleanup) cleanup(base, native);
        if (err && errlen)
            snprintf(err, errlen, "guest memory exhausted for %s facade",
                     type_name ? type_name : "native");
        return -1;
    }
    rs->objects[free_slot].native = native;
    rs->objects[free_slot].base = base;
    rs->objects[free_slot].cleanup = cleanup;
    rs->objects[free_slot].token = facade;
    rs->objects[free_slot].refs = 1;
    rs->objects[free_slot].type = type;
    if (!cleanup) rs->objects[free_slot].flags |= EMU68K_OBJ_BORROWED;
    if (type == EMU_OBJ_Object)
        bug("[emu68k/boopsi] facade %08lx registered for native %p\n",
            (unsigned long)facade, native);
    memset((UBYTE *)guest0 + facade, 0, facade_size);
    emu68k_to_guest(guest0, facade, native, fields, field_count);
    if (token) *token = facade;
    return 0;
}

/* Register a native object at an address that already exists inside another
 * guest facade. Screen.ViewPort is the first case: classic code takes
 * &screen->ViewPort and passes that inline address to graphics.library, while
 * native code must receive &native_screen->ViewPort. No memory is allocated
 * and no pointer is written; the supplied guest address is the typed token. */
LONG emu68k_object_alias_to_guest(APTR guest0, ULONG token, APTR native,
                                  UWORD type, const char *type_name,
                                  char *err, ULONG errlen)
{
    struct Emu68kRunState *rs = run_state(guest0);
    int i, free_slot = -1;

    if (!rs || !token || !native)
    {
        if (err && errlen)
            snprintf(err, errlen, "invalid embedded %s object alias",
                     type_name ? type_name : "native");
        return -1;
    }
    if (emu68k_require_guest_range(token, 1, type_name, err, errlen) < 0)
        return -1;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
    {
        struct Emu68kObject *o = &rs->objects[i];
        if (o->token == token && o->native)
        {
            if (o->native == native && o->type == type)
            {
                o->refs++;
                return 0;
            }
            if (err && errlen)
                snprintf(err, errlen, "embedded %s alias %08lx is already in use",
                         type_name ? type_name : "native", (unsigned long)token);
            return -1;
        }
        if (o->native == native && o->type == type)
        {
            if (err && errlen)
                snprintf(err, errlen, "embedded %s object already has alias %08lx",
                         type_name ? type_name : "native", (unsigned long)o->token);
            return -1;
        }
        if (!o->native && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0)
    {
        if (err && errlen)
            snprintf(err, errlen, "more live %s objects than this bridge keeps",
                     type_name ? type_name : "native");
        return -1;
    }
    rs->objects[free_slot].native = native;
    rs->objects[free_slot].token = token;
    rs->objects[free_slot].refs = 1;
    rs->objects[free_slot].type = type;
    rs->objects[free_slot].flags = EMU68K_OBJ_BORROWED;
    return 0;
}

void emu68k_object_release(APTR guest0, ULONG token, UWORD type)
{
    struct Emu68kRunState *rs = run_state(guest0);
    struct Emu68kObject *o = object_by_token(rs, token);
    if (!o || o->type != type) return;
    if (o->refs > 1)
        o->refs--;
    else if (o->flags & EMU68K_OBJ_BORROWED)
        /* Unlocking a public screen ends the native lock, but does not by
         * itself destroy the screen or its ColorMap.  Classic programs often
         * retain and immediately use those pointer identities after unlock.
         * Keep a weak mapping until an actual owner-consuming call (or run
         * teardown) invalidates it; never call a destructor for a borrow. */
        o->refs = 0;
    else
    {
        object_retire(rs, o);
        memset(o, 0, sizeof *o);
    }
}

void emu68k_object_consume(APTR guest0, ULONG token, UWORD type)
{
    struct Emu68kRunState *rs = run_state(guest0);
    struct Emu68kObject *o = object_by_token(rs, token);
    if (!o || o->type != type) return;
    object_retire(rs, o);
    memset(o, 0, sizeof *o);
}

/* Linked native families are destroyed through their head pointer, while the
 * guest may hold a token for every member returned along the way. Invalidate a
 * member by native identity before its storage is released. */
void emu68k_object_consume_native(APTR guest0, APTR native, UWORD type)
{
    struct Emu68kRunState *rs = run_state(guest0);
    int i;
    if (!rs || !native) return;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
        if (rs->objects[i].native == native && rs->objects[i].type == type)
        {
            object_retire(rs, &rs->objects[i]);
            memset(&rs->objects[i], 0, sizeof rs->objects[i]);
        }
}

/* ---- GUEST-SIDE STRUCTURE WRITES ------------------------------------------
 * Guest memory is BIG-ENDIAN with 32-bit fields; this side is little-endian
 * with 64-bit ones. So a structure is never copied, it is rebuilt a field at a
 * time, and every offset comes from emu68k_layouts.h - generated from the AROS
 * headers for both targets, never counted by hand. */
static void gw8(APTR guest0, ULONG addr, UBYTE v)
{
    ((UBYTE *)guest0)[addr] = v;
}

/* ---- ANCHORPATH: A RETAINED SHADOW ----------------------------------------
 * MatchFirst/MatchNext/MatchEnd are one operation, not three calls: the
 * AnchorPath carries the live state of a directory scan between them,
 * including a chain of AChain structures dos.library allocated. Those are
 * NATIVE pointers, so the guest can never be shown them and the structure
 * cannot be rebuilt per call the way a FileInfoBlock is. The native one is
 * kept for the life of the scan, keyed by the guest's own, and only the fields
 * the program reads travel back. */
static struct AnchorPath *scan_find(struct Emu68kRunState *rs, ULONG guest)
{
    int i;
    if (!rs) return NULL;
    for (i = 0; i < EMU68K_MAX_SCANS; i++)
        if (rs->scans[i].guest == guest && rs->scans[i].nap) return rs->scans[i].nap;
    return NULL;
}

static void scan_drop(struct Emu68kRunState *rs, ULONG guest)
{
    int i;
    if (!rs) return;
    for (i = 0; i < EMU68K_MAX_SCANS; i++)
        if (rs->scans[i].guest == guest)
        {
            if (rs->scans[i].nap) FreeVec(rs->scans[i].nap);
            rs->scans[i].nap = NULL;
            rs->scans[i].guest = 0;
        }
}

/* What the program reads after a match: the entry found, the assembled path,
 * and the flag/break bytes. */
static void ap_to_guest(APTR guest0, ULONG gap, const struct AnchorPath *n,
                        UWORD strlen_)
{
    /* The fixed façade, including the nested FileInfoBlock, comes entirely
     * from the generated dual-ABI field table. Only the variable trailing
     * path buffer needs the scan length kept by this retained shadow. */
    emu68k_to_guest(guest0, gap, n, emu_fields_AnchorPath,
                    EMU_NFIELDS(emu_fields_AnchorPath));
    if (strlen_)
    {
        UWORD i;
        for (i = 0; i < strlen_; i++)
        {
            UBYTE c = (UBYTE)n->ap_Buf[i];
            gw8(guest0, gap + M68K_AnchorPath_ap_Buf + i, c);
            if (!c) break;
        }
    }
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
    int       (*fn)(int lvo, struct Emu68kRegs *r, APTR guest0, APTR base,
                    char *err, ULONG errlen);
    UBYTE       kind;
    UBYTE       tried;
    APTR        base;
};

/* Written from the generated list, so a library cannot be generated for and
 * then not routed here - which is silent, and looks exactly like the library
 * having no crossing for the vector that was called. */
#define EMU_GENLIB_ROW(name, fn, basekind) { name, fn, basekind, 0, NULL },
static struct EmuGenLib g_genlibs[] =
{
    EMU68K_GEN_LIBS(EMU_GENLIB_ROW)
};
#undef EMU_GENLIB_ROW

/* Called from the run's own process, before the guest starts. See the caller
 * for why the difference in context matters. Failures are not reported here:
 * a library nothing calls is not a problem, and one that IS called reports
 * itself precisely at the crossing. */
void Emu68k_OSCallPreopen(void)
{
    unsigned i;

    for (i = 0; i < sizeof(g_genlibs) / sizeof(g_genlibs[0]); i++)
    {
        struct EmuGenLib *g = &g_genlibs[i];
        if (g->kind == GENBASE_OPEN && !g->base)
            g->base = OpenLibrary(g->name, 0);
    }
}

/* ---- The BOOPSI super chain for a guest-created class -------------------
 *
 * A guest class's facade cannot carry native cl_Super (a native pointer has
 * no guest form). MakeClass therefore plants a guest-side STUB IClass there:
 * its dispatcher h_Entry is a reserved vector below the caller's own
 * intuition base - an address the engine already traps on - and its h_Data
 * names the native superclass as a Class token. The guest dispatcher's
 * DoSuperMethodA lands here.
 *
 * OM_NEW is served by making the object on the NATIVE superclass with the
 * same tag domain the outer call used, and answering with a guest FACADE
 * sized to cover the guest class's cl_InstOffset + cl_InstSize, so the
 * guest dispatcher's INST_DATA arithmetic lands in guest-writable memory
 * that native code never reads. Every other method refuses by name until a
 * program drives it. */
#define EMU68K_SUPER_LVO 250   /* offset -1500, far above every real vector */

static LONG class_super_stub(struct Emu68kRunState *rs, APTR guest0,
                             struct Emu68kRegs *r, char *err, ULONG errlen)
{
    struct Emu68kObject *co = object_by_token(rs, r->d[0]);
    Class *cl, *super;
    ULONG stok = 0, stub;

    if (!co) return 0;                     /* NULL result: nothing to plant */
    cl = co->native;
    super = cl->cl_Super;
    if (!super) return 0;
    if (!rs->guest_alloc)
    {
        if (err && errlen)
            snprintf(err, errlen, "no guest allocator for a class super stub");
        return -1;
    }
    if (emu68k_object_to_guest(guest0, super, EMU_OBJ_Class, NULL, NULL,
                               "Class", &stok, err, errlen) < 0)
        return -1;
    stub = rs->guest_alloc(rs->run, M68K_IClass_SIZEOF);
    if (!stub)
    {
        if (err && errlen)
            snprintf(err, errlen, "guest memory exhausted for a super stub");
        return -1;
    }
    emu68k_scalar_to_guest(guest0, stub + M68K_IClass_cl_Dispatcher_h_Entry, 4,
                           r->a[6] - EMU68K_SUPER_LVO * 6u);
    emu68k_scalar_to_guest(guest0, stub + M68K_IClass_cl_Dispatcher_h_Data, 4,
                           stok);
    emu68k_scalar_to_guest(guest0, stub + M68K_IClass_cl_InstOffset, 2,
                           cl->cl_InstOffset);
    emu68k_scalar_to_guest(guest0, stub + M68K_IClass_cl_InstSize, 2,
                           cl->cl_InstSize);
    emu68k_scalar_to_guest(guest0, r->d[0] + M68K_IClass_cl_Super, 4, stub);
    return 0;
}

/* Whether a native class chain roots in the named system class. Decides
 * which struct view a facade of its objects carries. */
static BOOL class_roots_in(Class *cl, const char *id)
{
    int depth;
    for (depth = 0; cl && depth < 32; cl = cl->cl_Super, depth++)
        if (cl->cl_ID && strcmp((const char *)cl->cl_ID, id) == 0)
            return TRUE;
    return FALSE;
}

/* A class made by the guest can be handed back to NewObjectA in either of
 * BOOPSI's equivalent forms: directly as classPtr, or indirectly after
 * AddClass by passing its classID.  The generated crossing already recognizes
 * the direct form.  Find the same object-table identity for the named form so
 * it receives the very same temporary dispatcher bridge.
 *
 * Merely finding a native Class with the name is not enough: system classes
 * and native subclasses live in this table too.  A guest-created class has a
 * readable guest IClass facade whose dispatcher entry was installed by guest
 * code and points back into the guest arena. */
static BOOL guest_class_by_id(struct Emu68kRunState *rs, APTR guest0,
                              const char *id, ULONG *guest_class,
                              Class **native_class)
{
    int i;

    if (guest_class) *guest_class = 0;
    if (native_class) *native_class = NULL;
    if (!rs || !id || !*id) return FALSE;

    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
    {
        struct Emu68kObject *o = &rs->objects[i];
        Class *cl;
        ULONG entry;

        if (!o->native || o->type != EMU_OBJ_Class)
            continue;
        cl = (Class *)o->native;
        if (!cl->cl_ID || strcmp((const char *)cl->cl_ID, id) != 0)
            continue;
        if (emu68k_require_guest_range(o->token, M68K_IClass_SIZEOF,
                                       "BOOPSI Class", NULL, 0) < 0)
            continue;
        if (!(o->flags & EMU68K_OBJ_GUEST_CLASS))
        {
            entry = gr32(guest0,
                         o->token + M68K_IClass_cl_Dispatcher_h_Entry);
            if (emu68k_require_guest_range(entry, 2, "BOOPSI dispatcher",
                                           NULL, 0) < 0)
                continue;
        }
        if (guest_class) *guest_class = o->token;
        if (native_class) *native_class = cl;
        return TRUE;
    }
    return FALSE;
}

static void intuition_object_cleanup(APTR base, APTR object)
{
    struct IntuitionBase *IntuitionBase = base;
    DisposeObject(object);
}

static void graphics_bitmap_cleanup(APTR base, APTR object)
{
    struct GfxBase *GfxBase = base;
    FreeBitMap((struct BitMap *)object);
}

/* A facade of a Gadget-rooted object must READ like a Gadget to the guest:
 * guest library code walks and tests its own gadgets (FreeGadgets checks
 * GadgetType before disposing). Converted ONCE, at creation, with the same
 * reviewed field map the Gadget mirror uses (scalars only - pointer fields
 * stay guest-meaningful): later guest read-modify-writes, like gadtools
 * setting GTYP_GADTOOLS, must survive further crossings. The family link is
 * guest-side too: any known gadget whose NATIVE NextGadget is this object
 * gets this facade's address written into its guest NextGadget. */
static void facade_gadget_view(struct Emu68kRunState *rs, APTR guest0,
                               APTR nobj, ULONG token)
{
    const struct EmuMirror *m = emu68k_mirror_Gadget;
    struct Emu68kObject *self = object_by_native(rs, nobj);
    int i;

    if (!m || !self) return;
    self->flags |= EMU68K_OBJ_GADGET_VIEW;
    emu68k_to_guest(guest0, token, nobj, m->fields, m->field_count);
    if (m->native_link < 0 || m->guest_link < 0) return;
    for (i = 0; i < EMU68K_MAX_OBJECTS; i++)
    {
        struct Emu68kObject *o = &rs->objects[i];
        if (!o->native || o == self) continue;
        if (o->type != EMU_OBJ_Gadget &&
            !(o->type == EMU_OBJ_Object && (o->flags & EMU68K_OBJ_GADGET_VIEW)))
            continue;
        if (*(APTR *)((UBYTE *)o->native + m->native_link) == nobj)
            emu68k_scalar_to_guest(guest0, o->token + (ULONG)m->guest_link, 4,
                                   token);
    }
}

static int super_dispatch(struct Emu68kRunState *rs, APTR guest0, APTR base,
                          struct Emu68kRegs *r, char *err, ULONG errlen)
{
    struct IntuitionBase *IntuitionBase = base;
    ULONG stub = r->a[0];                 /* CallHook A0: the stub IClass    */
    ULONG msg  = r->a[1];
    ULONG gobject = r->a[2];              /* OM_NEW: class; methods: object  */
    ULONG stok, method, gtags, inst_end, token = 0;
    APTR super = NULL, current_class = NULL, nobj;
    struct opSet native_message;
    struct Emu68kObject *super_object;
    struct Emu68kBoopsiBridge super_bridge;
    BOOL bridge_super = FALSE;
    struct TagItem ntags[71];
    UQUAD scratch[192];

    if (emu68k_require_guest_range(stub, M68K_IClass_SIZEOF, "super stub",
                                   err, errlen) < 0 ||
        emu68k_require_guest_range(msg, 4, "super message", err, errlen) < 0)
        return 1;
    stok   = gr32(guest0, stub + M68K_IClass_cl_Dispatcher_h_Data);
    method = gr32(guest0, msg);
    bug("[emu68k/boopsi] super method=%lx stub=%lx stok=%lx gcls=%lx\n",
        (unsigned long)method, (unsigned long)stub, (unsigned long)stok,
        (unsigned long)gobject);
    if (method != OM_NEW && method != OM_DISPOSE)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: super method %08lx "
                     "unserved (OM_NEW and OM_DISPOSE cross)",
                     (unsigned long)method);
        return 1;
    }
    if (emu68k_object_from_guest(guest0, stok, EMU_OBJ_Class, 0, "Class",
                                 &super, err, errlen) < 0)
        return 1;
    if (method == OM_NEW)
    {
        if (emu68k_require_guest_range(msg, 12, "OM_NEW super message",
                                       err, errlen) < 0 ||
            emu68k_object_from_guest(guest0, gobject, EMU_OBJ_Class, 0,
                                     "Class", &current_class,
                                     err, errlen) < 0)
            return 1;
        gtags = gr32(guest0, msg + 4);
        if (emu68k_tags_to_native_known(
                guest0, gtags, emu68k_domain_intuition_new_object,
                ntags, 71, scratch, sizeof scratch, err, errlen) < 0)
            return 1;
    }
    else
    {
        gtags = 0;
        if (emu68k_object_from_guest(guest0, gobject, EMU_OBJ_Object, 0,
                                     "Object", &nobj, err, errlen) < 0)
            return 1;
    }
    /* A guest class may inherit from another guest class.  Its native backing
     * deliberately has no callable native dispatcher: the dispatcher lives
     * at a 32-bit address in this run.  Calling NewObjectA on that backing
     * directly therefore jumps through NULL.  Install the same temporary
     * native->guest dispatcher bridge used by the outer NewObjectA; recursive
     * super chains nest safely because each bridge restores only the class it
     * temporarily owns. */
    super_object = object_by_token(rs, stok);
    if (super_object &&
        (super_object->flags & EMU68K_OBJ_GUEST_CLASS))
    {
        if (emu68k_boopsi_prepare(guest0, stok, super, gtags,
                                  &super_bridge, err, errlen) < 0)
            return 1;
        bridge_super = TRUE;
    }
    if (method == OM_DISPOSE)
    {
        ULONG native_method = OM_DISPOSE;
        r->d[0] = (ULONG)CoerceMethodA((Class *)super, (Object *)nobj,
                                      (Msg)&native_method);
        if (bridge_super &&
            emu68k_boopsi_finish(&super_bridge, err, errlen) < 0)
            return 1;
        return 0;
    }
    native_message.MethodID = OM_NEW;
    native_message.ops_AttrList = gtags ? ntags : NULL;
    native_message.ops_GInfo = NULL;
    /* This is DoSuperMethodA, not NewObjectA(super).  During OM_NEW the
     * object argument is the ORIGINAL subclass. rootclass uses its object size
     * and installs it as OCLASS; allocating the superclass itself loses every
     * guest subclass layer and, for rootclass, simply returns NULL. */
    nobj = (APTR)CoerceMethodA((Class *)super, (Object *)current_class,
                               (Msg)&native_message);
    if (bridge_super &&
        emu68k_boopsi_finish(&super_bridge, err, errlen) < 0)
        return 1;
    if (!nobj)
    {
        r->d[0] = 0;
        return 0;
    }
    /* The annex bound comes from the guest class itself; a class that lies
     * about its instance size only corrupts its own annex. */
    inst_end = 64;
    if (emu68k_require_guest_range(gobject, M68K_IClass_SIZEOF, "class",
                                   NULL, 0) >= 0)
    {
        ULONG off = emu68k_scalar_from_guest(guest0,
                        gobject + M68K_IClass_cl_InstOffset, 2);
        ULONG size = emu68k_scalar_from_guest(guest0,
                        gobject + M68K_IClass_cl_InstSize, 2);
        if (off + size > inst_end) inst_end = off + size;
        if (inst_end > 4096) inst_end = 4096;
    }
    {
        BOOL is_new = object_by_native(rs, nobj) == NULL;
        if (emu68k_object_to_guest_facade(guest0, nobj, EMU_OBJ_Object, NULL,
                                          NULL, "Object", inst_end, NULL, 0,
                                          &token, err, errlen) < 0)
            return 1;
        if (is_new && class_roots_in((Class *)super, "gadgetclass"))
            facade_gadget_view(rs, guest0, nobj, token);
    }
    r->d[0] = token;
    return 0;
}

static int gen_dispatch(const char *libname, int lvo, struct Emu68kRegs *r,
                        APTR guest0, APTR DOSBase, char *err, ULONG errlen)
{
    unsigned i;

    for (i = 0; i < sizeof(g_genlibs) / sizeof(g_genlibs[0]); i++)
    {
        struct EmuGenLib *g = &g_genlibs[i];

        if (strcmp(libname, g->name) != 0)
            continue;

        if (!g->base)
        {
            switch (g->kind)
            {
            case GENBASE_DOS:  g->base = DOSBase;                    break;
            case GENBASE_EXEC: g->base = SysBase;                    break;
            default:           g->base = OpenLibrary(g->name, 0);    break;
            }
            g->tried = 1;
            if (!g->base)
                bug("[emu68k] OpenLibrary(\"%s\") failed\n", g->name);
        }
        if (!g->base)
        {
            /* Not the same thing as having no crossing for the vector, and it
             * used to be reported as if it were: the crossings are all here
             * and the library they call into is what is missing. */
            if (err && errlen)
                snprintf(err, errlen, "%s could not be opened natively", g->name);
            return 1;
        }
        {
            /* Deep tag conversions allocate run-lifetime memory, and the
             * marshaller has no run in its signature: the crossing scope is
             * what names it. */
            struct Emu68kRunState *prev = g_persist_rs;
            int rc;
            emu68k_persist_alloc = emu68k_persist_from_run;
            g_persist_rs = run_state(guest0);
            if (lvo == EMU68K_SUPER_LVO &&
                strcmp(g->name, "intuition.library") == 0)
                rc = super_dispatch(g_persist_rs, guest0, g->base, r,
                                    err, errlen);
            else
                rc = g->fn(lvo, r, guest0, g->base, err, errlen);
            /* MakeClass (vector 113): the class machinery is execution
             * substrate, served here beside ports and processes; when the
             * importer learns to describe it, this moves behind policy. */
            if (rc == 0 && lvo == 113 &&
                strcmp(g->name, "intuition.library") == 0 && r->d[0] &&
                class_super_stub(g_persist_rs, guest0, r, err, errlen) < 0)
                rc = 1;
            g_persist_rs = prev;
            return rc;
        }
    }
    return 1;
}

static APTR gen_base_for(const char *libname, APTR DOSBase)
{
    unsigned i;
    for (i = 0; i < sizeof(g_genlibs) / sizeof(g_genlibs[0]); i++)
    {
        struct EmuGenLib *g = &g_genlibs[i];
        if (strcmp(libname, g->name) != 0) continue;
        if (!g->base)
        {
            if (g->kind == GENBASE_DOS) g->base = DOSBase;
            else if (g->kind == GENBASE_EXEC) g->base = SysBase;
            else g->base = OpenLibrary(g->name, 0);
            g->tried = 1;
        }
        return g->base;
    }
    return NULL;
}

/* Called when a guest run finishes: drop its handles and abandon any scan it
 * left open, so the slot is reusable and nothing outlives the program. */
void Emu68k_OSCallEndRun(APTR guest0)
{
    struct Emu68kRunState *rs;
    int i;

    for (i = 0; i < EMU68K_MAX_RUNS; i++)
    {
        if (g_runs[i].guest0 != guest0) continue;
        rs = &g_runs[i];
        /* Give the process its own PROGDIR: back before anything else: the
         * saved lock belongs to DOS, the applied one is ours to free. */
        if (rs->progdir_active)
        {
            SetProgramDir(rs->progdir_saved);
            if (rs->progdir_lock) UnLock(rs->progdir_lock);
            rs->progdir_lock = BNULL;
            rs->progdir_saved = BNULL;
            rs->progdir_active = FALSE;
            rs->progdir_applied[0] = '\0';
        }
        /* Devices the program opened and never closed. The native MsgPort
         * behind each one owns a signal, so leaving them is a leak the OS
         * reports at process exit - and a program that faults never closes
         * anything. */
        for (int j = 0; j < EMU68K_MAX_DEV; j++)
            if (rs->dev[j].req)
            {
                struct MsgPort *mp = rs->dev[j].req->io_Message.mn_ReplyPort;
                CloseDevice(rs->dev[j].req);
                DeleteIORequest(rs->dev[j].req);
                if (mp) DeleteMsgPort(mp);
                rs->dev[j].req = NULL;
            }
        for (int j = 0; j < EMU68K_MAX_SCANS; j++)
            if (rs->scans[j].nap)
            {
                MatchEnd(rs->scans[j].nap);
                FreeVec(rs->scans[j].nap);
            }
        /* These resources are paired with guest facades, not entries in the
         * ordinary object table.  Release them before closing windows and
         * bitmaps: the native side still owns the real messages, list nodes
         * and framebuffer locks. */
        {
            struct IntuitionBase *IntuitionBase = (struct IntuitionBase *)
                gen_base_for("intuition.library", NULL);
            struct Library *GadToolsBase = (struct Library *)
                gen_base_for("gadtools.library", NULL);
            if (IntuitionBase)
            {
                for (int j = 0; j < EMU68K_MAX_IMSG; j++)
                    if (rs->imsg[j].native)
                    {
                        if (rs->imsg[j].original && GadToolsBase)
                        {
                            struct IntuiMessage *orig =
                                GT_PostFilterIMsg(rs->imsg[j].native);
                            if (orig) ReplyMsg((struct Message *)orig);
                        }
                        else if (rs->imsg[j].allocated)
                            FreeIntuiMessage(rs->imsg[j].native);
                        else
                            ReplyMsg((struct Message *)rs->imsg[j].native);
                    }
                for (int j = 0; j < 8; j++)
                    if (rs->monitors[j].native)
                        FreeMonitorList(rs->monitors[j].native);
                if (rs->pubscreen_list)
                    UnlockPubScreenList();
                for (int j = 0; j < EMU68K_MAX_IDCMP; j++)
                    if (rs->pointer_shadow[j].words)
                    {
                        if (rs->pointer_shadow[j].window)
                            ClearPointer((struct Window *)
                                         rs->pointer_shadow[j].window);
                        FreeVec(rs->pointer_shadow[j].words);
                        rs->pointer_shadow[j].words = NULL;
                    }
            }
        }
        {
            struct Library *CyberGfxBase = (struct Library *)
                gen_base_for("cybergraphics.library", NULL);
            if (CyberGfxBase)
            {
                for (int j = 0; j < 8; j++)
                    if (rs->cmodes[j].native)
                        FreeCModeList(rs->cmodes[j].native);
                for (int j = 0; j < 16; j++)
                    if (rs->cyberlock[j].handle)
                    {
                        memcpy(rs->cyberlock[j].native_base,
                               gptr(guest0, rs->cyberlock[j].guest_buffer),
                               rs->cyberlock[j].size);
                        UnLockBitMap(rs->cyberlock[j].handle);
                    }
            }
        }
        /* Bridge-issued objects first, mirrors second. A native Window still
         * points at the mirrors of the gadgets the program gave it, so closing
         * it has to happen while those mirrors are still there. */
        for (int pass = 0; pass < 2; pass++)
            for (int j = 0; j < EMU68K_MAX_OBJECTS; j++)
            {
                BOOL mirror = (rs->objects[j].flags &
                               EMU68K_OBJ_GUEST_OWNED) != 0;
                if (mirror != (pass == 1)) continue;
                if (!rs->objects[j].native || !rs->objects[j].cleanup) continue;
                while (rs->objects[j].refs)
                {
                    rs->objects[j].refs--;
                    rs->objects[j].cleanup(rs->objects[j].base,
                                           rs->objects[j].native);
                }
            }
        /* After the objects: a disposed class instance may read its label
         * structure right up to the end. */
        while (rs->persist_head)
        {
            struct Emu68kPersistHdr *h = rs->persist_head;
            rs->persist_head = h->next;
            FreeVec(h);
        }
        if (g_persist_rs == rs)
            g_persist_rs = NULL;
        memset(rs, 0, sizeof *rs);
    }
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
    struct Emu68kOSCallCtx *ctx = user;
    APTR DOSBase = ctx ? ctx->dosbase : NULL;
    struct Emu68kRunState *rs = run_state(guest0);
    ULONG modify_flags_old = 0, modify_flags_new = 0, modify_window_token = 0;

    if (!rs)
    {
        if (err && errlen)
            snprintf(err, errlen, "more 68k programs running at once than this "
                     "bridge keeps state for");
        return 1;
    }
    rs->run = ctx ? ctx->run : NULL;
    rs->guest_alloc = ctx ? ctx->guest_alloc : NULL;
    rs->device_base = ctx ? ctx->device_base : NULL;
    rs->call_hook = ctx ? ctx->call_hook : NULL;
    rs->set_mouse_buttons = ctx ? ctx->set_mouse_buttons : NULL;

    /* PROGDIR: is resolved by THIS process, which runs every context of the
     * run. A program a guest starts itself would otherwise resolve it to the
     * directory of whichever program the process was created for. Point the
     * process at the running context's own program directory instead; the
     * original is restored at end of run. */
    if (ctx && ctx->progdir && DOSBase)
    {
        const char *want = ctx->progdir(ctx->run);
        if (want && want[0] && strcmp(want, rs->progdir_applied) != 0)
        {
            BPTR lock = Lock((CONST_STRPTR)want, SHARED_LOCK);
            if (lock)
            {
                BPTR previous = SetProgramDir(lock);
                if (!rs->progdir_active)
                {
                    rs->progdir_saved = previous;   /* the process's own */
                    rs->progdir_active = TRUE;
                }
                else if (rs->progdir_lock)
                    UnLock(rs->progdir_lock);       /* one of ours */
                rs->progdir_lock = lock;
                snprintf(rs->progdir_applied, sizeof rs->progdir_applied,
                         "%s", want);
            }
        }
    }

    /* ---- EVENT BROKER / IDCMP ADAPTER ---------------------------------------
     *
     * A window's UserPort crosses as a facade: the guest holds a readable COPY
     * of the native MsgPort. A program then sits in GetMsg on that copy - which
     * is the whole shape of an Amiga event loop - and nothing ever arrives,
     * because the messages are on the NATIVE port and are native structures
     * with native pointers in them.
     *
     * So take one from the native port and rebuild it where the guest can read
     * it. The reply has to find its way back to the message Intuition actually
     * handed out, so the pairing is remembered rather than reconstructed. */
    /* ---- IDCMP DELIVERY -----------------------------------------------------
     *
     * A window's UserPort is a NATIVE port holding NATIVE messages. The guest
     * cannot read either, so a message is taken from the native port and
     * rebuilt in guest memory on the guest port bound to it.
     *
     * The pump runs when the program WAITS, not when it calls GetMsg. The
     * ordinary Amiga event loop is Wait -> GetMsg -> ReplyMsg, and a program
     * that is blocked in Wait never reaches GetMsg: pumping there would deliver
     * only to a program that polls. So the message is enqueued and the port's
     * signal bit set BEFORE the wait is answered, which is exactly what a real
     * PutMsg does.
     *
     * Only typed, explicitly registered sources are pumped. A worker process's
     * own pr_MsgPort is an ordinary mailbox for its private protocol and must
     * never receive guessed native data. A0 selects one guest destination;
     * when it is zero D0 is the Wait signal mask. D1 reports how many sources
     * matched even when none had a message, separating idle from unbound. */
    if (strcmp(libname, "exec.library") == 0 && lvo == EXEC_LVO_EVENT_PUMP)
    {
        ULONG selected_port = r->a[0];
        ULONG selected_mask = r->d[0];
        ULONG delivered = 0;
        ULONG matched = 0;
        ULONG classes = 0;
        ULONG last_class = 0;
        ULONG last_code = 0;
        int i;

        r->d[0] = 0;
        r->d[1] = 0;
        r->d[2] = 0;
        r->d[3] = 0;
        r->d[4] = 0;
        for (i = 0; i < EMU68K_MAX_IDCMP; i++)
        {
            ULONG guest_port = rs->idcmp[i].guest_port;
            struct MsgPort *native_port;
            ULONG bit;

            if (!guest_port || !rs->idcmp[i].window)
                continue;
            bit = *((UBYTE *)guest0 + guest_port + M68K_MsgPort_mp_SigBit);
            if (selected_port) {
                if (guest_port != selected_port) continue;
            } else {
                if (bit >= 32 || !(selected_mask & (1UL << bit))) continue;
            }
            native_port = ((struct Window *)rs->idcmp[i].window)->UserPort;
            if (!native_port) continue;
            matched++;

            for (;;)
            {
                struct IntuiMessage *im;
                ULONG guest_msg;
                int slot;

                im = (struct IntuiMessage *)GetMsg(native_port);
                if (!im) break;
                /* Some classic desktop software uses Intuition for delivery
                 * but polls CIA-A PRA for the live select-button state while
                 * handling the message. Keep the host engine's narrow,
                 * read-only CIA input view synchronized with native IDCMP. */
                if (im->Class == IDCMP_MOUSEBUTTONS)
                {
                    if (im->Code == SELECTDOWN)
                        rs->mouse_buttons |= 1UL;
                    else if (im->Code == SELECTUP)
                        rs->mouse_buttons &= ~1UL;
                    else if (im->Code == MENUDOWN)
                        rs->mouse_buttons |= 2UL;
                    else if (im->Code == MENUUP)
                        rs->mouse_buttons &= ~2UL;
                    if (rs->set_mouse_buttons)
                        rs->set_mouse_buttons(rs->run,
                                              (unsigned int)rs->mouse_buttons);
                }
                classes |= im->Class;
                last_class = im->Class;
                last_code = im->Code;
                if (im->Class == IDCMP_ACTIVEWINDOW)
                    rs->idcmp[i].active_seen = TRUE;
                else if (im->Class == IDCMP_INACTIVEWINDOW)
                {
                    rs->idcmp[i].active_seen = FALSE;
                    rs->idcmp[i].synth_classes = 0;
                }
                for (slot = 0; slot < EMU68K_MAX_IMSG; slot++)
                    if (!rs->imsg[slot].native) break;
                if (slot == EMU68K_MAX_IMSG || !rs->guest_alloc ||
                    !(guest_msg = rs->guest_alloc(rs->run,
                                                  M68K_IntuiMessage_SIZEOF)))
                {
                    ReplyMsg((struct Message *)im);
                    break;
                }
                intui_message_to_guest(rs, guest0, guest_msg, im);
                rs->imsg[slot].native = im;
                rs->imsg[slot].guest  = guest_msg;
                rs->imsg[slot].allocated = FALSE;

                idcmp_queue_guest(guest0, guest_port, bit, guest_msg);
                delivered++;
            }

            /* Replay an activation edge this window's ModifyIDCMP raced
             * against. The message is this bridge's own allocation; the
             * program's ReplyMsg frees it instead of replying to Intuition. */
            if ((rs->idcmp[i].synth_classes & IDCMP_ACTIVEWINDOW) &&
                rs->guest_alloc)
            {
                struct IntuitionBase *IntuitionBase = (struct IntuitionBase *)
                    gen_base_for("intuition.library", NULL);
                struct Window *nw = (struct Window *)rs->idcmp[i].window;
                struct IntuiMessage *im = NULL;
                ULONG guest_msg = 0;
                int slot;

                rs->idcmp[i].synth_classes &= ~(ULONG)IDCMP_ACTIVEWINDOW;
                for (slot = 0; slot < EMU68K_MAX_IMSG; slot++)
                    if (!rs->imsg[slot].native) break;
                if (IntuitionBase && slot < EMU68K_MAX_IMSG)
                    im = AllocIntuiMessage(nw);
                if (im &&
                    !(guest_msg = rs->guest_alloc(rs->run,
                                                  M68K_IntuiMessage_SIZEOF)))
                {
                    FreeIntuiMessage(im);
                    im = NULL;
                }
                if (im)
                {
                    im->Class = IDCMP_ACTIVEWINDOW;
                    im->Code = 0;
                    im->Qualifier = 0;
                    im->MouseX = nw->MouseX;
                    im->MouseY = nw->MouseY;
                    CurrentTime(&im->Seconds, &im->Micros);
                    intui_message_to_guest(rs, guest0, guest_msg, im);
                    rs->imsg[slot].native = im;
                    rs->imsg[slot].guest = guest_msg;
                    rs->imsg[slot].allocated = TRUE;
                    idcmp_queue_guest(guest0, guest_port, bit, guest_msg);
                    delivered++;
                    classes |= IDCMP_ACTIVEWINDOW;
                    last_class = IDCMP_ACTIVEWINDOW;
                    last_code = 0;
                    rs->idcmp[i].active_seen = TRUE;
                }
            }
        }
        r->d[0] = delivered;
        r->d[1] = matched;
        r->d[2] = classes;
        r->d[3] = last_class;
        r->d[4] = last_code;
        return 0;
    }
    /* GadTools filters a native IntuiMessage before the application sees it.
     * The broker has already removed and paired that native message with a
     * guest facade, so filter the remembered original and rewrite the SAME
     * facade. This retains GadTools' private filter context until GT_ReplyIMsg
     * performs the paired post-filter and native reply. */
    if (strcmp(libname, "gadtools.library") == 0 &&
        lvo == GADTOOLS_LVO_GT_GETIMSG)
    {
        struct Library *GadToolsBase = (struct Library *)
            gen_base_for("gadtools.library", NULL);
        ULONG guest_msg = r->a[1];
        int i;
        if (!GadToolsBase) return 1;
        for (i = 0; i < EMU68K_MAX_IMSG; i++)
            if (rs->imsg[i].native && rs->imsg[i].guest == guest_msg)
            {
                struct IntuiMessage *orig = rs->imsg[i].native;
                struct IntuiMessage *filtered = GT_FilterIMsg(orig);
                if (!filtered)
                {
                    ReplyMsg((struct Message *)orig);
                    memset(&rs->imsg[i], 0, sizeof rs->imsg[i]);
                    r->d[0] = 0;
                    return 0;
                }
                rs->imsg[i].native = filtered;
                rs->imsg[i].original = orig;
                intui_message_to_guest(rs, guest0, guest_msg, filtered);
                r->d[0] = guest_msg;
                return 0;
            }
        r->d[0] = 0;
        return 0;
    }
    if (strcmp(libname, "gadtools.library") == 0 &&
        lvo == GADTOOLS_LVO_GT_REPLYIMSG)
    {
        struct Library *GadToolsBase = (struct Library *)
            gen_base_for("gadtools.library", NULL);
        int i;
        if (!GadToolsBase) return 1;
        for (i = 0; i < EMU68K_MAX_IMSG; i++)
            if (rs->imsg[i].native && rs->imsg[i].guest == r->a[1])
            {
                struct IntuiMessage *orig =
                    GT_PostFilterIMsg(rs->imsg[i].native);
                if (orig) ReplyMsg((struct Message *)orig);
                memset(&rs->imsg[i], 0, sizeof rs->imsg[i]);
                r->d[0] = 0;
                return 0;
            }
        r->d[0] = 0;
        return 0;
    }
    /* A reply has to reach the message Intuition is waiting to get back, so the
     * pairing is remembered rather than reconstructed. */
    if (strcmp(libname, "exec.library") == 0 && lvo == EXEC_LVO_REPLYMSG)
    {
        int i;
        for (i = 0; i < EMU68K_MAX_IMSG; i++)
            if (rs->imsg[i].native && rs->imsg[i].guest == r->a[1])
            {
                if (rs->imsg[i].original)
                {
                    struct Library *GadToolsBase = (struct Library *)
                        gen_base_for("gadtools.library", NULL);
                    struct IntuiMessage *orig = GadToolsBase ?
                        GT_PostFilterIMsg(rs->imsg[i].native) : NULL;
                    if (orig) ReplyMsg((struct Message *)orig);
                }
                else if (rs->imsg[i].allocated)
                {
                    /* This bridge's own allocation: Intuition is not waiting
                     * for it, so a reply would corrupt a foreign port. */
                    struct IntuitionBase *IntuitionBase =
                        (struct IntuitionBase *)
                        gen_base_for("intuition.library", NULL);
                    if (IntuitionBase)
                        FreeIntuiMessage(rs->imsg[i].native);
                }
                else
                    ReplyMsg((struct Message *)rs->imsg[i].native);
                memset(&rs->imsg[i], 0, sizeof rs->imsg[i]);
                return 0;
            }
        return 1;                        /* the guest's own message: not ours */
    }

    /* The supported shared-IDCMP pattern: the program puts its own port in
     * Window->UserPort and calls ModifyIDCMP. The native window keeps its
     * NATIVE port - guest memory must never be handed to Intuition - and the
     * two are bound here so the pump knows where that window's input goes.
     * Several windows sharing one port is the normal case, not an edge one. */
    if (strcmp(libname, "intuition.library") == 0 && lvo == INT_LVO_MODIFYIDCMP)
    {
        struct Emu68kObject *w = object_by_token(rs, r->a[0]);
        if (w && w->type == EMU_OBJ_Window)
            modify_flags_old = ((struct Window *)w->native)->IDCMPFlags;
        modify_flags_new = r->d[0];
        modify_window_token = r->a[0];
        idcmp_bind_window(rs, guest0, r->a[0]);
        /* deliberately no return: the crossing itself still has to run */
    }
    if (strcmp(libname, "intuition.library") == 0 &&
        lvo == INTUITION_LVO_CLOSEWINDOW)
        idcmp_unbind_window(rs, r->a[0]);

    /* ---- DEVICES ------------------------------------------------------------
     *
     * Open the REAL device. A device this system does not have has to fail the
     * way a missing device fails; succeeding and behaving oddly afterwards is
     * the worst of both. What the guest gets is a base in io_Device - the same
     * facade a bridged library gets - so the device's vectors reach the bridge.
     *
     * The IORequest the device gets is ours, allocated natively. The guest's is
     * big-endian with 32-bit pointers and could never be queued on a native
     * device; the two are paired so a close finds the right one. */
    if (strcmp(libname, "exec.library") == 0 && lvo == EXEC_LVO_OPENDEVICE)
    {
        const char *name = guest_cstr(guest0, r->a[0], 64);
        struct MsgPort *port;
        struct IORequest *req;
        ULONG base;
        int i, slot = -1;

        if (!name) return 1;
        for (i = 0; i < EMU68K_MAX_DEV; i++)
            if (!rs->dev[i].req) { slot = i; break; }
        if (slot < 0) return 1;

        port = CreateMsgPort();
        if (!port) return 1;
        req = (struct IORequest *)CreateIORequest(port, sizeof(struct IOStdReq));
        if (!req) { DeleteMsgPort(port); return 1; }
        if (OpenDevice((CONST_STRPTR)name, (ULONG)r->d[0], req,
                       (ULONG)r->d[1]) != 0)
        {
            DeleteIORequest(req);
            DeleteMsgPort(port);
            return 1;                    /* let the host name the gap          */
        }
        rs->dev[slot].req = req;
        rs->dev[slot].guest_req = r->a[1];
        snprintf(rs->dev[slot].name, sizeof rs->dev[slot].name, "%s", name);

        /* A base the guest can call through, reusing one per device name so a
         * second open of the same device is the same base, as it is natively. */
        base = ctx && ctx->device_base ? ctx->device_base(rs->run, name) : 0;
        if (!base)
        {
            CloseDevice(req);
            DeleteIORequest(req);
            DeleteMsgPort(port);
            rs->dev[slot].req = NULL;
            return 1;
        }
        if (r->a[1])
        {
            gw32(guest0, r->a[1] + M68K_IORequest_io_Device, base);
            gw32(guest0, r->a[1] + M68K_IORequest_io_Unit, 0);
        }
        r->d[0] = 0;
        return 0;
    }
    if (strcmp(libname, "exec.library") == 0 && lvo == EXEC_LVO_CLOSEDEVICE)
    {
        int i;
        for (i = 0; i < EMU68K_MAX_DEV; i++)
            if (rs->dev[i].req && rs->dev[i].guest_req == r->a[0])
            {
                struct MsgPort *port = rs->dev[i].req->io_Message.mn_ReplyPort;
                CloseDevice(rs->dev[i].req);
                DeleteIORequest(rs->dev[i].req);
                DeleteMsgPort(port);
                rs->dev[i].req = NULL;
                rs->dev[i].guest_req = 0;
                return 0;
            }
        return 0;
    }

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
            r->d[0] = handle_token(rs, Output());
            return 0;

        case DOS_LVO_INPUT:
            r->d[0] = handle_token(rs, Input());
            return 0;

        case DOS_LVO_WRITE:      /* Write(BPTR file D1, APTR buf D2, LONG len D3) */
            r->d[0] = (ULONG)Write(handle_bptr(rs, r->d[1]),
                                   gptr(guest0, r->d[2]), (LONG)r->d[3]);
            return 0;

        case DOS_LVO_READ:       /* Read(BPTR file D1, APTR buf D2, LONG len D3)  */
            r->d[0] = (ULONG)Read(handle_bptr(rs, r->d[1]),
                                  gptr(guest0, r->d[2]), (LONG)r->d[3]);
            return 0;

        case DOS_LVO_OPEN:       /* Open(STRPTR name D1, LONG mode D2) -> BPTR    */
            r->d[0] = handle_token(rs, Open((CONST_STRPTR)gptr(guest0, r->d[1]),
                                        (LONG)r->d[2]));
            return 0;

        case DOS_LVO_CLOSE:      /* Close(BPTR file D1)                           */
            r->d[0] = (ULONG)Close(handle_bptr(rs, r->d[1]));
            handle_release(rs, r->d[1]);
            return 0;

        case DOS_LVO_SEEK:       /* Seek(BPTR D1, LONG pos D2, LONG mode D3)      */
            r->d[0] = (ULONG)Seek(handle_bptr(rs, r->d[1]), (LONG)r->d[2],
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
            r->d[0] = (ULONG)IsInteractive(handle_bptr(rs, r->d[1]));
            return 0;

        case DOS_LVO_WAITFORCHAR:     /* WaitForChar(BPTR file D1, LONG tmo D2)   */
            r->d[0] = (ULONG)WaitForChar(handle_bptr(rs, r->d[1]), (LONG)r->d[2]);
            return 0;

        case DOS_LVO_FLUSH:           /* Flush(BPTR file D1)                      */
            r->d[0] = (ULONG)Flush(handle_bptr(rs, r->d[1]));
            return 0;

        /* A lock is a BPTR like a file handle, so it crosses through the same
         * table. What a program may NOT be handed is the native BPTR itself:
         * it is 64-bit and a 68k register is not. */
        case DOS_LVO_LOCK:            /* Lock(STRPTR name D1, LONG mode D2)       */
            r->d[0] = handle_token(rs, Lock((CONST_STRPTR)gptr(guest0, r->d[1]),
                                        (LONG)r->d[2]));
            return 0;

        case DOS_LVO_UNLOCK:          /* UnLock(BPTR lock D1)                     */
            UnLock(handle_bptr(rs, r->d[1]));
            handle_release(rs, r->d[1]);
            return 0;

        case DOS_LVO_DUPLOCK:         /* DupLock(BPTR lock D1)                    */
            r->d[0] = handle_token(rs, DupLock(handle_bptr(rs, r->d[1])));
            return 0;

        case DOS_LVO_CREATEDIR:       /* CreateDir(STRPTR name D1)                */
            r->d[0] = handle_token(rs, CreateDir((CONST_STRPTR)gptr(guest0, r->d[1])));
            return 0;

        case DOS_LVO_MAKELINK:
            r->d[0] = (ULONG)MakeLink((CONST_STRPTR)gptr(guest0, r->d[1]),
                r->d[3] ? (SIPTR)gptr(guest0, r->d[2])
                        : (SIPTR)handle_bptr(rs, r->d[2]),
                (LONG)r->d[3]);
            return 0;

        /* FilePart/PathPart return a pointer INTO the string they were given.
         * A native pointer is meaningless to the guest and does not fit a 68k
         * register, but the OFFSET is exactly the same on both sides, so the
         * answer is the guest's own pointer advanced by it. */
        case DOS_LVO_FILEPART:
        case DOS_LVO_PATHPART:
        {
            STRPTR p = (STRPTR)gptr(guest0, r->d[1]), q;
            if (!p) { r->d[0] = 0; return 0; }
            q = (lvo == DOS_LVO_FILEPART) ? FilePart(p) : PathPart(p);
            r->d[0] = q ? (ULONG)(r->d[1] + (ULONG)(q - p)) : 0;
            return 0;
        }

        /* [T3b] Pattern matching goes to AROS's OWN MatchFirst: the AmigaDOS
         * pattern syntax is dos.library's, and reimplementing it here would be
         * a second, subtly different matcher. Only the structure crosses. */
        case DOS_LVO_MATCHFIRST:
        case DOS_LVO_MATCHNEXT:
        {
            ULONG gap = (lvo == DOS_LVO_MATCHFIRST) ? r->d[2] : r->d[1];
            const UBYTE *g = (const UBYTE *)guest0 + gap;
            UWORD slen = (UWORD)((g[M68K_AnchorPath_ap_Strlen] << 8) |
                                  g[M68K_AnchorPath_ap_Strlen + 1]);
            struct AnchorPath *nap;
            LONG rc;

            if (lvo == DOS_LVO_MATCHFIRST)
            {
                scan_drop(rs, gap);                    /* a restart on the same one */
                nap = AllocVec(sizeof(struct AnchorPath) + slen + 1,
                               MEMF_ANY | MEMF_CLEAR);
                if (!nap) { r->d[0] = ERROR_NO_FREE_STORE; return 0; }
                /* the settings the program filled in before calling */
                nap->ap_Strlen    = slen;
                nap->ap_Flags     = g[M68K_AnchorPath_ap_Flags];
                nap->ap_BreakBits = (LONG)
                    (((ULONG)g[M68K_AnchorPath_ap_BreakBits] << 24) |
                     ((ULONG)g[M68K_AnchorPath_ap_BreakBits + 1] << 16) |
                     ((ULONG)g[M68K_AnchorPath_ap_BreakBits + 2] << 8) |
                       (ULONG)g[M68K_AnchorPath_ap_BreakBits + 3]);
                {
                    int i;
                    for (i = 0; i < EMU68K_MAX_SCANS; i++)
                        if (!rs->scans[i].nap) break;
                    if (i == EMU68K_MAX_SCANS)
                    {
                        FreeVec(nap);
                        snprintf(err, errlen, "more concurrent MatchFirst scans "
                                 "than this bridge keeps state for");
                        return 1;
                    }
                    rs->scans[i].guest = gap;
                    rs->scans[i].nap   = nap;
                }
                /* A guest that passes a NULL pattern is a guest bug, but it
                 * must not become OUR crash: native MatchFirst dereferences it
                 * and the whole run dies inside a library call, reported as a
                 * fault in translated code with no obvious cause. Fail it the
                 * AmigaOS way and let the program see an error. */
                if (!r->d[1])
                {
                    FreeVec(nap);
                    scan_drop(rs, gap);
                    SetIoErr(ERROR_OBJECT_NOT_FOUND);
                    r->d[0] = ERROR_OBJECT_NOT_FOUND;
                    return 0;
                }
                rc = MatchFirst((CONST_STRPTR)gptr(guest0, r->d[1]), nap);
            }
            else
            {
                nap = scan_find(rs, gap);
                if (!nap) { r->d[0] = ERROR_OBJECT_WRONG_TYPE; return 0; }
                rc = MatchNext(nap);
            }

            if (rc == 0)
                ap_to_guest(guest0, gap, nap, slen);
            else
                scan_drop(rs, gap);                    /* the scan is over          */
            r->d[0] = (ULONG)rc;
            return 0;
        }

        case DOS_LVO_MATCHEND:
        {
            struct AnchorPath *nap = scan_find(rs, r->d[1]);
            if (nap) MatchEnd(nap);
            scan_drop(rs, r->d[1]);
            return 0;
        }

        case DOS_LVO_CURRENTDIR:      /* CurrentDir(BPTR lock D1) -> the old one  */
            r->d[0] = handle_token(rs, CurrentDir(handle_bptr(rs, r->d[1])));
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

    if (strcmp(libname, "workbench.library") == 0)
    {
        /* Registering with Workbench asks it to SEND the program messages, on
         * a port the program then waits on. Nothing delivers a message into a
         * guest yet: it runs only while we run it, and its port is a structure
         * in its own arena that native code cannot put a message on. So the
         * registration is declined rather than accepted and never honoured,
         * which is what a program is told when Workbench will not take it, and
         * is the answer every one of these is written to cope with.
         *
         * The removals succeed: removing something never added is a no-op, and
         * a program that tidies up on exit should not fail doing it. */
        switch (lvo)
        {
        case WB_LVO_ADDAPPWINDOW:
        case WB_LVO_ADDAPPICON:
        case WB_LVO_ADDAPPMENUITEM:
        case WB_LVO_ADDAPPWINDOWDROPZONE:
            bug("[emu68k] Workbench registration declined (LVO %d): no message"
                " delivery into a 68k guest yet\n", lvo);
            r->d[0] = 0;
            return 0;
        case WB_LVO_REMOVEAPPWINDOW:
        case WB_LVO_REMOVEAPPICON:
        case WB_LVO_REMOVEAPPMENUITEM:
        case WB_LVO_REMOVEAPPWINDOWDROPZONE:
            r->d[0] = DOSTRUE;
            return 0;
        }
    }

    if (strcmp(libname, "graphics.library") == 0)
    {
        if (lvo == GRAPHICS_LVO_BLTBITMAPRASTPORT)
        {
            struct BitMap srcshadow, dstbmshadow, *srcbm;
            struct RastPort dstshadow, *dstrp;

            if (guest_bitmap_view(rs, guest0, r->a[0], &srcshadow, &srcbm,
                                  "BltBitMapRastPort source", err,
                                  errlen) < 0 ||
                guest_render_rastport(rs, guest0, r->a[1], &dstshadow,
                                      &dstbmshadow, &dstrp, err, errlen) < 0)
                return 1;
            BltBitMapRastPort(srcbm, (WORD)r->d[0], (WORD)r->d[1], dstrp,
                              (WORD)r->d[2], (WORD)r->d[3],
                              (WORD)r->d[4], (WORD)r->d[5], r->d[6]);
            return 0;
        }

        if (lvo == GRAPHICS_LVO_SETRAST)
        {
            struct Emu68kObject *rpo = object_by_token(rs, r->a[1]);
            ULONG bitmap, bytesperrow, rows, depth, plane, size;

            if (!r->a[1] || (rpo && rpo->type != EMU_OBJ_RastPort))
                return 1;
            if (rpo)
            {
                SetRast((struct RastPort *)rpo->native, r->d[0]);
                return 0;
            }
            if (emu68k_require_guest_range(r->a[1], M68K_RastPort_SIZEOF,
                                           "SetRast RastPort", err,
                                           errlen) < 0)
                return 1;
            bitmap = gr32(guest0, r->a[1] + M68K_RastPort_BitMap);
            if (!bitmap || emu68k_require_guest_range(
                    bitmap, M68K_BitMap_SIZEOF, "SetRast BitMap",
                    err, errlen) < 0)
                return 1;
            bytesperrow = emu68k_scalar_from_guest(
                guest0, bitmap + M68K_BitMap_BytesPerRow, 2);
            rows = emu68k_scalar_from_guest(
                guest0, bitmap + M68K_BitMap_Rows, 2);
            depth = *((UBYTE *)guest0 + bitmap + M68K_BitMap_Depth);
            if (!bytesperrow || !rows || !depth || depth > 8 ||
                (UQUAD)bytesperrow * rows > 0xffffffffUL)
            {
                if (err && errlen)
                    snprintf(err, errlen, "SetRast invalid guest BitMap "
                             "geometry %lux%lu depth %lu",
                             (unsigned long)bytesperrow,
                             (unsigned long)rows, (unsigned long)depth);
                return 1;
            }
            size = bytesperrow * rows;
            for (plane = 0; plane < depth; plane++)
            {
                ULONG pixels = gr32(guest0, bitmap + M68K_BitMap_Planes +
                                     plane * 4);
                if (!pixels || emu68k_require_guest_range(
                        pixels, size, "SetRast plane", err, errlen) < 0)
                    return 1;
                memset((UBYTE *)guest0 + pixels,
                       (r->d[0] & (1UL << plane)) ? 0xff : 0x00, size);
            }
            return 0;
        }

        if (lvo == GRAPHICS_LVO_TEXTLENGTH ||
            lvo == GRAPHICS_LVO_TEXTEXTENT ||
            lvo == GRAPHICS_LVO_TEXTFIT)
        {
            struct RastPort shadow, *rp;
            struct TextExtent extent, constraint;
            ULONG text_count = (UWORD)r->d[0];

            if (guest_text_rastport(rs, guest0, r->a[1], &shadow, &rp,
                                    err, errlen) < 0 ||
                emu68k_require_guest_range(r->a[0], text_count,
                                           "text bytes", err, errlen) < 0)
                return 1;
            if (lvo == GRAPHICS_LVO_TEXTLENGTH)
            {
                r->d[0] = TextLength(rp, gptr(guest0, r->a[0]), text_count);
                return 0;
            }
            if (emu68k_require_guest_range(r->a[2], M68K_TextExtent_SIZEOF,
                                           "TextExtent output", err,
                                           errlen) < 0)
                return 1;
            if (lvo == GRAPHICS_LVO_TEXTEXTENT)
            {
                TextExtent(rp, gptr(guest0, r->a[0]), text_count, &extent);
                guest_text_extent_from_native(guest0, r->a[2], &extent);
                return 0;
            }
            if (r->a[3])
            {
                if (emu68k_require_guest_range(r->a[3],
                                               M68K_TextExtent_SIZEOF,
                                               "TextFit constraint", err,
                                               errlen) < 0)
                    return 1;
                guest_text_extent_to_native(guest0, r->a[3], &constraint);
            }
            r->d[0] = TextFit(rp, gptr(guest0, r->a[0]), text_count,
                              &extent, r->a[3] ? &constraint : NULL,
                              (LONG)r->d[1],
                              r->d[2], r->d[3]);
            guest_text_extent_from_native(guest0, r->a[2], &extent);
            return 0;
        }

        if (lvo == GRAPHICS_LVO_SETAPEN ||
            lvo == GRAPHICS_LVO_SETBPEN ||
            lvo == GRAPHICS_LVO_SETDRMD ||
            lvo == GRAPHICS_LVO_SETABPENDRMD ||
            lvo == GRAPHICS_LVO_SETOUTLINEPEN ||
            lvo == GRAPHICS_LVO_SETWRITEMASK ||
            lvo == GRAPHICS_LVO_SETMAXPEN ||
            lvo == GRAPHICS_LVO_SETSOFTSTYLE)
        {
            ULONG rp = (lvo == GRAPHICS_LVO_SETAPEN ||
                        lvo == GRAPHICS_LVO_SETBPEN ||
                        lvo == GRAPHICS_LVO_SETDRMD ||
                        lvo == GRAPHICS_LVO_SETABPENDRMD ||
                        lvo == GRAPHICS_LVO_SETSOFTSTYLE) ? r->a[1] : r->a[0];
            ULONG value0 = r->d[0];
            struct Emu68kObject *rpo = object_by_token(rs, rp);
            UBYTE *g = (UBYTE *)guest0;

            if (!rp || (rpo && rpo->type != EMU_OBJ_RastPort) ||
                emu68k_require_guest_range(rp, M68K_RastPort_SIZEOF,
                                           "graphics setter RastPort", err,
                                           errlen) < 0)
                return 1;

            if (rpo)
            {
                struct RastPort *native = (struct RastPort *)rpo->native;
                switch (lvo)
                {
                case GRAPHICS_LVO_SETAPEN: SetAPen(native, r->d[0]); break;
                case GRAPHICS_LVO_SETBPEN: SetBPen(native, r->d[0]); break;
                case GRAPHICS_LVO_SETDRMD: SetDrMd(native, r->d[0]); break;
                case GRAPHICS_LVO_SETABPENDRMD:
                    SetABPenDrMd(native, r->d[0], r->d[1], r->d[2]); break;
                case GRAPHICS_LVO_SETOUTLINEPEN:
                    r->d[0] = SetOutlinePen(native, r->d[0]); break;
                case GRAPHICS_LVO_SETWRITEMASK:
                    r->d[0] = SetWriteMask(native, r->d[0]); break;
                case GRAPHICS_LVO_SETMAXPEN: SetMaxPen(native, r->d[0]); break;
                case GRAPHICS_LVO_SETSOFTSTYLE:
                    r->d[0] = SetSoftStyle(native, r->d[0], r->d[1]); break;
                }
            }

            switch (lvo)
            {
            case GRAPHICS_LVO_SETAPEN:
                g[rp + M68K_RastPort_FgPen] = (UBYTE)value0;
                g[rp + M68K_RastPort_linpatcnt] = 15;
                guest_rastport_enable_pens(guest0, rp);
                break;
            case GRAPHICS_LVO_SETBPEN:
                g[rp + M68K_RastPort_BgPen] = (UBYTE)value0;
                g[rp + M68K_RastPort_linpatcnt] = 15;
                guest_rastport_enable_pens(guest0, rp);
                guest_rastport_minterms(guest0, rp);
                break;
            case GRAPHICS_LVO_SETDRMD:
                g[rp + M68K_RastPort_DrawMode] = (UBYTE)value0;
                g[rp + M68K_RastPort_linpatcnt] = 15;
                guest_rastport_minterms(guest0, rp);
                break;
            case GRAPHICS_LVO_SETABPENDRMD:
                g[rp + M68K_RastPort_FgPen] = (UBYTE)value0;
                g[rp + M68K_RastPort_BgPen] = (UBYTE)r->d[1];
                g[rp + M68K_RastPort_DrawMode] = (UBYTE)r->d[2];
                g[rp + M68K_RastPort_linpatcnt] = 15;
                guest_rastport_enable_pens(guest0, rp);
                guest_rastport_minterms(guest0, rp);
                break;
            case GRAPHICS_LVO_SETOUTLINEPEN:
            {
                ULONG oldpen = g[rp + M68K_RastPort_AOlPen];
                g[rp + M68K_RastPort_AOlPen] = (UBYTE)value0;
                g[rp + M68K_RastPort_Flags + 1] |= AREAOUTLINE;
                if (!rpo) r->d[0] = oldpen;
                break;
            }
            case GRAPHICS_LVO_SETWRITEMASK:
                g[rp + M68K_RastPort_Mask] = (UBYTE)value0;
                if (!rpo) r->d[0] = 0xffffffffUL;
                break;
            case GRAPHICS_LVO_SETMAXPEN:
                if (value0)
                {
                    ULONG v = value0;
                    v |= v >> 1; v |= v >> 2; v |= v >> 4;
                    g[rp + M68K_RastPort_Mask] = (UBYTE)v;
                }
                break;
            case GRAPHICS_LVO_SETSOFTSTYLE:
            {
                ULONG fontp = emu68k_scalar_from_guest(guest0,
                    rp + M68K_RastPort_Font, 4);
                ULONG fontstyle, realenable;
                UBYTE algostyle;

                if (!fontp || emu68k_require_guest_range(
                        fontp, M68K_TextFont_SIZEOF,
                        "SetSoftStyle TextFont", err, errlen) < 0)
                    return 1;
                fontstyle = g[fontp + M68K_TextFont_tf_Style];
                realenable = r->d[1] & ~fontstyle;
                algostyle = g[rp + M68K_RastPort_AlgoStyle];
                algostyle = (UBYTE)((~realenable & algostyle) |
                                    (realenable & value0));
                g[rp + M68K_RastPort_AlgoStyle] = algostyle;
                if (!rpo) r->d[0] = algostyle | fontstyle;
                break;
            }
            }
            return 0;
        }

        if (lvo == GRAPHICS_LVO_EXTENDFONT)
        {
            struct Emu68kObject *tfo = r->a[0]
                ? textfont_resolve(guest0, r->a[0], err, errlen) : NULL;
            if (!tfo) return 1;
            if (r->a[1])
            {
                /* The tag list selects a different rendering path; converting
                 * it means deciding what each tag does to an adopted font. */
                if (err && errlen)
                    snprintf(err, errlen, "capability gap: ExtendFont with a "
                             "tag list");
                return 1;
            }
            r->d[0] = ExtendFont((struct TextFont *)tfo->native, NULL);
            return 0;
        }

        if (lvo == GRAPHICS_LVO_SETFONT)
        {
            struct Emu68kObject *rpo = object_by_token(rs, r->a[1]);
            struct Emu68kObject *tfo = r->a[0]
                ? textfont_resolve(guest0, r->a[0], err, errlen) : NULL;
            struct GfxBase *GfxBase = (struct GfxBase *)gen_base_for(
                "graphics.library", DOSBase);
            struct TextFont *font;
            ULONG font_token = r->a[0];

            if (!r->a[1] || (rpo && rpo->type != EMU_OBJ_RastPort))
                return 1;
            if (r->a[0] && !tfo)
            {
                if (err && errlen && !err[0])
                    snprintf(err, errlen, "SetFont received unknown TextFont %08lx",
                             (unsigned long)r->a[0]);
                return 1;
            }
            font = tfo ? (struct TextFont *)tfo->native : NULL;

            /* A NULL TextFont means GfxBase->DefaultFont.  Guest code may
             * subsequently read rp->Font, so expose that borrowed native
             * font through the same guest-readable facade used by OpenFont.
             * This is not an owned OpenFont result and must never be closed
             * during bridge cleanup. */
            if (!font)
            {
                if (!GfxBase || !(font = GfxBase->DefaultFont) ||
                    emu68k_object_to_guest_facade(guest0, font,
                        EMU_OBJ_TextFont, GfxBase, NULL, "TextFont",
                        EMU68K_TEXTFONT_NAME_OFF + 64,
                        emu_fields_TextFont, EMU_NFIELDS(emu_fields_TextFont),
                        &font_token, err, errlen) < 0)
                    return 1;
                if (font_token)
                {
                    ULONG name = font_token + EMU68K_TEXTFONT_NAME_OFF;
                    emu68k_cstr_to_guest(guest0, name,
                        font->tf_Message.mn_Node.ln_Name, 64);
                    emu68k_scalar_to_guest(guest0,
                        font_token + M68K_TextFont_tf_Message_mn_Node_ln_Name,
                        4, font->tf_Message.mn_Node.ln_Name ? name : 0);
                    if (textfont_extension_to_guest(rs, guest0, font_token,
                                                    err, errlen) < 0)
                        return 1;
                }
            }

            if (rpo)
                SetFont((struct RastPort *)rpo->native, font);
            else if (emu68k_require_guest_range(r->a[1], M68K_RastPort_SIZEOF,
                                                "SetFont RastPort", err,
                                                errlen) < 0)
                return 1;

            /* graphics/SetFont has only four observable mutations.  Keeping
             * those in the guest structure is both sufficient for a
             * program-owned RastPort and necessary for an issued facade that
             * guest code subsequently reads. */
            emu68k_scalar_to_guest(guest0, r->a[1] + M68K_RastPort_Font,
                                   4, font_token);
            emu68k_scalar_to_guest(guest0, r->a[1] + M68K_RastPort_TxWidth,
                                   2, font->tf_XSize);
            emu68k_scalar_to_guest(guest0, r->a[1] + M68K_RastPort_TxHeight,
                                   2, font->tf_YSize);
            emu68k_scalar_to_guest(guest0, r->a[1] + M68K_RastPort_TxBaseline,
                                   2, font->tf_Baseline);
            return 0;
        }

        if (lvo == GRAPHICS_LVO_BLTMASKBITMAPRASTPORT)
        {
            struct GfxBase *GfxBase = (struct GfxBase *)gen_base_for(
                "graphics.library", DOSBase);
            struct Emu68kObject *srco = object_by_token(rs, r->a[0]);
            struct Emu68kObject *dsto = object_by_token(rs, r->a[1]);
            struct BitMap guestbm, *srcbm;
            ULONG maskbytes = 0;
            PLANEPTR mask;

            if (!GfxBase || !dsto || dsto->type != EMU_OBJ_RastPort)
                return 1;
            if (srco && srco->type == EMU_OBJ_BitMap)
                srcbm = srco->native;
            else
            {
                ULONG gp = r->a[0], rows, bytesperrow, depth, i;
                if (emu68k_require_guest_range(gp, M68K_BitMap_SIZEOF,
                                               "BltMask source BitMap",
                                               err, errlen) < 0)
                    return 1;
                memset(&guestbm, 0, sizeof guestbm);
                bytesperrow = emu68k_scalar_from_guest(guest0,
                    gp + M68K_BitMap_BytesPerRow, 2);
                rows = emu68k_scalar_from_guest(guest0,
                    gp + M68K_BitMap_Rows, 2);
                depth = *((UBYTE *)guest0 + gp + M68K_BitMap_Depth);
                if (depth > 8 || (UQUAD)bytesperrow * rows > 0xffffffffUL)
                    return 1;
                guestbm.BytesPerRow = bytesperrow;
                guestbm.Rows = rows;
                guestbm.Flags = *((UBYTE *)guest0 + gp + M68K_BitMap_Flags);
                guestbm.Depth = depth;
                for (i = 0; i < depth; i++)
                {
                    ULONG plane = gr32(guest0, gp + M68K_BitMap_Planes + i * 4);
                    if (emu68k_require_guest_range(plane, bytesperrow * rows,
                                                   "BitMap plane", err,
                                                   errlen) < 0)
                        return 1;
                    guestbm.Planes[i] = gptr(guest0, plane);
                }
                maskbytes = bytesperrow * rows;
                srcbm = &guestbm;
            }
            if (!maskbytes)
                maskbytes = (ULONG)srcbm->BytesPerRow * srcbm->Rows;
            if (emu68k_require_guest_range(r->a[2], maskbytes,
                                           "BltMask mask", err, errlen) < 0)
                return 1;
            mask = gptr(guest0, r->a[2]);
            BltMaskBitMapRastPort(srcbm, (WORD)r->d[0], (WORD)r->d[1],
                (struct RastPort *)dsto->native, (WORD)r->d[2], (WORD)r->d[3],
                (WORD)r->d[4], (WORD)r->d[5], r->d[6], mask);
            return 0;
        }

        if (lvo == GRAPHICS_LVO_WRITEPIXELS8)
        {
            struct GfxBase *GfxBase = (struct GfxBase *)gen_base_for(
                "graphics.library", DOSBase);
            struct Emu68kObject *rpo = object_by_token(rs, r->a[0]);
            LONG height = (WORD)r->d[4] - (WORD)r->d[2] + 1;
            ULONG bytes, i;
            ULONG pixlut[256];
            if (!GfxBase || !rpo || rpo->type != EMU_OBJ_RastPort ||
                height <= 0 || (UQUAD)r->d[0] * (ULONG)height > 0xffffffffUL)
            { r->d[0] = 0; return 0; }
            bytes = r->d[0] * (ULONG)height;
            if (emu68k_require_guest_range(r->a[1], bytes, "WritePixels8 array",
                                           err, errlen) < 0)
                return 1;
            if (r->a[2])
            {
                if (emu68k_require_guest_range(r->a[2], sizeof pixlut,
                                               "WritePixels8 pixlut",
                                               err, errlen) < 0)
                    return 1;
                for (i = 0; i < 256; i++)
                    pixlut[i] = gr32(guest0, r->a[2] + i * 4);
            }
            r->d[0] = WritePixels8((struct RastPort *)rpo->native,
                gptr(guest0, r->a[1]), r->d[0], (WORD)r->d[1],
                (WORD)r->d[2], (WORD)r->d[3], (WORD)r->d[4],
                r->a[2] ? pixlut : NULL, r->d[5]);
            return 0;
        }

        /* AllocBitMap returns a STRUCTURE the program reads and writes, and
         * that structure in turn contains plane pointers the program follows.
         * A native BitMap cannot cross either boundary: its address and its
         * PLANEPTRs are 64-bit host pointers.  Build the ordinary classic
         * planar form in guest memory instead.  Generated crossings can then
         * rebase its planes when a native graphics call consumes it.
         *
         * For a legacy caller, BMF_DISPLAYABLE is an allocation/alignment
         * request, not a demand for AROS's native HIDD representation.  The
         * guest arena is its chip-addressable world, so the classic planar
         * facade is the compatible answer.  RTG/special-format and friend
         * bitmaps have driver-owned semantics, so those stay native and cross
         * as typed tokens: guest code may pass them back to graphics/cybergfx,
         * but can never mistake their 64-bit internals for a classic BitMap. */
        if (lvo == GFX_LVO_ALLOCBITMAP)
        {
            UWORD width = (UWORD)r->d[0], height = (UWORD)r->d[1];
            ULONG depth = r->d[2], flags = r->d[3];
            ULONG bytesperrow, plane_size, total, bitmap, planes = 0;
            UQUAD total64;
            int slot, i;

            if (!rs || !rs->guest_alloc)
            {
                r->d[0] = 0;
                return 0;
            }
            if (r->a[0] || depth > 8 ||
                (flags & ~(BMF_CLEAR | BMF_DISPLAYABLE | BMF_INTERLEAVED |
                           BMF_STANDARD | BMF_MINPLANES)))
            {
                struct BitMap friend_shadow, *friend = NULL, *native;
                struct GfxBase *GfxBase = (struct GfxBase *)gen_base_for(
                    "graphics.library", DOSBase);
                ULONG token = 0;

                if (!GfxBase || (r->a[0] &&
                    guest_bitmap_view(rs, guest0, r->a[0], &friend_shadow,
                                      &friend, "AllocBitMap friend",
                                      err, errlen) < 0))
                    return 1;
                native = AllocBitMap(width, height, depth, flags, friend);
                if (emu68k_object_to_guest(guest0, native, EMU_OBJ_BitMap,
                                            GfxBase, graphics_bitmap_cleanup,
                                            "BitMap", &token,
                                            err, errlen) < 0)
                    return 1;
                r->d[0] = token;
                return 0;
            }

            for (slot = 0; slot < EMU68K_MAX_BITMAPS; slot++)
                if (!rs->bitmaps[slot].guest) break;
            if (slot == EMU68K_MAX_BITMAPS)
            {
                if (err && errlen)
                    snprintf(err, errlen, "too many live guest BitMaps");
                return 1;
            }

            bytesperrow = (((ULONG)width + 15) >> 4) * 2;
            total64 = (UQUAD)bytesperrow * height;
            if (total64 > 0xffffffffUL ||
                (depth && total64 > 0xffffffffUL / depth))
            {
                r->d[0] = 0;
                return 0;
            }
            plane_size = (ULONG)total64;
            total = plane_size * depth;
            if (total > 0xffffffffUL - M68K_BitMap_SIZEOF)
            {
                r->d[0] = 0;
                return 0;
            }
            /* One allocation makes failure atomic in the bump-owned guest
             * heap and keeps the facade and its planar storage together. */
            bitmap = rs->guest_alloc(rs->run, M68K_BitMap_SIZEOF + total);
            if (!bitmap)
            {
                r->d[0] = 0;
                return 0;
            }
            if (total) planes = bitmap + M68K_BitMap_SIZEOF;

            memset(gptr(guest0, bitmap), 0, M68K_BitMap_SIZEOF);
            emu68k_scalar_to_guest(guest0,
                                   bitmap + M68K_BitMap_BytesPerRow, 2,
                                   bytesperrow);
            emu68k_scalar_to_guest(guest0, bitmap + M68K_BitMap_Rows, 2,
                                   height);
            gw8(guest0, bitmap + M68K_BitMap_Flags,
                (UBYTE)(flags | BMF_STANDARD));
            gw8(guest0, bitmap + M68K_BitMap_Depth, (UBYTE)depth);
            emu68k_scalar_to_guest(guest0, bitmap + M68K_BitMap_pad, 2, 0);
            for (i = 0; i < (int)depth; i++)
                emu68k_scalar_to_guest(guest0,
                                       bitmap + M68K_BitMap_Planes + i * 4,
                                       4, planes + (ULONG)i * plane_size);

            rs->bitmaps[slot].guest = bitmap;
            r->d[0] = bitmap;
            return 0;
        }

        if (lvo == GFX_LVO_FREEBITMAP)
        {
            struct Emu68kObject *o;
            int i;
            if (!r->a[0])
            {
                r->d[0] = 0;
                return 0;
            }
            o = object_by_token(rs, r->a[0]);
            if (o && o->type == EMU_OBJ_BitMap)
            {
                if (o->cleanup != graphics_bitmap_cleanup)
                {
                    if (err && errlen)
                        snprintf(err, errlen,
                                 "FreeBitMap received a borrowed native BitMap");
                    return 1;
                }
                graphics_bitmap_cleanup(o->base, o->native);
                emu68k_object_consume(guest0, r->a[0], EMU_OBJ_BitMap);
                r->d[0] = 0;
                return 0;
            }
            for (i = 0; i < EMU68K_MAX_BITMAPS; i++)
                if (rs->bitmaps[i].guest == r->a[0])
                {
                    /* Guest storage is owned by the run's bump allocator and
                     * is reclaimed with the arena; invalidate only identity. */
                    rs->bitmaps[i].guest = 0;
                    r->d[0] = 0;
                    return 0;
                }
            if (err && errlen)
                snprintf(err, errlen,
                         "FreeBitMap received unknown guest bitmap %08lx",
                         r->a[0]);
            return 1;
        }

        /* AllocRaster hands back an address the PROGRAM writes bitplanes into,
         * so it has to be an address the program can hold and reach: a native
         * one is 64 bits and outside the arena on both counts. The raster is
         * therefore allocated IN the guest, which is also where the generated
         * BitMap crossing expects to find planes it can rebase for a native
         * call. The size is the AmigaOS one, rows of whole words. */
        if (lvo == GFX_LVO_ALLOCRASTER)
        {
            struct Emu68kRunState *rs = run_state(guest0);
            ULONG bytesperrow = ((((ULONG)(UWORD)r->d[0] + 15) >> 3) & ~1UL);
            ULONG size = bytesperrow * (ULONG)(UWORD)r->d[1];

            if (!rs || !rs->guest_alloc || !size)
            {
                r->d[0] = 0;
                return 0;
            }
            r->d[0] = rs->guest_alloc(rs->run, size);
            return 0;
        }
        /* The guest heap is a bump allocator, so a raster is released when the
         * run ends. Saying so beats refusing a call the program must make. */
        if (lvo == GFX_LVO_FREERASTER)
        {
            r->d[0] = 0;
            return 0;
        }
    }

    if (strcmp(libname, "intuition.library") == 0)
    {
        struct IntuitionBase *IntuitionBase = (struct IntuitionBase *)
            gen_base_for("intuition.library", DOSBase);

        /* A hosted run may load a guest implementation of a library whose
         * native implementation has already registered the same public BOOPSI
         * class ID. Native MakeClass quite correctly rejects that duplicate,
         * but the two classes live in different execution domains: native
         * code cannot execute the guest dispatcher, and guest code must not be
         * handed the native class as if it could replace its own class.
         *
         * Make a private native class for storage/allocation, retain the guest
         * ID on it, and register the public name only in this run's object
         * table. Named NewObjectA and FindClass below consult that table first.
         * AddClass/RemoveClass are consequently no-ops for such a shadow: adding
         * it to Intuition's native ClassList would create the duplicate whose
         * meaning is undefined there. */
        if (lvo == INTUITION_LVO_MAKECLASS && r->a[0])
        {
            ULONG class_id_guest = r->a[0], existing = 0;
            const char *class_id = guest_cstr(guest0, class_id_guest, 256);
            struct Emu68kObject *co;
            int rc;

            if (!class_id)
            {
                if (err && errlen)
                    snprintf(err, errlen,
                             "MakeClass classID is not a bounded guest string");
                return 1;
            }
            if (guest_class_by_id(rs, guest0, class_id, &existing, NULL))
            {
                r->d[0] = 0;
                return 0;
            }
            r->a[0] = 0;
            rc = gen_dispatch("intuition.library", lvo, r, guest0, DOSBase,
                              err, errlen);
            r->a[0] = class_id_guest;
            if (rc != 0 || !r->d[0])
                return rc;
            co = object_by_token(rs, r->d[0]);
            if (!co || co->type != EMU_OBJ_Class)
            {
                if (err && errlen)
                    snprintf(err, errlen,
                             "MakeClass returned an untracked class facade");
                return 1;
            }
            ((Class *)co->native)->cl_ID = (ClassID)class_id;
            co->flags |= EMU68K_OBJ_GUEST_CLASS;
            emu68k_scalar_to_guest(guest0,
                                   co->token + M68K_IClass_cl_ID, 4,
                                   class_id_guest);
            return 0;
        }

        if ((lvo == INTUITION_LVO_ADDCLASS ||
             lvo == INTUITION_LVO_REMOVECLASS) && r->a[0])
        {
            struct Emu68kObject *co = object_by_token(rs, r->a[0]);
            /* MakeClass facades own their native backing class (FindClass
             * facades do not). AROS class startup commonly makes one private,
             * writes cl_ID through the returned 68k IClass, then calls
             * AddClass. Import that late ID before registering it per-run.
             * Never add the backing class to the native global list: its
             * dispatcher lives in guest code and native callers cannot invoke
             * it. A missing ID is a private class mistakenly passed to
             * AddClass; a harmless no-op avoids poisoning native FindClass
             * with a list node whose cl_ID is NULL. */
            if (co && co->type == EMU_OBJ_Class && co->cleanup)
            {
                if (lvo == INTUITION_LVO_ADDCLASS &&
                    !(co->flags & EMU68K_OBJ_GUEST_CLASS))
                {
                    ULONG guest_id = gr32(guest0,
                        co->token + M68K_IClass_cl_ID);
                    if (guest_id)
                    {
                        const char *class_id = guest_cstr(guest0, guest_id, 256);
                        if (!class_id)
                        {
                            if (err && errlen)
                                snprintf(err, errlen,
                                    "AddClass cl_ID is not a bounded guest string");
                            return 1;
                        }
                        ((Class *)co->native)->cl_ID = (ClassID)class_id;
                        co->flags |= EMU68K_OBJ_GUEST_CLASS;
                    }
                }
                r->d[0] = 0;
                return 0;
            }
        }

        if (lvo == INTUITION_LVO_FINDCLASS && r->a[0])
        {
            const char *class_id = guest_cstr(guest0, r->a[0], 256);
            ULONG guest_class = 0;
            if (!class_id)
            {
                if (err && errlen)
                    snprintf(err, errlen,
                             "FindClass classID is not a bounded guest string");
                return 1;
            }
            if (guest_class_by_id(rs, guest0, class_id, &guest_class, NULL))
            {
                r->d[0] = guest_class;
                return 0;
            }
        }

        /* NewObjectA's named form may resolve to a class the guest registered
         * with AddClass.  Native Intuition cannot call that class's 68k
         * dispatcher directly, so resolve its existing class facade and use
         * the same callback bridge as the pointer form.  Passing the native
         * class pointer deliberately bypasses a second name lookup; the empty
         * native taglist starts the method while the bridge supplies the
         * original guest taglist to OM_NEW. */
        if (lvo == INTUITION_LVO_NEWOBJECTA && !r->a[0] && r->a[1])
        {
            const char *class_id = guest_cstr(guest0, r->a[1], 256);
            ULONG guest_class = 0, object_token = 0;
            Class *native_class = NULL;

            if (!class_id)
            {
                if (err && errlen)
                    snprintf(err, errlen,
                             "NewObjectA classID is not a bounded guest string");
                return 1;
            }
            if (guest_class_by_id(rs, guest0, class_id, &guest_class,
                                  &native_class))
            {
                struct Emu68kBoopsiBridge bridge;
                struct TagItem empty_tags[1] = { { TAG_DONE, 0 } };
                APTR object;

                bug("[emu68k/boopsi] named guest class %s -> %08lx/%p\n",
                    class_id, (unsigned long)guest_class, native_class);
                if (!IntuitionBase ||
                    emu68k_boopsi_prepare(guest0, guest_class, native_class,
                                           r->a[2], &bridge,
                                           err, errlen) < 0)
                    return 1;
                object = NewObjectA(native_class, NULL, empty_tags);
                if (emu68k_boopsi_finish(&bridge, err, errlen) < 0)
                    return 1;
                if (emu68k_object_to_guest(guest0, object, EMU_OBJ_Object,
                                            IntuitionBase,
                                            intuition_object_cleanup,
                                            "Object", &object_token,
                                            err, errlen) < 0)
                    return 1;
                r->d[0] = object_token;
                return 0;
            }
        }

        if (lvo == INTUITION_LVO_SETWINDOWTITLES)
        {
            struct Emu68kObject *wo = object_by_token(rs, r->a[0]);
            CONST_STRPTR window_title, screen_title;

            if (!IntuitionBase || (r->a[0] &&
                (!wo || wo->type != EMU_OBJ_Window)))
                return 1;
            if (err && errlen) err[0] = 0;
            window_title = retained_guest_title(rs, guest0, r->a[1],
                                                 err, errlen);
            if (r->a[1] && !window_title) return 1;
            screen_title = retained_guest_title(rs, guest0, r->a[2],
                                                 err, errlen);
            if (r->a[2] && !screen_title) return 1;
            SetWindowTitles(wo ? (struct Window *)wo->native : NULL,
                            window_title, screen_title);
            return 0;
        }

        if (lvo == INTUITION_LVO_EASYREQUESTARGS)
        {
            struct Emu68kObject *wo = object_by_token(rs, r->a[0]);
            struct EasyStruct easy;
            ULONG title, text, gadgets, idcmp = 0;
            char formatted[4096];

            if (!IntuitionBase || (r->a[0] &&
                (!wo || wo->type != EMU_OBJ_Window)) ||
                emu68k_require_guest_range(r->a[1], M68K_EasyStruct_SIZEOF,
                    "EasyRequestArgs.easyStruct", err, errlen) < 0)
                return 1;
            memset(&easy, 0, sizeof easy);
            emu68k_from_guest_sized(guest0, r->a[1], &easy,
                emu_fields_EasyStruct, EMU_NFIELDS(emu_fields_EasyStruct),
                M68K_EasyStruct_SIZEOF);
            title = gr32(guest0, r->a[1] + M68K_EasyStruct_es_Title);
            text = gr32(guest0, r->a[1] + M68K_EasyStruct_es_TextFormat);
            gadgets = gr32(guest0, r->a[1] + M68K_EasyStruct_es_GadgetFormat);
            easy.es_Title = guest_cstr(guest0, title, 65536);
            easy.es_GadgetFormat = guest_cstr(guest0, gadgets, 65536);
            if ((title && !easy.es_Title) || !text ||
                (gadgets && !easy.es_GadgetFormat) ||
                easy_text_from_guest(guest0, text, r->a[3], formatted,
                                     sizeof formatted, err, errlen) < 0)
                return 1;
            easy.es_StructSize = sizeof easy;
            easy.es_TextFormat = formatted;
            if (r->a[2])
            {
                if (emu68k_require_guest_range(r->a[2], 4,
                        "EasyRequestArgs.IDCMP_ptr", err, errlen) < 0)
                    return 1;
                idcmp = gr32(guest0, r->a[2]);
            }
            r->d[0] = (ULONG)EasyRequestArgs(
                wo ? (struct Window *)wo->native : NULL, &easy,
                r->a[2] ? &idcmp : NULL, NULL);
            if (r->a[2]) gw32(guest0, r->a[2], idcmp);
            return 0;
        }

        if (lvo == INTUITION_LVO_CLEARPOINTER)
        {
            struct Emu68kObject *wo = object_by_token(rs, r->a[0]);
            int slot;

            if (!IntuitionBase || !wo || wo->type != EMU_OBJ_Window)
                return 1;
            ClearPointer((struct Window *)wo->native);
            for (slot = 0; slot < EMU68K_MAX_IDCMP; slot++)
                if (rs->pointer_shadow[slot].window == wo->native)
                {
                    FreeVec(rs->pointer_shadow[slot].words);
                    memset(&rs->pointer_shadow[slot], 0,
                           sizeof rs->pointer_shadow[slot]);
                    break;
                }
            return 0;
        }

        if (lvo == INTUITION_LVO_SETPOINTER)
        {
            struct Emu68kObject *wo = object_by_token(rs, r->a[0]);
            LONG height = (WORD)r->d[0], width = (WORD)r->d[1];
            ULONG words, i;
            UWORD *shadow;
            int slot, free_slot = -1;
            if (!IntuitionBase || !wo || wo->type != EMU_OBJ_Window ||
                !r->a[1] || height < 0 || width < 0 || width > 16)
            {
                if (err && errlen)
                    snprintf(err, errlen,
                             "SetPointer requires a known Window, sprite data, "
                             "and dimensions 0..16 pixels wide");
                return 1;
            }
            words = (ULONG)height * 2u + 4u;
            if (words > 65536u || emu68k_require_guest_range(r->a[1], words * 2u,
                    "SetPointer sprite words", err, errlen) < 0)
                return 1;
            for (slot = 0; slot < EMU68K_MAX_IDCMP; slot++)
            {
                if (rs->pointer_shadow[slot].window == wo->native) break;
                if (!rs->pointer_shadow[slot].window && free_slot < 0)
                    free_slot = slot;
            }
            if (slot == EMU68K_MAX_IDCMP) slot = free_slot;
            if (slot < 0)
            {
                if (err && errlen)
                    snprintf(err, errlen,
                             "SetPointer has more live window pointer shadows "
                             "than the bridge can retain");
                return 1;
            }
            shadow = AllocVec(words * sizeof(UWORD), MEMF_ANY);
            if (!shadow)
            {
                if (err && errlen)
                    snprintf(err, errlen, "SetPointer shadow allocation failed");
                return 1;
            }
            for (i = 0; i < words; i++)
                shadow[i] = (UWORD)emu68k_scalar_from_guest(guest0,
                                                            r->a[1] + i * 2, 2);
            SetPointer((struct Window *)wo->native, shadow, height, width,
                       (WORD)r->d[2], (WORD)r->d[3]);
            FreeVec(rs->pointer_shadow[slot].words);
            rs->pointer_shadow[slot].window = wo->native;
            rs->pointer_shadow[slot].words = shadow;
            return 0;
        }

        if (lvo == INTUITION_LVO_GETSCREENDATA)
        {
            struct Screen native_screen;
            struct Emu68kObject *so = object_by_token(rs, r->a[1]);
            ULONG size = (UWORD)r->d[0];
            if (!IntuitionBase || size > 84u ||
                emu68k_require_guest_range(r->a[0], size, "GetScreenData buffer",
                                           err, errlen) < 0)
            { r->d[0] = 0; return 0; }
            memset(&native_screen, 0, sizeof native_screen);
            r->d[0] = GetScreenData(&native_screen, sizeof native_screen,
                (UWORD)r->d[1], so && so->type == EMU_OBJ_Screen ? so->native : NULL);
            if (r->d[0])
                emu68k_to_guest_sized(guest0, r->a[0], &native_screen,
                    emu_fields_Screen, EMU_NFIELDS(emu_fields_Screen), size);
            return 0;
        }

        if (lvo == INTUITION_LVO_LOCKPUBSCREENLIST)
        {
            if (!IntuitionBase) return 1;
            if (!rs->pubscreen_list)
                rs->pubscreen_list = LockPubScreenList();
            if (rs->pubscreen_list && !rs->pubscreen_guest && rs->guest_alloc)
            {
                rs->pubscreen_guest = rs->guest_alloc(rs->run, M68K_List_SIZEOF);
                if (rs->pubscreen_guest)
                {
                    memset(gptr(guest0, rs->pubscreen_guest), 0, M68K_List_SIZEOF);
                    gw32(guest0, rs->pubscreen_guest + M68K_List_lh_Head,
                         rs->pubscreen_guest + M68K_List_lh_Tail);
                    gw32(guest0, rs->pubscreen_guest + M68K_List_lh_TailPred,
                         rs->pubscreen_guest + M68K_List_lh_Head);
                }
            }
            r->d[0] = rs->pubscreen_guest;
            return 0;
        }

        if (lvo == INTUITION_LVO_UNLOCKPUBSCREENLIST)
        {
            if (IntuitionBase && rs->pubscreen_list) UnlockPubScreenList();
            rs->pubscreen_list = NULL;
            return 0;
        }

        if (lvo == INTUITION_LVO_NEXTPUBSCREEN)
        {
            struct Emu68kObject *so = object_by_token(rs, r->a[0]);
            UBYTE *name;
            if (!IntuitionBase || emu68k_require_guest_range(r->a[1],
                    MAXPUBSCREENNAME + 1, "NextPubScreen name buffer",
                    err, errlen) < 0)
                return 1;
            name = NextPubScreen(so && so->type == EMU_OBJ_Screen ? so->native : NULL,
                                 gptr(guest0, r->a[1]));
            r->d[0] = name ? r->a[1] : 0;
            return 0;
        }

        if (lvo == INTUITION_LVO_ALLOCINTUIMESSAGE)
        {
            struct Emu68kObject *wo = object_by_token(rs, r->a[0]);
            struct IntuiMessage *native;
            ULONG guest;
            int slot;
            if (!IntuitionBase || !rs->guest_alloc) return 1;
            native = AllocIntuiMessage(wo && wo->type == EMU_OBJ_Window ?
                                       wo->native : NULL);
            if (!native) { r->d[0] = 0; return 0; }
            for (slot = 0; slot < EMU68K_MAX_IMSG; slot++)
                if (!rs->imsg[slot].native) break;
            guest = slot < EMU68K_MAX_IMSG ?
                    rs->guest_alloc(rs->run, M68K_IntuiMessage_SIZEOF) : 0;
            if (!guest) { FreeIntuiMessage(native); r->d[0] = 0; return 0; }
            intui_message_to_guest(rs, guest0, guest, native);
            rs->imsg[slot].native = native;
            rs->imsg[slot].guest = guest;
            rs->imsg[slot].allocated = TRUE;
            r->d[0] = guest;
            return 0;
        }

        if (lvo == INTUITION_LVO_FREEINTUIMESSAGE ||
            lvo == INTUITION_LVO_SENDINTUIMESSAGE)
        {
            int slot;
            for (slot = 0; slot < EMU68K_MAX_IMSG; slot++)
                if (rs->imsg[slot].native && rs->imsg[slot].guest ==
                    (lvo == INTUITION_LVO_FREEINTUIMESSAGE ? r->a[0] : r->a[1]))
                    break;
            if (slot == EMU68K_MAX_IMSG) return 1;
            emu68k_from_guest(guest0, rs->imsg[slot].guest,
                rs->imsg[slot].native, emu_fields_IntuiMessage,
                EMU_NFIELDS(emu_fields_IntuiMessage));
            if (lvo == INTUITION_LVO_FREEINTUIMESSAGE)
            {
                FreeIntuiMessage(rs->imsg[slot].native);
                rs->imsg[slot].native = NULL; rs->imsg[slot].guest = 0;
                rs->imsg[slot].allocated = FALSE;
            }
            else
            {
                struct Emu68kObject *wo = object_by_token(rs, r->a[0]);
                if (!wo || wo->type != EMU_OBJ_Window) return 1;
                SendIntuiMessage(wo->native, rs->imsg[slot].native);
                /* SendIntuiMessage transfers the native message to the
                 * window.  If it comes back through the IDCMP pump it gets a
                 * fresh guest pairing there; this allocation slot no longer
                 * owns it. */
                rs->imsg[slot].native = NULL; rs->imsg[slot].guest = 0;
                rs->imsg[slot].allocated = FALSE;
            }
            return 0;
        }

        if (lvo == INTUITION_LVO_GETDRAWINFOATTR)
        {
            struct Emu68kObject *dio = object_by_token(rs, r->a[0]);
            IPTR native_result;
            IPTR ok = FALSE;
            ULONG kind = r->d[0] & 0x00f00000u;
            if (!IntuitionBase || (r->a[0] &&
                (!dio || dio->type != EMU_OBJ_DrawInfo))) return 1;
            native_result = GetDrawInfoAttr(dio ? dio->native : NULL,
                                            r->d[0], &ok);
            if (r->a[1])
            {
                if (emu68k_require_guest_range(r->a[1], 4,
                        "GetDrawInfoAttr result", err, errlen) < 0) return 1;
                gw32(guest0, r->a[1], ok ? 0xffffffffUL : 0);
            }
            if (ok && (kind == GDIA_Font || kind == GDIA_CheckMark ||
                       kind == GDIA_MenuKey))
            {
                UWORD type = kind == GDIA_Font ? EMU_OBJ_TextFont : EMU_OBJ_Image;
                if (emu68k_object_to_guest(guest0, (APTR)native_result, type,
                        NULL, NULL, kind == GDIA_Font ? "TextFont" : "Image",
                        &r->d[0], err, errlen) < 0) return 1;
            }
            else r->d[0] = (ULONG)native_result;
            return 0;
        }

        if (lvo == INTUITION_LVO_GETMONITORLIST)
        {
            struct TagItem native_tags[2] = {{TAG_DONE, 0}, {TAG_DONE, 0}};
            Object **list;
            ULONG guest, count = 0, display = 0xffffffffUL;
            int slot;
            if (!IntuitionBase || !rs->guest_alloc) return 1;
            if (r->a[1])
            {
                ULONG p = r->a[1];
                for (unsigned guard = 0; guard < 64; guard++, p += 8)
                {
                    ULONG tag, data;
                    if (emu68k_require_guest_range(p, 8, "GetMonitorList tags",
                                                   err, errlen) < 0) return 1;
                    tag = gr32(guest0, p); data = gr32(guest0, p + 4);
                    if (!tag) break;
                    if (tag == GMLA_DisplayID) display = data;
                }
            }
            if (display != 0xffffffffUL)
            { native_tags[0].ti_Tag = GMLA_DisplayID; native_tags[0].ti_Data = display; }
            list = GetMonitorList(native_tags);
            if (!list) { r->d[0] = 0; return 0; }
            while (list[count] && count < 4096u) count++;
            if (count == 4096u) { FreeMonitorList(list); return 1; }
            guest = rs->guest_alloc(rs->run, (count + 1u) * 4u);
            if (!guest) { FreeMonitorList(list); r->d[0] = 0; return 0; }
            for (ULONG i = 0; i < count; i++)
            {
                ULONG token;
                if (emu68k_object_to_guest(guest0, list[i], EMU_OBJ_Object,
                        NULL, NULL, "Object", &token, err, errlen) < 0)
                { FreeMonitorList(list); return 1; }
                gw32(guest0, guest + i * 4u, token);
            }
            gw32(guest0, guest + count * 4u, 0);
            for (slot = 0; slot < 8; slot++) if (!rs->monitors[slot].native) break;
            if (slot == 8) { FreeMonitorList(list); return 1; }
            rs->monitors[slot].native = list; rs->monitors[slot].guest = guest;
            r->d[0] = guest;
            return 0;
        }

        if (lvo == INTUITION_LVO_FREEMONITORLIST)
        {
            for (int i = 0; i < 8; i++) if (rs->monitors[i].guest == r->a[1])
            {
                FreeMonitorList(rs->monitors[i].native);
                rs->monitors[i].native = NULL; rs->monitors[i].guest = 0;
                break;
            }
            return 0;
        }
    }

    if (strcmp(libname, "cybergraphics.library") == 0)
    {
        struct Library *CyberGfxBase = (struct Library *)gen_base_for(
            "cybergraphics.library", DOSBase);

        if (lvo == CYBERGRAPHICS_LVO_ALLOCCMODELISTTAGLIST)
        {
            struct TagItem ntags[16];
            struct List *native;
            struct CyberModeNode *node;
            ULONG guest, glist, count = 0, ntag = 0;
            int slot;
            if (!CyberGfxBase || !rs->guest_alloc) return 1;
            if (r->a[1])
            {
                ULONG p = r->a[1];
                for (unsigned guard = 0; guard < 64 && ntag < 15; guard++, p += 8)
                {
                    ULONG tag, data;
                    if (emu68k_require_guest_range(p, 8, "CModeList tags",
                                                   err, errlen) < 0) return 1;
                    tag = gr32(guest0, p); data = gr32(guest0, p + 4);
                    if (!tag) break;
                    if (tag >= CYBRMREQ_MinDepth && tag <= CYBRMREQ_MaxHeight)
                    { ntags[ntag].ti_Tag = tag; ntags[ntag++].ti_Data = data; }
                }
            }
            ntags[ntag].ti_Tag = TAG_DONE; ntags[ntag].ti_Data = 0;
            native = AllocCModeListTagList(ntags);
            if (!native) { r->d[0] = 0; return 0; }
            for (node = (struct CyberModeNode *)native->lh_Head;
                 node && node->Node.ln_Succ; node =
                 (struct CyberModeNode *)node->Node.ln_Succ)
                if (++count > 4096u) { FreeCModeList(native); return 1; }
            guest = rs->guest_alloc(rs->run,
                M68K_List_SIZEOF + count * M68K_CyberModeNode_SIZEOF);
            if (!guest) { FreeCModeList(native); r->d[0] = 0; return 0; }
            glist = guest;
            memset(gptr(guest0, guest), 0,
                   M68K_List_SIZEOF + count * M68K_CyberModeNode_SIZEOF);
            node = (struct CyberModeNode *)native->lh_Head;
            for (ULONG i = 0; i < count; i++, node =
                 (struct CyberModeNode *)node->Node.ln_Succ)
            {
                ULONG gn = guest + M68K_List_SIZEOF +
                           i * M68K_CyberModeNode_SIZEOF;
                ULONG prev = i ? gn - M68K_CyberModeNode_SIZEOF :
                                   glist + M68K_List_lh_Head;
                ULONG next = i + 1u < count ? gn + M68K_CyberModeNode_SIZEOF :
                                              glist + M68K_List_lh_Tail;
                gw32(guest0, gn + M68K_CyberModeNode_Node_ln_Succ, next);
                gw32(guest0, gn + M68K_CyberModeNode_Node_ln_Pred, prev);
                gw32(guest0, gn + M68K_CyberModeNode_Node_ln_Name,
                     gn + M68K_CyberModeNode_ModeText);
                memcpy(gptr(guest0, gn + M68K_CyberModeNode_ModeText),
                       node->ModeText, DISPLAYNAMELEN);
                gw32(guest0, gn + M68K_CyberModeNode_DisplayID, node->DisplayID);
                emu68k_scalar_to_guest(guest0, gn + M68K_CyberModeNode_Width,
                                       2, node->Width);
                emu68k_scalar_to_guest(guest0, gn + M68K_CyberModeNode_Height,
                                       2, node->Height);
                emu68k_scalar_to_guest(guest0, gn + M68K_CyberModeNode_Depth,
                                       2, node->Depth);
            }
            gw32(guest0, glist + M68K_List_lh_Head,
                 count ? guest + M68K_List_SIZEOF : glist + M68K_List_lh_Tail);
            gw32(guest0, glist + M68K_List_lh_Tail, 0);
            gw32(guest0, glist + M68K_List_lh_TailPred,
                 count ? guest + M68K_List_SIZEOF +
                         (count - 1u) * M68K_CyberModeNode_SIZEOF :
                         glist + M68K_List_lh_Head);
            for (slot = 0; slot < 8; slot++) if (!rs->cmodes[slot].native) break;
            if (slot == 8) { FreeCModeList(native); return 1; }
            rs->cmodes[slot].native = native; rs->cmodes[slot].guest = glist;
            r->d[0] = glist;
            return 0;
        }

        if (lvo == CYBERGRAPHICS_LVO_FREECMODELIST)
        {
            for (int i = 0; i < 8; i++) if (rs->cmodes[i].guest == r->a[0])
            {
                FreeCModeList(rs->cmodes[i].native);
                rs->cmodes[i].native = NULL; rs->cmodes[i].guest = 0;
                break;
            }
            return 0;
        }

        if (lvo == CYBERGRAPHICS_LVO_LOCKBITMAPTAGLIST)
        {
            struct Emu68kObject *bo = object_by_token(rs, r->a[0]);
            ULONG width = 0, height = 0, depth = 0, pixfmt = 0;
            ULONG bpp = 0, bpr = 0;
            APTR base = NULL, handle;
            struct TagItem tags[] = {
                {LBMI_WIDTH, (IPTR)&width}, {LBMI_HEIGHT, (IPTR)&height},
                {LBMI_DEPTH, (IPTR)&depth}, {LBMI_PIXFMT, (IPTR)&pixfmt},
                {LBMI_BYTESPERPIX, (IPTR)&bpp}, {LBMI_BYTESPERROW, (IPTR)&bpr},
                {LBMI_BASEADDRESS, (IPTR)&base}, {TAG_DONE, 0}
            };
            ULONG size, guest_buffer, guest_handle;
            int slot;
            if (!CyberGfxBase || !bo || bo->type != EMU_OBJ_BitMap ||
                !rs->guest_alloc) return 1;
            handle = LockBitMapTagList(bo->native, tags);
            if (!handle || !base || !height ||
                (UQUAD)bpr * height > 128u * 1024u * 1024u)
            { r->d[0] = 0; return 0; }
            size = bpr * height;
            guest_buffer = rs->guest_alloc(rs->run, size);
            guest_handle = rs->guest_alloc(rs->run, 4);
            if (!guest_buffer || !guest_handle)
            { UnLockBitMap(handle); r->d[0] = 0; return 0; }
            memcpy(gptr(guest0, guest_buffer), base, size);
            for (slot = 0; slot < 16; slot++) if (!rs->cyberlock[slot].handle) break;
            if (slot == 16) { UnLockBitMap(handle); return 1; }
            rs->cyberlock[slot].handle = handle;
            rs->cyberlock[slot].native_base = base;
            rs->cyberlock[slot].guest_handle = guest_handle;
            rs->cyberlock[slot].guest_buffer = guest_buffer;
            rs->cyberlock[slot].size = size;
            if (r->a[1])
            {
                ULONG p = r->a[1];
                for (unsigned guard = 0; guard < 64; guard++, p += 8)
                {
                    ULONG tag, dst, value = 0;
                    if (emu68k_require_guest_range(p, 8, "LockBitMap tags",
                                                   err, errlen) < 0) return 1;
                    tag = gr32(guest0, p); dst = gr32(guest0, p + 4);
                    if (!tag) break;
                    switch (tag) {
                    case LBMI_WIDTH: value = width; break;
                    case LBMI_HEIGHT: value = height; break;
                    case LBMI_DEPTH: value = depth; break;
                    case LBMI_PIXFMT: value = pixfmt; break;
                    case LBMI_BYTESPERPIX: value = bpp; break;
                    case LBMI_BYTESPERROW: value = bpr; break;
                    case LBMI_BASEADDRESS: value = guest_buffer; break;
                    default: continue;
                    }
                    if (emu68k_require_guest_range(dst, 4, "LockBitMap result",
                                                   err, errlen) < 0) return 1;
                    gw32(guest0, dst, value);
                }
            }
            r->d[0] = guest_handle;
            return 0;
        }

        if (lvo == CYBERGRAPHICS_LVO_UNLOCKBITMAP ||
            lvo == CYBERGRAPHICS_LVO_UNLOCKBITMAPTAGLIST)
        {
            for (int i = 0; i < 16; i++)
                if (rs->cyberlock[i].handle &&
                    rs->cyberlock[i].guest_handle == r->a[0])
                {
                    memcpy(rs->cyberlock[i].native_base,
                           gptr(guest0, rs->cyberlock[i].guest_buffer),
                           rs->cyberlock[i].size);
                    UnLockBitMap(rs->cyberlock[i].handle);
                    memset(&rs->cyberlock[i], 0, sizeof rs->cyberlock[i]);
                    return 0;
                }
            return 0;
        }

        if (lvo == CYBERGRAPHICS_LVO_DOCDRAWMETHODTAGLIST)
        {
            struct Emu68kObject *rpo = object_by_token(rs, r->a[1]);
            struct RastPort *rp;
            ULONG width = 0, height = 0, pixfmt = 0, bpp = 0, bpr = 0;
            APTR base = NULL, handle;
            struct TagItem tags[] = {
                {LBMI_WIDTH, (IPTR)&width}, {LBMI_HEIGHT, (IPTR)&height},
                {LBMI_PIXFMT, (IPTR)&pixfmt}, {LBMI_BYTESPERPIX, (IPTR)&bpp},
                {LBMI_BYTESPERROW, (IPTR)&bpr}, {LBMI_BASEADDRESS, (IPTR)&base},
                {TAG_DONE, 0}
            };
            ULONG size, buffer, msg, entry;
            unsigned result = 0;
            if (!CyberGfxBase || !rpo || rpo->type != EMU_OBJ_RastPort ||
                !rs->guest_alloc || !rs->call_hook ||
                emu68k_require_guest_range(r->a[0], M68K_Hook_SIZEOF,
                                           "CDraw Hook", err, errlen) < 0)
                return 1;
            rp = rpo->native;
            handle = LockBitMapTagList(rp->BitMap, tags);
            if (!handle || !base || (UQUAD)bpr * height > 128u * 1024u * 1024u)
                return 0;
            size = bpr * height;
            buffer = rs->guest_alloc(rs->run, size);
            msg = rs->guest_alloc(rs->run, M68K_CDrawMsg_SIZEOF);
            if (!buffer || !msg) { UnLockBitMap(handle); return 1; }
            memcpy(gptr(guest0, buffer), base, size);
            memset(gptr(guest0, msg), 0, M68K_CDrawMsg_SIZEOF);
            gw32(guest0, msg + M68K_CDrawMsg_cdm_MemPtr, buffer);
            gw32(guest0, msg + M68K_CDrawMsg_cdm_xsize, width);
            gw32(guest0, msg + M68K_CDrawMsg_cdm_ysize, height);
            emu68k_scalar_to_guest(guest0, msg + M68K_CDrawMsg_cdm_BytesPerRow,
                                   2, bpr);
            emu68k_scalar_to_guest(guest0, msg + M68K_CDrawMsg_cdm_BytesPerPix,
                                   2, bpp);
            emu68k_scalar_to_guest(guest0, msg + M68K_CDrawMsg_cdm_ColorModel,
                                   2, pixfmt);
            entry = gr32(guest0, r->a[0] + M68K_Hook_h_Entry);
            if (rs->call_hook(rs->run, entry, r->a[0], r->a[1], msg,
                              &result, err, errlen) != 0)
            { UnLockBitMap(handle); return 1; }
            memcpy(base, gptr(guest0, buffer), size);
            UnLockBitMap(handle);
            return 0;
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
    if (strcmp(libname, "graphics.library") == 0 &&
        (lvo == GRAPHICS_LVO_CLOSEFONT ||
         lvo == GRAPHICS_LVO_REMFONT ||
         lvo == GRAPHICS_LVO_EXTENDFONT ||
         lvo == GRAPHICS_LVO_STRIPFONT) &&
        textfont_extension_from_guest(rs, guest0,
            (lvo == GRAPHICS_LVO_CLOSEFONT || lvo == GRAPHICS_LVO_REMFONT)
                ? r->a[1] : r->a[0], err, errlen) < 0)
        return 1;
    if (err && errlen) err[0] = '\0';
    if (gen_dispatch(libname, lvo, r, guest0, DOSBase, err, errlen) == 0)
    {
        if (strcmp(libname, "intuition.library") == 0 &&
            (lvo == INTUITION_LVO_OPENWINDOW ||
             lvo == INTUITION_LVO_OPENWINDOWTAGLIST) && r->d[0])
            idcmp_bind_window(rs, guest0, r->d[0]);
        /* The activation edge fires natively during OpenWindow, whole quanta
         * before the guest can run ModifyIDCMP; on hardware the program wins
         * that race because activation is input.device-asynchronous. When
         * ModifyIDCMP newly enables IDCMP_ACTIVEWINDOW on the window that IS
         * active and this activation epoch delivered none, replay the one
         * edge the program was written to receive. */
        if (strcmp(libname, "intuition.library") == 0 &&
            lvo == INT_LVO_MODIFYIDCMP &&
            (modify_flags_new & IDCMP_ACTIVEWINDOW) &&
            !(modify_flags_old & IDCMP_ACTIVEWINDOW))
        {
            struct IntuitionBase *IntuitionBase = (struct IntuitionBase *)
                gen_base_for("intuition.library", NULL);
            struct Emu68kObject *w = object_by_token(rs, modify_window_token);
            if (IntuitionBase && w && w->type == EMU_OBJ_Window &&
                IntuitionBase->ActiveWindow == (struct Window *)w->native)
            {
                int i;
                for (i = 0; i < EMU68K_MAX_IDCMP; i++)
                    if (rs->idcmp[i].window == w->native &&
                        !rs->idcmp[i].active_seen)
                        rs->idcmp[i].synth_classes |= IDCMP_ACTIVEWINDOW;
            }
        }
        if (strcmp(libname, "graphics.library") == 0 &&
            ((lvo == GRAPHICS_LVO_OPENFONT && r->d[0]) ||
             (lvo == GRAPHICS_LVO_EXTENDFONT && r->d[0]) ||
             lvo == GRAPHICS_LVO_STRIPFONT) &&
            textfont_extension_to_guest(rs, guest0,
                lvo == GRAPHICS_LVO_OPENFONT ? r->d[0] : r->a[0],
                err, errlen) < 0)
            return 1;
        return 0;
    }

    /* A policy-compiled crossing can fail more precisely than "unknown LVO"
     * (unknown tag, refused object type, invalid guest range). Keep that
     * reason all the way back to the translated program's diagnostic. */
    if (err && errlen && err[0])
        return 1;

    /* Anything else is a capability gap, reported by name so the ledger says
     * exactly what to implement next. Never a guess. */
    if (err && errlen)
        snprintf(err, errlen, "%s LVO %d", libname, lvo);
    return 1;
}
