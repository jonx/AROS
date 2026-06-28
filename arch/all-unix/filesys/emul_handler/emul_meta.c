/*
    Copyright © 2026, The AROS Development Team. All rights reserved.

    Desc: ".<name>.amimeta" per-file metadata sidecar (R-SIDECAR) — AROS overlay.

    The pure parse/format logic is the hosted clean-room spike (R-SIDECAR in
    docs/features/host-volume/spec.md); the file I/O is routed through the
    overlay's dlsym'd LibCInterface (HostLib-locked) instead of direct libc.

    Two AmigaOS metadata items have no faithful POSIX slot and live here:
      - the file comment (fib_Comment), and
      - the AmigaOS-only protection bits (Archive/Pure/Script/Hold) that POSIX
        rwx cannot hold.
    The rwx bits keep coming from st_mode; the sidecar only carries the extras,
    and is written ONLY when a value is non-default (so the host dir stays clean).

    Format (line-oriented ASCII, forward-compatible):
        amimeta 1
        prot 0x<hex of the full AROS fib_Protection word>
        comment <AROS comment bytes, percent-escaped past printable ASCII>

    Deviations from the spec (intentional, justified):
      - Atomic replace uses open(O_CREAT|O_EXCL) on a per-task temp name rather
        than mkstemp() — mkstemp is not in the overlay's libc symbol set, and a
        host volume is served by a single handler process so writes to one
        sidecar are already serialised. The temp name embeds the task pointer so
        two volumes backing the same host dir never collide.
      - The comment is stored as escaped AROS (Latin-1) bytes, not re-encoded to
        UTF-8; it round-trips byte-exact to AROS, which is what matters here.
*/

#include "unix_hints.h"

#ifdef HOST_LONG_ALIGNED
#pragma pack(4)
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#pragma pack()

/* This prevents redefinition of struct timeval */
#define _AROS_TYPES_TIMEVAL_S_H_

#include <aros/debug.h>
#include <dos/dosasl.h>
#include <hidd/unixio.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/hostlib.h>

#include "emul_intern.h"
#include "emul_host.h"
#include "emul_unix.h"
#include "emul_hostvol.h"

/* ---- pure path / classification helpers ---------------------------------- */

size_t hv_sidecar_path(const char *filepath, char *dst, size_t dstcap)
{
    const char *slash = strrchr(filepath, '/');
    const char *base  = slash ? slash + 1 : filepath;
    size_t dirlen     = (size_t)(base - filepath);
    size_t need = 0;
    #define EMIT(b) do { if (need + 1 < dstcap) dst[need] = (b); need++; } while (0)
    for (size_t i = 0; i < dirlen; i++) EMIT(filepath[i]);
    EMIT('.');
    for (const char *q = base; *q; q++) EMIT(*q);
    static const char suf[] = ".amimeta";
    for (const char *q = suf; *q; q++) EMIT(*q);
    #undef EMIT
    if (dstcap > 0) dst[(need < dstcap) ? need : dstcap - 1] = '\0';
    return need;
}

int hv_is_sidecar_name(const char *name)
{
    if (name[0] != '.') return 0;
    size_t len = strlen(name);
    static const char suf[] = ".amimeta";
    size_t slen = sizeof(suf) - 1;
    if (len < slen + 1) return 0;
    return memcmp(name + len - slen, suf, slen) == 0;
}

int hv_meta_is_default(const HVMeta *m)
{
    /* A sidecar is needed only for what POSIX rwx + the host cannot hold: the
     * AmigaOS-only protection bits and the comment. A plain rwx-only change
     * (already in st_mode) must NOT spawn a sidecar. */
    return (m->prot & HV_FIBF_AMIGA_ONLY) == 0 && m->comment[0] == '\0';
}

/* ---- comment value escaping (printable-ASCII transparent, reversible) ----- */

static int is_transparent(unsigned char c)
{
    return c >= 0x20 && c <= 0x7E && c != '%';
}

