/*
    Copyright © 2026, The AROS Development Team. All rights reserved.

    Desc: ".ameta", one Amiga metadata file per host directory (R-SIDECAR).

    The format is shared with other tools that keep AROS files in host
    directories: one line-oriented UTF-8 file named .ameta in a directory holds
    the comment and the protection bits a host mode cannot hold for the
    entries of that directory. emul_ameta.c parses and writes it; this file
    finds a directory's .ameta, reads it and replaces it through the dlsym'd
    LibCInterface, HostLib-locked.

    The handler takes owner, group and other rwx from st_mode and every other
    protection bit from .ameta, the Delete bit included. A write holds an
    exclusive flock on the directory, re-reads the file, applies the change,
    drops the entries whose file is gone, writes a temporary ".ameta.t*" file
    beside it, checks that .ameta was not replaced meanwhile, and renames the
    temporary file into place, or deletes .ameta when no entry is left.
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
#include <sys/stat.h>

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
#include "ameta.h"

#ifndef LOCK_EX
#define LOCK_EX 2
#endif
#ifndef LOCK_UN
#define LOCK_UN 8
#endif

#define META_NAME      "/.ameta"
#define PATH_CAP       1024
#define WRITE_ATTEMPTS 8

/* ---- names ----------------------------------------------------------------- */

int hv_is_sidecar_name(const char *name)
{
    return strcmp(name, ".ameta") == 0 || strncmp(name, ".ameta.", 7) == 0;
}

int hv_meta_is_default(const HVMeta *m)
{
    return (m->prot & ~(uint32_t)AMETA_RECORD_MASK) == 0 && m->comment[0] == '\0';
}

/* Split "dir/name" into the directory and the name. */
static int split(const char *path, char *dir, size_t cap, const char **name)
{
    const char *slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path) : 0;
    if (!slash || len + 1 > cap)
        return -1;
    CopyMem((APTR)path, dir, len);
    dir[len] = '\0';
    *name = slash + 1;
    return 0;
}

static void join(char *dst, size_t cap, const char *dir, const char *tail, size_t taillen)
{
    size_t i = 0, j;
    while (dir[i] && i + 1 < cap) { dst[i] = dir[i]; i++; }
    for (j = 0; j < taillen && i + 1 < cap; j++) dst[i++] = tail[j];
    dst[i] = '\0';
}

/* ---- reading --------------------------------------------------------------- */

struct loaded
{
    char *buf;
    size_t len;
    int exists;
    struct stat id;
    ameta_arena arena;
    ameta_file file;
    int usable;
};

static void report(void *ctx, unsigned line, const char *why)
{
    bug("[emul] %s line %u: %s\n", (const char *)ctx, line, why);
}

static void unload(struct loaded *l)
{
    if (l->buf) FreeVec(l->buf);
    if (l->arena.base) FreeVec(l->arena.base);
    memset(l, 0, sizeof *l);
}

/* Read dir/.ameta. -1 when memory runs out; otherwise l->usable says whether
 * the file may be rewritten (a file of another version never is). */
static int load(struct emulbase *emulbase, const char *dir, struct loaded *l)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    char path[PATH_CAP];
    int fd;

    memset(l, 0, sizeof *l);
    join(path, sizeof path, dir, META_NAME, sizeof META_NAME - 1);

    HostLib_Lock();
    fd = iface->open(path, O_RDONLY, 0);
    AROS_HOST_BARRIER
    if (fd >= 0) {
        if (iface->fstat(fd, &l->id) == 0) {
            AROS_HOST_BARRIER
            size_t cap = (size_t)l->id.st_size + 1;
            if (cap <= AMETA_MAX_FILE + 1)
                l->buf = AllocVec(cap, MEMF_ANY);
            l->exists = 1;
            while (l->buf && l->len + 1 < cap) {
                ssize_t r = iface->read(fd, l->buf + l->len, cap - 1 - l->len);
                AROS_HOST_BARRIER
                if (r <= 0)
                    break;
                l->len += (size_t)r;
            }
        }
        AROS_HOST_BARRIER
        iface->close(fd);
        AROS_HOST_BARRIER
    }
    HostLib_Unlock();

    if (l->exists && !l->buf) {
        /* Larger than the format allows: report it and leave it alone. */
        report(path, 0, "file-too-large");
        l->usable = 0;
        return 0;
    }
    l->arena.cap = l->len * 4 + 65536;
    l->arena.base = AllocVec(l->arena.cap, MEMF_ANY);
    if (!l->arena.base)
        return -1;
    l->usable = ameta_parse(l->buf ? l->buf : "", l->len, &l->arena, &l->file, report, path)
                == AMETA_OK;
    return 0;
}

