/*
 * exfat-handler - mounting a volume
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#include <exec/types.h>
#include <exec/errors.h>
#include <dos/dos.h>
#include <dos/filehandler.h>
#include <devices/trackdisk.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "exfat_fs.h"
#include "exfat_protos.h"

#define DEBUG DEBUG_VOLUME
#include "exfat_debug.h"

/*
 * Map a validation result onto the DOS error the spec's mount-error table
 * requires. Kept in one place so the table and the code cannot drift.
 */
static LONG BootResultToError(enum exfat_boot_result r)
{
    switch (r)
    {
    case EXFAT_BOOT_OK:            return 0;
    case EXFAT_BOOT_NOT_EXFAT:     return ERROR_NOT_A_DOS_DISK;
    case EXFAT_BOOT_WRONG_VERSION: return ERROR_OBJECT_WRONG_TYPE;
    case EXFAT_BOOT_TEXFAT:        return ERROR_OBJECT_WRONG_TYPE;
    case EXFAT_BOOT_BAD_CHECKSUM:  return ERROR_DISK_NOT_VALIDATED;
    case EXFAT_BOOT_BAD_GEOMETRY:  return ERROR_BAD_NUMBER;
    }
    return ERROR_BAD_NUMBER;
}

/*
 * Raw device read, bypassing the cache.
 *
 * The cache is sized from the volume's sector size, which is precisely what
 * the boot sector has not told us yet, so the boot region is read directly.
 * Transfers are in whole device blocks (spec U1): a partial-block request is
 * not a legal transfer on a device whose blocks are larger.
 */
static LONG ReadDeviceBlocks(struct Globals *glob, UQUAD block, ULONG count,
    ULONG block_size, UBYTE *buf)
{
    UQUAD off;
    ULONG len;

    if (exfat_byte_range(block, count, block_size, &off, &len)
        != EXFAT_RANGE_OK)
        return IOERR_BADADDRESS;

    glob->diskioreq->iotd_Req.io_Offset = off & 0xFFFFFFFF;
    glob->diskioreq->iotd_Req.io_Actual = off >> 32;
    glob->diskioreq->iotd_Req.io_Length = len;
    glob->diskioreq->iotd_Req.io_Data = buf;
    glob->diskioreq->iotd_Req.io_Command = glob->readcmd;

    return DoIO((struct IORequest *)glob->diskioreq);
}

/*
 * Read and validate the boot region, then build the superblock.
 *
 * The order here is the spec's, and it is not free to change: each step
 * establishes what the next one needs. U1 obtains bytes before the logical
 * sector size is known, U2 learns it, U3 pins it against the device, U6 to U8
 * bound the volume by its partition, and only then is it meaningful to read
 * sectors 1 to 11 as logical sectors.
 */