static size_t esc_comment(const char *in, char *out, size_t cap)
{
    static const char hexd[] = "0123456789ABCDEF";
    size_t need = 0;
    #define PUT(b) do { if (need + 1 < cap) out[need] = (b); need++; } while (0)
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (is_transparent(*p)) { PUT((char)*p); }
        else { PUT('%'); PUT(hexd[*p >> 4]); PUT(hexd[*p & 0xF]); }
    }
    #undef PUT
    if (cap > 0) out[(need < cap) ? need : cap - 1] = '\0';
    return need;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void unesc_comment(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '%' && hexval((unsigned char)p[1]) >= 0 &&
                         hexval((unsigned char)p[2]) >= 0) {
            int v = (hexval((unsigned char)p[1]) << 4) | hexval((unsigned char)p[2]);
            if (o + 1 < cap) out[o] = (char)v;
            o++; p += 2;
        } else {
            if (o + 1 < cap) out[o] = *p;
            o++;
        }
    }
    if (cap > 0) out[(o < cap) ? o : cap - 1] = '\0';
}

/* Parse an unsigned hex/decimal word ("0x1F" or "31"); no libc strtoul. */
static uint32_t parse_u32(const char *s)
{
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        int h;
        while ((h = hexval((unsigned char)*s)) >= 0) { v = (v << 4) | (uint32_t)h; s++; }
    } else {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    }
    return v;
}

static size_t append_str(char *buf, size_t off, size_t cap, const char *s)
{
    while (*s && off + 1 < cap) buf[off++] = *s++;
    return off;
}

static size_t append_hex32(char *buf, size_t off, size_t cap, uint32_t v)
{
    static const char hexd[] = "0123456789ABCDEF";
    off = append_str(buf, off, cap, "0x");
    char d[8]; int n = 0;
    if (v == 0) { if (off + 1 < cap) buf[off++] = '0'; return off; }
    while (v) { d[n++] = hexd[v & 0xF]; v >>= 4; }
    while (n-- > 0 && off + 1 < cap) buf[off++] = d[n];
    return off;
}

/* ---- host I/O via the dlsym'd LibCInterface (HostLib-locked) -------------- *
 * Callers must NOT already hold HostLib_Lock(). */

