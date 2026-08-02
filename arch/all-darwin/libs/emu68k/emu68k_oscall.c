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
#include "emu68k_layouts.h"
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
#define DOS_LVO_EXAMINE       17   /* -102 */
#define DOS_LVO_EXNEXT        18   /* -108 */
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
#define EMU68K_MAX_SCANS   4
#define EMU68K_MAX_RUNS    4

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
struct Emu68kRunState
{
    APTR  guest0;                                    /* NULL = a free slot     */
    struct { BPTR bptr; } handles[EMU68K_MAX_HANDLES];
    struct { ULONG guest; struct AnchorPath *nap; } scans[EMU68K_MAX_SCANS];
};

static struct Emu68kRunState g_runs[EMU68K_MAX_RUNS];

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

/* ---- GUEST-SIDE STRUCTURE WRITES ------------------------------------------
 * Guest memory is BIG-ENDIAN with 32-bit fields; this side is little-endian
 * with 64-bit ones. So a structure is never copied, it is rebuilt a field at a
 * time, and every offset comes from emu68k_layouts.h - generated from the AROS
 * headers for both targets, never counted by hand. */
static void gw8(APTR guest0, ULONG addr, UBYTE v)
{
    ((UBYTE *)guest0)[addr] = v;
}

static void gw16(APTR guest0, ULONG addr, UWORD v)
{
    UBYTE *p = (UBYTE *)guest0 + addr;
    p[0] = (UBYTE)(v >> 8); p[1] = (UBYTE)v;
}

static void gw32(APTR guest0, ULONG addr, ULONG v)
{
    UBYTE *p = (UBYTE *)guest0 + addr;
    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v;
}

static void gwbytes(APTR guest0, ULONG addr, const UBYTE *src, ULONG n)
{
    CopyMem((APTR)src, (UBYTE *)guest0 + addr, n);
}

/* A native FileInfoBlock, as the guest's AmigaOS one. 14 of its 15 fields sit
 * at a different offset on the two sides, so this is the whole conversion. */
#define FIB_F(f)  (base + M68K_FileInfoBlock_##f)
static void fib_to_guest(APTR guest0, ULONG base, const struct FileInfoBlock *n)
{
    gw32(guest0, FIB_F(fib_DiskKey),      (ULONG)n->fib_DiskKey);
    gw32(guest0, FIB_F(fib_DirEntryType), (ULONG)n->fib_DirEntryType);
    gwbytes(guest0, FIB_F(fib_FileName),  (const UBYTE *)n->fib_FileName,
            sizeof n->fib_FileName > 108 ? 108 : sizeof n->fib_FileName);
    gw32(guest0, FIB_F(fib_Protection),   (ULONG)n->fib_Protection);
    gw32(guest0, FIB_F(fib_EntryType),    (ULONG)n->fib_EntryType);
    gw32(guest0, FIB_F(fib_Size),         (ULONG)n->fib_Size);
    gw32(guest0, FIB_F(fib_NumBlocks),    (ULONG)n->fib_NumBlocks);
    gw32(guest0, FIB_F(ds_Days),          (ULONG)n->fib_Date.ds_Days);
    gw32(guest0, FIB_F(ds_Minute),        (ULONG)n->fib_Date.ds_Minute);
    gw32(guest0, FIB_F(ds_Tick),          (ULONG)n->fib_Date.ds_Tick);
    gwbytes(guest0, FIB_F(fib_Comment),   (const UBYTE *)n->fib_Comment,
            sizeof n->fib_Comment > 80 ? 80 : sizeof n->fib_Comment);
    gw16(guest0, FIB_F(fib_OwnerUID),     (UWORD)n->fib_OwnerUID);
    gw16(guest0, FIB_F(fib_OwnerGID),     (UWORD)n->fib_OwnerGID);
    (void)gw8;
}

/* ---- ANCHORPATH: A RETAINED SHADOW ----------------------------------------
 * MatchFirst/MatchNext/MatchEnd are not three independent calls: the
 * AnchorPath carries the live state of a directory scan between them,
 * including a chain of AChain structures dos.library allocated. Those are
 * NATIVE pointers, so the guest can never be shown them and the structure
 * cannot be rebuilt per call the way a FileInfoBlock is.
 *
 * So the native AnchorPath is kept HERE for the life of the scan, keyed by the
 * guest's own AnchorPath address, and only the fields the program reads travel
 * back. This is the shadow pattern the design calls for whenever a callee
 * retains a pointer.
 *
 * Small and fixed: a guest running more scans at once than this is not a case
 * being served yet, and it fails cleanly rather than corrupting one. */
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

/* Copy back what the program reads after a match: the entry it found, the
 * assembled path, and the flag/break bytes. */
static void ap_to_guest(APTR guest0, ULONG gap, const struct AnchorPath *n,
                        UWORD strlen_)
{
    fib_to_guest(guest0, gap + M68K_AnchorPath_ap_Info, &n->ap_Info);
    gw8(guest0,  gap + M68K_AnchorPath_ap_Flags,      (UBYTE)n->ap_Flags);
    gw32(guest0, gap + M68K_AnchorPath_ap_FoundBreak, (ULONG)n->ap_FoundBreak);
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
        for (int j = 0; j < EMU68K_MAX_SCANS; j++)
            if (rs->scans[j].nap)
            {
                MatchEnd(rs->scans[j].nap);
                FreeVec(rs->scans[j].nap);
            }
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
    APTR DOSBase = user;
    struct Emu68kRunState *rs = run_state(guest0);

    if (!rs)
    {
        if (err && errlen)
            snprintf(err, errlen, "more 68k programs running at once than this "
                     "bridge keeps state for");
        return 1;
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

        /* [T3b] Examine/ExNext: call the NATIVE one into a NATIVE
         * FileInfoBlock and copy the fields back into the guest's own. The
         * guest's fib is 260 bytes in AmigaOS layout and this side's is 264 in
         * another; handing the guest pointer to dos.library directly would let
         * it write native-shaped fields into a guest-shaped structure. */
        case DOS_LVO_EXAMINE:
        case DOS_LVO_EXNEXT:
        {
            struct FileInfoBlock *nfib = AllocDosObject(DOS_FIB, NULL);
            ULONG gfib = r->d[2];
            if (!nfib) { r->d[0] = DOSFALSE; return 0; }
            /* ExNext continues a scan the guest started, so the position it is
             * resuming from lives in the guest's fib: carry it over. */
            if (lvo == DOS_LVO_EXNEXT)
            {
                const UBYTE *p = (const UBYTE *)guest0 + gfib
                               + M68K_FileInfoBlock_fib_DiskKey;
                nfib->fib_DiskKey = (IPTR)(((ULONG)p[0] << 24) |
                                           ((ULONG)p[1] << 16) |
                                           ((ULONG)p[2] << 8) | p[3]);
            }
            r->d[0] = (ULONG)((lvo == DOS_LVO_EXAMINE)
                              ? Examine(handle_bptr(rs, r->d[1]), nfib)
                              : ExNext(handle_bptr(rs, r->d[1]), nfib));
            if (r->d[0])
                fib_to_guest(guest0, gfib, nfib);
            FreeDosObject(DOS_FIB, nfib);
            return 0;
        }

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
