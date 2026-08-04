/*
    Copyright (C) 1995-2018, The AROS Development Team. All rights reserved.
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

#define DEBUG 0
#define DASYNC(x)
#define DEXAM(x)
#define DMOUNT(x)
#define DOPEN(x)
#define DREAD(x)
#define DWRITE(x)
#define DSEEK(x)

#include <aros/debug.h>
#include <aros/symbolsets.h>
#include <dos/dosasl.h>
#include <hidd/unixio.h>
#include <utility/date.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/hostlib.h>
#include <proto/kernel.h>
#include <proto/utility.h>

#include "emul_intern.h"
#include "emul_unix.h"
#include "emul_hostvol.h"

#define NO_CASE_SENSITIVITY

struct dirent *ReadDir(struct emulbase *emulbase, struct filehandle *fh, IPTR *dirpos);

/* --- Name bridge (R-CHARSET + R-NORM): AROS Latin-1 <-> host UTF-8 + NFC -----
 * AmigaOS/AROS filenames are conventionally ISO-8859-1; macOS paths are UTF-8,
 * and APFS is a bag-of-bytes that may store either NFC or NFD. We translate at
 * the boundary so accented/Unicode names round-trip and cross-form lookups
 * succeed. On non-Darwin unix hosts names pass through byte-for-byte (existing
 * behaviour preserved). Pure CPU — no host call, inside or outside the lock. */

#define HV_NAMEBUF 1024

static int CopyASCIIName(const char *src, char *dst, ULONG dstcap)
{
    ULONG i;

    if (!dstcap)
        return TRUE;
    if (!src)
    {
        dst[0] = 0;
        return TRUE;
    }

    for (i = 0; src[i] && i + 1 < dstcap; i++)
    {
        if (((unsigned char)src[i]) >= 0x80)
            return FALSE;
        dst[i] = src[i];
    }
    if (src[i] && ((unsigned char)src[i]) >= 0x80)
        return FALSE;
    dst[i] = 0;
    return TRUE;
}

/* AROS name -> host name (Latin-1 -> UTF-8, then NFC). Called by the portable
 * core's makefilename() before a name reaches a host syscall. */
void NameToHost(const char *aros, char *dst, ULONG dstcap)
{
#ifdef HOST_OS_darwin
    char utf8[HV_NAMEBUF];
    if (CopyASCIIName(aros, dst, dstcap))
        return;
    hv_latin1_to_utf8(aros, utf8, sizeof utf8);
    hv_to_nfc(utf8, dst, dstcap);
#else
    ULONG i;
    for (i = 0; aros[i] && i + 1 < dstcap; i++) dst[i] = aros[i];
    if (dstcap) dst[i] = 0;
#endif
}

/* host name -> AROS name (NFC, then UTF-8 -> Latin-1 with reversible escape).
 * Returns the AROS byte length. Used when handing readdir results to AROS. */
static ULONG NameToAros(const char *host, char *dst, ULONG dstcap)
{
#ifdef HOST_OS_darwin
    char nfc[HV_NAMEBUF];
    if (CopyASCIIName(host, dst, dstcap))
        return (ULONG)strlen(dst);
    hv_to_nfc(host, nfc, sizeof nfc);
    return (ULONG)hv_utf8_to_latin1(nfc, dst, dstcap);
#else
    ULONG i;
    for (i = 0; host[i] && i + 1 < dstcap; i++) dst[i] = host[i];
    if (dstcap) dst[i] = 0;
    return i;
#endif
}

/* NFC-aware case-insensitive match for fixcase: normalize the host d_name to
 * NFC, then ASCII case-fold against the (already host-form/NFC) target. */
static int NameHostMatch(struct emulbase *emulbase, const char *dname, const char *target)
{
#ifdef HOST_OS_darwin
    char nfc[HV_NAMEBUF];
    char ascii[HV_NAMEBUF];
    if (CopyASCIIName(dname, ascii, sizeof ascii))
        return Stricmp(ascii, (char *)target) == 0;
    hv_to_nfc(dname, nfc, sizeof nfc);
    return Stricmp(nfc, (char *)target) == 0;
#else
    return Stricmp((char *)dname, (char *)target) == 0;
#endif
}

/* --- Sidecar metadata helpers (R-SIDECAR) --------------------------------- *
 * Build the full host path of a directory entry (the dir handle's host path +
 * '/' + entry name); foundname==NULL means the handle's own path. */
static void entry_hostpath(struct filehandle *fh, const char *foundname, char *dst, ULONG cap)
{
    ULONG i = 0, j;
    const char *h = fh->hostname;

    while (h[i] && i + 1 < cap) { dst[i] = h[i]; i++; }
    if (foundname) {
        if (i + 1 < cap) dst[i++] = '/';
        for (j = 0; foundname[j] && i + 1 < cap; j++) dst[i++] = foundname[j];
    }
    if (cap) dst[i] = 0;
}

/* Read the entry's sidecar and apply it to an Examine result: OR the AmigaOS-
 * only protection bits onto *prot (rwx stays from st_mode) and copy the comment
 * (Latin-1). No-op (clean defaults) when there is no sidecar. */
static void apply_meta(struct emulbase *emulbase, const char *path, ULONG *prot,
                       char *comment, ULONG commentcap)
{
    HVMeta m;

    if (comment && commentcap)
        comment[0] = 0;
    if (MetaRead(emulbase, path, &m) == 1) {
        if (prot)
            *prot |= (m.prot & HV_FIBF_AMIGA_ONLY);
        if (comment && commentcap) {
            ULONG k;
            for (k = 0; m.comment[k] && k + 1 < commentcap; k++)
                comment[k] = m.comment[k];
            comment[k] = 0;
        }
    }
}

