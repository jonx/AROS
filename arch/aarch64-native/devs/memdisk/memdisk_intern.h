#ifndef MEMDISK_INTERN_H
#define MEMDISK_INTERN_H

#include <exec/devices.h>
#include <exec/io.h>

/* RAM-resident boot image, loaded by the host at a fixed physical address. */
#define SYS_IMG_BASE    0x30000000UL
#define SYS_IMG_SIZE    0x08000000UL
#define SYS_IMG_BLOCK   512

struct MemDiskBase
{
    struct Device   md_Device;
    struct Unit     md_Unit;
};

#endif /* MEMDISK_INTERN_H */
