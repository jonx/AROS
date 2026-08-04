/*
 * exfat-handler - root metadata and directory entry sets
 *
 * Copyright (C) 2026 The AROS Development Team
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <aros/debug.h>

#include <proto/exec.h>

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

static LONG ValidateStream(struct FSSuper *sb, struct exfat_stream *stream)
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

static UWORD SetChecksum(const UBYTE *set, ULONG count)
{
    ULONG i, bytes = count * ENTRY_SIZE;
    UWORD sum = 0;

    for (i = 0; i < bytes; i++)
        if (i != 2 && i != 3)
            sum = exfat_meta_rotate16(sum, set[i]);
    return sum;
}

static UWORD NameHash(struct FSSuper *sb, const UWORD *name, UWORD length)
{
    UWORD hash = 0, i, c;
    for (i = 0; i < length; i++)
    {
        c = sb->upcase[name[i]];
        hash = exfat_meta_rotate16(hash, (UBYTE)c);
        hash = exfat_meta_rotate16(hash, (UBYTE)(c >> 8));
    }
    return hash;
}

LONG ExfatNextEntry(struct FSSuper *sb, const struct exfat_stream *directory,
    ULONG *index, struct exfat_entry *entry)
{
    UBYTE set[19 * ENTRY_SIZE];
    ULONG i, count, names, need_names;
    UWORD name_pos;
    LONG err;

    for (;;)
    {
        err = ReadEntry(sb, directory, *index, set);
        if (err != 0)
            return err;
        if (set[0] == 0)
            return ERROR_NO_MORE_ENTRIES;
        if ((set[0] & 0x80) == 0)
        {
            (*index)++;
            continue;
        }
        if (set[0] != ENTRY_FILE)
        {
            if (set[0] == ENTRY_BITMAP || set[0] == ENTRY_UPCASE
                || set[0] == ENTRY_LABEL)
            {
                (*index)++;
                continue;
            }
            if ((set[0] & 0x40) == 0 && (set[0] & 0x20) == 0)
                return ERROR_BAD_NUMBER;
            (*index)++;
            continue;
        }

        count = (ULONG)set[1] + 1;
        if (count < 3 || count > 19)
            return ERROR_BAD_NUMBER;
        if (directory->length_known
            && ((UQUAD)*index + count) * ENTRY_SIZE > directory->data_length)
            return ERROR_BAD_NUMBER;
        for (i = 1; i < count; i++)
            if ((err = ReadEntry(sb, directory, *index + i,
                    set + i * ENTRY_SIZE)) != 0)
                return err;

        if (set[ENTRY_SIZE] != ENTRY_STREAM)
            return ERROR_BAD_NUMBER;
        entry->name_length = set[ENTRY_SIZE + 3];
        if (entry->name_length == 0 || entry->name_length > EXFAT_MAX_NAME)
            return ERROR_BAD_NUMBER;
        need_names = (entry->name_length + 14) / 15;
        names = count - 2;
        if (names != need_names)
            return ERROR_BAD_NUMBER;
        for (i = 2; i < count; i++)
            if (set[i * ENTRY_SIZE] != ENTRY_NAME)
            {
                /* A critical secondary invalidates this set, but its trusted
                   extent lets enumeration continue at the next primary. */
                *index += count;
                goto next_set;
            }

        *index += count;
        if (SetChecksum(set, count) != exfat_rd16(set, 2))
            continue;

        name_pos = 0;
        for (i = 2; i < count && name_pos < entry->name_length; i++)
        {
            ULONG j;
            for (j = 0; j < 15 && name_pos < entry->name_length; j++)
                entry->name[name_pos++] = exfat_rd16(set + i * ENTRY_SIZE,
                    2 + j * 2);
        }
        if (NameHash(sb, entry->name, entry->name_length)
            != exfat_rd16(set + ENTRY_SIZE, 4))
            continue;

        memset(&entry->stream, 0, sizeof(entry->stream));
        EntryStream(set + ENTRY_SIZE, &entry->stream);
        if ((err = ValidateStream(sb, &entry->stream)) != 0)
            return err;
        entry->attributes = exfat_rd16(set, 4);
        entry->modify_timestamp = exfat_rd32(set, 12);
        entry->modify_10ms = set[21];
        entry->modify_utc = (BYTE)set[23];
        entry->directory_index = *index - count;
        return 0;

next_set:
        ;
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