static void apply_meta_fib(struct emulbase *emulbase, const char *path, ULONG *prot,
                           UBYTE *comment, ULONG commentcap)
{
    HVMeta m;

    if (comment && commentcap)
        comment[0] = 0;
    if (MetaRead(emulbase, path, &m) == 1) {
        if (prot)
            *prot |= (m.prot & HV_FIBF_AMIGA_ONLY);
        if (comment && commentcap) {
            ULONG k;
            for (k = 0; m.comment[k] && k + 2 < commentcap && k < 255; k++)
                comment[k + 1] = m.comment[k];
            comment[0] = k;
        }
    }
}

/*********************************************************************************************/

/* Make an AROS error-code (<dos/dos.h>) out of a unix error-code. */
static LONG u2a[][2]=
{
    { ENOMEM   , ERROR_NO_FREE_STORE         },
    { ENOENT   , ERROR_OBJECT_NOT_FOUND      },
    { EEXIST   , ERROR_OBJECT_EXISTS         },
    { EACCES   , ERROR_WRITE_PROTECTED       }, /* AROS distinguishes between different
                                        kinds of privelege violation. Therefore
                                        a routine using err_u2a(emulbase) should check
                                        for ERROR_WRITE_PROTECTED and replace
                                        it by a different constant, if
                                        necessary. */
    { ENOTDIR     , ERROR_DIR_NOT_FOUND      },
    { ENOSPC      , ERROR_DISK_FULL          },
    { ENOTEMPTY   , ERROR_DIRECTORY_NOT_EMPTY},
    { EISDIR      , ERROR_OBJECT_WRONG_TYPE  },
    { ETXTBSY     , ERROR_OBJECT_IN_USE      },
    { ENAMETOOLONG, ERROR_OBJECT_TOO_LARGE   },
    { EROFS       , ERROR_WRITE_PROTECTED    },
    { 0           , 0                        }
};

static LONG errno_u2a(int err)
{
    ULONG i;

    for (i = 0; i < sizeof(u2a)/sizeof(u2a[0]); i++)
    {
        if (u2a[i][0] == err)
            return u2a[i][1];
    }

    return ERROR_UNKNOWN;
}

static inline LONG err_u2a(struct emulbase *emulbase)
{
    return errno_u2a(*emulbase->pdata.errnoPtr);
}

/*********************************************************************************************/

/* Make unix protection bits out of AROS protection bits. */
static mode_t prot_a2u(ULONG protect)
{
    mode_t uprot = 0;

    /* The following three flags are low-active! */
    if (!(protect & FIBF_EXECUTE))
        uprot |= S_IXUSR;
    if (!(protect & FIBF_WRITE))
        uprot |= S_IWUSR;
    if (!(protect & FIBF_READ))
        uprot |= S_IRUSR;

    if ((protect & FIBF_GRP_EXECUTE))
        uprot |= S_IXGRP;
    if ((protect & FIBF_GRP_WRITE))
        uprot |= S_IWGRP;
    if ((protect & FIBF_GRP_READ))
        uprot |= S_IRGRP;

    if ((protect & FIBF_OTR_EXECUTE))
        uprot |= S_IXOTH;
    if ((protect & FIBF_OTR_WRITE))
        uprot |= S_IWOTH;
    if ((protect & FIBF_OTR_READ))
        uprot |= S_IROTH;

    if ((protect & FIBF_SCRIPT))
        uprot |= S_ISVTX;

    return uprot;
}

/*********************************************************************************************/

/* Make AROS protection bits out of unix protection bits. */
static ULONG prot_u2a(mode_t protect)
{
    ULONG aprot = 0;

    /* The following three (AROS) flags are low-active! */
    if (!(protect & S_IRUSR))
        aprot |= FIBF_READ;
    if (!(protect & S_IWUSR))
        aprot |= FIBF_WRITE;
    if (!(protect & S_IXUSR))
        aprot |= FIBF_EXECUTE;

    /* The following flags are high-active again. */
    if ((protect & S_IRGRP))
        aprot |= FIBF_GRP_READ;
    if ((protect & S_IWGRP))
        aprot |= FIBF_GRP_WRITE;
    if ((protect & S_IXGRP))
        aprot |= FIBF_GRP_EXECUTE;

    if ((protect & S_IROTH))
        aprot |= FIBF_OTR_READ;
    if ((protect & S_IWOTH))
        aprot |= FIBF_OTR_WRITE;
    if ((protect & S_IXOTH))
        aprot |= FIBF_OTR_EXECUTE;

    if ((protect & S_ISVTX))
        aprot |= FIBF_SCRIPT;

    return aprot;
}

/*********************************************************************************************/

static void timestamp2datestamp(struct emulbase *emulbase, time_t *timestamp, struct DateStamp *datestamp)
{
    struct ClockData date;
    struct tm *tm;

    HostLib_Lock();

    tm = emulbase->pdata.SysIFace->localtime(timestamp);
    AROS_HOST_BARRIER
    
    HostLib_Unlock();

    date.year  = tm->tm_year + 1900;
    date.month = tm->tm_mon + 1;
    date.mday  = tm->tm_mday;
    date.hour  = tm->tm_hour;
    date.min   = tm->tm_min;
    date.sec   = tm->tm_sec;

    ULONG secs = Date2Amiga(&date);

    datestamp->ds_Days = secs / (60 * 60 * 24);
    secs %= (60 * 60 * 24);
    datestamp->ds_Minute = secs / 60;
    secs %= 60;
    datestamp->ds_Tick = secs * TICKS_PER_SECOND;
}

/*********************************************************************************************/

static time_t datestamp2timestamp(struct emulbase *emulbase, struct DateStamp *datestamp)
{
    ULONG secs = datestamp->ds_Days * (60 * 60 * 24) +
                 datestamp->ds_Minute * 60 +
                 datestamp->ds_Tick / TICKS_PER_SECOND;
    
    struct ClockData date;
    struct tm tm;
    time_t ret;

    Amiga2Date(secs, &date);

    tm.tm_year = date.year - 1900;
    tm.tm_mon = date.month - 1;
    tm.tm_mday = date.mday;
    tm.tm_hour = date.hour;
    tm.tm_min = date.min;
    tm.tm_sec = date.sec;
    
    ret = emulbase->pdata.SysIFace->mktime(&tm);
    AROS_HOST_BARRIER

    return ret;
}

