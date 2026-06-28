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
#include <dos/dostags.h>              /* CreateNewProc / NP_* */
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

#define COCOA_CLIP_MAX_BYTES (1024UL * 1024UL)

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

    if (len > COCOA_CLIP_MAX_BYTES)
    {
        D(bug("[Cocoa] clip: refusing oversized AROS clipboard write (%lu bytes, max %lu)\n",
              (unsigned long)len, (unsigned long)COCOA_CLIP_MAX_BYTES));
        return FALSE;
    }

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
                        LONG rawsize = cn->cn_Size;
                        ULONG chunk;
                        UBYTE *nb;

                        if (rawsize <= 0)
                            continue;
                        chunk = (ULONG)rawsize;

                        if (chunk > COCOA_CLIP_MAX_BYTES ||
                            size > COCOA_CLIP_MAX_BYTES - chunk)
                        {
                            D(bug("[Cocoa] clip: refusing oversized PRIMARY_CLIP text (> %lu bytes)\n",
                                  (unsigned long)COCOA_CLIP_MAX_BYTES));
                            ok = FALSE;
                            break;
                        }

                        nb = AllocVec(size + chunk + 1, MEMF_ANY);
                        if (!nb) { ok = FALSE; break; }
                        if (buf) { CopyMem(buf, nb, size); FreeVec(buf); }
                        buf = nb;
                        if (ReadChunkBytes(iff, buf + size, chunk) != (LONG)chunk)
                        { ok = FALSE; break; }
                        size += chunk;
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

/* Short printable preview of a byte buffer for the debug log: up to 32 bytes,
   non-printables shown as '.', NUL-terminated. Static buffer — this task is the
   only caller and it logs one line at a time. */
