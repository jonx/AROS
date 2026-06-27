/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Clipboard bridge for the darwin Cocoa HIDD — keeps the AROS
          clipboard.device PRIMARY_CLIP in sync with the macOS NSPasteboard,
          both ways, via the host shim libpasteboard.dylib (hostlib.resource).

    Clean-room from docs/features/clipboard-bridge/spec.md and the in-tree FTXT
    wrap/unwrap shape of developer/debug/test/misc/hostcb.c (APL/LGPL, ours).
    No GPL emulator/agent source (WinUAE/FS-UAE/Amiberry/E-UAE/vdagent) was read.

    Design (spec scheme B, polling variant — robust under the threaded host
    scheduler): one low-priority task polls both sides on a Delay() tick. It does
    NOT take a cross-thread Signal from a host pthread (the fragile path) — it
    reads host_pb_change_count() and the AROS CBD_CURRENTWRITEID itself, and
    propagates whichever side changed. Encoding crosses the wall as the host
    shim's own helpers: FTXT CHRS bytes are ISO-8859-1, NSPasteboard is UTF-8.

    R-LOOPBREAK: each cross-write is the OTHER end's next "change", so the task
    records the changeCount its own host write produced (ourHostWrite) and the
    write-id its own AROS write produced (ourArosWrite), and never re-propagates
    a change equal to its own token. One direction per tick (host checked first).
*/

#define DEBUG 1

#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <devices/clipboard.h>
#include <libraries/iffparse.h>
#include <datatypes/textclass.h>      /* ID_FTXT, ID_CHRS */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/iffparse.h>
#include <proto/hostlib.h>

#include "cocoa_intern.h"
#include <aros/debug.h>

#define HostLibBase (xsd.hostlib)

/* HostLib_GetInterface resolves these in order into struct PBInterface. */
static const char *const pb_symbols[] =
{
    "host_pb_get_text",
    "host_pb_set_text",
    "host_pb_change_count",
    "host_pb_free",
    "host_latin1_to_utf8",
    "host_utf8_to_latin1",
    NULL
};

/* Task-local AROS-side handles. IFFParseBase is the library base the proto/iffparse
   inline stubs reference (declared extern there) — define it here (non-static). */
struct Library         *IFFParseBase;
static struct MsgPort  *clipport;
static struct IOClipReq *clipio;        /* raw PRIMARY_CLIP unit, for CBD_CURRENTWRITEID */

/* ---- host calls: each wrapped in the Forbid()+HostLib_Lock() discipline so the
   preemptive scheduler can't switch a task mid-host-syscall (the "out of stack"
   hazard). Memory the host returns is read between calls without the lock — the
   lock serialises CALLS, not the shared address space. ---------------------- */

static long pb_change_count(void)
{
    long r;
    Forbid(); HostLib_Lock();
    r = xsd.pb->host_pb_change_count();
    AROS_HOST_BARRIER
    HostLib_Unlock(); Permit();
    return r;
}

static char *pb_get_text(unsigned long *len)   /* UTF-8, or NULL if no text */
{
    char *out = NULL; unsigned long l = 0;
    Forbid(); HostLib_Lock();
    (void)xsd.pb->host_pb_get_text(&out, &l);
    AROS_HOST_BARRIER
    HostLib_Unlock(); Permit();
    if (len) *len = l;
    return out;
}

static long pb_set_text(const char *utf8, unsigned long len)   /* returns its changeCount */
{
    long r;
    Forbid(); HostLib_Lock();
    r = xsd.pb->host_pb_set_text(utf8, len);
    AROS_HOST_BARRIER
    HostLib_Unlock(); Permit();
    return r;
}

static char *pb_l2u(const UBYTE *latin1, unsigned long len, unsigned long *outlen)
{
    char *out = NULL; unsigned long l = 0; int rc;
    Forbid(); HostLib_Lock();
    rc = xsd.pb->host_latin1_to_utf8(latin1, len, &out, &l);
    AROS_HOST_BARRIER
    HostLib_Unlock(); Permit();
    if (rc != 0) return NULL;
    if (outlen) *outlen = l;
    return out;
}

static UBYTE *pb_u2l(const char *utf8, unsigned long len, unsigned long *outlen)
{
    unsigned char *out = NULL; unsigned long l = 0; int rc;
    Forbid(); HostLib_Lock();
    rc = xsd.pb->host_utf8_to_latin1(utf8, len, 1 /*translit '?'*/, &out, &l);
    AROS_HOST_BARRIER
    HostLib_Unlock(); Permit();
    if (rc != 0) return NULL;
    if (outlen) *outlen = l;
    return (UBYTE *)out;
}

