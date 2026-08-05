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

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include "emu68k_intern.h"
#include "emu68k_gen.h"
#include "emu68k_layouts.h"
#include LC_LIBDEFS_FILE

#include <aros/debug.h>

#include "emu68k_guest_offsets.h"
#include <aros/asmcall.h>
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
#define DOS_LVO_FILEPART     145   /* -870 */
#define DOS_LVO_PATHPART     146   /* -876 */
#define DOS_LVO_MATCHFIRST   137   /* -822 */
#define DOS_LVO_MATCHNEXT    138   /* -828 */
#define DOS_LVO_MATCHEND     139   /* -834 */
#define DOS_LVO_IOERR   22    /* -132: what every failed dos call is followed by */
#define DOS_LVO_GETPROGRAMNAME 96   /* -576 */
#define DOS_LVO_GETVAR        151   /* -906 */
#define DOS_LVO_SETVAR        152   /* -912 */
#define DOS_LVO_PRINTFAULT 79 /* -474 */
#define DOS_LVO_SETIOERR   77 /* -462 */
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

/* Say what a requester ASKED before answering it.
 *
 * The answer given below is "no", which is the only safe reply when there is
 * nobody to ask, but a program that then tidies up and exits looks identical to
 * one that failed for no reason. The question is the difference, so it is
 * reported. struct EasyStruct is read at its AmigaOS offsets: it is not in the
 * AROS headers to derive a layout from, and the guest's copy is the one being
 * described anyway.
 *
 * The format strings are shown as the program wrote them. Substituting the
 * argument list would mean widening a 32-bit RAWARG array to native varargs
 * against the format, which is real work and belongs with the decision to
 * display requesters for real. */
#define EASY_TITLE   8
#define EASY_TEXT    12
#define EASY_GADGETS 16
#define EASY_SIZEOF  20