/*********************************************************************************************/

#ifdef NO_CASE_SENSITIVITY

static void fixcase(struct emulbase *emulbase, char *pathname)
{
    struct LibCInterface *iface = emulbase->pdata.SysIFace;
    struct dirent       *de;
    struct stat st;
    DIR                 *dir;
    char                *pathstart, *pathend;
    BOOL                dirfound;
    int                 res;
    long                casesens;

    pathstart = pathname;

    res = emulbase->pdata.SysIFace->lstat((const char *)pathname, &st);
    AROS_HOST_BARRIER

    if (res == 0)
        /* Pathname exists, no need to fix anything */
        return;

    while((pathstart = strchr(pathstart, '/')))
    {
        pathstart++;
            
        pathend = strchr(pathstart, '/');
        if (pathend) *pathend = '\0';

        dirfound = TRUE;
            
        res = emulbase->pdata.SysIFace->lstat(pathname, &st);
        AROS_HOST_BARRIER
        if (res != 0)
        {
            dirfound = FALSE;

            pathstart[-1] = '\0';
            dir = emulbase->pdata.SysIFace->opendir(pathname);
            AROS_HOST_BARRIER
            /* R-CASE: ask the just-opened (existing) parent whether the volume
             * is case-SENSITIVE. If so, the exact name not existing is final and
             * we must NOT fold to a different-case sibling. pathconf returns 1
             * for case-sensitive, 0 for the macOS default (case-insensitive),
             * <0 on error — so on the normal Mac this is 0 and behaviour is
             * unchanged. UNVERIFIED across APFS variants; a CASE= mount option
             * could override it later. */
            casesens = emulbase->pdata.SysIFace->pathconf(pathname, _PC_CASE_SENSITIVE);
            AROS_HOST_BARRIER
            pathstart[-1] = '/';

            if (dir && casesens <= 0)
            {
                while(1)
                {
                    de = emulbase->pdata.SysIFace->readdir(dir);
                    AROS_HOST_BARRIER
                    if (!de)
                        break;

                    if (NameHostMatch(emulbase, de->d_name, pathstart))
                    {
                        dirfound = TRUE;
                        strcpy(pathstart, de->d_name);
                        break;
                    }
                }
                iface->closedir(dir);
                AROS_HOST_BARRIER
            }
            else if (dir)
            {
                /* case-sensitive volume: do not case-fold */
                iface->closedir(dir);
                AROS_HOST_BARRIER
            }
        } /* if (stat((const char *)pathname, &st) != 0) */
            
        if (pathend) *pathend = '/';

        if (!dirfound) break;

    } /* while((pathpos = strchr(pathpos, '/))) */
}

#else

#define fixcase(emulbase, pathname)

#endif

/*-------------------------------------------------------------------------------------------*/

