/*
 * exfat-handler - stream I/O and FAT traversal
 *
 * Copyright (C) 2026 The AROS Development Team
 */

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "exfat_fs.h"
#include "exfat_meta.h"
#include "exfat_protos.h"

#define EXFAT_EOC_MIN 0xfffffff8UL

static LONG ReadSector(struct FSSuper *sb, UQUAD sector, UBYTE *out)
{
    struct Globals *glob = sb->glob;
    UBYTE *data;
    APTR block = Cache_GetBlock(sb->cache, sector, &data);

    if (block == NULL)
        return ERROR_UNKNOWN;
    CopyMem(data, out, sb->sector_size);
    Cache_FreeBlock(sb->cache, block);
    return 0;
}

LONG ExfatFatNext(struct FSSuper *sb, ULONG cluster, ULONG *next)
{
    UQUAD byte_offset, sector;
    ULONG in_sector;
    UBYTE *data;
    APTR block;

    if (!exfat_cluster_valid(sb, cluster))
        return ERROR_BAD_NUMBER;

    byte_offset = (UQUAD)cluster * 4;
    sector = sb->fat_start + byte_offset / sb->sector_size;
    in_sector = (ULONG)(byte_offset % sb->sector_size);
    block = Cache_GetBlock(sb->cache, sector, &data);
    if (block == NULL)
        return ERROR_UNKNOWN;
    *next = exfat_rd32(data, in_sector);
    Cache_FreeBlock(sb->cache, block);

    if (*next >= EXFAT_EOC_MIN)
        return ERROR_NO_MORE_ENTRIES;
    if (*next == 0 || *next == 0xfffffff7UL || !exfat_cluster_valid(sb, *next))
        return ERROR_BAD_NUMBER;
    return 0;
}

static BOOL ClusterAllocated(const struct FSSuper *sb, ULONG cluster)
{
    ULONG bit;

    if (sb->bitmap == NULL)
        return TRUE;                    /* while loading the bitmap itself */
    if (!exfat_cluster_valid(sb, cluster))
        return FALSE;
    bit = cluster - 2;
    return (sb->bitmap[bit >> 3] & (1U << (bit & 7))) != 0;
}

LONG ExfatClusterAt(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD ordinal, ULONG *result)
{
    ULONG cur, i;
    UQUAD count;
    LONG err;

    if (!exfat_cluster_valid(sb, stream->first_cluster))
        return ERROR_BAD_NUMBER;

    if (stream->length_known)
    {
        if (!exfat_stream_cluster_count(stream->data_length,
                sb->cluster_size, &count))
            return ERROR_BAD_NUMBER;
        if (ordinal >= count)
            return ERROR_SEEK_ERROR;
    }
    else if (ordinal >= sb->cluster_count)
        return ERROR_BAD_NUMBER;        /* also bounds a cyclic root chain */

    if (stream->contiguous)
    {
        UQUAD candidate = (UQUAD)stream->first_cluster + ordinal;
        if (candidate > 0xffffffffULL
            || !exfat_cluster_valid(sb, (ULONG)candidate))
            return ERROR_BAD_NUMBER;
        cur = (ULONG)candidate;
    }
    else
    {
        ULONG slow = stream->first_cluster;
        ULONG fast = stream->first_cluster;
        BOOL cycle_check = TRUE;

        cur = stream->first_cluster;
        for (i = 0; (UQUAD)i < ordinal; i++)
        {
            err = ExfatFatNext(sb, cur, &cur);
            if (err != 0)
                return err == ERROR_NO_MORE_ENTRIES
                    ? (stream->length_known
                        ? ERROR_BAD_NUMBER : ERROR_NO_MORE_ENTRIES)
                    : err;

            if (cycle_check)
            {
                err = ExfatFatNext(sb, slow, &slow);
                if (err != 0)
                    cycle_check = FALSE;
                if (cycle_check)
                {
                    err = ExfatFatNext(sb, fast, &fast);
                    if (err == 0)
                        err = ExfatFatNext(sb, fast, &fast);
                    if (err != 0)
                        cycle_check = FALSE;
                }
                if (cycle_check && slow == fast)
                    return ERROR_BAD_NUMBER;
            }
        }
    }

    if (!ClusterAllocated(sb, cur))
        return ERROR_BAD_NUMBER;
    *result = cur;
    return 0;
}

