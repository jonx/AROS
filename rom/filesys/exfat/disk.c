/*
 * fat-handler - FAT12/16/32 filesystem handler
 *
 * Copyright (C) 2006 Marek Szyprowski
 * Copyright (C) 2007-2015 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 *
 * $Id$
 */

#define USE_INLINE_STDARG

#include <exec/types.h>
#include <exec/errors.h>

#include <devices/newstyle.h>
#include <devices/trackdisk.h>

#include <proto/exec.h>

#include <string.h>
#include <stdio.h>

#include "exfat_fs.h"
#include "exfat_protos.h"

#define DEBUG DEBUG_MISC
#include "exfat_debug.h"

/* TD64 commands */
#ifndef TD_READ64
#define TD_READ64  24
#define TD_WRITE64 25
#endif

void ProcessDiskChange(struct Globals *glob)
{
    D(bug("\nGot disk change request\n"));

    if (glob->disk_inhibited != 0)
    {
        D(bug("Disk is inhibited, ignoring disk change\n"));
        return;
    }

    glob->diskioreq->iotd_Req.io_Command = TD_CHANGESTATE;
    glob->diskioreq->iotd_Req.io_Data = NULL;
    glob->diskioreq->iotd_Req.io_Length = 0;
    glob->diskioreq->iotd_Req.io_Flags = IOF_QUICK;
    DoIO((struct IORequest *)glob->diskioreq);

    if (glob->diskioreq->iotd_Req.io_Error == 0
        && glob->diskioreq->iotd_Req.io_Actual == 0)
    {
        /* Disk has been inserted. */
        D(bug("\tDisk has been inserted\n"));
        glob->disk_inserted = TRUE;
        DoDiskInsert(glob);
    }
    else
    {
        /* Disk has been removed. */
        D(bug("\tDisk has been removed\n"));
        glob->disk_inserted = FALSE;
        DoDiskRemove(glob);
    }

    D(bug("Done\n"));
}

void UpdateDisk(struct Globals *glob)
{
    if (glob->sb)
        Cache_Flush(glob->sb->cache);

    glob->diskioreq->iotd_Req.io_Command = CMD_UPDATE;
    DoIO((struct IORequest *)glob->diskioreq);

    /* Turn off motor (where applicable) if nothing has happened during the
     * last timer period */
    if (!glob->restart_timer)
    {
        D(bug("Stopping drive motor\n"));
        glob->diskioreq->iotd_Req.io_Command = TD_MOTOR;
        glob->diskioreq->iotd_Req.io_Length = 0;
        DoIO((struct IORequest *)glob->diskioreq);
    }
}

/* Probe the device to determine 64-bit support */
void Probe64BitSupport(struct Globals *glob)
{
    struct NSDeviceQueryResult nsd_query;
    UWORD *nsd_cmd;

    glob->readcmd = CMD_READ;
    glob->writecmd = CMD_WRITE;

    /* Probe TD64 */
    glob->diskioreq->iotd_Req.io_Command = TD_READ64;
    glob->diskioreq->iotd_Req.io_Offset = 0;
    glob->diskioreq->iotd_Req.io_Length = 0;
    glob->diskioreq->iotd_Req.io_Actual = 0;
    glob->diskioreq->iotd_Req.io_Data = 0;

    if (DoIO((struct IORequest *)glob->diskioreq) != IOERR_NOCMD)
    {
        D(bug("Probe_64bit_support:"
            " device supports 64-bit trackdisk extensions\n"));
        glob->readcmd = TD_READ64;
        glob->writecmd = TD_WRITE64;
    }

    /* Probe NSD */
    glob->diskioreq->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
    glob->diskioreq->iotd_Req.io_Length =
        sizeof(struct NSDeviceQueryResult);
    glob->diskioreq->iotd_Req.io_Data = (APTR) &nsd_query;

    if (DoIO((struct IORequest *)glob->diskioreq) == 0)
        for (nsd_cmd = nsd_query.SupportedCommands; *nsd_cmd != 0;
            nsd_cmd++)
        {
            if (*nsd_cmd == NSCMD_TD_READ64)
            {
                D(bug("Probe_64bit_support:"
                    " device supports NSD 64-bit trackdisk extensions\n"));
                glob->readcmd = NSCMD_TD_READ64;
                glob->writecmd = NSCMD_TD_WRITE64;
                break;
            }
        }
}

/* N.B. returns an Exec error code, not a DOS error code! */
/*
 * Sector numbers are UQUAD. A %lu vararg slot is 32-bit on m68k and i386, so
 * they are formatted here and passed as strings.
 */
