/*
 * exfat-handler - root metadata and directory entry sets
 *
 * Copyright (C) 2026 The AROS Development Team
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "exfat_fs.h"
#include "exfat_meta.h"
#include "exfat_protos.h"

#define ENTRY_SIZE       32
#define ENTRY_BITMAP     0x81
#define ENTRY_UPCASE     0x82
#define ENTRY_LABEL      0x83
#define ENTRY_FILE       0x85
#define ENTRY_STREAM     0xc0
#define ENTRY_NAME       0xc1
#define EXFAT_ENTRY_SET_MAX 256U
static LONG ReadEntry(struct FSSuper *sb, const struct exfat_stream *dir,
    ULONG index, UBYTE out[ENTRY_SIZE])
{
    ULONG got;
    UQUAD off = (UQUAD)index * ENTRY_SIZE;
    LONG err = ExfatReadStream(sb, dir, off, out, ENTRY_SIZE, &got);

    if (err != 0)
        return err;
    return got == ENTRY_SIZE ? 0 : ERROR_BAD_NUMBER;
}

static void EntryStream(const UBYTE *e, struct exfat_stream *stream)
{
    stream->first_cluster = exfat_rd32(e, 20);
    stream->valid_data_length = exfat_rd64(e, 8);
    stream->data_length = exfat_rd64(e, 24);
    stream->contiguous = (e[1] & 2) != 0;
    stream->length_known = TRUE;
}

static LONG ValidateStream(struct FSSuper *sb,
    const struct exfat_stream *stream)
{
    UQUAD clusters;

    if (stream->valid_data_length > stream->data_length)
        return ERROR_BAD_NUMBER;
    if (stream->data_length == 0)
        return stream->first_cluster == 0 ? 0 : ERROR_BAD_NUMBER;
    if (!exfat_cluster_valid(sb, stream->first_cluster))
        return ERROR_BAD_NUMBER;
    if (!exfat_stream_cluster_count(stream->data_length, sb->cluster_size,
            &clusters))
        return ERROR_BAD_NUMBER;
    if (clusters > sb->cluster_count)
        return ERROR_BAD_NUMBER;
    if (stream->contiguous
        && clusters - 1 > (UQUAD)sb->cluster_count + 1
            - stream->first_cluster)
        return ERROR_BAD_NUMBER;
    return 0;
}

static LONG LoadUpcase(struct FSSuper *sb, const struct exfat_stream *stream,
    ULONG expected_checksum)
{
    struct Globals *glob = sb->glob;
    UBYTE *raw;
    ULONG length, checksum = 0, pos = 0, cp = 0;
    LONG err;

    err = ExfatLoadStream(sb, stream, &raw, &length);
    if (err != 0)
        return err;
    if ((length & 1) != 0)
    {
        FreeVec(raw);
        return ERROR_BAD_NUMBER;
    }
    while (pos < length)
        checksum = exfat_meta_rotate32(checksum, raw[pos++]);
    if (checksum != expected_checksum)
    {
        FreeVec(raw);
        return ERROR_DISK_NOT_VALIDATED;
    }

    sb->upcase = AllocVec(65536UL * sizeof(UWORD), MEMF_PUBLIC);
    if (sb->upcase == NULL)
    {
        FreeVec(raw);
        return ERROR_NO_FREE_STORE;
    }

    if (!exfat_expand_upcase(raw, length, sb->upcase, 65536UL, &cp))
        err = ERROR_BAD_NUMBER;
    FreeVec(raw);
    if (err == 0 && cp != 65536UL)
        err = ERROR_BAD_NUMBER;
    if (err != 0)
    {
        FreeVec(sb->upcase);
        sb->upcase = NULL;
    }
    return err;
}

static LONG LoadFileEntrySet(struct FSSuper *sb,
    const struct exfat_stream *directory, ULONG index,
    UBYTE **set_out, ULONG *count_out)
{
    struct Globals *glob = sb->glob;
    UBYTE first[ENTRY_SIZE], *set;
    ULONG count, i;
    LONG err;

    err = ReadEntry(sb, directory, index, first);
    if (err != 0)
        return err;
    count = (ULONG)first[1] + 1;
    if (first[0] != ENTRY_FILE || count < 3
        || (directory->length_known
            && ((UQUAD)index + count) * ENTRY_SIZE
                > directory->data_length))
        return ERROR_BAD_NUMBER;
    set = AllocVec(count * ENTRY_SIZE, MEMF_PUBLIC);
    if (set == NULL)
        return ERROR_NO_FREE_STORE;
    CopyMem(first, set, ENTRY_SIZE);
    for (i = 1; i < count; i++)
    {
        err = ReadEntry(sb, directory, index + i, set + i * ENTRY_SIZE);
        if (err != 0)
        {
            FreeVec(set);
            return err == ERROR_NO_MORE_ENTRIES ? ERROR_BAD_NUMBER : err;
        }
    }
    if (set[ENTRY_SIZE] != ENTRY_STREAM
        || exfat_entry_set_checksum(set, (UBYTE)(count - 1))
            != exfat_rd16(set, 2))
    {
        FreeVec(set);
        return ERROR_DISK_NOT_VALIDATED;
    }
    *set_out = set;
    *count_out = count;
    return 0;
}

static LONG LoadGenericEntrySet(struct FSSuper *sb,
    const struct exfat_stream *directory, ULONG index,
    const UBYTE first[ENTRY_SIZE], UBYTE **set_out, ULONG *count_out)
{
    struct Globals *glob = sb->glob;
    UBYTE *set;
    ULONG count = (ULONG)first[1] + 1, i;
    LONG err;

    if (directory->length_known
        && ((UQUAD)index + count) * ENTRY_SIZE > directory->data_length)
        return ERROR_BAD_NUMBER;
    set = AllocVec(count * ENTRY_SIZE, MEMF_PUBLIC);
    if (set == NULL)
        return ERROR_NO_FREE_STORE;
    CopyMem(first, set, ENTRY_SIZE);
    for (i = 1; i < count; i++)
    {
        err = ReadEntry(sb, directory, index + i, set + i * ENTRY_SIZE);
        if (err != 0)
        {
            FreeVec(set);
            return err == ERROR_NO_MORE_ENTRIES ? ERROR_BAD_NUMBER : err;
        }
        if ((set[i * ENTRY_SIZE] & 0xc0U) != 0xc0U)
        {
            FreeVec(set);
            return ERROR_BAD_NUMBER;
        }
    }
    if (exfat_entry_set_checksum(set, (UBYTE)(count - 1))
            != exfat_rd16(set, 2))
    {
        FreeVec(set);
        return ERROR_DISK_NOT_VALIDATED;
    }
    *set_out = set;
    *count_out = count;
    return 0;
}

static LONG GenericAllocation(struct FSSuper *sb, const UBYTE *entry,
    ULONG flag_offset, struct exfat_stream *allocation, BOOL *present)
{
    UBYTE flags = entry[flag_offset];

    *present = FALSE;
    if ((flags & (UBYTE)~3U) != 0 || ((flags & 1U) == 0 && (flags & 2U) != 0))
        return ERROR_BAD_NUMBER;
    if ((flags & 1U) == 0)
        return 0;
    memset(allocation, 0, sizeof(*allocation));
    allocation->first_cluster = exfat_rd32(entry, 20);
    allocation->data_length = exfat_rd64(entry, 24);
    allocation->valid_data_length = allocation->data_length;
    allocation->contiguous = (flags & 2U) != 0;
    allocation->length_known = TRUE;
    if (allocation->data_length == 0 && allocation->contiguous)
        return ERROR_BAD_NUMBER;
    *present = TRUE;
    return ValidateStream(sb, allocation);
}

static LONG CheckBenignPrimarySet(struct FSSuper *sb,
    const struct exfat_stream *directory, ULONG index,
    const UBYTE first[ENTRY_SIZE], ULONG *count_out, BOOL purge)
{
    struct Globals *glob = sb->glob;
    struct exfat_stream allocation;
    UBYTE *set;
    ULONG count, i, got;
    BOOL present;
    LONG err = LoadGenericEntrySet(sb, directory, index, first, &set, &count);

    if (err != 0)
        return err;
    err = GenericAllocation(sb, set, 4, &allocation, &present);
    for (i = 1; err == 0 && i < count; i++)
        err = GenericAllocation(sb, set + i * ENTRY_SIZE, 1,
            &allocation, &present);
    if (err != 0 || !purge)
    {
        FreeVec(set);
        if (err == 0)
            *count_out = count;
        return err;
    }

    /* Hide the whole generic set before releasing any of its ownership. */
    set[0] &= 0x7f;
    err = ExfatWriteStream(sb, directory, (UQUAD)index * ENTRY_SIZE,
        set, ENTRY_SIZE, &got);
    if (err == 0 && got != ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err == 0)
        err = ExfatFlushWriteStage(sb);

    if (err == 0)
    {
        err = GenericAllocation(sb, set, 4, &allocation, &present);
        if (err == 0 && present)
            err = ExfatFreeStream(sb, &allocation);
    }
    for (i = 1; err == 0 && i < count; i++)
    {
        err = GenericAllocation(sb, set + i * ENTRY_SIZE, 1,
            &allocation, &present);
        if (err == 0 && present)
            err = ExfatFreeStream(sb, &allocation);
    }
    for (i = 1; i < count; i++)
        set[i * ENTRY_SIZE] &= 0x7f;
    if (err == 0)
    {
        err = ExfatWriteStream(sb, directory, (UQUAD)index * ENTRY_SIZE,
            set, count * ENTRY_SIZE, &got);
        if (err == 0 && got != count * ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err != 0)
        sb->write_transaction_failed = TRUE;
    FreeVec(set);
    if (err == 0)
        *count_out = count;
    return err;
}