static void pb_free_host(void *p)
{
    if (!p) return;
    Forbid(); HostLib_Lock();
    xsd.pb->host_pb_free(p);
    AROS_HOST_BARRIER
    HostLib_Unlock(); Permit();
}

/* ---- AROS clipboard.device / iffparse helpers (FTXT, shape from hostcb.c) -- */

static LONG clip_write_id(void)
{
    if (!clipio) return -1;
    clipio->io_Command = CBD_CURRENTWRITEID;
    clipio->io_Offset  = 0;
    clipio->io_Data    = NULL;
    clipio->io_Length  = 0;
    DoIO((struct IORequest *)clipio);
    return clipio->io_ClipID;
}

/* Write `bytes` (ISO-8859-1) as a FORM FTXT / CHRS clip into PRIMARY_CLIP. */
static BOOL clip_write_ftxt(const UBYTE *bytes, ULONG len)
{
    struct IFFHandle *iff;
    BOOL ok = FALSE;

    iff = AllocIFF();
    if (!iff) return FALSE;

    if ((iff->iff_Stream = (IPTR)OpenClipboard(PRIMARY_CLIP)))
    {
        InitIFFasClip(iff);
        if (!OpenIFF(iff, IFFF_WRITE))
        {
            if (!PushChunk(iff, ID_FTXT, ID_FORM, IFFSIZE_UNKNOWN))
            {
                if (!PushChunk(iff, 0, ID_CHRS, IFFSIZE_UNKNOWN))
                {
                    if (WriteChunkBytes(iff, (APTR)bytes, len) == (LONG)len)
                        ok = TRUE;
                    PopChunk(iff);
                }
                PopChunk(iff);
            }
            CloseIFF(iff);
        }
        CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
    }
    FreeIFF(iff);
    return ok;
}

/* Read PRIMARY_CLIP's FORM FTXT into a fresh AllocVec'd ISO-8859-1 buffer
   (NUL-terminated; *outlen excludes the NUL). Tolerates a non-FTXT/empty clip
   (returns FALSE, no crash). Multi-CHRS segments are concatenated. */
static BOOL clip_read_ftxt(UBYTE **out, ULONG *outlen)
{
    struct IFFHandle *iff;
    UBYTE *buf = NULL;
    ULONG  size = 0;
    BOOL   ok = FALSE;

    *out = NULL; *outlen = 0;

    iff = AllocIFF();
    if (!iff) return FALSE;

    if ((iff->iff_Stream = (IPTR)OpenClipboard(PRIMARY_CLIP)))
    {
        InitIFFasClip(iff);
        if (!OpenIFF(iff, IFFF_READ))
        {
            if (!StopChunk(iff, ID_FTXT, ID_CHRS))
            {
                for (;;)
                {
                    struct ContextNode *cn;
                    LONG error = ParseIFF(iff, IFFPARSE_SCAN);
                    if ((error != 0) && (error != IFFERR_EOC)) break;

                    cn = CurrentChunk(iff);
                    if (!cn) continue;

                    if ((cn->cn_Type == ID_FTXT) && (cn->cn_ID == ID_CHRS))
                    {
                        UBYTE *nb = AllocVec(size + cn->cn_Size + 1, MEMF_ANY);
                        if (!nb) { ok = FALSE; break; }
                        if (buf) { CopyMem(buf, nb, size); FreeVec(buf); }
                        buf = nb;
                        if (ReadChunkBytes(iff, buf + size, cn->cn_Size) != (LONG)cn->cn_Size)
                        { ok = FALSE; break; }
                        size += cn->cn_Size;
                        buf[size] = '\0';
                        ok = TRUE;
                    }
                }
            }
            CloseIFF(iff);
        }
        CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
    }
    FreeIFF(iff);

    if (ok && buf) { *out = buf; *outlen = size; return TRUE; }
    if (buf) FreeVec(buf);
    return FALSE;
}

/* ---- the sync task ------------------------------------------------------- */