STRPTR FmtSector(UQUAD sector, STRPTR buf)
{
    STRPTR p = buf + SECTORSTR_LEN - 1;

    *p = '\0';
    do
    {
        *--p = '0' + (UBYTE)(sector % 10);
        sector /= 10;
    }
    while (sector != 0);

    return p;
}

LONG AccessDisk(BOOL do_write, UQUAD num, ULONG nblocks, ULONG block_size,
    UBYTE *data, APTR priv)
{
    struct Globals *glob = priv;
    UQUAD off;
    ULONG err;
    UQUAD start, total;
    ULONG io_len;
    BOOL retry = TRUE;
    TEXT vol_name[100];
    TEXT s1[SECTORSTR_LEN], s2[SECTORSTR_LEN], s3[SECTORSTR_LEN];

#if DEBUG_CACHESTATS > 1
    ErrorMessage("Accessing %lu sector(s) starting at %s.\n"
        "First volume sector is %s, sector size is %lu.\n", "OK", nblocks,
         (IPTR)FmtSector(num, s1),
         (IPTR)FmtSector(glob->sb->first_device_sector, s2), block_size);
#endif

    /* Clip the request to the volume. See exfat_bounds.h for the rules. */
    if (glob->sb)
    {
        UQUAD skipped;
        enum exfat_range r;

        start = glob->sb->first_device_sector;
        total = glob->sb->total_sectors;

        r = exfat_clip(&num, &nblocks, &skipped, start, total);

        if (r == EXFAT_RANGE_OK)
            data += skipped * block_size;
        else
        {
            if (num != glob->last_num)
            {
                glob->last_num = num;

                if (r == EXFAT_RANGE_BADGEOMETRY)
                    ErrorMessage("The volume geometry is not usable:\n"
                        "first sector %s, %s sector(s).\n"
                        "The volume was mounted with a size that cannot be\n"
                        "addressed. Please check your disk and/or report\n"
                        "this problem to the developers team.", "OK",
                        (IPTR)FmtSector(start, s1),
                        (IPTR)FmtSector(total, s2));
                else
                    ErrorMessage("A handler attempted to %s %lu sector(s)\n"
                        "starting from %s, outside the actual volume space.\n"
                        "The volume runs from %s for %s sector(s).\n"
                        "Either your disk is damaged or it is a bug in\n"
                        "the handler. Please check your disk and/or\n"
                        "report this problem to the developers team.", "OK",
                        (IPTR) (do_write ? "write" : "read"), nblocks,
                        (IPTR)FmtSector(num, s1),
                        (IPTR)FmtSector(start, s2),
                        (IPTR)FmtSector(total, s3));
            }
            return IOERR_BADADDRESS;
        }
    }

    if (exfat_byte_range(num, nblocks, block_size, &off, &io_len)
        != EXFAT_RANGE_OK)
    {
        D(bug("[exfat] byte range for sector %s x %lu overflows\n",
            FmtSector(num, s1), nblocks));
        return IOERR_BADADDRESS;
    }

    while (retry)
    {
        glob->diskioreq->iotd_Req.io_Offset = off & 0xFFFFFFFF;
        glob->diskioreq->iotd_Req.io_Actual = off >> 32;

        glob->diskioreq->iotd_Req.io_Length = io_len;
        glob->diskioreq->iotd_Req.io_Data = data;
        glob->diskioreq->iotd_Req.io_Command =
            do_write ? glob->writecmd : glob->readcmd;

        err = DoIO((struct IORequest *)glob->diskioreq);

        if (err != 0)
        {
            if (glob->sb && glob->sb->volume.name[0] != '\0')
                snprintf(vol_name, 100, "Volume %s",
                    glob->sb->volume.name + 1);
            else
                snprintf(vol_name, 100, "Device %s",
                    AROS_BSTR_ADDR(glob->devnode->dol_Name));

            if (nblocks > 1)
                retry = ErrorMessage("%s\nhas a %s error\n"
                    "in the block range\n%s to %s",
                    "Retry|Cancel", (IPTR)vol_name,
                    (IPTR)(do_write ? "write" : "read"),
                    (IPTR)FmtSector(num, s1),
                    (IPTR)FmtSector(exfat_last_sector(num, nblocks), s2));
            else
                retry = ErrorMessage("%s\nhas a %s error\n"
                    "on block %s",
                    "Retry|Cancel", (IPTR)vol_name,
                    (IPTR)(do_write ? "write" : "read"),
                    (IPTR)FmtSector(num, s1));
        }
        else
            retry = FALSE;
    }

    return err;
}
