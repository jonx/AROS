/* exfat-handler - allocation bitmap mutation */

#include <exec/types.h>
#include <dos/dos.h>

#include "exfat_fs.h"
#include "exfat_meta.h"
#include "exfat_protos.h"

LONG ExfatPlanContiguous(struct FSSuper *sb, ULONG hint, ULONG count,
    ULONG *first)
{
    if (count == 0 || count > sb->free_clusters)
        return ERROR_DISK_FULL;
    return exfat_bitmap_find_free_run(sb->bitmap, sb->cluster_count,
        hint, count, first) ? 0 : ERROR_DISK_FULL;
}

LONG ExfatReserveBitmapRange(struct FSSuper *sb, ULONG first, ULONG count,
    BOOL allocated)
{
    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (!exfat_bitmap_range_valid(first, count, sb->cluster_count))
        return ERROR_BAD_NUMBER;
    if (!exfat_bitmap_range_is(sb->bitmap, first, count, sb->cluster_count,
            allocated ? 0 : 1))
        return ERROR_OBJECT_IN_USE;
    if (allocated && count > sb->free_clusters)
        return ERROR_DISK_FULL;

    sb->write_transaction_changed = TRUE;
    exfat_bitmap_set_range(sb->bitmap, first, count, allocated);
    if (allocated)
        sb->free_clusters -= count;
    else
        sb->free_clusters += count;
    sb->allocation_changed = TRUE;

    return 0;
}

LONG ExfatWriteBitmapRange(struct FSSuper *sb, ULONG first, ULONG count)
{
    ULONG first_byte, last_byte, got;
    UBYTE mask;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (!exfat_bitmap_range_valid(first, count, sb->cluster_count)
        || !exfat_bitmap_address(first, sb->cluster_count,
            &first_byte, &mask)
        || !exfat_bitmap_address(first + count - 1, sb->cluster_count,
            &last_byte, &mask))
        return ERROR_BAD_NUMBER;

    err = ExfatWriteStream(sb, &sb->bitmap_stream, first_byte,
        sb->bitmap + first_byte, last_byte - first_byte + 1, &got);
    if (err != 0 || got != last_byte - first_byte + 1)
    {
        sb->write_transaction_failed = TRUE;
        return err != 0 ? err : ERROR_UNKNOWN;
    }
    return 0;
}

LONG ExfatSetBitmapRange(struct FSSuper *sb, ULONG first, ULONG count,
    BOOL allocated)
{
    LONG err = ExfatReserveBitmapRange(sb, first, count, allocated);

    return err == 0 ? ExfatWriteBitmapRange(sb, first, count) : err;
}

LONG ExfatGrowFile(struct FSSuper *sb, struct exfat_entry *entry,
    UQUAD new_length)
{
    struct exfat_stream old = entry->stream;
    UQUAD old_count64, new_count64;
    ULONG old_count, new_count, added, run, i, last;
    BOOL needs_fat = FALSE;
    BOOL convert_contiguous = FALSE;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (!old.length_known || new_length <= old.data_length)
        return ERROR_BAD_NUMBER;
    if (!exfat_stream_cluster_count(old.data_length, sb->cluster_size,
            &old_count64)
        || !exfat_stream_cluster_count(new_length, sb->cluster_size,
            &new_count64)
        || new_count64 > sb->cluster_count)
        return ERROR_OBJECT_TOO_LARGE;
    if (new_count64 == old_count64)
    {
        entry->stream.data_length = new_length;
        return 0;
    }

    old_count = (ULONG)old_count64;
    new_count = (ULONG)new_count64;
    added = new_count - old_count;
    if (old_count == 0)
    {
        err = ExfatPlanContiguous(sb, 2, added, &run);
    }
    else if (old.contiguous
        && (UQUAD)old.first_cluster + old_count + added
            <= (UQUAD)sb->cluster_count + 2
        && exfat_bitmap_range_is(sb->bitmap,
            old.first_cluster + old_count, added, sb->cluster_count, 0))
    {
        run = old.first_cluster + old_count;
        err = 0;
    }
    else
    {
        err = ExfatPlanContiguous(sb,
            old_count != 0 ? old.first_cluster + old_count : 2,
            added, &run);
        needs_fat = TRUE;
        convert_contiguous = old.contiguous;
    }
    if (err != 0)
        return err;

    err = ExfatReserveBitmapRange(sb, run, added, TRUE);
    if (err == 0)
        err = ExfatZeroClusterRun(sb, run, added);
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    if (err != 0)
        return err;

    if (needs_fat)
    {
        if (convert_contiguous)
        {
            for (i = 0; i < old_count && err == 0; i++)
                err = ExfatSetFatEntry(sb, old.first_cluster + i,
                    i + 1 < old_count ? old.first_cluster + i + 1 : run);
        }
        else
        {
            err = ExfatClusterAt(sb, &old, old_count - 1, &last);
            if (err == 0)
                err = ExfatSetFatEntry(sb, last, run);
        }
        for (i = 0; i < added && err == 0; i++)
            err = ExfatSetFatEntry(sb, run + i,
                i + 1 < added ? run + i + 1 : 0xffffffffUL);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        if (err != 0)
            return err;
    }

    err = ExfatWriteBitmapRange(sb, run, added);
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    if (err != 0)
        return err;

    if (old_count == 0)
    {
        entry->stream.first_cluster = run;
        entry->stream.contiguous = TRUE;
    }
    else if (convert_contiguous)
        entry->stream.contiguous = FALSE;
    entry->stream.data_length = new_length;
    return 0;
}