int MetaRead(struct emulbase *emulbase, const char *filepath, HVMeta *out)
{
    char dir[PATH_CAP];
    const char *name;
    struct loaded l;
    ameta_entry *e;
    int found = 0;

    out->prot = HV_PROT_DEFAULT;
    out->comment[0] = '\0';
    if (split(filepath, dir, sizeof dir, &name) != 0 || load(emulbase, dir, &l) != 0)
        return -1;
    e = l.usable ? ameta_find(&l.file, (const unsigned char *)name, strlen(name)) : NULL;
    if (e) {
        if (e->has_prot)
            out->prot = (uint32_t)e->prot;
        ameta_comment_to_latin1(e->comment, e->comment_len, out->comment, sizeof out->comment);
        found = 1;
    }
    unload(&l);
    return found;
}

/* ---- writing --------------------------------------------------------------- */

struct directory
{
    struct emulbase *emulbase;
    const char *dir;
};

static int present(void *ctx, const unsigned char *name, size_t len)
{
    struct directory *d = ctx;
    struct emulbase *emulbase = d->emulbase;   /* HostLib_Lock() names it */
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    char path[PATH_CAP], tail[PATH_CAP];
    struct stat st;
    int rc;

    if (len + 2 > sizeof tail)
        return 1;
    tail[0] = '/';
    CopyMem((APTR)name, tail + 1, len);
    join(path, sizeof path, d->dir, tail, len + 1);
    HostLib_Lock();
    rc = iface->lstat(path, &st);
    AROS_HOST_BARRIER
    HostLib_Unlock();
    return rc == 0;
}

static int same_identity(struct emulbase *emulbase, const struct loaded *l, const char *path)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    struct stat now;
    int rc;
    HostLib_Lock();
    rc = iface->stat((char *)path, &now);
    AROS_HOST_BARRIER
    HostLib_Unlock();
    if (rc != 0)
        return !l->exists;
    return l->exists && now.st_dev == l->id.st_dev && now.st_ino == l->id.st_ino
           && now.st_size == l->id.st_size && now.st_mtime == l->id.st_mtime;
}

/* Write body to a new ".ameta.t*" file beside path; its name goes to tmp. */
static int write_temporary(struct emulbase *emulbase, const char *path, char *tmp,
                           const char *body, size_t len)
{
    static ULONG serial;
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    static const char hex[] = "0123456789abcdef";
    int attempt, fd = -1;

    for (attempt = 0; attempt < 16 && fd < 0; attempt++) {
        IPTR stamp = (IPTR)FindTask(NULL) ^ (IPTR)(++serial << 20);
        char tail[24];
        size_t t = 0;
        int k;
        tail[t++] = '.';
        tail[t++] = 't';
        for (k = (int)(2 * sizeof stamp) - 1; k >= 0; k--)
            tail[t++] = hex[(stamp >> (4 * k)) & 0xF];
        join(tmp, PATH_CAP, path, tail, t);
        HostLib_Lock();
        fd = iface->open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
        AROS_HOST_BARRIER
        HostLib_Unlock();
    }
    if (fd < 0)
        return -1;

    HostLib_Lock();
    /* See emul_host.c: darwin/aarch64 hosted can lose the variadic open()
     * mode across the host-libc function pointer boundary. */
    iface->chmod(tmp, 0644);
    AROS_HOST_BARRIER
    {
        size_t done = 0;
        while (done < len) {
            ssize_t n = iface->write(fd, body + done, len - done);
            AROS_HOST_BARRIER
            if (n <= 0)
                break;
            done += (size_t)n;
        }
        iface->fsync(fd);
        AROS_HOST_BARRIER
        iface->close(fd);
        AROS_HOST_BARRIER
        if (done != len) {
            iface->unlink(tmp);
            AROS_HOST_BARRIER
            HostLib_Unlock();
            return -1;
        }
    }
    HostLib_Unlock();
    return 0;
}

