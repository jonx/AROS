/* exfat-handler - single-FAT exFAT formatter */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/filehandler.h>

#include <proto/exec.h>

#include <string.h>

#include "exfat_fs.h"
#include "exfat_meta.h"
#include "exfat_protos.h"

#define FORMAT_FAT_OFFSET 24
#define ENTRY_BITMAP 0x81
#define ENTRY_UPCASE 0x82
#define ENTRY_LABEL  0x83

static LONG WriteBlocks(struct Globals *glob, UQUAD first, ULONG count,
    ULONG block_size, UBYTE *data)
{
    return AccessDisk(TRUE, first, count, block_size, data, glob) == 0
        ? 0 : ERROR_UNKNOWN;
}

static UBYTE ShiftFor(ULONG value)
{
    UBYTE shift = 0;

    while ((1UL << shift) != value)
        shift++;
    return shift;
}

static ULONG AlignUp(ULONG value, ULONG alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

LONG ExfatFormatVolume(struct Globals *glob, CONST_STRPTR name,
    ULONG name_length)
{
    struct DosEnvec *de = BADDR(glob->fssm->fssm_Environ);
    UQUAD part_start, part_blocks;
    ULONG block_size, cluster_size, sectors_per_cluster;
    ULONG fat_length = 1, heap_offset = 0, cluster_count = 0;
    ULONG previous_heap, bitmap_length, bitmap_clusters;
    ULONG upcase_clusters, root_cluster, allocated_clusters;
    ULONG bitmap_cluster = 2, upcase_cluster;
    ULONG sector, entries_per_sector, value, i, j;
    UQUAD first_entry, entry;
    ULONG upcase_checksum = 0, boot_checksum = 0, serial;
    UBYTE sector_shift, cluster_shift;
    UBYTE upcase[60], *boot_region = NULL, *buffer = NULL;
    UBYTE *boot, *checksum_sector, *root;
    LONG err = 0;

    if (name_length == 0 || name_length > 15)
        return ERROR_INVALID_COMPONENT_NAME;
    for (i = 0; i < name_length; i++)
        if ((UBYTE)name[i] < 0x20 || name[i] == '/' || name[i] == ':'
            || name[i] == '\\' || name[i] == '*' || name[i] == '?'
            || name[i] == '"' || name[i] == '<' || name[i] == '>'
            || name[i] == '|')
            return ERROR_INVALID_COMPONENT_NAME;
    if (exfat_mountlist_extent(de->de_LowCyl, de->de_HighCyl,
            de->de_Surfaces, de->de_BlocksPerTrack, de->de_SizeBlock,
            &part_start, &part_blocks, &block_size) != EXFAT_RANGE_OK
        || block_size < 512 || block_size > 4096
        || (block_size & (block_size - 1)) != 0
        || part_blocks < (1024UL * 1024 / block_size))
        return ERROR_BAD_NUMBER;

    sector_shift = ShiftFor(block_size);
    if (part_blocks <= (256ULL * 1024 * 1024) / block_size)
        cluster_size = 4096;
    else if (part_blocks <= (32ULL * 1024 * 1024 * 1024) / block_size)
        cluster_size = 32768;
    else
        cluster_size = 131072;
    if (cluster_size < block_size)
        cluster_size = block_size;
    sectors_per_cluster = cluster_size / block_size;
    cluster_shift = ShiftFor(sectors_per_cluster);

    previous_heap = 0;
    for (i = 0; i < 16; i++)
    {
        heap_offset = AlignUp(FORMAT_FAT_OFFSET + fat_length,
            sectors_per_cluster);
        if ((UQUAD)heap_offset >= part_blocks)
            return ERROR_BAD_NUMBER;
        cluster_count = (ULONG)((part_blocks - heap_offset)
            / sectors_per_cluster);
        if (cluster_count == 0 || cluster_count > 0xfffffff5UL)
            return ERROR_BAD_NUMBER;
        fat_length = (ULONG)(((UQUAD)cluster_count + 2) * 4
            + block_size - 1) / block_size;
        if (heap_offset == previous_heap)
            break;
        previous_heap = heap_offset;
    }
    if (i == 16 || (UQUAD)heap_offset
            + (UQUAD)cluster_count * sectors_per_cluster > part_blocks)
        return ERROR_BAD_NUMBER;

    bitmap_length = (ULONG)(((UQUAD)cluster_count + 7) >> 3);
    bitmap_clusters = (bitmap_length + cluster_size - 1) / cluster_size;
    memset(upcase, 0, sizeof(upcase));
    exfat_wr16(upcase, 0, 0xffff);
    exfat_wr16(upcase, 2, 97);
    for (i = 0; i < 26; i++)
        exfat_wr16(upcase, 4 + i * 2, (UWORD)('A' + i));
    exfat_wr16(upcase, 56, 0xffff);
    exfat_wr16(upcase, 58, 65413);
    for (i = 0; i < sizeof(upcase); i++)
        upcase_checksum = exfat_meta_rotate32(upcase_checksum, upcase[i]);
    upcase_clusters = (sizeof(upcase) + cluster_size - 1) / cluster_size;
    upcase_cluster = bitmap_cluster + bitmap_clusters;
    root_cluster = upcase_cluster + upcase_clusters;
    allocated_clusters = bitmap_clusters + upcase_clusters + 1;
    if ((UQUAD)allocated_clusters > cluster_count)
        return ERROR_DISK_FULL;

    boot_region = AllocVec((ULONG)(12 * block_size), MEMF_PUBLIC | MEMF_CLEAR);
    buffer = AllocVec(cluster_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (boot_region == NULL || buffer == NULL)
    {
        err = ERROR_NO_FREE_STORE;
        goto out;
    }

    entries_per_sector = block_size / 4;
    for (sector = 0; sector < fat_length && err == 0; sector++)
    {
        memset(buffer, 0, block_size);
        first_entry = sector * entries_per_sector;
        for (j = 0; j < entries_per_sector; j++)
        {
            entry = first_entry + j;
            value = 0;
            if (entry == 0)
                value = 0xfffffff8UL;
            else if (entry == 1 || entry == root_cluster)
                value = 0xffffffffUL;
            else if (entry >= bitmap_cluster
                && entry < bitmap_cluster + bitmap_clusters)
                value = entry + 1 < bitmap_cluster + bitmap_clusters
                    ? (ULONG)entry + 1 : 0xffffffffUL;
            else if (entry >= upcase_cluster
                && entry < upcase_cluster + upcase_clusters)
                value = entry + 1 < upcase_cluster + upcase_clusters
                    ? (ULONG)entry + 1 : 0xffffffffUL;
            if (value != 0)
                exfat_wr32(buffer, j * 4, value);
        }
        err = WriteBlocks(glob, part_start + FORMAT_FAT_OFFSET + sector,
            1, block_size, buffer);
    }

    memset(buffer, 0, cluster_size);
    for (i = 0; i < allocated_clusters; i++)
        buffer[i >> 3] |= (UBYTE)(1U << (i & 7));
    for (i = 0; i < bitmap_clusters && err == 0; i++)
    {
        err = WriteBlocks(glob, part_start + heap_offset
            + (UQUAD)(bitmap_cluster - 2 + i) * sectors_per_cluster,
            sectors_per_cluster, block_size, buffer);
        memset(buffer, 0, cluster_size);
    }

    if (err == 0)
    {
        memset(buffer, 0, cluster_size);
        memcpy(buffer, upcase, sizeof(upcase));
    }
    for (i = 0; i < upcase_clusters && err == 0; i++)
    {
        err = WriteBlocks(glob, part_start + heap_offset
            + (UQUAD)(upcase_cluster - 2 + i) * sectors_per_cluster,
            sectors_per_cluster, block_size, buffer);
        memset(buffer, 0, cluster_size);
    }

    if (err == 0)
    {
        root = buffer;
        memset(root, 0, cluster_size);
        root[0] = ENTRY_BITMAP;
        exfat_wr32(root, 20, bitmap_cluster);
        exfat_wr64(root, 24, bitmap_length);
        root[32] = ENTRY_UPCASE;
        exfat_wr32(root + 32, 4, upcase_checksum);
        exfat_wr32(root + 32, 20, upcase_cluster);
        exfat_wr64(root + 32, 24, sizeof(upcase));
        root[64] = ENTRY_LABEL;
        root[65] = (UBYTE)name_length;
        for (i = 0; i < name_length; i++)
            exfat_wr16(root + 64, 2 + i * 2, (UBYTE)name[i]);
        err = WriteBlocks(glob, part_start + heap_offset
            + (UQUAD)(root_cluster - 2) * sectors_per_cluster,
            sectors_per_cluster, block_size, root);
    }

    boot = boot_region;
    boot[0] = 0xeb; boot[1] = 0x76; boot[2] = 0x90;
    memcpy(boot + 3, "EXFAT   ", 8);
    exfat_wr64(boot, 64, part_start);
    exfat_wr64(boot, 72, part_blocks);
    exfat_wr32(boot, 80, FORMAT_FAT_OFFSET);
    exfat_wr32(boot, 84, fat_length);
    exfat_wr32(boot, 88, heap_offset);
    exfat_wr32(boot, 92, cluster_count);
    exfat_wr32(boot, 96, root_cluster);
    serial = (ULONG)part_start ^ (ULONG)(part_start >> 32)
        ^ (ULONG)part_blocks ^ (ULONG)(part_blocks >> 32)
        ^ cluster_count ^ 0x58464154UL;
    exfat_wr32(boot, 100, serial);
    exfat_wr16(boot, 104, 0x0100);
    exfat_wr16(boot, 106, 0);
    boot[108] = sector_shift;
    boot[109] = cluster_shift;
    boot[110] = 1;
    boot[111] = 0x80;
    boot[112] = (UBYTE)(((UQUAD)allocated_clusters * 100) / cluster_count);
    boot[510] = 0x55; boot[511] = 0xaa;
    for (i = 1; i <= 10; i++)
        exfat_wr32(boot_region + i * block_size, block_size - 4,
            0xaa550000UL);
    for (i = 0; i <= 10; i++)
        boot_checksum = exfat_boot_checksum(boot_checksum,
            boot_region + i * block_size, block_size, i);
    checksum_sector = boot_region + 11 * block_size;
    for (j = 0; j < block_size; j += 4)
        exfat_wr32(checksum_sector, j, boot_checksum);

    if (err == 0)
        err = WriteBlocks(glob, part_start, 12, block_size, boot_region);
    if (err == 0)
        err = WriteBlocks(glob, part_start + 12, 12,
            block_size, boot_region);
    if (err == 0 && !SyncDisk(glob))
        err = ERROR_DISK_NOT_VALIDATED;

out:
    if (buffer != NULL)
        FreeVec(buffer);
    if (boot_region != NULL)
        FreeVec(boot_region);
    return err;
}
