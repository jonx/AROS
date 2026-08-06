/*
 * exfat-handler - AmigaDOS packet surface
 *
 * Copyright (C) 2026 The AROS Development Team
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dos64.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "exfat_fs.h"
#include "exfat_protos.h"

static LONG TestLock(struct exfat_lock *lock, struct Globals *glob)
{
    if (lock == NULL)
        return glob->sb != NULL ? 0 : ERROR_DEVICE_NOT_MOUNTED;
    if (lock->magic != EXFAT_LOCK_MAGIC || lock->sb == NULL)
        return ERROR_INVALID_LOCK;
    if (!lock->sb->online || lock->sb != glob->sb
        || lock->fl_Volume != MKBADDR(lock->sb->doslist))
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
    struct exfat_lock *lock, *other;

    if (path_length > EXFAT_MAX_PATH)
    {
        SetIoErr(ERROR_LINE_TOO_LONG);
        return NULL;
    }
    for (other = sb->locks; other != NULL; other = other->next)
    {
        BOOL same_object = (path_length == 0 && other->path_length == 0)
            || (path_length != 0 && other->path_length != 0
                && parent->first_cluster == other->parent.first_cluster
                && entry->directory_index == other->entry.directory_index);

        if (same_object && (access == EXCLUSIVE_LOCK
                || other->fl_Access == EXCLUSIVE_LOCK))
        {
            SetIoErr(ERROR_OBJECT_IN_USE);
            return NULL;
        }
    }
    lock = AllocVec(sizeof(*lock), MEMF_PUBLIC | MEMF_CLEAR);
    if (lock == NULL)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return NULL;
    }
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
    lock->next = sb->locks;
    sb->locks = lock;
    sb->lock_count++;
    return lock;
}

static void DropLock(struct exfat_lock *lock)
{
    if (lock != NULL && lock->magic == EXFAT_LOCK_MAGIC)
    {
        struct FSSuper *sb = lock->sb;
        struct Globals *glob = sb->glob;
        struct exfat_lock **link;

        if (lock->write_transaction && sb->online)
            (void)ExfatAbortWrite(sb);
        if (sb->write_handle == lock)
            sb->write_handle = NULL;
        for (link = &sb->locks; *link != NULL; link = &(*link)->next)
        {
            if (*link == lock)
            {
                *link = lock->next;
                break;
            }
        }
        lock->magic = 0;
        sb->lock_count--;
        FreeVec(lock);
        if (!sb->online && sb->lock_count == 0)
        {
            BOOL inserted = glob->disk_inserted
                && glob->disk_inhibited == 0 && glob->sb == NULL;
            (void)ExfatAttemptDestroyVolume(sb);
            if (inserted)
                DoDiskInsert(glob);
        }
    }
}

static void RefreshDirectoryLocks(struct FSSuper *sb,
    const struct exfat_stream *old_stream,
    const struct exfat_stream *parent,
    const struct exfat_entry *directory)
{
    struct exfat_lock *lock;

    if (!directory->stream.length_known)
        return;
    for (lock = sb->locks; lock != NULL; lock = lock->next)
    {
        if (lock->parent.first_cluster == parent->first_cluster
            && lock->entry.directory_index == directory->directory_index)
            lock->entry = *directory;
        if (lock->parent.first_cluster == old_stream->first_cluster)
            lock->parent = directory->stream;
    }
}

static BOOL PathIsWithin(struct FSSuper *sb,
    const UBYTE *ancestor, ULONG ancestor_length,
    const UBYTE *path, ULONG path_length)
{
    ULONG i;

    if (ancestor_length == 0 || path_length <= ancestor_length
        || path[ancestor_length] != '/')
        return FALSE;
    for (i = 0; i < ancestor_length; i++)
        if (sb->upcase[(UBYTE)ancestor[i]]
            != sb->upcase[(UBYTE)path[i]])
            return FALSE;
    return TRUE;
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
    return *out != NULL ? 0
        : (IoErr() != 0 ? IoErr() : ERROR_NO_FREE_STORE);
}

static LONG PrepareCreatePath(struct FSSuper *sb, struct exfat_lock *base,
    CONST_STRPTR path, ULONG length, struct exfat_entry *directory,
    struct exfat_stream *directory_parent,
    CONST_STRPTR *name, ULONG *name_length, UBYTE *canonical,
    ULONG *canonical_length)
{
    struct Globals *glob = sb->glob;
    struct exfat_entry parent;
    struct exfat_stream grandparent = sb->root;
    ULONG colon = 0, slash = ~(ULONG)0, leaf, parent_length, i;
    LONG err;

    for (i = 0; i < length; i++)
    {
        if (path[i] == ':')
            colon = i + 1;
        else if (path[i] == '/' && i >= colon)
            slash = i;
    }
    leaf = slash != ~(ULONG)0 ? slash + 1 : colon;
    if (leaf >= length)
        return ERROR_INVALID_COMPONENT_NAME;
    *name = path + leaf;
    *name_length = length - leaf;

    if (slash != ~(ULONG)0 || colon != 0)
    {
        parent_length = slash != ~(ULONG)0 ? slash : colon;
        err = ResolvePath(sb, base, path, parent_length, &parent,
            &grandparent, canonical, canonical_length);
        if (err != 0)
            return err;
    }
    else if (base != NULL)
    {
        parent = base->entry;
        grandparent = base->parent;
        *canonical_length = base->path_length;
        CopyMem(base->path, canonical, *canonical_length);
    }
    else
    {
        RootEntry(sb, &parent);
        grandparent = sb->root;
        *canonical_length = 0;
    }
    if ((parent.attributes & EXFAT_ATTR_DIRECTORY) == 0)
        return ERROR_OBJECT_WRONG_TYPE;
    if (*name_length > EXFAT_MAX_NAME)
        return ERROR_INVALID_COMPONENT_NAME;
    for (i = 0; i < *name_length; i++)
        if ((UBYTE)(*name)[i] < 0x20 || (*name)[i] == ':'
            || (*name)[i] == '/')
            return ERROR_INVALID_COMPONENT_NAME;
    if (*canonical_length != 0)
    {
        if (*canonical_length == EXFAT_MAX_PATH)
            return ERROR_LINE_TOO_LONG;
        canonical[(*canonical_length)++] = '/';
    }
    if (*name_length > EXFAT_MAX_PATH - *canonical_length)
        return ERROR_LINE_TOO_LONG;
    CopyMem(*name, canonical + *canonical_length, *name_length);
    *canonical_length += *name_length;
    canonical[*canonical_length] = 0;
    *directory = parent;
    *directory_parent = grandparent;
    return 0;
}

static LONG CopyLock(struct exfat_lock *source, struct exfat_lock **out)
{
    struct Globals *glob = source->sb->glob;

    *out = NewLock(source->sb, &source->entry, &source->parent,
        (STRPTR)source->path, source->path_length, source->fl_Access);
    if (*out == NULL)
        return IoErr() != 0 ? IoErr() : ERROR_NO_FREE_STORE;
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

static void FillFIB(struct Globals *glob, const struct exfat_entry *entry,
    struct FileInfoBlock *fib, ULONG disk_key)
{
    TEXT name[MAXFILENAMELENGTH];
    ULONG len;
    UQUAD blocks;

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
    fib->fib_Protection = 0;
    if (entry->attributes & EXFAT_ATTR_READONLY)
        fib->fib_Protection |= FIBF_WRITE | FIBF_DELETE;
    if (entry->attributes & EXFAT_ATTR_ARCHIVE)
        fib->fib_Protection |= FIBF_ARCHIVE;
    fib->fib_Size = entry->stream.data_length;
    blocks = entry->stream.data_length >> 9;
    if ((entry->stream.data_length & 511) != 0)
        blocks++;
    fib->fib_NumBlocks = blocks;

    (void)ExfatUnpackDateStamp(glob, entry->modify_timestamp,
        entry->modify_10ms, (UBYTE)entry->modify_utc, &fib->fib_Date);
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

static LONG SizeFromOffset(const struct exfat_lock *lock, QUAD offset,
    LONG mode, UQUAD *size)
{
    UQUAD base, magnitude;

    if (mode == OFFSET_BEGINNING)
        base = 0;
    else if (mode == OFFSET_CURRENT)
        base = lock->position;
    else if (mode == OFFSET_END)
        base = lock->entry.stream.data_length;
    else
        return ERROR_SEEK_ERROR;

    if (offset < 0)
    {
        magnitude = (UQUAD)(-(offset + 1)) + 1;
        if (magnitude > base)
            return ERROR_SEEK_ERROR;
        *size = base - magnitude;
    }
    else
    {
        magnitude = (UQUAD)offset;
        if (magnitude > ~(UQUAD)0 - base)
            return ERROR_OBJECT_TOO_LARGE;
        *size = base + magnitude;
    }
    return 0;
}

#if (__WORDSIZE != 64)
static BOOL IsDosPacket64Action(LONG type)
{
    switch (type)
    {
    case ACTION_CHANGE_FILE_POSITION64:
    case ACTION_GET_FILE_POSITION64:
    case ACTION_CHANGE_FILE_SIZE64:
    case ACTION_GET_FILE_SIZE64:
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void ExfatProcessPackets(struct Globals *glob)
{
    struct Message *msg;
    struct DosPacket *dp;

    while ((msg = GetMsg(glob->ourport)) != NULL)
    {
        IPTR res = DOSFALSE;
        QUAD res64 = DOSFALSE;
        LONG err = 0;
#if (__WORDSIZE != 64)
        struct DosPacket64 *dp64;
        BOOL packet64;
#endif
        dp = (struct DosPacket *)msg->mn_Node.ln_Name;
#if (__WORDSIZE != 64)
        dp64 = (struct DosPacket64 *)dp;
        packet64 = IsDosPacket64Action(dp->dp_Type)
            && dp64->dp_Res0 == DP64_INIT;
#endif

#ifdef EXFAT_TEST_FAILPOINTS
        if (dp->dp_Type == ACTION_EXFAT_ARM_FAILPOINT)
        {
            if ((ULONG)dp->dp_Arg1 != EXFAT_FAILPOINT_COOKIE
                || dp->dp_Arg2 <= 0 || glob->sb == NULL
                || glob->sb->write_transaction_active)
                err = ERROR_BAD_NUMBER;
            else
            {
                glob->sb->test_fail_after_sync = (ULONG)dp->dp_Arg2;
                res = DOSTRUE;
            }
        }
        else
#endif
        switch (dp->dp_Type)
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
            if (lock == NULL || lock->magic != EXFAT_LOCK_MAGIC
                || lock->sb == NULL || lock->sb->glob != glob)
                err = ERROR_INVALID_LOCK;
            else { DropLock(lock); res = DOSTRUE; }
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
                if (lock == NULL) { RootEntry(glob->sb, &root); FillFIB(glob, &root, fib, 0); }
                else { lock->enum_index = 0; FillFIB(glob, &lock->entry, fib, 0); }
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
                if (err == 0) { FillFIB(glob, &entry, fib, index); res = DOSTRUE; }
            }
            break;
        }
        case ACTION_DELETE_OBJECT:
        {
            struct exfat_lock *base = BADDR(dp->dp_Arg1), *lock = NULL;
            BOOL mutation_attempted = FALSE;

            err = TestLock(base, glob);
            if (err == 0 && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
                err = LockPath(glob, base, AROS_BSTR_ADDR(dp->dp_Arg2),
                    AROS_BSTR_strlen(dp->dp_Arg2), EXCLUSIVE_LOCK, &lock);
            if (err == 0
                && (lock->entry.attributes & EXFAT_ATTR_DIRECTORY))
                err = ExfatPrepareDirectoryDelete(glob->sb,
                    &lock->entry.stream, FALSE);
            if (err == 0)
                err = ExfatBeginWrite(glob->sb);
            if (err == 0
                && (lock->entry.attributes & EXFAT_ATTR_DIRECTORY))
                err = ExfatPrepareDirectoryDelete(glob->sb,
                    &lock->entry.stream, TRUE);
            if (err == 0)
            {
                mutation_attempted = TRUE;
                err = ExfatDeleteEntry(glob->sb, &lock->parent,
                    &lock->entry);
            }
            if (err == 0)
                err = ExfatCommitWrite(glob->sb);
            if (err != 0 && mutation_attempted
                && glob->sb->write_transaction_active)
                (void)ExfatAbortWrite(glob->sb);
            if (lock != NULL)
                DropLock(lock);
            if (err == 0)
                res = DOSTRUE;
            break;
        }
        case ACTION_RENAME_OBJECT:
        {
            struct exfat_lock *source_base = BADDR(dp->dp_Arg1);
            struct exfat_lock *target_base = BADDR(dp->dp_Arg3);
            struct exfat_lock *source = NULL, *other;
            CONST_STRPTR source_path = AROS_BSTR_ADDR(dp->dp_Arg2);
            CONST_STRPTR target_path = AROS_BSTR_ADDR(dp->dp_Arg4), name;
            ULONG source_length = AROS_BSTR_strlen(dp->dp_Arg2);
            ULONG target_length = AROS_BSTR_strlen(dp->dp_Arg4);
            ULONG name_length, canonical_length = 0;
            UBYTE canonical[EXFAT_MAX_PATH + 1];
            struct exfat_entry target_directory, target_entry, existing;
            struct exfat_stream target_parent, old_directory;
            struct exfat_stream existing_parent;
            UBYTE existing_path[EXFAT_MAX_PATH + 1];
            ULONG existing_length = 0;
            BOOL mutation_attempted = FALSE;

            err = TestLock(source_base, glob);
            if (err == 0)
                err = TestLock(target_base, glob);
            if (err == 0 && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
                err = LockPath(glob, source_base, source_path,
                    source_length, EXCLUSIVE_LOCK, &source);
            if (err == 0 && source->path_length == 0)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0)
            {
                for (other = glob->sb->locks; other != NULL;
                    other = other->next)
                    if (other != source && PathIsWithin(glob->sb,
                            source->path, source->path_length,
                            other->path, other->path_length))
                    {
                        err = ERROR_OBJECT_IN_USE;
                        break;
                    }
            }
            if (err == 0)
            {
                err = ResolvePath(glob->sb, target_base, target_path,
                    target_length, &existing, &existing_parent,
                    existing_path, &existing_length);
                if (err == 0)
                    err = ERROR_OBJECT_EXISTS;
            }
            if (err == ERROR_OBJECT_NOT_FOUND)
                err = PrepareCreatePath(glob->sb, target_base,
                    target_path, target_length, &target_directory,
                    &target_parent, &name, &name_length, canonical,
                    &canonical_length);
            if (err == 0
                && (source->entry.attributes & EXFAT_ATTR_DIRECTORY)
                && PathIsWithin(glob->sb, source->path,
                    source->path_length, canonical, canonical_length))
                err = ERROR_OBJECT_IN_USE;
            if (err == 0)
                old_directory = target_directory.stream;
            if (err == 0)
                err = ExfatBeginWrite(glob->sb);
            if (err == 0)
            {
                mutation_attempted = TRUE;
                err = ExfatRenameEntry(glob->sb, &source->parent,
                    &source->entry, &target_directory, &target_parent,
                    name, name_length, &target_entry);
            }
            if (err == 0)
                err = ExfatCommitWrite(glob->sb);
            if (err == 0)
                RefreshDirectoryLocks(glob->sb, &old_directory,
                    &target_parent, &target_directory);
            if (err != 0 && mutation_attempted
                && glob->sb->write_transaction_active)
                (void)ExfatAbortWrite(glob->sb);
            if (source != NULL)
                DropLock(source);
            if (err == 0)
                res = DOSTRUE;
            break;
        }
        case ACTION_CREATE_DIR:
        {
            struct exfat_lock *base = BADDR(dp->dp_Arg1), *lock = NULL;
            CONST_STRPTR path = AROS_BSTR_ADDR(dp->dp_Arg2), name;
            ULONG path_length = AROS_BSTR_strlen(dp->dp_Arg2);
            ULONG name_length, canonical_length = 0;
            UBYTE canonical[EXFAT_MAX_PATH + 1];
            struct exfat_entry directory;
            struct exfat_stream directory_parent, old_directory;
            struct exfat_entry entry;
            BOOL mutation_attempted = FALSE;

            err = TestLock(base, glob);
            if (err == 0 && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
                err = LockPath(glob, base, path, path_length,
                    SHARED_LOCK, &lock);
            if (err == 0)
            {
                DropLock(lock);
                lock = NULL;
                err = ERROR_OBJECT_EXISTS;
            }
            if (err == ERROR_OBJECT_NOT_FOUND)
                err = PrepareCreatePath(glob->sb, base, path, path_length,
                    &directory, &directory_parent, &name, &name_length,
                    canonical,
                    &canonical_length);
            if (err == 0)
                old_directory = directory.stream;
            if (err == 0)
                err = ExfatBeginWrite(glob->sb);
            if (err == 0)
            {
                mutation_attempted = TRUE;
                err = ExfatCreateDirectoryEntry(glob->sb, &directory,
                    &directory_parent, name, name_length, &entry);
            }
            if (err == 0)
            {
                lock = NewLock(glob->sb, &entry, &directory.stream,
                    (STRPTR)canonical, canonical_length, SHARED_LOCK);
                if (lock == NULL)
                    err = IoErr() != 0 ? IoErr() : ERROR_NO_FREE_STORE;
            }
            if (err == 0)
                err = ExfatCommitWrite(glob->sb);
            if (err == 0)
                RefreshDirectoryLocks(glob->sb, &old_directory,
                    &directory_parent, &directory);
            if (err != 0 && mutation_attempted
                && glob->sb->write_transaction_active)
                (void)ExfatAbortWrite(glob->sb);
            if (err == 0)
                res = (IPTR)MKBADDR(lock);
            else if (lock != NULL)
                DropLock(lock);
            break;
        }
        case ACTION_SET_PROTECT:
        {
            struct exfat_lock *base = BADDR(dp->dp_Arg2), *lock = NULL;
            ULONG protection = (ULONG)dp->dp_Arg4;
            UWORD attributes;
            BOOL mutation_attempted = FALSE;

            err = TestLock(base, glob);
            if (err == 0 && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
                err = LockPath(glob, base, AROS_BSTR_ADDR(dp->dp_Arg3),
                    AROS_BSTR_strlen(dp->dp_Arg3), EXCLUSIVE_LOCK, &lock);
            if (err == 0 && lock->path_length == 0)
                err = ERROR_INVALID_LOCK;
            attributes = err == 0 ? lock->entry.attributes : 0;
            attributes &= (UWORD)~(EXFAT_ATTR_ARCHIVE | EXFAT_ATTR_READONLY);
            if (protection & FIBF_ARCHIVE)
                attributes |= EXFAT_ATTR_ARCHIVE;
            if ((protection & (FIBF_WRITE | FIBF_DELETE))
                    == (FIBF_WRITE | FIBF_DELETE))
                attributes |= EXFAT_ATTR_READONLY;
            if (err == 0)
                err = ExfatBeginWrite(glob->sb);
            if (err == 0)
            {
                mutation_attempted = TRUE;
                err = ExfatUpdateEntryAttributes(glob->sb, &lock->parent,
                    &lock->entry, attributes);
            }
            if (err == 0)
                err = ExfatCommitWrite(glob->sb);
            if (err != 0 && mutation_attempted
                && glob->sb->write_transaction_active)
                (void)ExfatAbortWrite(glob->sb);
            if (lock != NULL)
                DropLock(lock);
            if (err == 0)
                res = DOSTRUE;
            break;
        }
        case ACTION_SET_DATE:
        {
            struct exfat_lock *base = BADDR(dp->dp_Arg2), *lock = NULL;
            const struct DateStamp *date = (const struct DateStamp *)dp->dp_Arg4;
            ULONG packed = 0;
            UBYTE ten_ms = 0, utc = 0;
            BOOL mutation_attempted = FALSE;

            err = TestLock(base, glob);
            if (err == 0 && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
                err = LockPath(glob, base, AROS_BSTR_ADDR(dp->dp_Arg3),
                    AROS_BSTR_strlen(dp->dp_Arg3), EXCLUSIVE_LOCK, &lock);
            if (err == 0 && lock->path_length == 0)
                err = ERROR_INVALID_LOCK;
            if (err == 0)
                err = ExfatPackDateStamp(glob, date, &packed, &ten_ms, &utc);
            if (err == 0)
                err = ExfatBeginWrite(glob->sb);
            if (err == 0)
            {
                mutation_attempted = TRUE;
                err = ExfatUpdateEntryDate(glob->sb, &lock->parent,
                    &lock->entry, packed, ten_ms, (BYTE)utc,
                    EXFAT_DATE_MODIFIED);
            }
            if (err == 0)
                err = ExfatCommitWrite(glob->sb);
            if (err != 0 && mutation_attempted
                && glob->sb->write_transaction_active)
                (void)ExfatAbortWrite(glob->sb);
            if (lock != NULL)
                DropLock(lock);
            if (err == 0)
                res = DOSTRUE;
            break;
        }
        case ACTION_RENAME_DISK:
        {
            CONST_STRPTR name = AROS_BSTR_ADDR(dp->dp_Arg1);
            ULONG name_length = AROS_BSTR_strlen(dp->dp_Arg1);
            ULONG i;
            struct VolumeIdentity old_volume;
            UBYTE *new_name_buffer = NULL;
            BPTR old_dos_name;
            BOOL mutation_attempted = FALSE;

            if (glob->sb == NULL)
                err = ERROR_NO_DISK;
            if (err == 0 && (name_length == 0 || name_length > 15))
                err = ERROR_INVALID_COMPONENT_NAME;
            for (i = 0; err == 0 && i < name_length; i++)
                if ((UBYTE)name[i] < 0x20 || name[i] == '/'
                    || name[i] == ':' || name[i] == '\\'
                    || name[i] == '*' || name[i] == '?'
                    || name[i] == '"' || name[i] == '<'
                    || name[i] == '>' || name[i] == '|')
                    err = ERROR_INVALID_COMPONENT_NAME;
            if (err == 0 && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
            {
#ifdef AROS_FAST_BPTR
                new_name_buffer = AllocVec(name_length + 1,
                    MEMF_PUBLIC | MEMF_CLEAR);
                if (new_name_buffer != NULL)
                    CopyMem(name, new_name_buffer, name_length);
#else
                new_name_buffer = AllocVec(name_length + 2,
                    MEMF_PUBLIC | MEMF_CLEAR);
                if (new_name_buffer != NULL)
                {
                    new_name_buffer[0] = (UBYTE)name_length;
                    CopyMem(name, new_name_buffer + 1, name_length);
                }
#endif
                if (new_name_buffer == NULL)
                    err = ERROR_NO_FREE_STORE;
            }
            if (err == 0)
                old_volume = glob->sb->volume;
            if (err == 0)
                err = ExfatBeginWrite(glob->sb);
            if (err == 0)
            {
                mutation_attempted = TRUE;
                err = ExfatSetVolumeLabel(glob->sb, name, name_length);
            }
            if (err == 0)
                err = ExfatCommitWrite(glob->sb);
            if (err != 0 && mutation_attempted
                && glob->sb->write_transaction_active)
                (void)ExfatAbortWrite(glob->sb);
            if (err != 0 && mutation_attempted)
                glob->sb->volume = old_volume;
            if (err == 0)
            {
                while (!AttemptLockDosList(LDF_VOLUMES | LDF_WRITE))
                    ExfatProcessPackets(glob);
                old_dos_name = glob->sb->doslist->dol_Name;
                glob->sb->doslist->dol_Name = MKBADDR(new_name_buffer);
                UnLockDosList(LDF_VOLUMES | LDF_WRITE);
                FreeVec(BADDR(old_dos_name));
                new_name_buffer = NULL;
                res = DOSTRUE;
            }
            if (new_name_buffer != NULL)
                FreeVec(new_name_buffer);
            break;
        }
        case ACTION_FORMAT:
        {
            CONST_STRPTR name = AROS_BSTR_ADDR(dp->dp_Arg1);
            ULONG name_length = AROS_BSTR_strlen(dp->dp_Arg1), i;

            if (!glob->disk_inserted)
                err = ERROR_NO_DISK;
            if (err == 0 && glob->sb != NULL
                && glob->sb->lock_count != 0)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (name_length == 0 || name_length > 15))
                err = ERROR_INVALID_COMPONENT_NAME;
            for (i = 0; err == 0 && i < name_length; i++)
                if ((UBYTE)name[i] < 0x20 || name[i] == '/'
                    || name[i] == ':' || name[i] == '\\'
                    || name[i] == '*' || name[i] == '?'
                    || name[i] == '"' || name[i] == '<'
                    || name[i] == '>' || name[i] == '|')
                    err = ERROR_INVALID_COMPONENT_NAME;
            if (err == 0 && glob->sb != NULL)
                DoDiskRemove(glob);
            if (err == 0)
                err = ExfatFormatVolume(glob, name, name_length);
            if (err == 0)
            {
                DoDiskInsert(glob);
                if (glob->sb == NULL)
                    err = glob->mount_error != 0
                        ? glob->mount_error : ERROR_NOT_A_DOS_DISK;
            }
            if (err == 0)
                res = DOSTRUE;
            break;
        }
        case ACTION_FINDINPUT:
        case ACTION_FINDUPDATE:
        {
            struct FileHandle *fh = BADDR(dp->dp_Arg1);
            struct exfat_lock *base = BADDR(dp->dp_Arg2), *lock;
            BOOL update = dp->dp_Type == ACTION_FINDUPDATE;

            err = TestLock(base, glob);
            if (err == 0 && update && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && update
                && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
                err = LockPath(glob, base, AROS_BSTR_ADDR(dp->dp_Arg3),
                    AROS_BSTR_strlen(dp->dp_Arg3),
                    update ? EXCLUSIVE_LOCK : SHARED_LOCK, &lock);
            if (err == 0 && (lock->entry.attributes & EXFAT_ATTR_DIRECTORY))
            { DropLock(lock); err = ERROR_OBJECT_WRONG_TYPE; }
            if (err == 0 && update
                && (lock->entry.attributes & EXFAT_ATTR_READONLY))
            { DropLock(lock); err = ERROR_DISK_WRITE_PROTECTED; }
            if (err == 0)
            {
                if (update)
                {
                    lock->writable = TRUE;
                    glob->sb->write_handle = lock;
                }
                fh->fh_Arg1 = (IPTR)MKBADDR(lock);
                fh->fh_Port = DOSFALSE;
                res = DOSTRUE;
            }
            break;
        }
        case ACTION_FINDOUTPUT:
        {
            struct FileHandle *fh = BADDR(dp->dp_Arg1);
            struct exfat_lock *base = BADDR(dp->dp_Arg2), *lock = NULL;
            CONST_STRPTR path = AROS_BSTR_ADDR(dp->dp_Arg3), name;
            ULONG path_length = AROS_BSTR_strlen(dp->dp_Arg3);
            ULONG name_length, canonical_length = 0;
            UBYTE canonical[EXFAT_MAX_PATH + 1];
            struct exfat_entry directory;
            struct exfat_stream directory_parent, old_directory;
            struct exfat_entry entry;
            BOOL mutation_attempted = FALSE;
            BOOL created = FALSE;

            err = TestLock(base, glob);
            if (err == 0 && glob->sb->write_handle != NULL)
                err = ERROR_OBJECT_IN_USE;
            if (err == 0 && (glob->sb->volume_flags
                    & (EXFAT_VOLUMEFLAG_DIRTY
                        | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                err = ERROR_DISK_NOT_VALIDATED;
            if (err == 0)
                err = LockPath(glob, base, path, path_length,
                    EXCLUSIVE_LOCK, &lock);
            if (err == 0 && (lock->entry.attributes & EXFAT_ATTR_DIRECTORY))
            {
                DropLock(lock);
                lock = NULL;
                err = ERROR_OBJECT_WRONG_TYPE;
            }
            if (err == 0 && (lock->entry.attributes & EXFAT_ATTR_READONLY))
            {
                DropLock(lock);
                lock = NULL;
                err = ERROR_DISK_WRITE_PROTECTED;
            }
            if (err == ERROR_OBJECT_NOT_FOUND)
            {
                err = PrepareCreatePath(glob->sb, base, path, path_length,
                    &directory, &directory_parent, &name, &name_length,
                    canonical,
                    &canonical_length);
                if (err == 0)
                    old_directory = directory.stream;
                if (err == 0)
                    err = ExfatBeginWrite(glob->sb);
                if (err == 0)
                {
                    mutation_attempted = TRUE;
                    err = ExfatCreateEntry(glob->sb, &directory,
                        &directory_parent, name, name_length, &entry);
                    if (err == 0)
                        created = TRUE;
                }
                if (err == 0)
                {
                    lock = NewLock(glob->sb, &entry, &directory.stream,
                        (STRPTR)canonical, canonical_length,
                        EXCLUSIVE_LOCK);
                    if (lock == NULL)
                        err = IoErr() != 0 ? IoErr() : ERROR_NO_FREE_STORE;
                }
            }
            else if (err == 0 && lock->entry.stream.data_length != 0)
            {
                err = ExfatBeginWrite(glob->sb);
                if (err == 0)
                {
                    mutation_attempted = TRUE;
                    err = ExfatResizeFile(glob->sb, &lock->parent,
                        &lock->entry, 0);
                    if (err == 0)
                        lock->contents_modified = TRUE;
                }
            }
            if (err == 0)
            {
                if (created)
                    RefreshDirectoryLocks(glob->sb, &old_directory,
                        &directory_parent, &directory);
                lock->writable = TRUE;
                lock->write_transaction = mutation_attempted;
                glob->sb->write_handle = lock;
                fh->fh_Arg1 = (IPTR)MKBADDR(lock);
                fh->fh_Port = DOSFALSE;
                res = DOSTRUE;
            }
            else if (mutation_attempted)
            {
                if (glob->sb->write_transaction_active)
                    (void)ExfatAbortWrite(glob->sb);
                if (lock != NULL)
                    DropLock(lock);
            }
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
        case ACTION_WRITE:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            LONG requested = (LONG)dp->dp_Arg3;
            ULONG wrote = 0;
            BOOL valid_lock;
            BOOL mutation_attempted = FALSE;
            UQUAD old_valid = 0, ending = 0;

            err = TestLock(lock, glob);
            valid_lock = err == 0 && lock != NULL;
            if (err == 0 && (lock == NULL || !lock->writable
                    || lock->fl_Access != EXCLUSIVE_LOCK))
                err = ERROR_DISK_WRITE_PROTECTED;
            if (err == 0 && requested < 0)
                err = ERROR_BAD_NUMBER;
            if (err == 0 && requested != 0
                && lock->position > ~(UQUAD)0 - (ULONG)requested)
                err = ERROR_OBJECT_TOO_LARGE;
            if (err == 0 && requested != 0)
            {
                old_valid = lock->entry.stream.valid_data_length;
                ending = lock->position + (ULONG)requested;
            }
            if (err == 0 && requested != 0 && !lock->write_transaction)
            {
                err = ExfatBeginWrite(lock->sb);
                if (err == 0)
                    lock->write_transaction = TRUE;
            }
            if (err == 0 && requested != 0)
            {
                mutation_attempted = TRUE;
                if (ending > lock->entry.stream.data_length)
                    err = ExfatGrowFile(lock->sb, &lock->entry, ending);
            }
            if (err == 0 && requested != 0)
            {
                if (lock->position > old_valid)
                    err = ExfatZeroStream(lock->sb, &lock->entry.stream,
                        old_valid, lock->position - old_valid);
            }
            if (err == 0 && requested != 0)
            {
                err = ExfatWriteStream(lock->sb, &lock->entry.stream,
                    lock->position, (CONST_APTR)dp->dp_Arg2,
                    (ULONG)requested, &wrote);
            }
            if (err == 0 && requested != 0 && ending > old_valid)
            {
                lock->entry.stream.valid_data_length = ending;
                err = ExfatUpdateEntryStream(lock->sb, &lock->parent,
                    &lock->entry);
            }
            if (err == 0)
            {
                lock->position += wrote;
                if (wrote != 0)
                    lock->contents_modified = TRUE;
                res = wrote;
            }
            else
            {
                if (valid_lock && mutation_attempted
                    && lock->write_transaction)
                {
                    if (lock->sb->write_transaction_active)
                        (void)ExfatAbortWrite(lock->sb);
                    lock->write_transaction = FALSE;
                }
                res = -1;
            }
            break;
        }
        case ACTION_SET_FILE_SIZE:
        case ACTION_SET_FILE_SIZE64:
        case ACTION_CHANGE_FILE_SIZE64:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            QUAD offset;
            LONG mode;
            UQUAD new_size = 0;
            BOOL mutation_attempted = FALSE;

#if (__WORDSIZE != 64)
            if (dp->dp_Type == ACTION_SET_FILE_SIZE64)
            {
                err = ERROR_ACTION_NOT_KNOWN;
                res = -1;
                break;
            }
            if (dp->dp_Type == ACTION_CHANGE_FILE_SIZE64 && !packet64)
            {
                err = ERROR_BAD_NUMBER;
                res = -1;
                break;
            }
            offset = packet64 ? dp64->dp_Arg2 : (QUAD)dp->dp_Arg2;
            mode = packet64 ? (LONG)dp64->dp_Arg3 : (LONG)dp->dp_Arg3;
#else
            offset = (QUAD)dp->dp_Arg2;
            mode = (LONG)dp->dp_Arg3;
#endif
            err = TestLock(lock, glob);
            if (err == 0 && (lock == NULL || !lock->writable
                    || lock->fl_Access != EXCLUSIVE_LOCK))
                err = ERROR_DISK_WRITE_PROTECTED;
            if (err == 0)
                err = SizeFromOffset(lock, offset, mode, &new_size);
            if (err == 0 && dp->dp_Type == ACTION_SET_FILE_SIZE
                && new_size > 0x7fffffffULL)
                err = ERROR_OBJECT_TOO_LARGE;
            if (err == 0 && new_size != lock->entry.stream.data_length
                && !lock->write_transaction)
            {
                err = ExfatBeginWrite(lock->sb);
                if (err == 0)
                    lock->write_transaction = TRUE;
            }
            if (err == 0 && new_size != lock->entry.stream.data_length)
            {
                mutation_attempted = TRUE;
                err = ExfatResizeFile(lock->sb, &lock->parent,
                    &lock->entry, new_size);
            }
            if (err == 0)
            {
                if (mutation_attempted)
                    lock->contents_modified = TRUE;
                if (lock->position > new_size)
                    lock->position = new_size;
                if (dp->dp_Type == ACTION_CHANGE_FILE_SIZE64)
                {
                    res64 = DOSTRUE;
                    res = DOSTRUE;
                }
                else
                {
                    res64 = (QUAD)new_size;
                    res = (IPTR)new_size;
                }
            }
            else
            {
                if (lock != NULL && mutation_attempted
                    && lock->write_transaction)
                {
                    if (lock->sb->write_transaction_active)
                        (void)ExfatAbortWrite(lock->sb);
                    lock->write_transaction = FALSE;
                }
                res64 = -1;
                res = -1;
            }
            break;
        }
        case ACTION_SEEK:
        case ACTION_SEEK64:
        case ACTION_CHANGE_FILE_POSITION64:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            UQUAD old;
            QUAD offset;
            LONG mode;

#if (__WORDSIZE != 64)
            if (dp->dp_Type == ACTION_SEEK64)
            {
                err = ERROR_ACTION_NOT_KNOWN;
                res = -1;
                break;
            }
            offset = packet64 ? dp64->dp_Arg2 : (QUAD)dp->dp_Arg2;
            mode = packet64 ? (LONG)dp64->dp_Arg3 : (LONG)dp->dp_Arg3;
#else
            offset = (QUAD)dp->dp_Arg2;
            mode = (LONG)dp->dp_Arg3;
#endif
            err = TestLock(lock, glob);
            if (err == 0 && lock == NULL) err = ERROR_INVALID_LOCK;
            if (err == 0)
                err = SeekPosition(lock, offset, mode, &old);
            if (err == 0)
            {
                res64 = dp->dp_Type == ACTION_CHANGE_FILE_POSITION64
                    ? (QUAD)DOSTRUE : (QUAD)old;
                res = (IPTR)res64;
            }
            else
            {
                res64 = -1;
                res = -1;
            }
            break;
        }
        case ACTION_GET_FILE_POSITION64:
        case ACTION_GET_FILE_SIZE64:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            err = TestLock(lock, glob);
            if (err == 0 && lock == NULL) err = ERROR_INVALID_LOCK;
            if (err == 0)
            {
                res64 = (QUAD)(dp->dp_Type == ACTION_GET_FILE_POSITION64
                    ? lock->position : lock->entry.stream.data_length);
                res = (IPTR)res64;
            }
            else
            {
                res64 = -1;
                res = -1;
            }
            break;
        }
        case ACTION_END:
        {
            struct exfat_lock *lock = BADDR(dp->dp_Arg1);
            err = TestLock(lock, glob);
            /* Closing is always safe for a handle owned by this handler.
               Offline media cannot accept a commit, but the stale handle
               still has to be releasable. */
            if (err == ERROR_DEVICE_NOT_MOUNTED && lock != NULL
                && lock->magic == EXFAT_LOCK_MAGIC && lock->sb != NULL
                && lock->sb->glob == glob && !lock->sb->online)
                err = 0;
            if (err == 0 && lock != NULL && lock->write_transaction)
            {
                lock->write_transaction = FALSE;
                if (lock->sb->online)
                {
                    if (lock->contents_modified)
                        err = ExfatTouchEntry(lock->sb, &lock->parent,
                            &lock->entry,
                            EXFAT_DATE_MODIFIED | EXFAT_DATE_ACCESSED);
                    if (err == 0)
                        err = ExfatCommitWrite(lock->sb);
                    else if (lock->sb->write_transaction_active)
                        (void)ExfatAbortWrite(lock->sb);
                }
            }
            if (err == 0)
                res = DOSTRUE;
            if (lock != NULL && lock->magic == EXFAT_LOCK_MAGIC)
                DropLock(lock);
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
                if ((glob->sb->volume_flags
                        & (EXFAT_VOLUMEFLAG_DIRTY
                            | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
                    id->id_DiskState = ID_WRITE_PROTECTED;
                else if (glob->sb->write_transaction_active)
                    id->id_DiskState = ID_VALIDATING;
                else
                    id->id_DiskState = ID_VALIDATED;
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
            if (dp->dp_Arg1 == DOSTRUE) { glob->disk_inhibited++; if (glob->disk_inhibited == 1) DoDiskRemove(glob); }
            else if (glob->disk_inhibited != 0) { glob->disk_inhibited--; if (glob->disk_inhibited == 0) ProcessDiskChange(glob); }
            res = DOSTRUE;
            break;
        case ACTION_DIE:
            if ((glob->sb != NULL && glob->sb->lock_count != 0)
                || glob->sblist.mlh_Head->mln_Succ != NULL)
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

#if (__WORDSIZE != 64)
        if (packet64)
        {
            /* Preserve dp_Res0 == DP64_INIT.  It overlays the standard
               dp_Res1 and tells dos64.library that the wide fields below are
               valid rather than a legacy 32-bit handler response. */
            dp64->dp_Res1 = res64;
            dp64->dp_Res2 = (ULONG)err;
        }
        else
#endif
        {
            dp->dp_Res1 = res;
            dp->dp_Res2 = err;
        }
        {
            struct MsgPort *reply = dp->dp_Port;
            dp->dp_Port = glob->ourport;
            msg->mn_Node.ln_Name = (STRPTR)dp;
            PutMsg(reply, msg);
        }
    }
}
