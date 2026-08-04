/*
 * exfat-handler - read-only AmigaDOS packet surface
 *
 * Copyright (C) 2026 The AROS Development Team
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dos64.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include <proto/exec.h>

#include <string.h>

#include "exfat_fs.h"
#include "exfat_protos.h"

static LONG TestLock(struct exfat_lock *lock, struct Globals *glob)
{
    if (lock == NULL)
        return glob->sb != NULL ? 0 : ERROR_DEVICE_NOT_MOUNTED;
    if (lock->magic != EXFAT_LOCK_MAGIC || lock->sb == NULL)
        return ERROR_INVALID_LOCK;
    if (lock->sb != glob->sb || lock->fl_Volume != MKBADDR(lock->sb->doslist))
        return ERROR_DEVICE_NOT_MOUNTED;
    return 0;
}

static void RootEntry(struct FSSuper *sb, struct exfat_entry *entry)
{
    ULONG i;
    memset(entry, 0, sizeof(*entry));
    entry->stream = sb->root;
    entry->attributes = EXFAT_ATTR_DIRECTORY;
    entry->name_length = sb->volume.name[0];
    for (i = 0; i < entry->name_length; i++)
        entry->name[i] = sb->volume.name[i + 1];
}

static struct exfat_lock *NewLock(struct FSSuper *sb,
    const struct exfat_entry *entry, const struct exfat_stream *parent,
    CONST_STRPTR path, ULONG path_length, LONG access)
{
    struct Globals *glob = sb->glob;
    struct exfat_lock *lock;

    if (path_length > EXFAT_MAX_PATH)
        return NULL;
    lock = AllocVec(sizeof(*lock), MEMF_PUBLIC | MEMF_CLEAR);
    if (lock == NULL)
        return NULL;
    lock->fl_Access = access;
    lock->fl_Task = sb->glob->ourport;
    lock->fl_Volume = MKBADDR(sb->doslist);
    lock->fl_Key = (IPTR)lock;
    lock->magic = EXFAT_LOCK_MAGIC;
    lock->sb = sb;
    lock->entry = *entry;
    lock->parent = *parent;
    lock->enum_index = 0;
    lock->path_length = (UWORD)path_length;
    if (path_length != 0)
        CopyMem(path, lock->path, path_length);
    lock->path[path_length] = 0;
    sb->lock_count++;
    return lock;
}

static void DropLock(struct exfat_lock *lock)
{
    if (lock != NULL && lock->magic == EXFAT_LOCK_MAGIC)
    {
        struct Globals *glob = lock->sb->glob;
        lock->magic = 0;
        lock->sb->lock_count--;
        FreeVec(lock);
    }
}

static LONG ResolvePath(struct FSSuper *sb, struct exfat_lock *base,
    CONST_STRPTR path, ULONG length, struct exfat_entry *result,
    struct exfat_stream *parent, UBYTE *canonical, ULONG *canonical_length)
{
    struct exfat_entry current, found;
    ULONG at = 0, colon = length, outlen = 0;

    while (colon > 0 && path[colon - 1] != ':')
        colon--;
    if (colon != 0)
    {
        at = colon;
        RootEntry(sb, &current);
    }
    else if (base != NULL)
    {
        current = base->entry;
        outlen = base->path_length;
        memcpy(canonical, base->path, outlen);
    }
    else
        RootEntry(sb, &current);

    while (at < length)
    {
        ULONG start = at, n;

        while (at < length && path[at] != '/')
            at++;
        n = at - start;
        if (n != 0)
        {
            LONG err;
            if ((current.attributes & EXFAT_ATTR_DIRECTORY) == 0)
                return ERROR_OBJECT_WRONG_TYPE;
            *parent = current.stream;
            err = ExfatFindEntry(sb, &current.stream, path + start, n, &found);
            if (err != 0)
                return err;
            current = found;
            if (outlen != 0)
            {
                if (outlen == EXFAT_MAX_PATH)
                    return ERROR_LINE_TOO_LONG;
                canonical[outlen++] = '/';
            }
            if (n > EXFAT_MAX_PATH - outlen)
                return ERROR_LINE_TOO_LONG;
            memcpy(canonical + outlen, path + start, n);
            outlen += n;
        }
        else if (at < length)
        {
            /* AmigaDOS empty path component means parent. Resolve it from
               the canonical root-relative path. */
            ULONG cut = outlen;
            while (cut > 0 && canonical[cut - 1] != '/')
                cut--;
            outlen = cut != 0 ? cut - 1 : 0;
            if (outlen == 0)
                RootEntry(sb, &current);
            else
            {
                ULONG dummy = 0;
                UBYTE rebuilt[EXFAT_MAX_PATH + 1];
                RootEntry(sb, &current);
                if (ResolvePath(sb, NULL, (STRPTR)canonical, outlen,
                        &current, parent, rebuilt, &dummy) != 0)
                    return ERROR_OBJECT_NOT_FOUND;
            }
        }
        if (at < length)
            at++;
    }

    *result = current;
    *canonical_length = outlen;
    canonical[outlen] = 0;
    return 0;
}