static void report_easyrequest(APTR guest0, ULONG easy)
{
    const char *title, *text, *gadgets;

    if (emu68k_require_guest_range(easy, EASY_SIZEOF, "EasyStruct", NULL, 0) < 0)
        return;
    title   = guest_cstr(guest0, gr32(guest0, easy + EASY_TITLE), 256);
    text    = guest_cstr(guest0, gr32(guest0, easy + EASY_TEXT), 1024);
    gadgets = guest_cstr(guest0, gr32(guest0, easy + EASY_GADGETS), 256);

    bug("[emu68k] requester answered no: \"%s\" / \"%s\" [%s]\n",
        title ? title : "", text ? text : "", gadgets ? gadgets : "");
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

struct Emu68kObject
{
    APTR native;
    APTR base;
    EmuObjectCleanup cleanup;
    ULONG token;
    ULONG refs;
    UWORD type;
    UWORD flags;
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
#define EXEC_LVO_PUMP     9001   /* private: "drain the port bound to this one" */
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
    /* Intuition's own messages, paired with the guest copies handed out, so a
     * reply reaches the message Intuition is waiting to get back. */
    struct { APTR native; ULONG guest; } imsg[EMU68K_MAX_IMSG];
    /* Which guest port a window's IDCMP is delivered to. Recorded when the
     * program says so, never guessed: several windows commonly share one. */
    struct { APTR window; ULONG guest_port; } idcmp[EMU68K_MAX_IDCMP];
    /* Devices opened for the guest. The native IORequest is OURS: the guest's
     * is big-endian and 32-bit and can never be handed to a device. */
    struct { struct IORequest *req; ULONG guest_req; char name[32]; } dev[EMU68K_MAX_DEV];
    ULONG (*device_base)(emu68k_run_h r, const char *name);
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

AROS_UFH3(static IPTR, emu68k_native_boopsi_entry,
          AROS_UFHA(Class *, cl, A0),
          AROS_UFHA(Object *, object, A2),
          AROS_UFHA(Msg, message, A1))
{
    AROS_USERFUNC_INIT

    struct Emu68kBoopsiBridge *bridge = cl ? cl->cl_Dispatcher.h_Data : NULL;
    struct Emu68kRunState *rs = bridge ? bridge->state : NULL;
    unsigned int result = 0;
    ULONG guest_message, guest_object, method;
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
    guest_message = rs->guest_alloc(rs->run,
                                    method == OM_NEW ? 12 :
                                    method == OM_GET ? 16 : 4);
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
        p[12] = p[13] = p[14] = p[15] = 0;
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
        /* Guest attribute values are guest-meaningful (scalars, or guest
         * addresses the querying guest reads back through GetAttr); the
         * native caller gets the raw 32-bit value. */
        struct opGet *og = (struct opGet *)message;
        if (og->opg_Storage)
            *og->opg_Storage = (IPTR)(ULONG)((p[12] << 24) | (p[13] << 16) |
                                             (p[14] << 8) | p[15]);
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

    if (!bridge) return 0;
    cl = bridge->native_class;
    if (cl) cl->cl_Dispatcher = bridge->saved_dispatcher;
    if (!bridge->failed) return 0;
    if (err && errlen)
        snprintf(err, errlen, "68k BOOPSI callback failed: %s", bridge->error);
    return -1;
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

static void emu68k_mirror_cleanup(APTR base, APTR object)
{
    (void)base;
    FreeVec(object);
}

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

/* Every byte of the guest structure this crossing does NOT carry must still be
 * zero. A render Image, a label, a SpecialInfo is a guest pointer with no
 * native meaning; dropping it quietly would draw nothing and blame nobody. */
static LONG mirror_check_cover(APTR guest0, ULONG addr, const struct EmuMirror *m,
                               ULONG span, const char *type_name,
                               char *err, ULONG errlen)
{
    ULONG b;
    for (b = 0; b < span; b++)
    {
        int covered = (m->guest_link >= 0 && (LONG)b >= m->guest_link &&
                       (LONG)b < m->guest_link + 4);
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
            snprintf(err, errlen, "capability gap: %s at %08lx sets byte %lu, "
                     "which this mirror cannot carry", type_name,
                     (unsigned long)addr, (unsigned long)b);
        return -1;
    }
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

LONG emu68k_object_adopt_guest(APTR guest0, ULONG addr, UWORD type,
                               const char *type_name,
                               const struct EmuMirror *m,
                               APTR *native, char *err, ULONG errlen)
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
        if (o && !(o->flags & EMU68K_OBJ_GUEST_OWNED))
        {
            /* At the head this is not adoption at all: the program is passing
             * back an object the bridge issued, which resolves normally. Deeper
             * in the chain it is a family that mixes the two, and their tokens
             * do not mean the same thing - only a mirror's is guest memory. */
            if (count == 0 && o->type == type)
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
        if (mirror_check_cover(guest0, walk, m, span, type_name, err, errlen) < 0)
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
            mirror = AllocVec(m->native_size, MEMF_CLEAR);
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
        }

        emu68k_from_guest_sized(guest0, walk, mirror, m->fields, m->field_count,
                                mirror_guest_size(guest0, walk, m));
        if (m->native_link >= 0)
            *(APTR *)((UBYTE *)mirror + m->native_link) = NULL;
        if (prev && m->native_link >= 0)
            *(APTR *)((UBYTE *)prev + m->native_link) = mirror;
        if (!head) head = mirror;
        prev = mirror;

        if (m->guest_link < 0) break;
        walk = gr32(guest0, walk + m->guest_link);
    }

    mirror_commit(rs);
    if (native) *native = head;
    return 0;
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
        if (m->guest_link >= 0)
            emu68k_scalar_to_guest(guest0, self->token + m->guest_link, 4,
                                   next_token);
        node = next;
    }
    return 0;
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
    if (token) *token = rs->objects[free_slot].token;
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
    return 0;
}

void emu68k_object_release(APTR guest0, ULONG token, UWORD type)
{
    struct Emu68kObject *o = object_by_token(run_state(guest0), token);
    if (!o || o->type != type) return;
    if (o->refs > 1)
        o->refs--;
    else
        memset(o, 0, sizeof *o);
}

void emu68k_object_consume(APTR guest0, ULONG token, UWORD type)
{
    struct Emu68kObject *o = object_by_token(run_state(guest0), token);
    if (!o || o->type != type) return;
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
            memset(&rs->objects[i], 0, sizeof rs->objects[i]);
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

void emu68k_to_guest(APTR guest0, ULONG gbase, const void *native,
                     const struct EmuField *f, int n);
void emu68k_from_guest(APTR guest0, ULONG gbase, void *native,
                       const struct EmuField *f, int n);

#define EMU_NFIELDS(t) ((int)(sizeof (t) / sizeof (t)[0]))

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
    ULONG gcls = r->a[2];                 /* OM_NEW convention: o is the cl  */
    ULONG stok, method, gtags, inst_end, token = 0;
    APTR super = NULL, nobj;
    struct TagItem ntags[71];
    UQUAD scratch[192];

    if (emu68k_require_guest_range(stub, M68K_IClass_SIZEOF, "super stub",
                                   err, errlen) < 0 ||
        emu68k_require_guest_range(msg, 12, "super message", err, errlen) < 0)
        return 1;
    stok   = gr32(guest0, stub + M68K_IClass_cl_Dispatcher_h_Data);
    method = gr32(guest0, msg);
    bug("[emu68k/boopsi] super method=%lx stub=%lx stok=%lx gcls=%lx\n",
        (unsigned long)method, (unsigned long)stub, (unsigned long)stok,
        (unsigned long)gcls);
    if (method != OM_NEW)
    {
        if (err && errlen)
            snprintf(err, errlen, "capability gap: super method %08lx "
                     "unserved (only OM_NEW crosses)", (unsigned long)method);
        return 1;
    }
    if (emu68k_object_from_guest(guest0, stok, EMU_OBJ_Class, 0, "Class",
                                 &super, err, errlen) < 0)
        return 1;
    gtags = gr32(guest0, msg + 4);
    if (emu68k_tags_to_native(guest0, gtags, emu68k_domain_intuition_new_object,
                              ntags, 71, scratch, sizeof scratch,
                              err, errlen) < 0)
        return 1;
    nobj = NewObjectA((struct IClass *)super, NULL, gtags ? ntags : NULL);
    if (!nobj)
    {
        r->d[0] = 0;
        return 0;
    }
    /* The annex bound comes from the guest class itself; a class that lies
     * about its instance size only corrupts its own annex. */
    inst_end = 64;
    if (emu68k_require_guest_range(gcls, M68K_IClass_SIZEOF, "class",
                                   NULL, 0) >= 0)
    {
        ULONG off = emu68k_scalar_from_guest(guest0,
                        gcls + M68K_IClass_cl_InstOffset, 2);
        ULONG size = emu68k_scalar_from_guest(guest0,
                        gcls + M68K_IClass_cl_InstSize, 2);
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

    /* ---- IDCMP DELIVERY -----------------------------------------------------
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
     * Only ports BOUND to a window are pumped. A worker process's own
     * pr_MsgPort is an ordinary mailbox for the messages its dispatcher sends
     * it, and native input must never be broadcast into one. */
    if (strcmp(libname, "exec.library") == 0 && lvo == EXEC_LVO_PUMP)
    {
        ULONG guest_port = r->a[0];
        struct MsgPort *native_port = NULL;
        ULONG delivered = 0;
        int i;

        r->d[0] = 0;
        for (i = 0; i < EMU68K_MAX_IDCMP; i++)
            if (rs->idcmp[i].guest_port == guest_port && rs->idcmp[i].window)
            {
                native_port = ((struct Window *)rs->idcmp[i].window)->UserPort;
                break;
            }
        if (!native_port)
        {
            struct Emu68kObject *o = object_by_token(rs, guest_port);
            if (o && o->type == EMU_OBJ_MsgPort)
                native_port = (struct MsgPort *)o->native;
        }
        if (!native_port) return 0;          /* an ordinary guest mailbox     */

        for (;;)
        {
            struct IntuiMessage *im;
            ULONG guest_msg, list, tailpred, task;
            int slot;

            im = (struct IntuiMessage *)GetMsg(native_port);
            if (!im) break;
            for (slot = 0; slot < EMU68K_MAX_IMSG; slot++)
                if (!rs->imsg[slot].native) break;
            if (slot == EMU68K_MAX_IMSG || !rs->guest_alloc ||
                !(guest_msg = rs->guest_alloc(rs->run, M68K_IntuiMessage_SIZEOF)))
            {
                ReplyMsg((struct Message *)im);  /* never strand Intuition's  */
                break;
            }
            memset((UBYTE *)guest0 + guest_msg, 0, M68K_IntuiMessage_SIZEOF);
            emu68k_to_guest(guest0, guest_msg, im, emu_fields_IntuiMessage,
                            EMU_NFIELDS(emu_fields_IntuiMessage));
            rs->imsg[slot].native = im;
            rs->imsg[slot].guest  = guest_msg;

            /* AddTail on the guest port's own list, byte for byte as exec
             * leaves it, so the program's GetMsg is the ordinary path. */
            list = guest_port + M68K_MsgPort_mp_MsgList_lh_Head;
            tailpred = gr32(guest0, list + M68K_List_lh_TailPred);
            gw32(guest0, guest_msg, list + M68K_List_lh_Tail);
            gw32(guest0, guest_msg + 4, tailpred);
            gw32(guest0, tailpred, guest_msg);
            gw32(guest0, list + M68K_List_lh_TailPred, guest_msg);
            delivered++;

            task = gr32(guest0, guest_port + M68K_MsgPort_mp_SigTask);
            {
                ULONG bit = *((UBYTE *)guest0 + guest_port +
                              M68K_MsgPort_mp_SigBit);
                if (task && bit < 32)
                    gw32(guest0, task + M68K_Task_tc_SigRecvd,
                         gr32(guest0, task + M68K_Task_tc_SigRecvd) | (1u << bit));
            }
        }
        r->d[0] = delivered;
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
                ReplyMsg((struct Message *)rs->imsg[i].native);
                rs->imsg[i].native = NULL;
                rs->imsg[i].guest = 0;
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
        {
            ULONG port = gr32(guest0, w->token + M68K_Window_UserPort);
            int i, free_slot = -1;
            for (i = 0; i < EMU68K_MAX_IDCMP; i++)
            {
                if (rs->idcmp[i].window == w->native)
                { rs->idcmp[i].guest_port = port; free_slot = -2; break; }
                if (!rs->idcmp[i].window && free_slot < 0) free_slot = i;
            }
            if (free_slot >= 0)
            {
                rs->idcmp[free_slot].window = w->native;
                rs->idcmp[free_slot].guest_port = port;
            }
        }
        /* deliberately no return: the crossing itself still has to run */
    }

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

    if (strcmp(libname, "intuition.library") == 0)
    {
        /* The requesters. Both ASK THE USER, and a 68k program driven from a
         * script or headlessly has nobody to ask - which is the same situation
         * pr_WindowPtr = -1 already handles for dos, so the answer is the same:
         * do not display anything, and give the negative reply.
         *
         * AutoRequest returns FALSE for the negative gadget; EasyRequestArgs
         * returns 0, which is its rightmost gadget and conventionally Cancel.
         * A program that asks "proceed?" therefore stops, which is the safe
         * reading of no answer - it does not silently proceed on the user's
         * behalf. Showing the text would mean converting IntuiText/EasyStruct
         * and formatting a RAWARG list, which is separate work.
         */
        if (lvo == 58) { r->d[0] = DOSFALSE; return 0; }   /* AutoRequest      */
        if (lvo == 98)                                     /* EasyRequestArgs  */
        {
            report_easyrequest(guest0, r->a[1]);
            r->d[0] = 0;
            return 0;
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
         * bitmaps do have driver-owned semantics and deliberately remain
         * capability gaps. */
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
                if (err && errlen)
                    snprintf(err, errlen,
                             "AllocBitMap needs native display/RTG/friend semantics "
                             "(depth=%lu flags=%08lx friend=%08lx)",
                             depth, flags, r->a[0]);
                return 1;
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
            int i;
            if (!r->a[0])
            {
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
    if (err && errlen) err[0] = '\0';
    if (gen_dispatch(libname, lvo, r, guest0, DOSBase, err, errlen) == 0)
        return 0;

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