int MetaRead(struct emulbase *emulbase, const char *filepath, HVMeta *out)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    char path[1024], buf[1024];
    int fd, rc = 0;
    ssize_t total = 0, r = 0;

    out->prot = HV_PROT_DEFAULT;
    out->comment[0] = '\0';

    hv_sidecar_path(filepath, path, sizeof path);

    HostLib_Lock();
    fd = iface->open(path, O_RDONLY, 0);
    AROS_HOST_BARRIER
    if (fd < 0) { HostLib_Unlock(); return 0; }   /* absent -> defaults */

    while ((r = iface->read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) {
        AROS_HOST_BARRIER
        total += r;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    AROS_HOST_BARRIER
    iface->close(fd);
    AROS_HOST_BARRIER
    HostLib_Unlock();

    if (r < 0) return -1;
    buf[total] = '\0';

    int saw_header = 0;
    char *line = buf, *nl;
    do {
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *sp = strchr(line, ' ');
        const char *key = line, *val = "";
        if (sp) { *sp = '\0'; val = sp + 1; }

        if (strcmp(key, "amimeta") == 0)      saw_header = 1;
        else if (strcmp(key, "prot") == 0)    out->prot = parse_u32(val);
        else if (strcmp(key, "comment") == 0) {
            char tmp[HV_COMMENT_MAX * 3 + 1];
            unesc_comment(val, tmp, sizeof tmp);
            strncpy(out->comment, tmp, HV_COMMENT_MAX);
            out->comment[HV_COMMENT_MAX] = '\0';
        }
        line = nl ? nl + 1 : NULL;
    } while (line && *line);

    (void)rc;
    if (!saw_header) return -1;
    return 1;
}

int MetaWrite(struct emulbase *emulbase, const char *filepath, const HVMeta *m)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    char path[1024], tmp[1024 + 24], body[HV_COMMENT_MAX * 3 + 256];
    int fd, ret = 0;
    size_t off = 0, wr = 0;

    hv_sidecar_path(filepath, path, sizeof path);

    /* Default metadata: remove any sidecar, keep the host dir clean. */
    if (hv_meta_is_default(m)) {
        HostLib_Lock();
        iface->unlink(path);
        AROS_HOST_BARRIER
        HostLib_Unlock();
        return 0;
    }

    off = append_str(body, off, sizeof body, "amimeta 1\nprot ");
    off = append_hex32(body, off, sizeof body, m->prot);
    off = append_str(body, off, sizeof body, "\n");
    if (m->comment[0]) {
        char esc[HV_COMMENT_MAX * 3 + 1];
        esc_comment(m->comment, esc, sizeof esc);
        off = append_str(body, off, sizeof body, "comment ");
        off = append_str(body, off, sizeof body, esc);
        off = append_str(body, off, sizeof body, "\n");
    }

    /* temp name = "<sidecar>.t<taskptr-hex>" in the SAME dir (atomic rename).
     * The task pointer keeps it unique across volumes sharing a host dir. */
    {
        size_t pl = strlen(path);
        APTR task = FindTask(NULL);
        if (pl + 20 >= sizeof tmp) return -1;
        CopyMem(path, tmp, pl);
        size_t to = append_str(tmp, pl, sizeof tmp, ".t");
        to = append_hex32(tmp, to, sizeof tmp, (uint32_t)(IPTR)task);
        tmp[to] = '\0';
    }

    HostLib_Lock();
    fd = iface->open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    AROS_HOST_BARRIER
    if (fd < 0) { HostLib_Unlock(); return -1; }

    while (wr < off) {
        ssize_t n = iface->write(fd, body + wr, off - wr);
        AROS_HOST_BARRIER
        if (n < 0) { iface->close(fd); AROS_HOST_BARRIER
                     iface->unlink(tmp); AROS_HOST_BARRIER
                     HostLib_Unlock(); return -1; }
        wr += (size_t)n;
    }
    iface->close(fd);
    AROS_HOST_BARRIER
    if (iface->rename(tmp, path) != 0) {
        AROS_HOST_BARRIER
        iface->unlink(tmp);
        AROS_HOST_BARRIER
        ret = -1;
    } else
        AROS_HOST_BARRIER
    HostLib_Unlock();

    return ret;
}

/* Rename the sidecar alongside a renamed data file (best-effort). */
void MetaRename(struct emulbase *emulbase, const char *oldpath, const char *newpath)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    char os[1024], ns[1024];
    hv_sidecar_path(oldpath, os, sizeof os);
    hv_sidecar_path(newpath, ns, sizeof ns);
    HostLib_Lock();
    iface->rename(os, ns);          /* ENOENT if no sidecar — harmless */
    AROS_HOST_BARRIER
    HostLib_Unlock();
}

/* Delete the sidecar alongside a deleted data file (best-effort). */
void MetaDelete(struct emulbase *emulbase, const char *filepath)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    char s[1024];
    hv_sidecar_path(filepath, s, sizeof s);
    HostLib_Lock();
    iface->unlink(s);               /* ENOENT if no sidecar — harmless */
    AROS_HOST_BARRIER
    HostLib_Unlock();
}

/* ACTION_SET_COMMENT: persist the AROS file comment to the sidecar, preserving
 * any existing protection bits. An empty comment with no extra prot bits
 * removes the sidecar (MetaWrite's omit-when-default). */
LONG DoSetComment(struct emulbase *emulbase, char *fullname, const char *comment)
{
    HVMeta m;
    ULONG i;

    MetaRead(emulbase, fullname, &m);       /* keep existing prot bits */
    for (i = 0; comment[i] && i < HV_COMMENT_MAX; i++)
        m.comment[i] = comment[i];
    m.comment[i] = 0;

    return (MetaWrite(emulbase, fullname, &m) == 0) ? 0 : ERROR_DISK_FULL;
}