static int inline nocase_lstat(struct emulbase *emulbase, char *file_name, struct stat *st)
{
    int ret;

    fixcase(emulbase, file_name);
    ret = emulbase->pdata.SysIFace->lstat(file_name, st);
    AROS_HOST_BARRIER

    return ret;
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_unlink(struct emulbase *emulbase, char *pathname)
{
    int ret;

    fixcase(emulbase, pathname);
    ret = emulbase->pdata.SysIFace->unlink((const char *)pathname);
    AROS_HOST_BARRIER
    
    return ret;
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_mkdir(struct emulbase *emulbase, char *pathname, mode_t mode)
{
    int ret;

    fixcase(emulbase, pathname);
    ret = emulbase->pdata.SysIFace->mkdir(pathname, mode);
    AROS_HOST_BARRIER

    return ret;
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_rmdir(struct emulbase *emulbase, char *pathname)
{
    int ret;

    fixcase(emulbase, pathname);
    ret = emulbase->pdata.SysIFace->rmdir(pathname);
    AROS_HOST_BARRIER
    
    return ret;
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_link(struct emulbase *emulbase, char *oldpath, char *newpath)
{
    int ret;

    fixcase(emulbase, oldpath);
    fixcase(emulbase, newpath);

    ret = emulbase->pdata.SysIFace->link(oldpath, newpath);
    AROS_HOST_BARRIER

    return ret;
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_symlink(struct emulbase *emulbase, char *oldpath, char *newpath)
{
    fixcase(emulbase, oldpath);
    fixcase(emulbase, newpath);

    return emulbase->pdata.SysIFace->symlink(oldpath, newpath);
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_rename(struct emulbase *emulbase, char *oldpath, char *newpath)
{
    struct stat st;
    int ret;
    int changecap = 0;

    if ((strcmp(oldpath, newpath) != 0) && (Stricmp(oldpath, newpath) == 0))
        changecap = 1; /* A request to change capitalisation of name */
    
    fixcase(emulbase, oldpath);
    if (!changecap)
        fixcase(emulbase, newpath);

    /* AmigaDOS Rename does not allow overwriting */
    ret = emulbase->pdata.SysIFace->lstat(newpath, &st);
    AROS_HOST_BARRIER
    if (ret == 0)
        return ERROR_OBJECT_EXISTS;

    ret = emulbase->pdata.SysIFace->rename(oldpath, newpath);
    AROS_HOST_BARRIER

    return ret;
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_chmod(struct emulbase *emulbase, char *path, mode_t mode)
{
    int ret;

    fixcase(emulbase, path);

    ret = emulbase->pdata.SysIFace->chmod(path, mode);
    AROS_HOST_BARRIER
    
    return ret;
}

/*-------------------------------------------------------------------------------------------*/

static inline int nocase_readlink(struct emulbase *emulbase, char *path, char *buffer, SIPTR size)
{
    int ret;

    fixcase(emulbase, path);

    ret = emulbase->pdata.SysIFace->readlink(path, buffer, size);
    AROS_HOST_BARRIER
    
    return ret;
}

static inline int nocase_utime(struct emulbase *emulbase, char *path, const struct utimbuf *times)
{
    int ret;

    fixcase(emulbase, path);
    ret = emulbase->pdata.SysIFace->utime(path, times);
    AROS_HOST_BARRIER
    
    return ret;
}

/*-------------------------------------------------------------------------------------------*/

LONG DoOpen(struct emulbase *emulbase, struct filehandle *fh, LONG access, LONG mode, LONG protect, BOOL AllowDir)
{
    struct stat st;
    LONG ret = ERROR_OBJECT_WRONG_TYPE;
    int r;
    BOOL creating = FALSE;

    DOPEN(bug("[emul] Opening host name: %s\n", fh->hostname));

    HostLib_Lock();

    r = nocase_lstat(emulbase, fh->hostname, &st);
    /* File name case is already adjusted here, so after this we can call UNIX functions directly */

    if (r == -1)
    {
        /* Non-existing objects can be files opened for writing */
        st.st_mode = S_IFREG;
        if ((mode == MODE_NEWFILE) || (mode == MODE_READWRITE))
            creating = TRUE;
    }

    DOPEN(bug("[emul] lstat() returned %d, st_mode is 0x%08X\n", r, st.st_mode));

    if (S_ISREG(st.st_mode))
    {
        /* Object is a plain file */
        int flags = O_RDONLY;
        if (access != ACCESS_READ)
            flags = O_RDWR;

        switch (mode)
        {
        case MODE_NEWFILE:
            flags |= O_TRUNC;
            /* Fallthrough */

        case MODE_READWRITE:
            flags |= O_CREAT;
        }

        r = emulbase->pdata.SysIFace->open(fh->hostname, flags, 0770);
        AROS_HOST_BARRIER

        if (r < 0 && err_u2a(emulbase) == ERROR_WRITE_PROTECTED)
        {
            /* Try again with read-only access. This is needed because AROS
             * FS handlers should only pay attention to R/W protection flags
             * when the corresponding operation is attempted on the file */
            r = emulbase->pdata.SysIFace->open(fh->hostname, O_RDONLY, 0770);
            AROS_HOST_BARRIER
        }
        if (r >= 0)
        {
            if (creating)
            {
                /* Darwin/aarch64 hosted crosses into host libc through a
                 * variadic open() function pointer. Apply the creation mode
                 * again via non-variadic chmod() so fresh files do not inherit
                 * a lost vararg as mode 000.
                 */
                emulbase->pdata.SysIFace->chmod(fh->hostname, 0770);
                AROS_HOST_BARRIER
            }
            fh->type = FHD_FILE;
            fh->fd   = (void *)(IPTR)r;
            ret = 0;
        }
        else
            ret = err_u2a(emulbase);
    }

    if (AllowDir && S_ISDIR(st.st_mode))
    {
        /* Object is a directory */
        fh->fd = emulbase->pdata.SysIFace->opendir(fh->hostname);
#ifndef HOST_OS_android
        if (fh->fd != NULL)
            fh->ph.dirpos_first = emulbase->pdata.SysIFace->telldir(fh->fd);
#endif
        AROS_HOST_BARRIER

        if (fh->fd)
        {
            fh->type = FHD_DIRECTORY;
            ret = 0;
        }
        else
            ret = err_u2a(emulbase);
    }

    if (S_ISLNK(st.st_mode))
        /* Object is a softlink */
        ret = ERROR_IS_SOFT_LINK;

    HostLib_Unlock();

    return ret;
}

void DoClose(struct emulbase *emulbase, struct filehandle *current)
{
    HostLib_Lock();

    switch(current->type)
    {
    case FHD_FILE:
        /* Nothing will happen if type has FHD_STDIO set, this is intentional */
        emulbase->pdata.SysIFace->close((IPTR)current->fd);
        AROS_HOST_BARRIER
        break;

    case FHD_DIRECTORY:
        emulbase->pdata.SysIFace->closedir(current->fd);
        AROS_HOST_BARRIER
        break;
    }

    HostLib_Unlock();
}

LONG DoRead(struct emulbase *emulbase, struct filehandle *fh, APTR buff, ULONG len, SIPTR *err)
{
    SIPTR error;
    int res;

    DREAD(bug("[emul] Reading %u bytes from fd %ld\n", len, fh->fd));

    /* Wait until the fd is ready to read. We reuse UnixIO capabilities for this. */
    error = Hidd_UnixIO_Wait(emulbase->pdata.unixio, (long)fh->fd, vHidd_UnixIO_Read);
    if (error)
    {
        *err = errno_u2a(error);
        return -1;
    }

    HostLib_Lock();

    DREAD(bug("[emul] FD %ld ready for read\n", fh->fd));

    if (fh->type & FHD_STDIO)
    {
        int res2;
        struct pollfd pfd = {(long)fh->fd, POLLIN, 0};

        /*
         * When reading from stdin, we have to read character-by-character until
         * we read as many characters as we wanted, or there's nothing more to read.
         * Without this read() can return an error. For example this happens on Darwin
         * when the shell requests a single read of 208 bytes.
         */
        res = 0;
        do
        {
            res2 = emulbase->pdata.SysIFace->read((long)fh->fd, buff++, 1);
            AROS_HOST_BARRIER

            if (res2 == -1)
                break;

            /* read() == 0 means end-of-file (e.g. the interactive console was
             * closed, or Ctrl-D). Stop here and return the bytes gathered so far
             * rather than counting an unwritten (garbage/NUL) byte - otherwise an
             * interactive Shell spins forever reading NULs instead of seeing EOF. */
            if (res2 == 0)
                break;

            if (res++ == len)
                break;

            res2 = emulbase->pdata.SysIFace->poll(&pfd, 1, 0);
            AROS_HOST_BARRIER

        } while (res2 > 0);

        if (res2 == -1)
            res = -1;
    }
    else
    {
        /* It's not stdin. Read as much as we need to. */
        res = emulbase->pdata.SysIFace->read((long)fh->fd, buff, len);
        AROS_HOST_BARRIER
    }

    if (res == -1)
        error = err_u2a(emulbase);

    HostLib_Unlock();

    DREAD(bug("[emul] Result %d, error %ld\n", len, error));

    *err = error;
    return res;
}

LONG DoWrite(struct emulbase *emulbase, struct filehandle *fh, CONST_APTR buff, ULONG len, SIPTR *err)
{
    SIPTR error = 0;

    DWRITE(bug("[emul] Writing %u bytes to fd %ld\n", len, fh->fd));

    HostLib_Lock();

    len = emulbase->pdata.SysIFace->write((IPTR)fh->fd, buff, len);
    AROS_HOST_BARRIER
    if (len == -1)
        error = err_u2a(emulbase);

    HostLib_Unlock();

    *err = error;
    return len;
}

SIPTR DoSeek(struct emulbase *emulbase, struct filehandle *fh, SIPTR offset, ULONG mode, SIPTR *err)
{
    off_t res;
    SIPTR oldpos = 0, newpos;
    struct stat st;

    DSEEK(bug("[emul] DoSeek(%d, %d, %d)\n", (int)fh->fd, offset, mode));

    HostLib_Lock();

    res = oldpos = LSeek((IPTR)fh->fd, 0, SEEK_CUR);
    AROS_HOST_BARRIER
    if (res != -1)
        res = emulbase->pdata.SysIFace->fstat((int)(IPTR)fh->fd, &st);
    AROS_HOST_BARRIER

    DSEEK(bug("[emul] Original position: %lu\n", (unsigned long)oldpos));

    if (res != -1)
    {
        switch (mode) {
        case OFFSET_BEGINNING:
            newpos = offset;
            mode = SEEK_SET;
            break;

        case OFFSET_CURRENT:
            newpos = offset + res;
            mode = SEEK_CUR;
            break;

        default:
            newpos = offset + st.st_size;
            mode = SEEK_END;
        }

        if (newpos > st.st_size)
            res = -1;
    }

    if (res != -1)
    {
        res = LSeek((IPTR)fh->fd, offset, mode);
        AROS_HOST_BARRIER

        DSEEK(bug("[emul] New position: %lu\n", (unsigned long)res));
    }

    if (res == -1)
        oldpos = -1;

    HostLib_Unlock();

    *err = ERROR_SEEK_ERROR;
    return oldpos;
}

LONG DoMkDir(struct emulbase *emulbase, struct filehandle *fh, ULONG protect)
{
    LONG ret;

    protect = prot_a2u(protect);

    HostLib_Lock();

    ret = nocase_mkdir(emulbase, fh->hostname, protect);
    if (!ret)
    {
        fh->type = FHD_DIRECTORY;
        fh->fd   = emulbase->pdata.SysIFace->opendir(fh->hostname);
#ifndef HOST_OS_android
        if (fh->fd != NULL)
            fh->ph.dirpos_first = emulbase->pdata.SysIFace->telldir(fh->fd);
#endif
        AROS_HOST_BARRIER
    }

    if ((ret == -1) || (fh->fd == NULL))
        ret = err_u2a(emulbase);

    HostLib_Unlock();

    return ret;
}

LONG DoDelete(struct emulbase *emulbase, char *name)
{
    LONG ret;
    struct stat st;

    HostLib_Lock();

    ret = nocase_lstat(emulbase, name, &st);

    if (!ret)
    {
        if (S_ISDIR(st.st_mode))
        {
            ret = emulbase->pdata.SysIFace->rmdir(name);
            AROS_HOST_BARRIER
        }
        else
        {
            ret = emulbase->pdata.SysIFace->unlink(name);
            AROS_HOST_BARRIER
        }
    }

    if (ret)
        ret = err_u2a(emulbase);

    HostLib_Unlock();

    /* R-SIDECAR: drop the metadata sidecar alongside the deleted object. */
    if (!ret)
        MetaDelete(emulbase, name);

    return ret;
}

LONG DoChMod(struct emulbase *emulbase, char *filename, ULONG prot)
{
    LONG ret;
    
    HostLib_Lock();

    ret = nocase_chmod(emulbase, filename, prot_a2u(prot));
    if (ret)
        ret = err_u2a(emulbase);

    HostLib_Unlock();

    /* R-SIDECAR: rwx went to st_mode; persist the AmigaOS-only protection bits
     * (Archive/Pure/Script/Hold) to the sidecar (keeping any existing comment).
     * MetaWrite removes the sidecar if nothing non-default remains. */
    if (!ret)
    {
        HVMeta m;
        MetaRead(emulbase, filename, &m);
        m.prot = prot;
        MetaWrite(emulbase, filename, &m);
    }

    return ret;
}

LONG DoHardLink(struct emulbase *emulbase, char *fn, char *oldfile)
{
    LONG error;

    HostLib_Lock();

    error = nocase_link(emulbase, oldfile, fn);
    if (error)
        error = err_u2a(emulbase);

    HostLib_Unlock();

    return error;
}

LONG DoSymLink(struct emulbase *emulbase, char *dest, char *src)
{
    LONG error;

    HostLib_Lock();

    error = nocase_symlink(emulbase, dest, src);
    if (error)
        error = err_u2a(emulbase);

    HostLib_Unlock();

    return error;
}

int DoReadLink(struct emulbase *emulbase, char *filename, char *buffer, ULONG size, LONG *err)
{
    int res;

    HostLib_Lock();

    res = nocase_readlink(emulbase, filename, buffer, size);
    if (res == -1)
        *err = err_u2a(emulbase);
    else if (res == size)
        /* Buffer was too small */
        res = -2;

    HostLib_Unlock();

    return res;
}

LONG DoRename(struct emulbase *emulbase, char *filename, char *newfilename)
{
    LONG error;

    HostLib_Lock();

    error = nocase_rename(emulbase, filename, newfilename);
    if (error && error != ERROR_OBJECT_EXISTS)
        error = err_u2a(emulbase);

    HostLib_Unlock();

    /* R-SIDECAR: keep the metadata sidecar paired with its renamed data file. */
    if (!error)
        MetaRename(emulbase, filename, newfilename);

    return error;
}

LONG DoSetDate(struct emulbase *emulbase, char *name, struct DateStamp *date)
{
    struct utimbuf times;
    LONG res;

    HostLib_Lock();

    times.actime = datestamp2timestamp(emulbase, date);
    times.modtime = times.actime;

    res = nocase_utime(emulbase, name, &times);
    if (res < 0)
        res = err_u2a(emulbase);

    HostLib_Unlock();

    return res;
}

SIPTR DoSetSize(struct emulbase *emulbase, struct filehandle *fh, SIPTR offset, ULONG mode, SIPTR *err)
{
    SIPTR absolute = 0;
    SIPTR error = 0;

    HostLib_Lock();

    switch (mode) {
    case OFFSET_BEGINNING:
        break;

    case OFFSET_CURRENT:
        absolute = LSeek((IPTR)fh->fd, 0, SEEK_CUR);
        AROS_HOST_BARRIER
        break;

    case OFFSET_END:
        absolute = LSeek((IPTR)fh->fd, 0, SEEK_END);
        AROS_HOST_BARRIER
        break;

    default:
        error = ERROR_UNKNOWN;
    }

    if (absolute == -1)
        error = err_u2a(emulbase);

    if (!error)
    {
        absolute += offset;
        error = FTruncate((IPTR)fh->fd, absolute);
        AROS_HOST_BARRIER
        if (error)
            error = err_u2a(emulbase);
    }

    HostLib_Unlock();

    if (error)
        absolute = -1;

    *err = error;
    return absolute;
}

LONG DoStatFS(struct emulbase *emulbase, char *path, struct InfoData *id)
{
    struct statfs buf;
    LONG err;
    
    HostLib_Lock();

    err = emulbase->pdata.SysIFace->statfs(path, &buf);
    AROS_HOST_BARRIER
    if (err)
        err = err_u2a(emulbase);

    HostLib_Unlock();

    if (!err)
    {
        id->id_NumSoftErrors = 0;
        id->id_DiskState = ID_VALIDATED;
        id->id_NumBlocks = buf.f_blocks;
        id->id_NumBlocksUsed = buf.f_blocks - buf.f_bavail;
        id->id_BytesPerBlock = buf.f_bsize;
    }

    return err;
}

LONG DoRewindDir(struct emulbase *emulbase, struct filehandle *fh)
{
    HostLib_Lock();

    emulbase->pdata.SysIFace->rewinddir(fh->fd);
    AROS_HOST_BARRIER

    HostLib_Unlock();

    /* Directory search position has been reset */
    fh->ph.dirpos = 0;

    /* rewinddir() never fails */
    return 0;
}

static LONG stat_entry(struct emulbase *emulbase, struct filehandle *fh, STRPTR FoundName, struct stat *st)
{
    STRPTR filename, name;
    ULONG plen, flen;
    LONG err = 0;

    DEXAM(bug("[emul] stat_entry(): filehandle's path: %s\n", fh->hostname));
    if (FoundName)
    {
        DEXAM(bug("[emul] ...containing object: %s\n", FoundName));
        plen = strlen(fh->hostname);
        flen = strlen(FoundName);
        name = AllocVecPooled(emulbase->mempool, plen + flen + 2);
        if (NULL == name)
            return ERROR_NO_FREE_STORE;

        strcpy(name, fh->hostname);
        filename = name + plen;
        *filename++ = '/';
        strcpy(filename, FoundName);
    } else
        name = fh->hostname;
  
    DEXAM(bug("[emul] Full name: %s\n", name));

    HostLib_Lock();

    err = emulbase->pdata.SysIFace->lstat(name, st);
    AROS_HOST_BARRIER
    if (err)
        err = err_u2a(emulbase);

    HostLib_Unlock();

    if (FoundName)
    {
        DEXAM(bug("[emul] Freeing full name\n"));
        FreeVecPooled(emulbase->mempool, name);
    }
    return err;
}

LONG DoExamineEntry(struct emulbase *emulbase, struct filehandle *fh, char *EntryName,
                   struct ExAllData *ead, ULONG size, ULONG type)
{
    char arosname[MAXFILENAMELENGTH];
    HVMeta meta;
    int hasmeta = 0;
    STRPTR next, end, last, name;
    struct stat st;
    LONG err;

    DEXAM(bug("[emul] DoExamineEntry(0x%p, %s, 0x%p, %u, %u)\n", fh, EntryName, ead, size, type));

    /* Return an error, if supplied type is not supported. */
    if(type>ED_OWNER)
        return ERROR_BAD_NUMBER;

    /* Check, if the supplied buffer is large enough. */
    next=(STRPTR)ead+sizes[type];
    end =(STRPTR)ead+size;
    DEXAM(bug("[emul] ead 0x%p, next 0x%p, end 0x%p\n", ead, next, end));

    if(next>end) /* > is correct. Not >= */
        return ERROR_BUFFER_OVERFLOW;

    err = stat_entry(emulbase, fh, EntryName, &st);
    if (err)
        return err;

    /* R-SIDECAR: read the entry's sidecar once for the comment + extra prot. */
    {
        char ep[1024];
        entry_hostpath(fh, EntryName, ep, sizeof ep);
        hasmeta = (MetaRead(emulbase, ep, &meta) == 1);
    }

    DEXAM(KrnPrintf("[emul] File mode %o, size %u\n", st.st_mode, st.st_size));
    DEXAM(KrnPrintf("[emul] Filling in information\n"));
    DEXAM(KrnPrintf("[emul] ead 0x%p, next 0x%p, end 0x%p, size %u, type %u\n", ead, next, end, size, type));

    switch(type)
    {
        default:
        case ED_OWNER:
            ead->ed_OwnerUID    = st.st_uid;
            ead->ed_OwnerGID    = st.st_gid;
        case ED_COMMENT:
            ead->ed_Comment=next;
            {
                const char *cm = hasmeta ? meta.comment : "";
                while (*cm) {
                    if (next >= end)
                        return ERROR_BUFFER_OVERFLOW;
                    *next++ = *cm++;
                }
            }
            *next = '\0'; next++;
            if(next>=end)
                return ERROR_BUFFER_OVERFLOW;
        case ED_DATE:
        {
            struct DateStamp stamp;

            timestamp2datestamp(emulbase, &st.st_mtime, &stamp);
            ead->ed_Days        = stamp.ds_Days;
            ead->ed_Mins        = stamp.ds_Minute;
            ead->ed_Ticks       = stamp.ds_Tick;
        }
        case ED_PROTECTION:
            ead->ed_Prot        = prot_u2a(st.st_mode);
            if (hasmeta)
                ead->ed_Prot |= (meta.prot & HV_FIBF_AMIGA_ONLY);
        case ED_SIZE:
            ead->ed_Size        = st.st_size;
        case ED_TYPE:
            if (S_ISDIR(st.st_mode)) {
                if (EntryName || fh->name[0])
                    ead->ed_Type = ST_USERDIR;
                else
                    ead->ed_Type = ST_ROOT;
            } else if (S_ISLNK(st.st_mode))
                ead->ed_Type = ST_SOFTLINK;
            else
                ead->ed_Type = ST_FILE;
            
        case ED_NAME:
            if (EntryName)
                last = EntryName;
            else if (*fh->name) {
                name = fh->name;
                last = name;
                while(*name) {
                    if(*name++ == '/')
                        last = name;
                }
            } else
                last = fh->volumename;

            /* A dir entry or the handle's host path is host bytes (UTF-8) and
             * needs charset+NFC translation to the AROS name; the volume name
             * is already AROS text and passes through unchanged. */
            if (last != (STRPTR)fh->volumename) {
                NameToAros(last, arosname, sizeof arosname);
                last = arosname;
            }

            ead->ed_Name=next;
            for(;;)
            {
                if(next>=end)
                    return ERROR_BUFFER_OVERFLOW;
                if(!(*next++=*last++))
                    break;
            }
        case 0:
            ead->ed_Next=(struct ExAllData *)(((IPTR)next+AROS_PTRALIGN-1)&~(AROS_PTRALIGN-1));
            return 0;
    }
}

/*********************************************************************************************/

LONG DoExamineNext(struct emulbase *emulbase, struct filehandle *fh,
                  struct FileInfoBlock *FIB)
{
    ULONG i;
    struct stat st;
    struct dirent *dir;
    LONG err;

    /* This operation does not make any sense on a file */
    if (fh->type != FHD_DIRECTORY)
        return ERROR_OBJECT_WRONG_TYPE;

    HostLib_Lock();

    /*
     * First of all we have to go to the position where Examine() or
     * ExNext() stopped the previous time so we can read the next entry!
     * On Android this is handled by ReadDir() artificially tracking
     * current search position in the filehandle.
     */
#ifndef HOST_OS_android
    emulbase->pdata.SysIFace->seekdir(fh->fd, FIB->fib_DiskKey);
    AROS_HOST_BARRIER
#endif

    /* hm, let's read the data now! */
    dir = ReadDir(emulbase, fh, &FIB->fib_DiskKey);

    HostLib_Unlock();

    if (!dir)
        return ERROR_NO_MORE_ENTRIES;

    err = stat_entry(emulbase, fh, dir->d_name, &st);
    if (err)
    {
        DEXAM(bug("stat_entry() failed for %s\n", dir->d_name));
        return err;
    }

    DEXAM(KrnPrintf("[emul] File mode %o, size %u\n", st.st_mode, st.st_size));

    FIB->fib_OwnerUID   = st.st_uid;
    FIB->fib_OwnerGID   = st.st_gid;
    timestamp2datestamp(emulbase, &st.st_mtime, &FIB->fib_Date);
    FIB->fib_Protection = prot_u2a(st.st_mode);
    FIB->fib_Size       = st.st_size;

    /* R-SIDECAR: comment + AmigaOS-only protection bits from ".<name>.amimeta" */
    {
        char ep[1024];
        entry_hostpath(fh, dir->d_name, ep, sizeof ep);
        apply_meta_fib(emulbase, ep, &FIB->fib_Protection,
                       FIB->fib_Comment, sizeof FIB->fib_Comment);
    }

    if (S_ISDIR(st.st_mode))
    {
        FIB->fib_DirEntryType = ST_USERDIR; /* S_ISDIR(st.st_mode)?(*fh->name?ST_USERDIR:ST_ROOT):0*/
    }
    else if(S_ISLNK(st.st_mode))
    {
        FIB->fib_DirEntryType = ST_SOFTLINK;
    }
    else
    {
        FIB->fib_DirEntryType = ST_FILE;
    }

    DEXAM(bug("[emul] DirentryType %d\n", FIB->fib_DirEntryType));

    /* host name -> AROS name (charset + NFC), then set the BSTR length byte */
    i = NameToAros(dir->d_name, (char *)&FIB->fib_FileName[1], MAXFILENAMELENGTH - 1);
    if (i > MAXFILENAMELENGTH - 1)
        i = MAXFILENAMELENGTH - 1;
    FIB->fib_FileName[0] = i;

    return 0;
}

/*********************************************************************************************/

LONG DoExamineAll(struct emulbase *emulbase, struct filehandle *fh, struct ExAllData *ead,
                  struct ExAllControl *eac, ULONG size, ULONG type, struct DosLibrary *DOSBase)
{
    struct ExAllData *last=NULL;
    STRPTR end=(STRPTR)ead+size;
    struct dirent *dir;
    LONG error = 0;
#ifndef HOST_OS_android
    SIPTR oldpos;
#endif

    eac->eac_Entries = 0;
    if(fh->type!=FHD_DIRECTORY)
        return ERROR_OBJECT_WRONG_TYPE;

    DEXAM(bug("[emul] examine_all()\n"));


#ifndef HOST_OS_android
    HostLib_Lock();

    if (eac->eac_LastKey == 0)
    {
        /* Theoretically this doesn't work if opendir/telldir "handle"
           can be 0 for a dir entry which is not the first one! */
           
        eac->eac_LastKey = fh->ph.dirpos_first;
    }
    
    emulbase->pdata.SysIFace->seekdir(fh->fd, eac->eac_LastKey);
    AROS_HOST_BARRIER

    HostLib_Unlock();
#endif

    for(;;)
    {
        HostLib_Lock();

#ifndef HOST_OS_android
        oldpos = eac->eac_LastKey;
                
        //oldpos = emulbase->pdata.SysIFace->telldir(fh->fd);
        //AROS_HOST_BARRIER
#endif

        *emulbase->pdata.errnoPtr = 0;
        dir = ReadDir(emulbase, fh, &eac->eac_LastKey);

        if (!dir)
            error = err_u2a(emulbase);

        HostLib_Unlock();

        if (!dir)
            break;

        DEXAM(bug("[emul] Found entry %s\n", dir->d_name));

        if (eac->eac_MatchString && !MatchPatternNoCase(eac->eac_MatchString, dir->d_name)) {
            DEXAM(bug("[emul] Entry does not match, skipping\n"));
            continue;
        }

        error = DoExamineEntry(emulbase, fh, dir->d_name, ead, end-(STRPTR)ead, type);
        if(error)
            break;

        if ((eac->eac_MatchFunc) && !CALLHOOKPKT(eac->eac_MatchFunc, ead, &type))
          continue;

        eac->eac_Entries++;
        last=ead;
        ead=ead->ed_Next;
    }
    if (last!=NULL)
        last->ed_Next=NULL;

    if ((error==ERROR_BUFFER_OVERFLOW) && last)
    {
#ifdef HOST_OS_android
        eac->eac_LastKey--;
#else
        eac->eac_LastKey = oldpos;

        //HostLib_Lock();
        //emulbase->pdata.SysIFace->seekdir(fh->fd, oldpos);
        //AROS_HOST_BARRIER
        //HostLib_Unlock();
#endif
        /* Examination will continue from the current position */
        return 0;
    }

    if(!error)
        error = ERROR_NO_MORE_ENTRIES;
    /* Reading the whole directory has been completed, so reset position */
    DoRewindDir(emulbase, fh);

    return error;
}

/* Read a host environment variable and return a private (mempool) copy of its
 * value, or NULL if unset. Used by the AROS_HOST_VOLUME launcher hook to mount
 * a host folder our way (so our ;WRITE keyword reaches new_volume intact). */
char *GetHostEnv(struct emulbase *emulbase, const char *name)
{
    char *val, *copy = NULL;

    HostLib_Lock();
    val = emulbase->pdata.SysIFace->getenv((char *)name);
    AROS_HOST_BARRIER
    if (val && val[0])
    {
        int len = strlen(val);

        copy = AllocVecPooled(emulbase->mempool, len + 1);
        if (copy)
            CopyMem(val, copy, len + 1);
    }
    HostLib_Unlock();

    return copy;
}

char *GetHomeDir(struct emulbase *emulbase, char *sp)
{
    char *home = NULL;
    char *newunixpath = NULL;
    char *sp_end;
#ifndef HOST_OS_android
    BOOL do_endpwent = FALSE;
#endif

    HostLib_Lock();

    /* "~<name>" means home of user <name> */
    if ((sp[1] == '\0') || (sp[1] == '/'))
    {
        sp_end = sp + 1;
        home = emulbase->pdata.SysIFace->getenv("HOME");
        AROS_HOST_BARRIER
    }
#ifndef HOST_OS_android
    else
    {
        struct passwd *pwd;
        WORD           cmplen;
                
        for(sp_end = sp + 1; sp_end[0] != '\0' && sp_end[0] != '/'; sp_end++);
        cmplen = sp_end - sp - 1;

        while(1)
        {
            pwd = emulbase->pdata.SysIFace->getpwent();
            AROS_HOST_BARRIER

            if (!pwd)
                break;

            if(memcmp(pwd->pw_name, sp + 1, cmplen) == 0)
            {
                if (pwd->pw_name[cmplen] == '\0')
                {
                    home = pwd->pw_dir;
                    break;
                }
            }
        }
        do_endpwent = TRUE;
    }
#endif

    if (home)
    {
        int hlen = strlen(home);
        int splen = strlen(sp_end);

        newunixpath = AllocVecPooled(emulbase->mempool, hlen + splen + 1);
        if (newunixpath)
        {
            char *s = newunixpath;

            CopyMem(home, s, hlen);
            s += hlen;
            CopyMem(sp_end, s, splen);
            s += splen;
            *s = 0;
        }
    }

#ifndef HOST_OS_android
    if (do_endpwent)
    {
        emulbase->pdata.SysIFace->endpwent();
        AROS_HOST_BARRIER
    }
#endif

    HostLib_Unlock();

    return newunixpath;
}

ULONG GetCurrentDir(struct emulbase *emulbase, char *path, ULONG len)
{
    char *res;

    DMOUNT(bug("[emul] GetCurrentDir(0x%p, %u)\n", path, len));

    HostLib_Lock();

    res = emulbase->pdata.SysIFace->getcwd(path, len);
    AROS_HOST_BARRIER

    HostLib_Unlock();

    DMOUNT(bug("[emul] getcwd() returned %s\n", res));
    return res ? TRUE : FALSE;
}

BOOL CheckDir(struct emulbase *emulbase, char *path)
{
    int res;
    struct stat st;

    DMOUNT(bug("[emul] CheckDir(%s)\n", path));

    HostLib_Lock();

    res = emulbase->pdata.SysIFace->stat(path, &st);
    AROS_HOST_BARRIER

    HostLib_Unlock();

    DMOUNT(bug("[emul] Result: %d, mode: %o\n", res, st.st_mode));
    if ((!res) && S_ISDIR(st.st_mode))
        return FALSE;

    return TRUE;
}