static LONG LockPath(struct Globals *glob, struct exfat_lock *base,
    CONST_STRPTR path, ULONG length, LONG access, struct exfat_lock **out)
{
    struct exfat_entry entry;
    struct exfat_stream parent = glob->sb->root;
    UBYTE canonical[EXFAT_MAX_PATH + 1];
    ULONG canonical_length = 0;
    LONG err;

    err = ResolvePath(glob->sb, base, path, length, &entry, &parent,
        canonical, &canonical_length);
    if (err != 0)
        return err;
    *out = NewLock(glob->sb, &entry, &parent, (STRPTR)canonical,
        canonical_length, access);
    return *out != NULL ? 0 : ERROR_NO_FREE_STORE;
}

static LONG CopyLock(struct exfat_lock *source, struct exfat_lock **out)
{
    *out = NewLock(source->sb, &source->entry, &source->parent,
        (STRPTR)source->path, source->path_length, source->fl_Access);
    if (*out == NULL)
        return ERROR_NO_FREE_STORE;
    (*out)->position = source->position;
    (*out)->enum_index = source->enum_index;
    return 0;
}

static LONG ParentLock(struct Globals *glob, struct exfat_lock *source,
    struct exfat_lock **out)
{
    ULONG len;
    if (source == NULL || source->path_length == 0)
    {
        *out = NULL;
        return 0;
    }
    len = source->path_length;
    while (len > 0 && source->path[len - 1] != '/')
        len--;
    if (len != 0)
        len--;
    return LockPath(glob, NULL, (STRPTR)source->path, len,
        SHARED_LOCK, out);
}

static void FillFIB(const struct exfat_entry *entry, struct FileInfoBlock *fib,
    ULONG disk_key)
{
    TEXT name[MAXFILENAMELENGTH];
    ULONG len;
    UQUAD blocks;
    ULONG packed = entry->modify_timestamp;
    ULONG year = 1980 + (packed >> 25);
    ULONG month = (packed >> 21) & 15;
    ULONG day = (packed >> 16) & 31;
    ULONG hour = (packed >> 11) & 31;
    ULONG minute = (packed >> 5) & 63;
    ULONG second = (packed & 31) * 2;
    ULONG days = 730; /* 1978-01-01 to 1980-01-01 */
    ULONG y, m;
    static const UBYTE month_days[12] =
        { 31,28,31,30,31,30,31,31,30,31,30,31 };

    memset(fib, 0, sizeof(*fib));
    ExfatNameToLocal(entry, name, sizeof(name));
    len = strlen(name);
    if (len > sizeof(fib->fib_FileName) - 2)
        len = sizeof(fib->fib_FileName) - 2;
    fib->fib_DiskKey = disk_key;
    fib->fib_DirEntryType = (entry->attributes & EXFAT_ATTR_DIRECTORY)
        ? ST_USERDIR : ST_FILE;
    fib->fib_EntryType = fib->fib_DirEntryType;
    fib->fib_FileName[0] = (UBYTE)len;
    memcpy(fib->fib_FileName + 1, name, len);
    fib->fib_FileName[len + 1] = 0;
    fib->fib_Protection = FIBF_WRITE | FIBF_DELETE;
    if (entry->attributes & EXFAT_ATTR_ARCHIVE)
        fib->fib_Protection |= FIBF_ARCHIVE;
    fib->fib_Size = entry->stream.data_length;
    blocks = entry->stream.data_length >> 9;
    if ((entry->stream.data_length & 511) != 0)
        blocks++;
    fib->fib_NumBlocks = blocks;

    if (month >= 1 && month <= 12 && day >= 1 && day <= 31
        && hour < 24 && minute < 60)
    {
        for (y = 1980; y < year; y++)
            days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
                ? 366 : 365;
        for (m = 1; m < month; m++)
        {
            days += month_days[m - 1];
            if (m == 2 && ((year % 4 == 0 && year % 100 != 0)
                || year % 400 == 0))
                days++;
        }
        days += day - 1;
        fib->fib_Date.ds_Days = days;
        fib->fib_Date.ds_Minute = hour * 60 + minute;
        fib->fib_Date.ds_Tick = second * TICKS_PER_SECOND
            + entry->modify_10ms / 2;
    }
}

