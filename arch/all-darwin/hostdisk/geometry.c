/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

/* This routine differs in different UNIX variants (using different IOCTLs) */

#include <aros/debug.h>
#include <devices/trackdisk.h>
#include <exec/memory.h>
#include <proto/hostlib.h>

#include "hostdisk_host.h"
#include "hostdisk_device.h"

/*
 * <sys/disk.h> reaches the host socket headers, which need BSD type names
 * this compile environment does not define. Encode the two requests locally.
 */
#define DK_IOR(group, number, size)             \
    (0x40000000UL                       |       \
     (((ULONG)(size) & 0x1fff) << 16)   |       \
     ((ULONG)(group) << 8)              |       \
     (ULONG)(number))

#define DK_GETBLOCKSIZE     DK_IOR('d', 24, 4)  /* uint32_t block size  */
#define DK_GETBLOCKCOUNT    DK_IOR('d', 25, 8)  /* uint64_t block count */

ULONG Host_DeviceGeometry(int file, struct DriveGeometry *dg, struct HostDiskBase *hdskBase)
{
    UQUAD sectors = 0;
    int ret, err;

    HostLib_Lock();
 
    ret = hdskBase->iface->ioctl(file, DK_GETBLOCKSIZE, &dg->dg_SectorSize);

    if (ret != -1)
        ret = hdskBase->iface->ioctl(file, DK_GETBLOCKCOUNT, &sectors);

    err = *hdskBase->errnoPtr;

    HostLib_Unlock();

    if (ret == -1)
    {
        D(bug("hostdisk: Error %d\n", err));

        return err;
    }

    if (sectors > 0xFFFFFFFFULL)
        sectors = 0xFFFFFFFFULL;

    dg->dg_TotalSectors = sectors;

    D(bug("hostdisk: %u sectors per %u bytes\n", dg->dg_TotalSectors, dg->dg_SectorSize));

    /*
     * This is all we can do on Darwin. They dropped CHS completely,
     * so we stay with LBA (CylSectors == 1)
     */
    dg->dg_Cylinders = dg->dg_TotalSectors;

    return 0;
}