LONG ExfatReadStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD offset, APTR buffer, ULONG length, ULONG *actual)
{
    struct Globals *glob = sb->glob;
    UBYTE *dst = buffer;
    UBYTE *sector_buf = NULL;
    ULONG done = 0;
    LONG err = 0;

    *actual = 0;
    if (length == 0)
        return 0;
    if (stream->length_known && offset >= stream->data_length)
        return 0;

    sector_buf = AllocVec(sb->sector_size, MEMF_PUBLIC);
    if (sector_buf == NULL)
        return ERROR_NO_FREE_STORE;

    while (done < length)
    {
        UQUAD pos = offset + done;
        UQUAD cluster_ordinal = pos / sb->cluster_size;
        ULONG in_cluster = (ULONG)(pos % sb->cluster_size);
        ULONG in_sector = in_cluster & (sb->sector_size - 1);
        ULONG cluster, take = sb->sector_size - in_sector;
        UQUAD sector;

        if (stream->length_known)
        {
            UQUAD left = stream->data_length - pos;
            if (pos >= stream->data_length)
                break;
            if ((UQUAD)take > left)
                take = (ULONG)left;
        }
        if (take > length - done)
            take = length - done;

        /* Uninitialised file tails are specified as zeroes, never disk data. */
        if (stream->length_known && pos >= stream->valid_data_length)
        {
            memset(dst + done, 0, take);
            done += take;
            continue;
        }
        if (stream->length_known
            && pos + take > stream->valid_data_length)
            take = (ULONG)(stream->valid_data_length - pos);

        err = ExfatClusterAt(sb, stream, cluster_ordinal, &cluster);
        if (err != 0)
            break;
        sector = EXFAT_SECTOR_FROM_CLUSTER(sb, cluster)
            + in_cluster / sb->sector_size;
        err = ReadSector(sb, sector, sector_buf);
        if (err != 0)
            break;
        CopyMem(sector_buf + in_sector, dst + done, take);
        done += take;
    }

    FreeVec(sector_buf);
    *actual = done;
    return err;
}

LONG ExfatWriteStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD offset, CONST_APTR buffer, ULONG length, ULONG *actual)
{
    struct Globals *glob = sb->glob;
    const UBYTE *src = buffer;
    ULONG done = 0;
    LONG err = 0;

    *actual = 0;
    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (offset > ~(UQUAD)0 - length)
        return ERROR_SEEK_ERROR;
    if (stream->length_known && (offset > stream->data_length
            || (UQUAD)length > stream->data_length - offset))
        return ERROR_SEEK_ERROR;

    while (done < length)
    {
        UQUAD pos = offset + done;
        UQUAD cluster_ordinal = pos / sb->cluster_size;
        ULONG in_cluster = (ULONG)(pos % sb->cluster_size);
        ULONG in_sector = in_cluster & (sb->sector_size - 1);
        ULONG cluster, take = sb->sector_size - in_sector;
        UQUAD sector;
        UBYTE *data;
        APTR block;

        if (take > length - done)
            take = length - done;
        err = ExfatClusterAt(sb, stream, cluster_ordinal, &cluster);
        if (err != 0)
            break;
        sector = EXFAT_SECTOR_FROM_CLUSTER(sb, cluster)
            + in_cluster / sb->sector_size;
        block = Cache_GetBlock(sb->cache, sector, &data);
        if (block == NULL)
        {
            err = IoErr() != 0 ? IoErr() : ERROR_UNKNOWN;
            break;
        }
        sb->write_transaction_changed = TRUE;
        CopyMem(src + done, data + in_sector, take);
        if (!Cache_MarkBlockDirtySector(sb->cache, block, sector))
            err = IoErr() != 0 ? IoErr() : ERROR_UNKNOWN;
        Cache_FreeBlock(sb->cache, block);
        if (err != 0)
            break;
        done += take;
    }

    *actual = done;
    if (err != 0)
        sb->write_transaction_failed = TRUE;
    return err;
}