static const char *clip_preview(const UBYTE *b, ULONG len)
{
    static char pv[40];
    ULONG i, n = (len < 32) ? len : 32;
    for (i = 0; i < n; i++)
    {
        UBYTE c = b[i];
        pv[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    pv[n] = '\0';
    return pv;
}

/* ---- the sync task ------------------------------------------------------- */

/*
 * ConClip creates this rendezvous port after the startup-sequence has reached
 * the console clipboard bridge. Use it as our readiness gate before touching
 * clipboard.device: opening clipboard.device can lazy-load iffparse/locale, and
 * doing that while DOS is still constructing the first CON: window has crashed
 * this hosted port in locale.library.
 */
#define COCOA_CONCLIP_PORTNAME "ConClip.rendezvous"

static BOOL cocoa_clipboard_wait_ready(void)
{
    ULONG waited = 0;

    for (;;)
    {
        struct MsgPort *port;

        Forbid();
        port = FindPort(COCOA_CONCLIP_PORTNAME);
        Permit();

        if (port)
        {
            D(bug("[Cocoa] clipboard bridge starting after %s became ready\n",
                  COCOA_CONCLIP_PORTNAME));
            return TRUE;
        }

        if (waited == 0 || (waited % 250) == 0)
            D(bug("[Cocoa] clipboard bridge waiting for %s\n",
                  COCOA_CONCLIP_PORTNAME));

        Delay(10);
        waited += 10;
    }
}

static void cocoa_clipboard_task(void)
{
    long  lastHostCC, ourHostWrite = -1;
    LONG  lastArosWid, ourArosWrite = -1;

    if (!cocoa_clipboard_wait_ready())
        return;

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
    D(bug("[Cocoa] clip: baseline macOS cc=%ld, AROS wid=%ld -- polling at ~5Hz\n",
          lastHostCC, (long)lastArosWid));

    ULONG beat = 0;
    for (;;)
    {
        long cc;
        Delay(10);                       /* ~5 Hz; clipboard latency is not critical */

        /* liveness heartbeat (~every 30s) so the poll loop is visibly alive even
           when nothing is being copied. */
        if (++beat >= 150)
        {
            beat = 0;
            D(bug("[Cocoa] clip: heartbeat (macOS cc=%ld, AROS wid=%ld)\n",
                  lastHostCC, (long)lastArosWid));
        }

        cc = pb_change_count();
        if (cc != lastHostCC)
        {
            /* ---- macOS clipboard changed -> push to AROS (unless it's our echo) ---- */
            D(bug("[Cocoa] clip: macOS pasteboard changed (cc %ld->%ld)\n", lastHostCC, cc));
            if (cc == ourHostWrite)
            {
                D(bug("[Cocoa] clip:   ...it's our own AROS->host write, ignored\n"));
            }
            else
            {
                unsigned long ul = 0;
                char *utf8 = pb_get_text(&ul);
                if (!utf8)
                {
                    D(bug("[Cocoa] clip:   no text flavour on the pasteboard (image/file?) -- skipped\n"));
                }
                else if (ul > COCOA_CLIP_MAX_BYTES)
                {
                    D(bug("[Cocoa] clip:   macOS text is too large (%lu bytes, max %lu) -- skipped\n",
                          ul, (unsigned long)COCOA_CLIP_MAX_BYTES));
                    pb_free_host(utf8);
                }
                else
                {
                    unsigned long ll = 0;
                    UBYTE *lat = pb_u2l(utf8, ul, &ll);
                    if (!lat)
                        D(bug("[Cocoa] clip:   UTF-8 (%lu B) -> Latin-1 transcode FAILED\n", ul));
                    else
                    {
                        if (clip_write_ftxt(lat, (ULONG)ll))
                        {
                            ourArosWrite = clip_write_id();   /* token: suppress our echo */
                            D(bug("[Cocoa] clip: host->AROS  %lu bytes \"%s\" -> PRIMARY_CLIP (new AROS wid=%ld)\n",
                                  ll, clip_preview(lat, ll), (long)ourArosWrite));
                        }
                        else
                            D(bug("[Cocoa] clip:   clip_write_ftxt to PRIMARY_CLIP FAILED\n"));
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
                D(bug("[Cocoa] clip: AROS clipboard changed (wid %ld->%ld)\n",
                      (long)lastArosWid, (long)wid));
                if (wid == ourArosWrite)
                {
                    D(bug("[Cocoa] clip:   ...it's our own host->AROS write, ignored\n"));
                }
                else
                {
                    UBYTE *lat = NULL; ULONG ll = 0;
                    if (!clip_read_ftxt(&lat, &ll))
                    {
                        D(bug("[Cocoa] clip:   PRIMARY_CLIP holds no FTXT text -- skipped\n"));
                    }
                    else
                    {
                        unsigned long ul = 0;
                        char *utf8 = pb_l2u(lat, ll, &ul);
                        if (!utf8)
                            D(bug("[Cocoa] clip:   Latin-1 (%lu B) -> UTF-8 transcode FAILED\n", (unsigned long)ll));
                        else
                        {
                            long newHostCC = pb_set_text(utf8, ul);
                            if (newHostCC >= 0)
                            {
                                ourHostWrite = newHostCC; /* token + advance host baseline */
                                lastHostCC   = newHostCC;
                                D(bug("[Cocoa] clip: AROS->host  %lu bytes \"%s\" -> NSPasteboard (new macOS cc=%ld)\n",
                                      ul, clip_preview(lat, ll), ourHostWrite));
                            }
                            else
                                D(bug("[Cocoa] clip:   NSPasteboard write FAILED\n"));
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

    /* A PROCESS, not a bare Task: clipboard.device file-backs PRIMARY_CLIP to
       CLIPS: (a DOS path), so the sync code needs a Process context (pr_*) for
       the file I/O — a NewCreateTask() task has none and faults in OpenDevice. */
    xsd.cliptask = (struct Task *)CreateNewProcTags(
        NP_Entry,     (IPTR)cocoa_clipboard_task,
        NP_Name,      (IPTR)"cocoa.hidd clipboard",
        NP_Priority,  5,
        NP_StackSize, 64 * 1024,
        TAG_DONE);
    return xsd.cliptask ? TRUE : FALSE;
}