static LONG SeekPosition(struct exfat_lock *lock, QUAD offset, LONG mode,
    UQUAD *old)
{
    UQUAD next;
    UQUAD magnitude;
    *old = lock->position;
    if (mode == OFFSET_BEGINNING)
    {
        if (offset < 0)
            return ERROR_SEEK_ERROR;
        next = (UQUAD)offset;
    }
    else if (mode == OFFSET_CURRENT)
    {
        if (offset < 0)
        {
            magnitude = (UQUAD)(-(offset + 1)) + 1;
            if (magnitude > lock->position)
                return ERROR_SEEK_ERROR;
            next = lock->position - magnitude;
        }
        else
        {
            magnitude = (UQUAD)offset;
            if (magnitude > lock->entry.stream.data_length - lock->position)
                return ERROR_SEEK_ERROR;
            next = lock->position + magnitude;
        }
    }
    else if (mode == OFFSET_END)
    {
        if (offset > 0)
            return ERROR_SEEK_ERROR;
        magnitude = offset < 0 ? (UQUAD)(-(offset + 1)) + 1 : 0;
        if (magnitude > lock->entry.stream.data_length)
            return ERROR_SEEK_ERROR;
        next = lock->entry.stream.data_length - magnitude;
    }
    else
        return ERROR_SEEK_ERROR;
    if (next > lock->entry.stream.data_length)
        return ERROR_SEEK_ERROR;
    lock->position = next;
    return 0;
}

static BOOL IsWriteAction(LONG type)
{
    switch (type)
    {
    case ACTION_WRITE:
    case ACTION_DELETE_OBJECT:
    case ACTION_RENAME_OBJECT:
    case ACTION_CREATE_DIR:
    case ACTION_SET_PROTECT:
    case ACTION_SET_DATE:
    case ACTION_SET_COMMENT:
    case ACTION_SET_FILE_SIZE:
    case ACTION_FORMAT:
    case ACTION_RENAME_DISK:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE:
    case ACTION_CHANGE_FILE_SIZE64:
    case ACTION_SET_FILE_SIZE64:
        return TRUE;
    default:
        return FALSE;
    }
}

