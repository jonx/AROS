/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    memdisk.device - presents a RAM-resident filesystem image, placed by the
    host at a fixed physical address, as a bootable block device (SYS:).
*/

#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/resident.h>

#include <devices/trackdisk.h>
#include <devices/newstyle.h>

#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include <libraries/expansion.h>
#include <libraries/expansionbase.h>

#include <proto/exec.h>
#include <proto/expansion.h>

#include <aros/libcall.h>
#include <aros/macros.h>
#include <aros/symbolsets.h>

#include <string.h>
#include <stddef.h>

#include "memdisk_intern.h"

#include LC_LIBDEFS_FILE

#define DEBUG 0
#include <aros/debug.h>

/* FAT32 dostype, as registered by fat-handler with FileSystem.resource. */
#define MEMDISK_DOSTYPE 0x46415402UL

#define MEMDISK_BLOCKS  (SYS_IMG_SIZE / SYS_IMG_BLOCK)

static const UWORD SupportedCommands[] =
{
    CMD_READ,
    CMD_WRITE,
    CMD_UPDATE,
    CMD_CLEAR,
    TD_GETGEOMETRY,
    TD_CHANGENUM,
    TD_CHANGESTATE,
    TD_PROTSTATUS,
    TD_MOTOR,
    ETD_READ,
    ETD_WRITE,
    TD_READ64,
    TD_WRITE64,
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_DEVICEQUERY,
    0
};

static inline UBYTE *memdisk_base(void)
{
    return (UBYTE *)(IPTR)SYS_IMG_BASE;
}

static void memdisk_AddBootNode(struct MemDiskBase *MemDiskBase)
{
    struct ExpansionBase *ExpansionBase;
    struct DeviceNode *devnode;
    IPTR pp[4 + DE_BOOTBLOCKS + 1];

    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!ExpansionBase)
    {
        D(bug("[memdisk] no expansion.library\n"));
        return;
    }

    memset(pp, 0, sizeof(pp));

    pp[0]                       = (IPTR)"SYS";
    pp[1]                       = (IPTR)"memdisk.device";
    pp[2]                       = 0;                     /* unit   */
    pp[3]                       = 0;                     /* flags  */
    pp[DE_TABLESIZE    + 4]     = DE_BOOTBLOCKS;
    pp[DE_SIZEBLOCK    + 4]     = SYS_IMG_BLOCK >> 2;    /* longwords per block */
    pp[DE_NUMHEADS     + 4]     = 1;
    pp[DE_SECSPERBLOCK + 4]     = 1;
    pp[DE_BLKSPERTRACK + 4]     = 1;
    pp[DE_RESERVEDBLKS + 4]     = 2;
    pp[DE_LOWCYL       + 4]     = 0;
    pp[DE_HIGHCYL      + 4]     = MEMDISK_BLOCKS - 1;
    pp[DE_NUMBUFFERS   + 4]     = 10;
    pp[DE_BUFMEMTYPE   + 4]     = MEMF_PUBLIC;
    pp[DE_MAXTRANSFER  + 4]     = 0x00200000;
    pp[DE_MASK         + 4]     = 0x7FFFFFFE;
    pp[DE_BOOTPRI      + 4]     = 5;
    pp[DE_DOSTYPE      + 4]     = MEMDISK_DOSTYPE;
    pp[DE_CONTROL      + 4]     = 0;
    pp[DE_BOOTBLOCKS   + 4]     = 2;

    devnode = MakeDosNode(pp);
    if (devnode)
        AddBootNode(pp[DE_BOOTPRI + 4], 0, devnode, NULL);
    else
        D(bug("[memdisk] MakeDosNode failed\n"));

    CloseLibrary((struct Library *)ExpansionBase);
}

static int GM_UNIQUENAME(Init)(LIBBASETYPEPTR MemDiskBase)
{
    MemDiskBase->md_Unit.unit_OpenCnt = 0;

    memdisk_AddBootNode(MemDiskBase);

    return TRUE;
}

static int GM_UNIQUENAME(Open)
(
    LIBBASETYPEPTR MemDiskBase,
    struct IORequest *io,
    ULONG unitnum,
    ULONG flags
)
{
    if (unitnum != 0)
    {
        io->io_Error = TDERR_BadUnitNum;
        return FALSE;
    }

    MemDiskBase->md_Unit.unit_OpenCnt++;
    io->io_Unit  = &MemDiskBase->md_Unit;
    io->io_Error = 0;

    return TRUE;
}

static int GM_UNIQUENAME(Close)
(
    LIBBASETYPEPTR MemDiskBase,
    struct IORequest *io
)
{
    if (MemDiskBase->md_Unit.unit_OpenCnt > 0)
        MemDiskBase->md_Unit.unit_OpenCnt--;
    io->io_Unit = (struct Unit *)~0;

    return TRUE;
}

ADD2INITLIB(GM_UNIQUENAME(Init), 0)
ADD2OPENDEV(GM_UNIQUENAME(Open), 0)
ADD2CLOSEDEV(GM_UNIQUENAME(Close), 0)