LONG ExfatLoadMetadata(struct FSSuper *sb)
{
    struct Globals *glob = sb->glob;
    struct exfat_stream bitmap = {0}, upcase = {0};
    UBYTE *bitmap_data;
    UQUAD bitmap_needed = ((UQUAD)sb->cluster_count + 7) >> 3;
    ULONG upcase_checksum = 0, index = 0, got, i;
    UBYTE e[ENTRY_SIZE];
    BOOL have_bitmap = FALSE, have_upcase = FALSE;
    LONG err;

    sb->volume.name[0] = 0;
    for (;; index++)
    {
        err = ReadEntry(sb, &sb->root, index, e);
        if (err == ERROR_NO_MORE_ENTRIES)
            break;
        if (err != 0)
        {
            bug("[exfat] metadata: root entry %lu read failed (%ld)\n",
                (unsigned long)index, (long)err);
            return err;
        }
        if (e[0] == 0)
            break;
        if ((e[0] & 0x80) == 0)
            continue;

        switch (e[0])
        {
        case ENTRY_BITMAP:
            if (have_bitmap || (e[1] & 1) != 0)
                return ERROR_OBJECT_WRONG_TYPE;
            bitmap.first_cluster = exfat_rd32(e, 20);
            bitmap.data_length = exfat_rd64(e, 24);
            bitmap.valid_data_length = bitmap.data_length;
            /* System files have no NoFatChain flag; follow their FAT chain. */
            bitmap.contiguous = FALSE;
            bitmap.length_known = TRUE;
            have_bitmap = TRUE;
            break;
        case ENTRY_UPCASE:
            if (have_upcase)
                return ERROR_OBJECT_WRONG_TYPE;
            upcase_checksum = exfat_rd32(e, 4);
            upcase.first_cluster = exfat_rd32(e, 20);
            upcase.data_length = exfat_rd64(e, 24);
            upcase.valid_data_length = upcase.data_length;
            upcase.contiguous = FALSE;
            upcase.length_known = TRUE;
            have_upcase = TRUE;
            break;
        case ENTRY_LABEL:
            if (e[1] > 15)
                return ERROR_BAD_NUMBER;
            sb->volume.name[0] = e[1];
            for (i = 0; i < e[1]; i++)
            {
                UWORD c = exfat_rd16(e, 2 + i * 2);
                sb->volume.name[i + 1] = c <= 255 ? (UBYTE)c : '_';
            }
            sb->volume.name[e[1] + 1] = 0;
            break;
        default:
            /* Unknown benign entries are ignorable; unknown critical
               primaries make the directory uninterpretable. */
            if ((e[0] & 0x40) == 0 && (e[0] & 0x20) == 0
                && e[0] != ENTRY_FILE)
                return ERROR_BAD_NUMBER;
            break;
        }
    }

    if (!have_bitmap || !have_upcase || bitmap.data_length < bitmap_needed)
    {
        bug("[exfat] metadata: missing/short bitmap or upcase table\n");
        return ERROR_BAD_NUMBER;
    }
    if ((err = ValidateStream(sb, &bitmap)) != 0
        || (err = ValidateStream(sb, &upcase)) != 0)
    {
        bug("[exfat] metadata: system stream invalid (%ld)\n", (long)err);
        return err;
    }
    if (bitmap_needed > 0xffffffffULL)
        return ERROR_BAD_NUMBER;

    bitmap_data = AllocVec((ULONG)bitmap_needed, MEMF_PUBLIC);
    if (bitmap_data == NULL)
        return ERROR_NO_FREE_STORE;
    /* Do not publish the bitmap until it is fully read. ExfatReadStream()
       treats a published bitmap as authoritative and validates every cluster
       against it; publishing this uninitialised allocation would make the
       bitmap bootstrap depend on stale allocator contents. */
    err = ExfatReadStream(sb, &bitmap, 0, bitmap_data,
        (ULONG)bitmap_needed, &got);
    if (err != 0 || got != (ULONG)bitmap_needed)
    {
        bug("[exfat] metadata: bitmap read failed (%ld, %lu/%lu)\n",
            (long)err, (unsigned long)got, (unsigned long)bitmap_needed);
        FreeVec(bitmap_data);
        return err != 0 ? err : ERROR_BAD_NUMBER;
    }
    sb->bitmap = bitmap_data;
    sb->bitmap_length = bitmap_needed;
    sb->bitmap_stream = bitmap;

    sb->free_clusters = 0;
    for (i = 0; i < sb->cluster_count; i++)
        if ((sb->bitmap[i >> 3] & (1U << (i & 7))) == 0)
            sb->free_clusters++;

    err = LoadUpcase(sb, &upcase, upcase_checksum);
    if (err != 0)
    {
        bug("[exfat] metadata: upcase load failed (%ld)\n", (long)err);
        return err;
    }
    if (sb->volume.name[0] == 0)
    {
        static const UBYTE unnamed[] = "Untitled";
        sb->volume.name[0] = sizeof(unnamed) - 1;
        CopyMem(unnamed, sb->volume.name + 1, sizeof(unnamed));
    }
    return 0;
}