static void cocoa_clipboard_task(void)
{
    long  lastHostCC, ourHostWrite = -1;
    LONG  lastArosWid, ourArosWrite = -1;

    IFFParseBase = OpenLibrary("iffparse.library", 36);
    if (!IFFParseBase) { D(bug("[Cocoa] clipboard: no iffparse.library\n")); return; }

    clipport = CreateMsgPort();
    if (clipport)
        clipio = (struct IOClipReq *)CreateIORequest(clipport, sizeof(struct IOClipReq));
    if (!clipio ||
        OpenDevice("clipboard.device", PRIMARY_CLIP, (struct IORequest *)clipio, 0))
    {
        D(bug("[Cocoa] clipboard: cannot open clipboard.device\n"));
        goto cleanup;
    }

    /* dlopen the host shim. Disable() across it so any Foundation thread inherits
       the blocked scheduler-signal mask (same reason as cocoa_hostlib_init). */
    Disable();
    xsd.pbHandle = HostLib_Open(PB_DYLIB_NAME, NULL);
    Enable();
    if (!xsd.pbHandle)
    {
        D(bug("[Cocoa] %s not found -> clipboard sync OFF (display unaffected)\n", PB_DYLIB_NAME));
        goto cleanup;
    }
    {
        ULONG errc = 0;
        xsd.pb = (struct PBInterface *)HostLib_GetInterface(xsd.pbHandle,
                                            (char **)pb_symbols, &errc);
        if ((!xsd.pb) || errc)
        {
            D(bug("[Cocoa] %s: %u symbols unresolved -> clipboard sync OFF\n", PB_DYLIB_NAME, errc));
            goto cleanup;
        }
    }

    D(bug("[Cocoa] clipboard bridge up: NSPasteboard <-> PRIMARY_CLIP\n"));

    lastHostCC  = pb_change_count();     /* baselines: only changes AFTER now sync */
    lastArosWid = clip_write_id();

    for (;;)
    {
        long cc;
        Delay(10);                       /* ~5 Hz; clipboard latency is not critical */

        cc = pb_change_count();
        if (cc != lastHostCC)
        {
            /* ---- macOS clipboard changed -> push to AROS (unless it's our echo) ---- */
            if (cc != ourHostWrite)
            {
                unsigned long ul = 0;
                char *utf8 = pb_get_text(&ul);
                if (utf8)
                {
                    unsigned long ll = 0;
                    UBYTE *lat = pb_u2l(utf8, ul, &ll);
                    if (lat)
                    {
                        if (clip_write_ftxt(lat, (ULONG)ll))
                        {
                            ourArosWrite = clip_write_id();   /* token: suppress our echo */
                            D(bug("[Cocoa] clip host->AROS: %lu bytes (cc=%ld)\n", ll, cc));
                        }
                        pb_free_host(lat);
                    }
                    pb_free_host(utf8);
                }
            }
            lastHostCC = cc;
        }
        else
        {
            /* ---- AROS clipboard changed -> push to macOS (unless it's our echo) ---- */
            LONG wid = clip_write_id();
            if (wid != lastArosWid)
            {
                if (wid != ourArosWrite)
                {
                    UBYTE *lat = NULL; ULONG ll = 0;
                    if (clip_read_ftxt(&lat, &ll))
                    {
                        unsigned long ul = 0;
                        char *utf8 = pb_l2u(lat, ll, &ul);
                        if (utf8)
                        {
                            ourHostWrite = pb_set_text(utf8, ul); /* token + advance host baseline */
                            lastHostCC   = ourHostWrite;
                            D(bug("[Cocoa] clip AROS->host: %lu bytes (wid=%ld)\n", ul, (long)wid));
                            pb_free_host(utf8);
                        }
                        FreeVec(lat);
                    }
                }
                lastArosWid = wid;
            }
        }
    }

cleanup:
    if (xsd.pb)       { HostLib_DropInterface((APTR *)xsd.pb); xsd.pb = NULL; }
    if (xsd.pbHandle) { HostLib_Close(xsd.pbHandle, NULL); xsd.pbHandle = NULL; }
    if (clipio)
    {
        if (clipio->io_Device) CloseDevice((struct IORequest *)clipio);
        DeleteIORequest((struct IORequest *)clipio); clipio = NULL;
    }
    if (clipport)    { DeleteMsgPort(clipport); clipport = NULL; }
    if (IFFParseBase){ CloseLibrary(IFFParseBase); IFFParseBase = NULL; }
}

/* ---- public: start the bridge task (non-fatal, like input) --------------- */

BOOL cocoa_clipboard_init(struct cocoahidd *xsd_)
{
    (void)xsd_;                          /* the task uses the global xsd */
    xsd.cliptask = NewCreateTask(TASKTAG_PC,        (IPTR)cocoa_clipboard_task,
                                 TASKTAG_NAME,      (IPTR)"cocoa.hidd clipboard",
                                 TASKTAG_PRI,       5,
                                 TASKTAG_STACKSIZE, 64 * 1024,
                                 TAG_DONE);
    return xsd.cliptask ? TRUE : FALSE;
}
