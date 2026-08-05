/*
 * exfat-handler - ordered metadata transaction foundation
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * ACTION_WRITE uses this for bounded in-place data updates.  Allocation and
 * directory writers share the same dirty-bit and durable-stage boundaries.
 */

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "exfat_fs.h"
#include "exfat_meta.h"
#include "exfat_protos.h"

/* VolumeFlags in the backup boot sector are explicitly stale in exFAT 1.00.
   Normal metadata transactions therefore update the main boot sector only. */
static LONG WriteMainBootStatus(struct FSSuper *sb, UWORD flags,
    BOOL update_percent)
{
    struct Globals *glob = sb->glob;
    UBYTE *data;
    APTR block;
    BOOL marked;

    block = Cache_GetBlock(sb->cache, sb->first_device_sector, &data);
    if (block == NULL)
        return IoErr() != 0 ? IoErr() : ERROR_UNKNOWN;

    exfat_wr16(data, EXFAT_BOOT_VOLUMEFLAGS, flags);
    if (update_percent)
    {
        data[EXFAT_BOOT_PERCENTINUSE] = exfat_percent_in_use(
            sb->free_clusters, sb->cluster_count);
        sb->geo.percent_in_use = data[EXFAT_BOOT_PERCENTINUSE];
    }
    marked = Cache_MarkBlockDirtySector(sb->cache, block,
        sb->first_device_sector);
    Cache_FreeBlock(sb->cache, block);
    if (!marked)
        return IoErr() != 0 ? IoErr() : ERROR_BAD_NUMBER;

    sb->volume_flags = flags;
    return 0;
}

static LONG SyncWrite(struct FSSuper *sb)
{
    if (!SyncDisk(sb->glob))
        return ERROR_DISK_NOT_VALIDATED;
#ifdef EXFAT_TEST_FAILPOINTS
    if (sb->test_fail_after_sync != 0
        && --sb->test_fail_after_sync == 0)
        return ERROR_DISK_NOT_VALIDATED;
#endif
    return 0;
}

/* Best effort after any failed commit step.  The caller still receives the
   original failure, and the in-memory state stays dirty even if the medium
   can no longer accept the flag write. */
static void KeepVolumeDirty(struct FSSuper *sb)
{
    UWORD flags = exfat_volume_flags_begin_write(sb->volume_flags);

    if (WriteMainBootStatus(sb, flags, FALSE) == 0)
        (void)SyncWrite(sb);
    sb->volume_flags = flags;
}

LONG ExfatBeginWrite(struct FSSuper *sb)
{
    UWORD original_flags;
    UWORD flags;
    LONG err;

    if (sb->write_transaction_active)
        return ERROR_OBJECT_IN_USE;
    if ((sb->volume_flags
            & (EXFAT_VOLUMEFLAG_DIRTY | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
        return ERROR_DISK_NOT_VALIDATED;
    /* Dirty metadata outside a transaction would defeat the ordering model.
       Refuse instead of silently publishing it ahead of VolumeDirty. */
    if (!Cache_IsClean(sb->cache))
        return ERROR_DISK_NOT_VALIDATED;

    original_flags = sb->volume_flags;
    flags = exfat_volume_flags_begin_write(original_flags);
    err = WriteMainBootStatus(sb, flags, FALSE);
    if (err == 0)
        err = SyncWrite(sb);
    if (err != 0)
    {
        sb->volume_flags = flags; /* conservative: do not attempt another */
        return err;
    }

    sb->write_transaction_active = TRUE;
    sb->write_transaction_failed = FALSE;
    sb->write_transaction_changed = FALSE;
    sb->write_transaction_original_flags = original_flags;
    sb->allocation_changed = FALSE;
    return 0;
}

LONG ExfatFlushWriteStage(struct FSSuper *sb)
{
    LONG err;

    if (!sb->write_transaction_active)
        return ERROR_OBJECT_IN_USE;
    if (sb->write_transaction_failed)
    {
        KeepVolumeDirty(sb);
        sb->write_transaction_active = FALSE;
        return ERROR_DISK_NOT_VALIDATED;
    }
    err = SyncWrite(sb);
    if (err != 0)
    {
        KeepVolumeDirty(sb);
        sb->write_transaction_active = FALSE;
    }
    return err;
}

LONG ExfatCommitWrite(struct FSSuper *sb)
{
    UWORD clean_flags;
    LONG err;

    if (!sb->write_transaction_active)
        return ERROR_OBJECT_IN_USE;

    if (sb->write_transaction_failed)
    {
        KeepVolumeDirty(sb);
        sb->write_transaction_active = FALSE;
        return ERROR_DISK_NOT_VALIDATED;
    }

    /* First make every preceding FAT/bitmap/directory stage durable. */
    err = SyncWrite(sb);
    if (err == 0)
    {
        clean_flags = exfat_volume_flags_finish_write(sb->volume_flags,
            FALSE, TRUE);
        err = WriteMainBootStatus(sb, clean_flags, sb->allocation_changed);
        if (err == 0)
            err = SyncWrite(sb);
    }

    if (err != 0)
        KeepVolumeDirty(sb);
    sb->write_transaction_active = FALSE;
    sb->write_transaction_changed = FALSE;
    return err;
}

LONG ExfatAbortWrite(struct FSSuper *sb)
{
    UWORD flags;
    LONG err;

    if (!sb->write_transaction_active)
        return ERROR_OBJECT_IN_USE;

    /* Allocation planning and other preflight checks can fail after the
       durable VolumeDirty barrier but before changing any filesystem state.
       Restore the exact original flags in that case; turning a harmless
       ENOSPC into a repair-required volume is both unnecessary and hostile.
       Every actual data/FAT/bitmap/directory mutation marks the transaction
       changed before touching memory or cache, so this is not a rollback. */
    if (!sb->write_transaction_changed && !sb->write_transaction_failed)
    {
        flags = sb->write_transaction_original_flags;
        err = WriteMainBootStatus(sb, flags, FALSE);
        if (err == 0)
            err = SyncWrite(sb);
        if (err != 0)
            KeepVolumeDirty(sb);
        sb->write_transaction_active = FALSE;
        sb->write_transaction_changed = FALSE;
        return err;
    }

    /* There is no rollback log after a real mutation.  Make any dirty cache
       sectors durable under VolumeDirty and leave repair/replay to a tool. */
    err = SyncWrite(sb);
    KeepVolumeDirty(sb);
    sb->write_transaction_active = FALSE;
    sb->write_transaction_changed = FALSE;
    return err;
}