static LONG memdisk_transfer(struct IOExtTD *iotd, UQUAD offset, BOOL write)
{
    ULONG len = iotd->iotd_Req.io_Length;
    APTR  buf = iotd->iotd_Req.io_Data;

    if (offset > SYS_IMG_SIZE || len > SYS_IMG_SIZE || (offset + len) > SYS_IMG_SIZE)
    {
        iotd->iotd_Req.io_Actual = 0;
        return IOERR_BADADDRESS;
    }

    if (write)
        CopyMem(buf, memdisk_base() + offset, len);
    else
        CopyMem(memdisk_base() + offset, buf, len);

    iotd->iotd_Req.io_Actual = len;
    return 0;
}

AROS_LH1(void, BeginIO,
    AROS_LHA(struct IOExtTD *, iotd, A1),
    struct MemDiskBase *, MemDiskBase, 5, memdisk)
{
    AROS_LIBFUNC_INIT

    ULONG cmd = iotd->iotd_Req.io_Command;
    LONG  err = 0;
    UQUAD offset;

    switch (cmd)
    {
        case CMD_READ:
        case ETD_READ:
            offset = (ULONG)iotd->iotd_Req.io_Offset;
            err = memdisk_transfer(iotd, offset, FALSE);
            break;

        case TD_READ64:
        case NSCMD_TD_READ64:
            offset = ((UQUAD)iotd->iotd_Req.io_Actual << 32) | (ULONG)iotd->iotd_Req.io_Offset;
            err = memdisk_transfer(iotd, offset, FALSE);
            break;

        case CMD_WRITE:
        case ETD_WRITE:
        case TD_FORMAT:
        case ETD_FORMAT:
            offset = (ULONG)iotd->iotd_Req.io_Offset;
            err = memdisk_transfer(iotd, offset, TRUE);
            break;

        case TD_WRITE64:
        case NSCMD_TD_WRITE64:
            offset = ((UQUAD)iotd->iotd_Req.io_Actual << 32) | (ULONG)iotd->iotd_Req.io_Offset;
            err = memdisk_transfer(iotd, offset, TRUE);
            break;

        case TD_GETGEOMETRY:
        {
            struct DriveGeometry *dg = (struct DriveGeometry *)iotd->iotd_Req.io_Data;
            dg->dg_SectorSize   = SYS_IMG_BLOCK;
            dg->dg_TotalSectors = MEMDISK_BLOCKS;
            dg->dg_Cylinders    = MEMDISK_BLOCKS;
            dg->dg_CylSectors   = 1;
            dg->dg_Heads        = 1;
            dg->dg_TrackSectors = 1;
            dg->dg_BufMemType   = MEMF_PUBLIC;
            dg->dg_DeviceType   = DG_DIRECT_ACCESS;
            dg->dg_Flags        = 0;
            iotd->iotd_Req.io_Actual = sizeof(struct DriveGeometry);
            break;
        }

        case NSCMD_DEVICEQUERY:
        {
            struct NSDeviceQueryResult *d = (struct NSDeviceQueryResult *)iotd->iotd_Req.io_Data;
            if (iotd->iotd_Req.io_Length < (LONG)offsetof(struct NSDeviceQueryResult, SupportedCommands) + (LONG)sizeof(UWORD *))
            {
                err = IOERR_BADLENGTH;
                break;
            }
            d->DevQueryFormat    = 0;
            d->SizeAvailable     = sizeof(struct NSDeviceQueryResult);
            d->DeviceType        = NSDEVTYPE_TRACKDISK;
            d->DeviceSubType     = 0;
            d->SupportedCommands = (UWORD *)SupportedCommands;
            iotd->iotd_Req.io_Actual = sizeof(struct NSDeviceQueryResult);
            break;
        }

        case TD_GETDRIVETYPE:
            iotd->iotd_Req.io_Actual = DRIVE3_5;
            break;

        case TD_CHANGENUM:
        case TD_CHANGESTATE:
        case TD_PROTSTATUS:
            /* media always present, never write protected, never changed */
            iotd->iotd_Req.io_Actual = 0;
            break;

        case CMD_UPDATE:
        case CMD_CLEAR:
        case ETD_UPDATE:
        case ETD_CLEAR:
        case TD_MOTOR:
        case ETD_MOTOR:
        case TD_SEEK:
        case ETD_SEEK:
        case TD_REMOVE:
        case TD_ADDCHANGEINT:
        case TD_REMCHANGEINT:
            /* no-op success */
            break;

        default:
            err = IOERR_NOCMD;
            break;
    }

    iotd->iotd_Req.io_Error = err;
    iotd->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;

    if (!(iotd->iotd_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&iotd->iotd_Req.io_Message);

    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, AbortIO,
    AROS_LHA(struct IOExtTD *, iotd, A1),
    struct MemDiskBase *, MemDiskBase, 6, memdisk)
{
    AROS_LIBFUNC_INIT

    return 0;

    AROS_LIBFUNC_EXIT
}