LONG ExfatZeroStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD offset, UQUAD length)
{
    UBYTE zeroes[512];
    ULONG take, actual;
    LONG err;

    memset(zeroes, 0, sizeof(zeroes));
    while (length != 0)
    {
        take = length > sizeof(zeroes) ? sizeof(zeroes) : (ULONG)length;
        err = ExfatWriteStream(sb, stream, offset, zeroes, take, &actual);
        if (err != 0 || actual != take)
        {
            sb->write_transaction_failed = TRUE;
            return err != 0 ? err : ERROR_UNKNOWN;
        }
        offset += take;
        length -= take;
    }
    return 0;
}

LONG ExfatZeroClusterRun(struct FSSuper *sb, ULONG first, ULONG count)
{
    struct Globals *glob = sb->glob;
    ULONG cluster, within;
    LONG err = 0;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (!exfat_bitmap_range_valid(first, count, sb->cluster_count))
        return ERROR_BAD_NUMBER;

    for (cluster = 0; cluster < count && err == 0; cluster++)
    {
        UQUAD sector = EXFAT_SECTOR_FROM_CLUSTER(sb, first + cluster);

        for (within = 0; within < (1UL << sb->cluster_shift); within++)
        {
            UBYTE *data;
            APTR block = Cache_GetBlock(sb->cache, sector + within, &data);

            if (block == NULL)
            {
                err = IoErr() != 0 ? IoErr() : ERROR_UNKNOWN;
                break;
            }
            sb->write_transaction_changed = TRUE;
            memset(data, 0, sb->sector_size);
            if (!Cache_MarkBlockDirtySector(sb->cache, block,
                    sector + within))
                err = IoErr() != 0 ? IoErr() : ERROR_UNKNOWN;
            Cache_FreeBlock(sb->cache, block);
            if (err != 0)
                break;
        }
    }
    if (err != 0)
        sb->write_transaction_failed = TRUE;
    return err;
}

LONG ExfatSetFatEntry(struct FSSuper *sb, ULONG cluster, ULONG value)
{
    struct Globals *glob = sb->glob;
    UQUAD byte_offset, sector;
    ULONG in_sector;
    UBYTE *data;
    APTR block;
    LONG err = 0;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (!exfat_cluster_valid(sb, cluster)
        || (value != 0 && value < EXFAT_EOC_MIN
            && !exfat_cluster_valid(sb, value)))
        return ERROR_BAD_NUMBER;

    byte_offset = (UQUAD)cluster * 4;
    sector = sb->fat_start + byte_offset / sb->sector_size;
    in_sector = (ULONG)(byte_offset % sb->sector_size);
    block = Cache_GetBlock(sb->cache, sector, &data);
    if (block == NULL)
        err = IoErr() != 0 ? IoErr() : ERROR_UNKNOWN;
    else
    {
        sb->write_transaction_changed = TRUE;
        exfat_wr32(data, in_sector, value);
        if (!Cache_MarkBlockDirtySector(sb->cache, block, sector))
            err = IoErr() != 0 ? IoErr() : ERROR_UNKNOWN;
        Cache_FreeBlock(sb->cache, block);
    }
    if (err != 0)
        sb->write_transaction_failed = TRUE;
    return err;
}

LONG ExfatLoadStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UBYTE **buffer, ULONG *length)
{
    struct Globals *glob = sb->glob;
    ULONG got;
    LONG err;

    *buffer = NULL;
    *length = 0;
    if (!stream->length_known || stream->data_length > 0xffffffffULL)
        return ERROR_BAD_NUMBER;
    if (stream->data_length == 0)
        return 0;

    *length = (ULONG)stream->data_length;
    *buffer = AllocVec(*length, MEMF_PUBLIC);
    if (*buffer == NULL)
        return ERROR_NO_FREE_STORE;
    err = ExfatReadStream(sb, stream, 0, *buffer, *length, &got);
    if (err != 0 || got != *length)
    {
        FreeVec(*buffer);
        *buffer = NULL;
        *length = 0;
        return err != 0 ? err : ERROR_BAD_NUMBER;
    }
    return 0;
}