LONG ExfatNextEntry(struct FSSuper *sb, const struct exfat_stream *directory,
    ULONG *index, struct exfat_entry *entry)
{
    struct Globals *glob = sb->glob;
    UBYTE first[ENTRY_SIZE], *set;
    ULONG i, count, names, need_names;
    UWORD name_pos;
    LONG err;

    for (;;)
    {
        set = NULL;
        err = ReadEntry(sb, directory, *index, first);
        if (err != 0)
            return err;
        if (first[0] == 0)
            return ERROR_NO_MORE_ENTRIES;
        if ((first[0] & 0x80) == 0)
        {
            (*index)++;
            continue;
        }
        if (first[0] != ENTRY_FILE)
        {
            if (first[0] == ENTRY_BITMAP || first[0] == ENTRY_UPCASE
                || first[0] == ENTRY_LABEL)
            {
                (*index)++;
                continue;
            }
            if ((first[0] & 0x40) == 0 && (first[0] & 0x20) == 0)
                return ERROR_BAD_NUMBER;
            (*index)++;
            continue;
        }

        count = (ULONG)first[1] + 1;
        if (count < 3 || count > EXFAT_ENTRY_SET_MAX)
            return ERROR_BAD_NUMBER;
        if (directory->length_known
            && ((UQUAD)*index + count) * ENTRY_SIZE > directory->data_length)
            return ERROR_BAD_NUMBER;
        set = AllocVec(count * ENTRY_SIZE, MEMF_PUBLIC);
        if (set == NULL)
            return ERROR_NO_FREE_STORE;
        CopyMem(first, set, ENTRY_SIZE);
        for (i = 1; i < count; i++)
            if ((err = ReadEntry(sb, directory, *index + i,
                    set + i * ENTRY_SIZE)) != 0)
            {
                FreeVec(set);
                return err == ERROR_NO_MORE_ENTRIES
                    ? ERROR_BAD_NUMBER : err;
            }

        if (set[ENTRY_SIZE] != ENTRY_STREAM)
        {
            FreeVec(set);
            return ERROR_BAD_NUMBER;
        }
        entry->name_length = set[ENTRY_SIZE + 3];
        if (entry->name_length == 0 || entry->name_length > EXFAT_MAX_NAME)
        {
            FreeVec(set);
            return ERROR_BAD_NUMBER;
        }
        need_names = (entry->name_length + 14) / 15;
        if (count < 2 + need_names)
        {
            FreeVec(set);
            return ERROR_BAD_NUMBER;
        }
        names = need_names;
        for (i = 2; i < 2 + names; i++)
            if (set[i * ENTRY_SIZE] != ENTRY_NAME)
            {
                /* A critical secondary invalidates this set, but its trusted
                   extent lets enumeration continue at the next primary. */
                *index += count;
                goto next_set;
            }
        /* Extensions after the mandatory Stream and File Name entries are
           usable only when they are in-use benign secondaries.  Preserve
           them as opaque bytes; a critical or structurally misplaced entry
           makes this set unrecognized without losing directory alignment. */
        for (; i < count; i++)
            if ((set[i * ENTRY_SIZE] & 0xe0U) != 0xe0U)
            {
                *index += count;
                goto next_set;
            }

        *index += count;
        if (exfat_entry_set_checksum(set, (UBYTE)(count - 1))
            != exfat_rd16(set, 2))
        {
            FreeVec(set);
            continue;
        }

        name_pos = 0;
        for (i = 2; i < 2 + names && name_pos < entry->name_length; i++)
        {
            ULONG j;
            for (j = 0; j < 15 && name_pos < entry->name_length; j++)
                entry->name[name_pos++] = exfat_rd16(set + i * ENTRY_SIZE,
                    2 + j * 2);
        }
        if (exfat_name_hash(sb->upcase, entry->name, entry->name_length)
            != exfat_rd16(set + ENTRY_SIZE, 4))
        {
            FreeVec(set);
            continue;
        }

        memset(&entry->stream, 0, sizeof(entry->stream));
        EntryStream(set + ENTRY_SIZE, &entry->stream);
        err = ValidateStream(sb, &entry->stream);
        if (err != 0)
        {
            FreeVec(set);
            return err;
        }
        entry->attributes = exfat_rd16(set, 4);
        entry->modify_timestamp = exfat_rd32(set, 12);
        entry->modify_10ms = set[21];
        entry->modify_utc = (BYTE)set[23];
        entry->directory_index = *index - count;
        FreeVec(set);
        return 0;

next_set:
        FreeVec(set);
    }
}