LONG ExfatResizeFile(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    UQUAD new_length)
{
    struct exfat_stream old = entry->stream;
    UQUAD old_count64, new_count64;
    ULONG old_count, new_count, released;
    ULONG first_free = 0, kept_last = 0, current, next, i;
    LONG next_err, err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (!old.length_known)
        return ERROR_BAD_NUMBER;
    if (new_length == old.data_length)
        return 0;
    if (new_length > old.data_length)
    {
        err = ExfatGrowFile(sb, entry, new_length);
        return err == 0
            ? ExfatUpdateEntryStream(sb, directory, entry) : err;
    }

    if (!exfat_stream_cluster_count(old.data_length, sb->cluster_size,
            &old_count64)
        || !exfat_stream_cluster_count(new_length, sb->cluster_size,
            &new_count64))
        return ERROR_BAD_NUMBER;
    old_count = (ULONG)old_count64;
    new_count = (ULONG)new_count64;
    released = old_count - new_count;

    if (released != 0)
    {
        if (old.contiguous)
            first_free = old.first_cluster + new_count;
        else
        {
            err = ExfatClusterAt(sb, &old, new_count, &first_free);
            if (err != 0)
                return err;
            if (new_count != 0)
            {
                err = ExfatClusterAt(sb, &old, new_count - 1, &kept_last);
                if (err != 0)
                    return err;
            }
        }
    }

    entry->stream.data_length = new_length;
    if (entry->stream.valid_data_length > new_length)
        entry->stream.valid_data_length = new_length;
    if (new_count == 0)
    {
        entry->stream.first_cluster = 0;
        entry->stream.contiguous = TRUE;
    }
    err = ExfatUpdateEntryStream(sb, directory, entry);
    if (err == 0 && released != 0)
        err = ExfatFlushWriteStage(sb);
    if (err != 0 || released == 0)
        return err;

    if (old.contiguous)
    {
        err = ExfatReserveBitmapRange(sb, first_free, released, FALSE);
        if (err == 0)
            err = ExfatWriteBitmapRange(sb, first_free, released);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        return err;
    }

    if (new_count != 0)
    {
        err = ExfatSetFatEntry(sb, kept_last, 0xffffffffUL);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        if (err != 0)
            return err;
    }

    current = first_free;
    for (i = 0; i < released; i++)
    {
        next_err = ExfatFatNext(sb, current, &next);
        if (next_err != 0 && next_err != ERROR_NO_MORE_ENTRIES)
            return next_err;
        if (i + 1 < released && next_err == ERROR_NO_MORE_ENTRIES)
            return ERROR_BAD_NUMBER;

        err = ExfatSetFatEntry(sb, current, 0);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        if (err == 0)
            err = ExfatReserveBitmapRange(sb, current, 1, FALSE);
        if (err == 0)
            err = ExfatWriteBitmapRange(sb, current, 1);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        if (err != 0)
            return err;
        if (i + 1 < released)
            current = next;
    }
    return 0;
}

LONG ExfatFreeStream(struct FSSuper *sb,
    const struct exfat_stream *stream)
{
    UQUAD count64;
    ULONG count, current, next = 0, i;
    LONG next_err, err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (!stream->length_known
        || !exfat_stream_cluster_count(stream->data_length,
            sb->cluster_size, &count64)
        || count64 > sb->cluster_count)
        return ERROR_BAD_NUMBER;
    if (count64 == 0)
        return 0;
    count = (ULONG)count64;

    if (stream->contiguous)
    {
        err = ExfatReserveBitmapRange(sb, stream->first_cluster,
            count, FALSE);
        if (err == 0)
            err = ExfatWriteBitmapRange(sb, stream->first_cluster, count);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        return err;
    }

    current = stream->first_cluster;
    for (i = 0; i < count; i++)
    {
        next_err = ExfatFatNext(sb, current, &next);
        if (next_err != 0 && next_err != ERROR_NO_MORE_ENTRIES)
            return next_err;
        if (i + 1 < count && next_err == ERROR_NO_MORE_ENTRIES)
            return ERROR_BAD_NUMBER;
        err = ExfatSetFatEntry(sb, current, 0);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        if (err == 0)
            err = ExfatReserveBitmapRange(sb, current, 1, FALSE);
        if (err == 0)
            err = ExfatWriteBitmapRange(sb, current, 1);
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        if (err != 0)
            return err;
        if (i + 1 < count)
            current = next;
    }
    return 0;
}