typedef int (*change_fn)(ameta_file *f, ameta_arena *a, void *arg);

static int rewrite_locked(struct emulbase *emulbase, const char *dir, change_fn change, void *arg)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    struct directory d = { emulbase, dir };
    char path[PATH_CAP], tmp[PATH_CAP];
    int attempt;

    join(path, sizeof path, dir, META_NAME, sizeof META_NAME - 1);
    for (attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
        struct loaded l;
        size_t need;
        char *body = NULL;
        int ok;

        if (load(emulbase, dir, &l) != 0)
            return -1;
        if (!l.usable) {
            /* Never overwrite what could not be read: it may be a newer version. */
            unload(&l);
            return -1;
        }
        /* The change first: the host operation it follows (a rename, a
         * delete) has already happened, so pruning first would take a
         * renamed file's entry for a stale one. */
        if (change(&l.file, &l.arena, arg) != 0) {
            unload(&l);
            return -1;
        }
        ameta_prune(&l.file, present, &d);
        need = ameta_format(&l.file, NULL, 0);
        if (need) {
            body = AllocVec(need, MEMF_ANY);
            if (!body) {
                unload(&l);
                return -1;
            }
            ameta_format(&l.file, body, need);
            if (write_temporary(emulbase, path, tmp, body, need) != 0) {
                FreeVec(body);
                unload(&l);
                return -1;
            }
        }
        if (!same_identity(emulbase, &l, path)) {
            if (need) {
                HostLib_Lock();
                iface->unlink(tmp);
                AROS_HOST_BARRIER
                HostLib_Unlock();
                FreeVec(body);
            }
            unload(&l);
            continue;
        }
        HostLib_Lock();
        if (need) {
            ok = iface->rename(tmp, path) == 0;
            AROS_HOST_BARRIER
            if (!ok) {
                iface->unlink(tmp);
                AROS_HOST_BARRIER
            }
        } else {
            struct stat st;
            ok = iface->unlink(path) == 0 || iface->lstat(path, &st) != 0;
            AROS_HOST_BARRIER
        }
        HostLib_Unlock();
        if (body)
            FreeVec(body);
        unload(&l);
        return ok ? 0 : -1;
    }
    bug("[emul] %s kept changing; metadata not written\n", path);
    return -1;
}

/* Writers of one directory's .ameta take turns on the directory itself; the
 * identity check in rewrite_locked guards against a writer that cannot lock. */
static int rewrite(struct emulbase *emulbase, const char *dir, change_fn change, void *arg)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    int lock, rc;

    HostLib_Lock();
    lock = iface->open((char *)dir, O_RDONLY, 0);
    AROS_HOST_BARRIER
    if (lock >= 0) {
        iface->flock(lock, LOCK_EX);
        AROS_HOST_BARRIER
    }
    HostLib_Unlock();

    rc = rewrite_locked(emulbase, dir, change, arg);

    if (lock >= 0) {
        HostLib_Lock();
        iface->flock(lock, LOCK_UN);
        AROS_HOST_BARRIER
        iface->close(lock);
        AROS_HOST_BARRIER
        HostLib_Unlock();
    }
    return rc;
}