static BOOL NameEqual(struct FSSuper *sb, const struct exfat_entry *entry,
    CONST_STRPTR name, ULONG length)
{
    ULONG i;
    if (length != entry->name_length)
        return FALSE;
    for (i = 0; i < length; i++)
        if (sb->upcase[entry->name[i]]
            != sb->upcase[(UBYTE)name[i]])
            return FALSE;
    return TRUE;
}

LONG ExfatFindEntry(struct FSSuper *sb, const struct exfat_stream *directory,
    CONST_STRPTR name, ULONG name_length, struct exfat_entry *entry)
{
    ULONG index = 0;
    LONG err;

    while ((err = ExfatNextEntry(sb, directory, &index, entry)) == 0)
        if (NameEqual(sb, entry, name, name_length))
            return 0;
    return err == ERROR_NO_MORE_ENTRIES ? ERROR_OBJECT_NOT_FOUND : err;
}

void ExfatNameToLocal(const struct exfat_entry *entry, STRPTR out,
    ULONG out_size)
{
    ULONG i, n = entry->name_length;
    if (out_size == 0)
        return;
    if (n >= out_size)
        n = out_size - 1;
    for (i = 0; i < n; i++)
        out[i] = entry->name[i] <= 255 ? (UBYTE)entry->name[i] : '_';
    out[n] = 0;
}

LONG ExfatPrepareDirectoryDelete(struct FSSuper *sb,
    const struct exfat_stream *directory, BOOL purge)
{
    UBYTE first[ENTRY_SIZE];
    ULONG index, count;
    LONG err;

    if (purge
        && (!sb->write_transaction_active || sb->write_transaction_failed))
        return ERROR_DISK_NOT_VALIDATED;

    /* First pass validates every invisible benign set and proves no File
       primary exists.  This prevents a later discovery from turning a failed
       non-empty delete into a partial mutation. */
    for (index = 0;;)
    {
        err = ReadEntry(sb, directory, index, first);
        if (err != 0)
            return err;
        if (first[0] == 0)
            break;
        if ((first[0] & 0x80U) == 0)
        {
            index++;
            continue;
        }
        if (first[0] == ENTRY_FILE)
            return ERROR_DIRECTORY_NOT_EMPTY;
        if ((first[0] & 0x40U) != 0 || (first[0] & 0x20U) == 0)
            return ERROR_BAD_NUMBER;
        err = CheckBenignPrimarySet(sb, directory, index, first,
            &count, FALSE);
        if (err != 0)
            return err;
        index += count;
    }
    if (!purge)
        return 0;

    for (index = 0;;)
    {
        err = ReadEntry(sb, directory, index, first);
        if (err != 0)
            return err;
        if (first[0] == 0)
            return 0;
        if ((first[0] & 0x80U) == 0)
        {
            index++;
            continue;
        }
        err = CheckBenignPrimarySet(sb, directory, index, first,
            &count, TRUE);
        if (err != 0)
            return err;
        index += count;
    }
}