static LONG MountVolume(struct Globals *glob, UQUAD part_start,
    UQUAD part_blocks, ULONG device_block_size, struct FSSuper *sb)
{
    struct exfat_geometry geo;
    enum exfat_boot_result r;
    UBYTE *buf = NULL;
    ULONG checksum = 0, i;
    LONG err = 0;

    /*
     * U1: obtain the first 512 bytes using device-aligned transfers. On a
     * 4096-byte device this reads one whole block and inspects its leading
     * 512 bytes; 512 is what we must *inspect*, not what we may *request*.
     */
    buf = AllocVec(device_block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL)
        return ERROR_NO_FREE_STORE;

    err = ReadDeviceBlocks(glob, part_start, 1, device_block_size, buf);
    if (err != 0)
    {
        FreeVec(buf);
        return ERROR_NOT_A_DOS_DISK;
    }

    /* U2: validate the boot sector, which establishes the logical size. */
    r = exfat_validate_boot(buf, device_block_size < 512
        ? device_block_size : 512, &geo);
    if (r != EXFAT_BOOT_OK)
    {
        FreeVec(buf);
        return BootResultToError(r);
    }

    /* U3: Phase 1 requires the two units to be the same. */
    r = exfat_check_sector_units(&geo, device_block_size);
    if (r != EXFAT_BOOT_OK)
    {
        D(bug("[exfat] logical sector %lu != device block %lu, refusing\n",
            (unsigned long)geo.sector_size, (unsigned long)device_block_size));
        FreeVec(buf);
        return BootResultToError(r);
    }

    /* U6, U7: the volume must fit inside the partition it was mounted on. */
    r = exfat_check_partition_fit(&geo, part_start, part_blocks);
    if (r != EXFAT_BOOT_OK)
    {
        D(bug("[exfat] volume claims more space than its partition,"
            " refusing\n"));
        FreeVec(buf);
        return BootResultToError(r);
    }

    /*
     * B1 to B3. Sectors 0 to 10 feed the checksum, sector 11 holds it
     * repeated. Units are now known equal, so these are logical sectors.
     */
    checksum = exfat_boot_checksum(0, buf, geo.sector_size, 0);
    for (i = 1; i < EXFAT_BOOT_CHECKSUM_SECTORS; i++)
    {
        err = ReadDeviceBlocks(glob, part_start + i, 1, geo.sector_size, buf);
        if (err != 0)
        {
            FreeVec(buf);
            return ERROR_NOT_A_DOS_DISK;
        }
        checksum = exfat_boot_checksum(checksum, buf, geo.sector_size, i);
    }

    err = ReadDeviceBlocks(glob, part_start + EXFAT_BOOT_CHECKSUM_SECTOR, 1,
        geo.sector_size, buf);
    if (err != 0)
    {
        FreeVec(buf);
        return ERROR_NOT_A_DOS_DISK;
    }

    if (!exfat_verify_boot_checksum(buf, geo.sector_size, checksum))
    {
        D(bug("[exfat] boot region checksum mismatch, refusing\n"));
        FreeVec(buf);
        return ERROR_DISK_NOT_VALIDATED;
    }

    FreeVec(buf);

    /* Everything below is derived only from validated values. */
    memset(sb, 0, sizeof(*sb));
    sb->glob = glob;
    sb->geo = geo;

    sb->sector_shift  = geo.sector_shift;
    sb->cluster_shift = geo.cluster_shift;
    sb->sector_size   = geo.sector_size;
    sb->cluster_size  = (ULONG)geo.sector_size << geo.cluster_shift;

    sb->first_device_sector = part_start;
    /*
     * U8: the access boundary is the validated VolumeLength, never the raw
     * field and never the partition size. A volume smaller than its
     * partition is normal, and the trailing blocks must not become readable
     * through the filesystem.
     */
    sb->total_sectors = geo.volume_length;

    sb->fat_start  = part_start + geo.fat_offset;
    sb->heap_start = part_start + geo.heap_offset;

    sb->cluster_count = geo.cluster_count;
    sb->root_cluster  = geo.root_cluster;

    /*
     * F4: the root directory is a FAT-chained stream with no directory entry
     * describing it, so its length is not known until the chain is walked.
     */
    sb->root.first_cluster     = geo.root_cluster;
    sb->root.data_length       = 0;
    sb->root.valid_data_length = 0;
    sb->root.contiguous        = FALSE;
    sb->root.length_known      = FALSE;

    sb->cache = Cache_CreateCache(glob, 64, 64, sb->sector_size,
        glob->gl_SysBase, glob->gl_DOSBase);
    if (sb->cache == NULL)
        return ERROR_NO_FREE_STORE;

    D(bug("[exfat] mounted: %lu sectors of %lu, %lu clusters, root at %lu\n",
        (unsigned long)sb->total_sectors, (unsigned long)sb->sector_size,
        (unsigned long)sb->cluster_count, (unsigned long)sb->root_cluster));

    return 0;
}

void DoDiskInsert(struct Globals *glob)
{
    struct FileSysStartupMsg *fssm = glob->fssm;
    struct DosEnvec *de;
    struct FSSuper *sb;
    UQUAD part_start, part_blocks;
    ULONG block_size, surfaces, spt, cylinder;
    LONG err;

    if (glob->sb != NULL || fssm == NULL)
        return;

    de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
    block_size = de->de_SizeBlock << 2;
    surfaces   = de->de_Surfaces;
    spt        = de->de_BlocksPerTrack;
    cylinder   = surfaces * spt;

    /*
     * The partition extent from the Mountlist, in device blocks. Computed in
     * UQUAD because a large disk overflows the 32-bit product, and checked
     * before use rather than after (spec U6).
     */
    part_start  = (UQUAD)de->de_LowCyl * cylinder;
    part_blocks = ((UQUAD)de->de_HighCyl + 1 - de->de_LowCyl) * cylinder;

    if (de->de_HighCyl < de->de_LowCyl || cylinder == 0 || block_size == 0
        || !exfat_geometry_ok(part_start, part_blocks))
    {
        D(bug("[exfat] Mountlist geometry is unusable\n"));
        return;
    }

    sb = AllocVec(sizeof(struct FSSuper), MEMF_PUBLIC | MEMF_CLEAR);
    if (sb == NULL)
        return;

    err = MountVolume(glob, part_start, part_blocks, block_size, sb);
    if (err != 0)
    {
        D(bug("[exfat] mount refused, error %ld\n", (long)err));
        FreeVec(sb);
        return;
    }

    glob->sb = sb;
}

void DoDiskRemove(struct Globals *glob)
{
    struct FSSuper *sb = glob->sb;

    if (sb == NULL)
        return;

    glob->sb = NULL;

    if (sb->cache != NULL)
        Cache_DestroyCache(sb->cache);

    FreeVec(sb);
}