struct set_args
{
    const char *name;
    const HVMeta *meta;
};

static int set_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const struct set_args *s = arg;
    size_t len = strlen(s->name);
    ameta_entry *e = ameta_get(f, a, (const unsigned char *)s->name, len);
    char utf8[HV_COMMENT_MAX * 6 + 1];
    if (!e)
        return -1;
    /* Bits above 31 belong to the entry, not to this 32-bit word: keep them. */
    e->prot = (e->has_prot ? e->prot & ~0xFFFFFFFFull : 0) | s->meta->prot;
    e->has_prot = 1;
    ameta_comment_to_utf8(s->meta->comment, utf8, sizeof utf8);
    return ameta_set_comment(e, a, (const unsigned char *)utf8, strlen(utf8));
}

int MetaWrite(struct emulbase *emulbase, const char *filepath, const HVMeta *m)
{
    char dir[PATH_CAP];
    struct set_args s;
    if (split(filepath, dir, sizeof dir, &s.name) != 0)
        return -1;
    s.meta = m;
    return rewrite(emulbase, dir, set_entry, &s);
}

struct rename_args
{
    const char *from, *to;
};

static int rename_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const struct rename_args *r = arg;
    return ameta_rename(f, a, (const unsigned char *)r->from, strlen(r->from),
                        (const unsigned char *)r->to, strlen(r->to));
}

struct move_args
{
    const ameta_entry *entry;
    const char *name;
};

static int put_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const struct move_args *m = arg;
    return ameta_copy_entry(f, a, m->entry, (const unsigned char *)m->name, strlen(m->name))
               ? 0 : -1;
}

struct delete_args
{
    const char *name;
};

static int delete_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const struct delete_args *d = arg;
    (void)a;
    ameta_remove(f, (const unsigned char *)d->name, strlen(d->name));
    return 0;
}

/* Keep the metadata with a renamed or moved file; call after the host rename. */
void MetaRename(struct emulbase *emulbase, const char *oldpath, const char *newpath)
{
    char olddir[PATH_CAP], newdir[PATH_CAP];
    const char *oldname, *newname;

    if (split(oldpath, olddir, sizeof olddir, &oldname) != 0
        || split(newpath, newdir, sizeof newdir, &newname) != 0)
        return;
    if (strcmp(olddir, newdir) == 0) {
        struct rename_args r = { oldname, newname };
        rewrite(emulbase, olddir, rename_entry, &r);
        return;
    }
    {
        struct loaded l;
        ameta_entry *e;
        if (load(emulbase, olddir, &l) != 0)
            return;
        e = l.usable ? ameta_find(&l.file, (const unsigned char *)oldname, strlen(oldname)) : NULL;
        if (e) {
            struct move_args m = { e, newname };
            struct delete_args d = { oldname };
            if (rewrite(emulbase, newdir, put_entry, &m) == 0)
                rewrite(emulbase, olddir, delete_entry, &d);
        }
        unload(&l);
    }
}

/* Drop the metadata of a deleted file; call after the host delete. */
void MetaDelete(struct emulbase *emulbase, const char *filepath)
{
    char dir[PATH_CAP];
    struct delete_args d;
    if (split(filepath, dir, sizeof dir, &d.name) != 0)
        return;
    rewrite(emulbase, dir, delete_entry, &d);
}

/* ACTION_SET_COMMENT: record the comment, keeping the entry's protection. */
LONG DoSetComment(struct emulbase *emulbase, char *fullname, const char *comment)
{
    HVMeta m;
    ULONG i;

    MetaRead(emulbase, fullname, &m);
    for (i = 0; comment[i] && i < HV_COMMENT_MAX; i++)
        m.comment[i] = comment[i];
    m.comment[i] = 0;

    return (MetaWrite(emulbase, fullname, &m) == 0) ? 0 : ERROR_DISK_FULL;
}