LONG ExfatUpdateEntryStream(struct FSSuper *sb,
    const struct exfat_stream *directory, const struct exfat_entry *entry)
{
    struct Globals *glob = sb->glob;
    UBYTE *set, *stream;
    ULONG count, got;
    UQUAD offset = (UQUAD)entry->directory_index * ENTRY_SIZE;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    err = LoadFileEntrySet(sb, directory, entry->directory_index,
        &set, &count);
    if (err != 0)
        return err;
    stream = set + ENTRY_SIZE;

    stream[1] = (UBYTE)((stream[1] & (UBYTE)~2U)
        | (entry->stream.contiguous ? 2U : 0U));
    exfat_wr64(stream, 8, entry->stream.valid_data_length);
    exfat_wr32(stream, 20, entry->stream.first_cluster);
    exfat_wr64(stream, 24, entry->stream.data_length);
    exfat_wr16(set, 2,
        exfat_entry_set_checksum(set, (UBYTE)(count - 1)));

    /* Data and allocation must be durable before a longer stream becomes
       reachable.  Then publish secondaries before their primary checksum. */
    err = ExfatFlushWriteStage(sb);
    if (err != 0)
    {
        FreeVec(set);
        return err;
    }
    err = ExfatWriteStream(sb, directory, offset + ENTRY_SIZE,
        set + ENTRY_SIZE, (count - 1) * ENTRY_SIZE, &got);
    if (err == 0 && got != (count - 1) * ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err == 0)
    {
        err = ExfatWriteStream(sb, directory, offset, set, ENTRY_SIZE, &got);
        if (err == 0 && got != ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err != 0)
        sb->write_transaction_failed = TRUE;
    FreeVec(set);
    return err;
}

static LONG FindFreeEntrySet(struct FSSuper *sb,
    const struct exfat_stream *directory, ULONG needed, ULONG *start,
    BOOL *uses_end)
{
    UBYTE raw[ENTRY_SIZE];
    ULONG index = 0, run = 0, run_start = 0;
    UQUAD limit = directory->length_known
        ? directory->data_length / ENTRY_SIZE : ~(UQUAD)0;
    BOOL end_seen = FALSE;
    LONG err;

    for (;; index++)
    {
        if ((UQUAD)index >= limit)
            return ERROR_DISK_FULL;
        err = ReadEntry(sb, directory, index, raw);
        if (err != 0)
            return !directory->length_known
                    && err == ERROR_NO_MORE_ENTRIES
                ? ERROR_DISK_FULL : err;
        if (raw[0] == 0)
            end_seen = TRUE;
        if (end_seen || (raw[0] & 0x80) == 0)
        {
            if (run == 0)
                run_start = index;
            run++;
            if (run == needed)
            {
                if (end_seen)
                {
                    /* The new set consumes the old end marker; prove that a
                       replacement marker still lies in allocated directory
                       storage before changing anything. */
                    if ((UQUAD)index + 1 >= limit)
                        return ERROR_DISK_FULL;
                    err = ReadEntry(sb, directory, index + 1, raw);
                    if (err != 0)
                        return !directory->length_known
                                && err == ERROR_NO_MORE_ENTRIES
                            ? ERROR_DISK_FULL : err;
                }
                *start = run_start;
                *uses_end = end_seen;
                return 0;
            }
        }
        else
            run = 0;
    }
}

static LONG GrowDirectory(struct FSSuper *sb,
    const struct exfat_stream *parent, struct exfat_entry *directory,
    ULONG needed)
{
    UQUAD bytes = ((UQUAD)needed + 1) * ENTRY_SIZE;
    ULONG count = (ULONG)((bytes + sb->cluster_size - 1)
        / sb->cluster_size);
    ULONG first, last, current, next, i;
    LONG err;

    if (count == 0)
        count = 1;
    if (directory->stream.length_known)
    {
        UQUAD old_length = directory->stream.data_length;

        if (!exfat_directory_can_grow(old_length, sb->cluster_size, count))
            return ERROR_OBJECT_TOO_LARGE;
        err = ExfatGrowFile(sb, directory,
            old_length + (UQUAD)count * sb->cluster_size);
        if (err == 0)
        {
            directory->stream.valid_data_length = old_length
                + (UQUAD)count * sb->cluster_size;
            err = ExfatUpdateEntryStream(sb, parent, directory);
        }
        if (err == 0)
            err = ExfatFlushWriteStage(sb);
        return err;
    }

    current = directory->stream.first_cluster;
    for (i = 0; i < sb->cluster_count; i++)
    {
        err = ExfatFatNext(sb, current, &next);
        if (err == ERROR_NO_MORE_ENTRIES)
            break;
        if (err != 0)
            return err;
        current = next;
    }
    if (i == sb->cluster_count)
        return ERROR_BAD_NUMBER;
    if (!exfat_directory_can_grow(((UQUAD)i + 1) * sb->cluster_size,
            sb->cluster_size, count))
        return ERROR_OBJECT_TOO_LARGE;
    last = current;

    err = ExfatPlanContiguous(sb, last + 1, count, &first);
    if (err == 0)
        err = ExfatReserveBitmapRange(sb, first, count, TRUE);
    if (err == 0)
        err = ExfatZeroClusterRun(sb, first, count);
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    for (i = 0; i < count && err == 0; i++)
        err = ExfatSetFatEntry(sb, first + i,
            i + 1 < count ? first + i + 1 : 0xffffffffUL);
    if (err == 0)
        err = ExfatSetFatEntry(sb, last, first);
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    if (err == 0)
        err = ExfatWriteBitmapRange(sb, first, count);
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    return err;
}

static LONG EnsureDirectorySpace(struct FSSuper *sb,
    const struct exfat_stream *parent, struct exfat_entry *directory,
    ULONG needed)
{
    ULONG index;
    BOOL uses_end;
    LONG err = FindFreeEntrySet(sb, &directory->stream, needed,
        &index, &uses_end);

    if (err != ERROR_DISK_FULL)
        return err;
    err = GrowDirectory(sb, parent, directory, needed);
    if (err != 0)
        return err;
    return FindFreeEntrySet(sb, &directory->stream, needed,
        &index, &uses_end);
}

static LONG CreateEntrySet(struct FSSuper *sb,
    struct exfat_entry *directory, const struct exfat_stream *parent,
    CONST_STRPTR name,
    ULONG name_length, UWORD attributes,
    const struct exfat_stream *initial, struct exfat_entry *entry)
{
    UBYTE set[19 * ENTRY_SIZE];
    UBYTE end_marker[ENTRY_SIZE];
    ULONG names, count, index, i, j, pos = 0, got;
    ULONG timestamp;
    UBYTE ten_ms, utc;
    BOOL uses_end;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (initial == NULL || !initial->length_known
        || ValidateStream(sb, initial) != 0)
        return ERROR_BAD_NUMBER;
    if (name_length == 0 || name_length > EXFAT_MAX_NAME)
        return ERROR_INVALID_COMPONENT_NAME;
    for (i = 0; i < name_length; i++)
        if ((UBYTE)name[i] < 0x20 || name[i] == '/' || name[i] == ':')
            return ERROR_INVALID_COMPONENT_NAME;

    names = (name_length + 14) / 15;
    count = names + 2;
    err = EnsureDirectorySpace(sb, parent, directory, count);
    if (err == 0)
        err = FindFreeEntrySet(sb, &directory->stream, count,
            &index, &uses_end);
    if (err != 0)
        return err;

    memset(entry, 0, sizeof(*entry));
    entry->stream = *initial;
    entry->attributes = attributes;
    entry->name_length = (UWORD)name_length;
    for (i = 0; i < name_length; i++)
        entry->name[i] = (UBYTE)name[i];

    memset(set, 0, sizeof(set));
    set[0] = ENTRY_FILE;
    set[1] = (UBYTE)(count - 1);
    exfat_wr16(set, 4, attributes);
    ExfatCurrentTimestamp(sb->glob, &timestamp, &ten_ms, &utc);
    exfat_wr32(set, 8, timestamp);
    exfat_wr32(set, 12, timestamp);
    exfat_wr32(set, 16, timestamp);
    set[20] = ten_ms;
    set[21] = ten_ms;
    set[22] = utc;
    set[23] = utc;
    set[24] = utc;
    set[ENTRY_SIZE] = ENTRY_STREAM;
    set[ENTRY_SIZE + 1] = (UBYTE)(1U
        | (initial->contiguous ? 2U : 0U)); /* AllocationPossible */
    set[ENTRY_SIZE + 3] = (UBYTE)name_length;
    exfat_wr16(set + ENTRY_SIZE, 4,
        exfat_name_hash(sb->upcase, entry->name, (UWORD)name_length));
    exfat_wr64(set + ENTRY_SIZE, 8, initial->valid_data_length);
    exfat_wr32(set + ENTRY_SIZE, 20, initial->first_cluster);
    exfat_wr64(set + ENTRY_SIZE, 24, initial->data_length);
    for (i = 0; i < names; i++)
    {
        UBYTE *name_entry = set + (i + 2) * ENTRY_SIZE;
        name_entry[0] = ENTRY_NAME;
        for (j = 0; j < 15 && pos < name_length; j++, pos++)
            exfat_wr16(name_entry, 2 + j * 2, (UBYTE)name[pos]);
    }
    exfat_wr16(set, 2,
        exfat_entry_set_checksum(set, (UBYTE)(count - 1)));

    if (uses_end)
    {
        memset(end_marker, 0, sizeof(end_marker));
        err = ExfatWriteStream(sb, &directory->stream,
            ((UQUAD)index + count) * ENTRY_SIZE,
            end_marker, ENTRY_SIZE, &got);
        if (err == 0 && got != ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err == 0)
    {
        err = ExfatWriteStream(sb, &directory->stream,
            ((UQUAD)index + 1) * ENTRY_SIZE, set + ENTRY_SIZE,
            (count - 1) * ENTRY_SIZE, &got);
        if (err == 0 && got != (count - 1) * ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err == 0)
    {
        err = ExfatWriteStream(sb, &directory->stream,
            (UQUAD)index * ENTRY_SIZE,
            set, ENTRY_SIZE, &got);
        if (err == 0 && got != ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err != 0)
    {
        sb->write_transaction_failed = TRUE;
        return err;
    }

    entry->directory_index = index;
    entry->modify_timestamp = timestamp;
    entry->modify_10ms = ten_ms;
    entry->modify_utc = (BYTE)utc;
    return 0;
}

LONG ExfatCreateEntry(struct FSSuper *sb,
    struct exfat_entry *directory, const struct exfat_stream *parent,
    CONST_STRPTR name,
    ULONG name_length, struct exfat_entry *entry)
{
    struct exfat_stream initial;

    memset(&initial, 0, sizeof(initial));
    initial.length_known = TRUE;
    initial.contiguous = TRUE;
    return CreateEntrySet(sb, directory, parent, name, name_length,
        EXFAT_ATTR_ARCHIVE, &initial, entry);
}

LONG ExfatCreateDirectoryEntry(struct FSSuper *sb,
    struct exfat_entry *directory, const struct exfat_stream *parent,
    CONST_STRPTR name,
    ULONG name_length, struct exfat_entry *entry)
{
    struct exfat_stream initial;
    ULONG cluster, count, i;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (name_length == 0 || name_length > EXFAT_MAX_NAME)
        return ERROR_INVALID_COMPONENT_NAME;
    for (i = 0; i < name_length; i++)
        if ((UBYTE)name[i] < 0x20 || name[i] == '/' || name[i] == ':')
            return ERROR_INVALID_COMPONENT_NAME;
    count = (name_length + 14) / 15 + 2;
    err = EnsureDirectorySpace(sb, parent, directory, count);
    if (err != 0)
        return err;
    err = ExfatPlanContiguous(sb, 2, 1, &cluster);
    if (err == 0)
        err = ExfatReserveBitmapRange(sb, cluster, 1, TRUE);
    if (err == 0)
        err = ExfatZeroClusterRun(sb, cluster, 1);
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    if (err == 0)
        err = ExfatWriteBitmapRange(sb, cluster, 1);
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    if (err != 0)
        return err;

    memset(&initial, 0, sizeof(initial));
    initial.first_cluster = cluster;
    initial.data_length = sb->cluster_size;
    initial.valid_data_length = sb->cluster_size;
    initial.length_known = TRUE;
    initial.contiguous = TRUE;
    return CreateEntrySet(sb, directory, parent, name, name_length,
        EXFAT_ATTR_DIRECTORY, &initial, entry);
}

LONG ExfatRenameEntry(struct FSSuper *sb,
    const struct exfat_stream *source_directory,
    const struct exfat_entry *source_entry,
    struct exfat_entry *target_directory,
    const struct exfat_stream *target_parent,
    CONST_STRPTR name, ULONG name_length, struct exfat_entry *target_entry)
{
    struct Globals *glob = sb->glob;
    UBYTE *source = NULL, *target = NULL;
    UBYTE end_marker[ENTRY_SIZE];
    ULONG source_count, source_names, extras, names, count, index, i, j;
    ULONG pos = 0, got;
    BOOL uses_end;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (name_length == 0 || name_length > EXFAT_MAX_NAME)
        return ERROR_INVALID_COMPONENT_NAME;
    for (i = 0; i < name_length; i++)
        if ((UBYTE)name[i] < 0x20 || name[i] == '/' || name[i] == ':')
            return ERROR_INVALID_COMPONENT_NAME;

    err = LoadFileEntrySet(sb, source_directory,
        source_entry->directory_index, &source, &source_count);
    if (err != 0)
        return err;

    source_names = ((ULONG)source[ENTRY_SIZE + 3] + 14) / 15;
    if (source[ENTRY_SIZE + 3] == 0
        || source_count < 2 + source_names)
    {
        err = ERROR_BAD_NUMBER;
        goto out;
    }
    for (i = 2; i < 2 + source_names; i++)
        if (source[i * ENTRY_SIZE] != ENTRY_NAME)
        {
            err = ERROR_OBJECT_WRONG_TYPE;
            goto out;
        }
    for (; i < source_count; i++)
        if ((source[i * ENTRY_SIZE] & 0xe0U) != 0xe0U)
        {
            err = ERROR_OBJECT_WRONG_TYPE;
            goto out;
        }
    extras = source_count - 2 - source_names;

    names = (name_length + 14) / 15;
    count = names + 2 + extras;
    if (count > EXFAT_ENTRY_SET_MAX)
    {
        err = ERROR_LINE_TOO_LONG;
        goto out;
    }
    err = EnsureDirectorySpace(sb, target_parent, target_directory, count);
    if (err == 0)
        err = FindFreeEntrySet(sb, &target_directory->stream, count,
            &index, &uses_end);
    if (err != 0)
        goto out;

    target = AllocVec(count * ENTRY_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (target == NULL)
    {
        err = ERROR_NO_FREE_STORE;
        goto out;
    }
    memcpy(target, source, 2 * ENTRY_SIZE);
    target[0] = ENTRY_FILE;
    target[1] = (UBYTE)(count - 1);
    target[ENTRY_SIZE] = ENTRY_STREAM;
    target[ENTRY_SIZE + 3] = (UBYTE)name_length;
    memset(target_entry, 0, sizeof(*target_entry));
    for (i = 0; i < name_length; i++)
        target_entry->name[i] = (UBYTE)name[i];
    exfat_wr16(target + ENTRY_SIZE, 4,
        exfat_name_hash(sb->upcase, target_entry->name,
            (UWORD)name_length));
    for (i = 0; i < names; i++)
    {
        UBYTE *name_entry = target + (i + 2) * ENTRY_SIZE;
        name_entry[0] = ENTRY_NAME;
        for (j = 0; j < 15 && pos < name_length; j++, pos++)
            exfat_wr16(name_entry, 2 + j * 2, (UBYTE)name[pos]);
    }
    if (extras != 0)
        memcpy(target + (2 + names) * ENTRY_SIZE,
            source + (2 + source_names) * ENTRY_SIZE,
            extras * ENTRY_SIZE);
    exfat_wr16(target, 2,
        exfat_entry_set_checksum(target, (UBYTE)(count - 1)));

    if (uses_end)
    {
        memset(end_marker, 0, sizeof(end_marker));
        err = ExfatWriteStream(sb, &target_directory->stream,
            ((UQUAD)index + count) * ENTRY_SIZE,
            end_marker, ENTRY_SIZE, &got);
        if (err == 0 && got != ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err == 0)
    {
        err = ExfatWriteStream(sb, &target_directory->stream,
            ((UQUAD)index + 1) * ENTRY_SIZE, target + ENTRY_SIZE,
            (count - 1) * ENTRY_SIZE, &got);
        if (err == 0 && got != (count - 1) * ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err == 0)
    {
        err = ExfatWriteStream(sb, &target_directory->stream,
            (UQUAD)index * ENTRY_SIZE, target, ENTRY_SIZE, &got);
        if (err == 0 && got != ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    if (err != 0)
        goto out;

    source[0] &= 0x7f;
    err = ExfatWriteStream(sb, source_directory,
        (UQUAD)source_entry->directory_index * ENTRY_SIZE,
        source, ENTRY_SIZE, &got);
    if (err == 0 && got != ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    for (i = 1; i < source_count; i++)
        source[i * ENTRY_SIZE] &= 0x7f;
    if (err == 0)
    {
        err = ExfatWriteStream(sb, source_directory,
            (UQUAD)source_entry->directory_index * ENTRY_SIZE,
            source, source_count * ENTRY_SIZE, &got);
        if (err == 0 && got != source_count * ENTRY_SIZE)
            err = ERROR_UNKNOWN;
    }
    if (err != 0)
    {
        sb->write_transaction_failed = TRUE;
        goto out;
    }

    *target_entry = *source_entry;
    target_entry->name_length = (UWORD)name_length;
    for (i = 0; i < name_length; i++)
        target_entry->name[i] = (UBYTE)name[i];
    target_entry->directory_index = index;
    err = 0;

out:
    if (target != NULL)
        FreeVec(target);
    if (source != NULL)
        FreeVec(source);
    return err;
}

LONG ExfatUpdateEntryAttributes(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    UWORD attributes)
{
    struct Globals *glob = sb->glob;
    UBYTE *set;
    ULONG count, got;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    err = LoadFileEntrySet(sb, directory, entry->directory_index,
        &set, &count);
    if (err != 0)
        return err;

    exfat_wr16(set, 4, attributes);
    exfat_wr16(set, 2,
        exfat_entry_set_checksum(set, (UBYTE)(count - 1)));
    err = ExfatWriteStream(sb, directory,
        (UQUAD)entry->directory_index * ENTRY_SIZE,
        set, ENTRY_SIZE, &got);
    if (err == 0 && got != ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err != 0)
    {
        sb->write_transaction_failed = TRUE;
        FreeVec(set);
        return err;
    }
    entry->attributes = attributes;
    FreeVec(set);
    return 0;
}

LONG ExfatUpdateEntryDate(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    ULONG timestamp, UBYTE ten_ms, BYTE utc, ULONG fields)
{
    struct Globals *glob = sb->glob;
    UBYTE *set;
    ULONG count, got;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed
        || (fields & ~(EXFAT_DATE_MODIFIED | EXFAT_DATE_ACCESSED)) != 0
        || fields == 0)
        return ERROR_DISK_NOT_VALIDATED;
    err = LoadFileEntrySet(sb, directory, entry->directory_index,
        &set, &count);
    if (err != 0)
        return err;

    if ((fields & EXFAT_DATE_MODIFIED) != 0)
    {
        exfat_wr32(set, 12, timestamp);
        set[21] = ten_ms;
        set[23] = (UBYTE)utc;
    }
    if ((fields & EXFAT_DATE_ACCESSED) != 0)
    {
        exfat_wr32(set, 16, timestamp);
        set[24] = (UBYTE)utc;
    }
    exfat_wr16(set, 2,
        exfat_entry_set_checksum(set, (UBYTE)(count - 1)));
    err = ExfatWriteStream(sb, directory,
        (UQUAD)entry->directory_index * ENTRY_SIZE,
        set, ENTRY_SIZE, &got);
    if (err == 0 && got != ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err != 0)
    {
        sb->write_transaction_failed = TRUE;
        FreeVec(set);
        return err;
    }
    if ((fields & EXFAT_DATE_MODIFIED) != 0)
    {
        entry->modify_timestamp = timestamp;
        entry->modify_10ms = ten_ms;
        entry->modify_utc = utc;
    }
    FreeVec(set);
    return 0;
}

LONG ExfatTouchEntry(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    ULONG fields)
{
    ULONG timestamp;
    UBYTE ten_ms, utc;

    ExfatCurrentTimestamp(sb->glob, &timestamp, &ten_ms, &utc);
    return ExfatUpdateEntryDate(sb, directory, entry, timestamp, ten_ms,
        (BYTE)utc, fields);
}

LONG ExfatSetVolumeLabel(struct FSSuper *sb, CONST_STRPTR name,
    ULONG name_length)
{
    struct exfat_entry root;
    UBYTE entry[ENTRY_SIZE], end_marker[ENTRY_SIZE];
    ULONG index = 0, label_index = 0, free_index;
    ULONG i, got;
    BOOL have_label = FALSE, uses_end;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    if (name_length == 0 || name_length > 15)
        return ERROR_INVALID_COMPONENT_NAME;
    for (i = 0; i < name_length; i++)
        if ((UBYTE)name[i] < 0x20 || name[i] == '/' || name[i] == ':'
            || name[i] == '\\' || name[i] == '*' || name[i] == '?'
            || name[i] == '"' || name[i] == '<' || name[i] == '>'
            || name[i] == '|')
            return ERROR_INVALID_COMPONENT_NAME;

    for (;; index++)
    {
        err = ReadEntry(sb, &sb->root, index, entry);
        if (err == ERROR_NO_MORE_ENTRIES || (err == 0 && entry[0] == 0))
            break;
        if (err != 0)
            return err;
        if (entry[0] == ENTRY_LABEL)
        {
            label_index = index;
            have_label = TRUE;
            break;
        }
    }

    memset(&root, 0, sizeof(root));
    root.stream = sb->root;
    root.attributes = EXFAT_ATTR_DIRECTORY;
    if (!have_label)
    {
        err = EnsureDirectorySpace(sb, &sb->root, &root, 1);
        if (err == 0)
            err = FindFreeEntrySet(sb, &root.stream, 1,
                &free_index, &uses_end);
        if (err != 0)
            return err;
        label_index = free_index;
        if (uses_end)
        {
            memset(end_marker, 0, sizeof(end_marker));
            err = ExfatWriteStream(sb, &root.stream,
                ((UQUAD)label_index + 1) * ENTRY_SIZE,
                end_marker, ENTRY_SIZE, &got);
            if (err == 0 && got != ENTRY_SIZE)
                err = ERROR_UNKNOWN;
            if (err != 0)
                return err;
        }
    }

    memset(entry, 0, sizeof(entry));
    entry[0] = ENTRY_LABEL;
    entry[1] = (UBYTE)name_length;
    for (i = 0; i < name_length; i++)
        exfat_wr16(entry, 2 + i * 2, (UBYTE)name[i]);
    err = ExfatWriteStream(sb, &root.stream,
        (UQUAD)label_index * ENTRY_SIZE, entry, ENTRY_SIZE, &got);
    if (err == 0 && got != ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err != 0)
    {
        sb->write_transaction_failed = TRUE;
        return err;
    }

    sb->volume.name[0] = (UBYTE)name_length;
    for (i = 0; i < name_length; i++)
        sb->volume.name[i + 1] = (UBYTE)name[i];
    sb->volume.name[name_length + 1] = 0;
    return 0;
}

LONG ExfatDeleteEntry(struct FSSuper *sb,
    const struct exfat_stream *directory, const struct exfat_entry *entry)
{
    struct Globals *glob = sb->glob;
    UBYTE *set;
    ULONG count, names, i, got;
    UQUAD offset = (UQUAD)entry->directory_index * ENTRY_SIZE;
    LONG err;

    if (!sb->write_transaction_active || sb->write_transaction_failed)
        return ERROR_DISK_NOT_VALIDATED;
    err = LoadFileEntrySet(sb, directory, entry->directory_index,
        &set, &count);
    if (err != 0)
        return err;

    /* Make the object unreachable and durable before releasing ownership. */
    set[0] &= 0x7f;
    err = ExfatWriteStream(sb, directory, offset, set, ENTRY_SIZE, &got);
    if (err == 0 && got != ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err == 0)
        err = ExfatFlushWriteStage(sb);
    if (err == 0)
        err = ExfatFreeStream(sb, &entry->stream);
    names = ((ULONG)set[ENTRY_SIZE + 3] + 14) / 15;
    if (err == 0 && (set[ENTRY_SIZE + 3] == 0 || count < 2 + names))
        err = ERROR_BAD_NUMBER;
    /* Unknown benign secondaries derive from the generic secondary
       template.  AllocationPossible makes FirstCluster/DataLength real and
       deletion must release that allocation even though the extension's
       private payload is opaque to us. */
    for (i = 2 + names; err == 0 && i < count; i++)
    {
        UBYTE *secondary = set + i * ENTRY_SIZE;

        if ((secondary[0] & 0xe0U) != 0xe0U)
            err = ERROR_OBJECT_WRONG_TYPE;
        else if ((secondary[1] & 1U) != 0)
        {
            struct exfat_stream allocation;

            memset(&allocation, 0, sizeof(allocation));
            allocation.first_cluster = exfat_rd32(secondary, 20);
            allocation.data_length = exfat_rd64(secondary, 24);
            allocation.valid_data_length = allocation.data_length;
            allocation.contiguous = (secondary[1] & 2U) != 0;
            allocation.length_known = TRUE;
            err = ValidateStream(sb, &allocation);
            if (err == 0)
                err = ExfatFreeStream(sb, &allocation);
        }
    }
    if (err != 0)
    {
        FreeVec(set);
        return err;
    }

    for (i = 1; i < count; i++)
        set[i * ENTRY_SIZE] &= 0x7f;
    err = ExfatWriteStream(sb, directory, offset, set,
        count * ENTRY_SIZE, &got);
    if (err == 0 && got != count * ENTRY_SIZE)
        err = ERROR_UNKNOWN;
    if (err != 0)
        sb->write_transaction_failed = TRUE;
    FreeVec(set);
    return err;
}