void ExfatProcessPackets(struct Globals *glob)
{
    struct Message *msg;
    struct DosPacket *dp;

    while ((msg = GetMsg(glob->ourport)) != NULL)
    {
        IPTR res = DOSFALSE;
        LONG err = 0;
        dp = (struct DosPacket *)msg->mn_Node.ln_Name;

        if (IsWriteAction(dp->dp_Type))
            err = ERROR_DISK_WRITE_PROTECTED;
        else switch (dp->dp_Type)
        {
        case ACTION_LOCATE_OBJECT:
        {
            struct exfat_lock *base = BADDR(dp->dp_Arg1), *lock;
            err = TestLock(base, glob);
            if (err == 0)
                err = LockPath(glob, base, AROS_BSTR_ADDR(dp->dp_Arg2),
                    AROS_BSTR_strlen(dp->dp_Arg2), (LONG)dp->dp_Arg3, &lock);
            if (err == 0)
                res = (IPTR)MKBADDR(lock);
            break;
        }
        case ACTION_FREE_LOCK:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            err = TestLock(lock, glob);
            if (err == 0) { DropLock(lock); res = DOSTRUE; }
            break;
        }
        case ACTION_COPY_DIR:
        case ACTION_COPY_DIR_FH:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1), *copy;
            err = TestLock(lock, glob);
            if (err == 0 && lock == NULL)
                err = LockPath(glob, NULL, "", 0, SHARED_LOCK, &copy);
            else if (err == 0)
                err = CopyLock(lock, &copy);
            if (err == 0) res = (IPTR)MKBADDR(copy);
            break;
        }
        case ACTION_PARENT:
        case ACTION_PARENT_FH:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1), *parent;
            err = TestLock(lock, glob);
            if (err == 0) err = ParentLock(glob, lock, &parent);
            if (err == 0) res = parent != NULL ? (IPTR)MKBADDR(parent) : 0;
            break;
        }
        case ACTION_SAME_LOCK:
        {
            struct exfat_lock *a = BADDR(dp->dp_Arg1), *b = BADDR(dp->dp_Arg2);
            err = TestLock(a, glob);
            if (err == 0) err = TestLock(b, glob);
            if (err == 0 && ((a == NULL && b == NULL)
                || (a != NULL && b != NULL && a->path_length == b->path_length
                    && memcmp(a->path, b->path, a->path_length) == 0)))
                res = DOSTRUE;
            break;
        }
        case ACTION_EXAMINE_OBJECT:
        case ACTION_EXAMINE_FH:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            struct FileInfoBlock *fib = BADDR(dp->dp_Arg2);
            struct exfat_entry root;
            err = TestLock(lock, glob);
            if (err == 0)
            {
                if (lock == NULL) { RootEntry(glob->sb, &root); FillFIB(&root, fib, 0); }
                else { lock->enum_index = 0; FillFIB(&lock->entry, fib, 0); }
                res = DOSTRUE;
            }
            break;
        }
        case ACTION_EXAMINE_NEXT:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            struct FileInfoBlock *fib = BADDR(dp->dp_Arg2);
            struct exfat_entry entry;
            ULONG index;
            err = TestLock(lock, glob);
            if (err == 0 && lock != NULL
                && (lock->entry.attributes & EXFAT_ATTR_DIRECTORY) == 0)
                err = ERROR_OBJECT_WRONG_TYPE;
            if (err == 0)
            {
                index = (ULONG)fib->fib_DiskKey;
                err = ExfatNextEntry(glob->sb,
                    lock != NULL ? &lock->entry.stream : &glob->sb->root,
                    &index, &entry);
                if (err == ERROR_NO_MORE_ENTRIES) err = ERROR_NO_MORE_ENTRIES;
                if (err == 0) { FillFIB(&entry, fib, index); res = DOSTRUE; }
            }
            break;
        }
        case ACTION_FINDINPUT:
        {
            struct FileHandle *fh = BADDR(dp->dp_Arg1);
            struct exfat_lock *base = BADDR(dp->dp_Arg2), *lock;
            err = TestLock(base, glob);
            if (err == 0)
                err = LockPath(glob, base, AROS_BSTR_ADDR(dp->dp_Arg3),
                    AROS_BSTR_strlen(dp->dp_Arg3), SHARED_LOCK, &lock);
            if (err == 0 && (lock->entry.attributes & EXFAT_ATTR_DIRECTORY))
            { DropLock(lock); err = ERROR_OBJECT_WRONG_TYPE; }
            if (err == 0)
            { fh->fh_Arg1 = (IPTR)MKBADDR(lock); fh->fh_Port = DOSFALSE; res = DOSTRUE; }
            break;
        }
        case ACTION_READ:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            ULONG got = 0;
            err = TestLock(lock, glob);
            if (err == 0 && lock == NULL) err = ERROR_INVALID_LOCK;
            if (err == 0)
                err = ExfatReadStream(lock->sb, &lock->entry.stream,
                    lock->position, (APTR)dp->dp_Arg2, (ULONG)dp->dp_Arg3, &got);
            if (err == 0) { lock->position += got; res = got; }
            else res = -1;
            break;
        }
        case ACTION_SEEK:
        case ACTION_SEEK64:
        case ACTION_CHANGE_FILE_POSITION64:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            UQUAD old;
            err = TestLock(lock, glob);
            if (err == 0 && lock == NULL) err = ERROR_INVALID_LOCK;
            if (err == 0)
                err = SeekPosition(lock, (QUAD)dp->dp_Arg2,
                    (LONG)dp->dp_Arg3, &old);
            if (err == 0)
                res = dp->dp_Type == ACTION_CHANGE_FILE_POSITION64
                    ? DOSTRUE : (IPTR)old;
            else res = -1;
            break;
        }
        case ACTION_GET_FILE_POSITION64:
        case ACTION_GET_FILE_SIZE64:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            err = TestLock(lock, glob);
            if (err == 0 && lock == NULL) err = ERROR_INVALID_LOCK;
            if (err == 0) res = (IPTR)(dp->dp_Type == ACTION_GET_FILE_POSITION64
                ? lock->position : lock->entry.stream.data_length);
            else res = -1;
            break;
        }
        case ACTION_END:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            err = TestLock(lock, glob);
            if (err == 0) { DropLock(lock); res = DOSTRUE; }
            break;
        }
        case ACTION_INFO:
        case ACTION_DISK_INFO:
        {
            struct InfoData *id = BADDR((dp->dp_Type == ACTION_INFO
                ? dp->dp_Arg2 : dp->dp_Arg1));
            memset(id, 0, sizeof(*id));
            if (glob->sb == NULL) { id->id_DiskType = ID_NO_DISK_PRESENT; err = ERROR_NO_DISK; }
            else
            {
                id->id_DiskState = ID_WRITE_PROTECTED;
                id->id_NumBlocks = glob->sb->cluster_count;
                id->id_NumBlocksUsed = glob->sb->cluster_count - glob->sb->free_clusters;
                id->id_BytesPerBlock = glob->sb->cluster_size;
                id->id_DiskType = ID_EXFAT_DISK;
                id->id_VolumeNode = MKBADDR(glob->sb->doslist);
                id->id_InUse = glob->sb->lock_count != 0;
                res = DOSTRUE;
            }
            break;
        }
        case ACTION_CURRENT_VOLUME:
            res = glob->sb != NULL ? (IPTR)MKBADDR(glob->sb->doslist) : 0;
            break;
        case ACTION_INHIBIT:
            if (dp->dp_Arg1 == DOSTRUE) { glob->disk_inhibited++; if (glob->disk_inhibited == 1 && glob->sb != NULL && glob->sb->lock_count == 0) DoDiskRemove(glob); }
            else if (glob->disk_inhibited != 0) { glob->disk_inhibited--; if (glob->disk_inhibited == 0) DoDiskInsert(glob); }
            res = DOSTRUE;
            break;
        case ACTION_DIE:
            if (glob->sb != NULL && glob->sb->lock_count != 0)
                err = ERROR_OBJECT_IN_USE;
            else { glob->quit = TRUE; glob->devnode->dol_Task = NULL; res = DOSTRUE; }
            break;
        case ACTION_IS_FILESYSTEM:
            res = DOSTRUE;
            break;
        default:
            err = ERROR_ACTION_NOT_KNOWN;
            break;
        }

        dp->dp_Res1 = res;
        dp->dp_Res2 = err;
        {
            struct MsgPort *reply = dp->dp_Port;
            dp->dp_Port = glob->ourport;
            msg->mn_Node.ln_Name = (STRPTR)dp;
            PutMsg(reply, msg);
        }
    }
}
