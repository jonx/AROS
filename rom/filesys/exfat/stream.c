/*
 * exfat-handler - read-only stream and FAT traversal
 *
 * Copyright (C) 2026 The AROS Development Team
 */

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>

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

static LONG FatNext(struct FSSuper *sb, ULONG cluster, ULONG *next)
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

static LONG ClusterAt(struct FSSuper *sb, const struct exfat_stream *stream,
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
            err = FatNext(sb, cur, &cur);
            if (err != 0)
                return err == ERROR_NO_MORE_ENTRIES
                    ? ERROR_BAD_NUMBER : err;

            if (cycle_check)
            {
                err = FatNext(sb, slow, &slow);
                if (err != 0)
                    cycle_check = FALSE;
                if (cycle_check)
                {
                    err = FatNext(sb, fast, &fast);
                    if (err == 0)
                        err = FatNext(sb, fast, &fast);
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

        err = ClusterAt(sb, stream, cluster_ordinal, &cluster);
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
