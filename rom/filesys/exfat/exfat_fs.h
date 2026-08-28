/*
 * exfat-handler - in-memory structures
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#ifndef EXFAT_FS_H
#define EXFAT_FS_H

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/exfat.h>
#include <exec/interrupts.h>

#include "exfat_bounds.h"
#include "exfat_boot.h"
#include "cache.h"

/*
 * Debug switches, per source file. See exfat_debug.h.
 */
#define DEBUG_MISC          0
#define DEBUG_CACHESTATS    0
#define DEBUG_VOLUME        0
#define DEBUG_DIRENTRY      0

#define EXFAT_POOL_SIZE     65536
#define EXFAT_MAX_NAME      255
#define EXFAT_MAX_PATH      1023

/*
 * A stream: anything with clusters behind it, whether a file's data or a
 * directory. Spec section 5.1.
 *
 * NoFatChain makes "what is the next cluster" a property of the stream rather
 * than of the volume, so traversal takes one of these rather than a bare
 * cluster number. A walker that knew only (volume, cluster) could not express
 * a contiguous stream at all, and one that knew only the flags could not tell
 * where the stream ends.
 */
struct exfat_stream
{
    ULONG first_cluster;       /* validated 2 ..= cluster_count + 1 */
    UQUAD data_length;         /* bytes */
    UQUAD valid_data_length;   /* bytes; <= data_length, rest reads as zero */
    BOOL  contiguous;          /* NoFatChain: do not consult the FAT */
    BOOL  length_known;        /* FALSE for the root directory */
};

struct VolumeIdentity
{
    UBYTE            name[34];      /* BCPL string, 11 UTF-16 units max */
    struct DateStamp create_time;
};

struct exfat_lock;
struct LocaleBase;

/*
 * Per-volume state.
 *
 * Sector-domain values are UQUAD throughout: absolute positions and volume
 * capacities. Cluster identifiers stay ULONG, because exFAT FAT entries are
 * 32 bits and the cluster domain is 32-bit by definition (spec S1, S3).
 */
struct FSSuper
{
    struct Node      node;
    struct Globals  *glob;
    struct DosList  *doslist;

    /* This FSSuper owns the cache and destroys it when the volume goes. */
    struct Cache    *cache;

    /* Absolute sector positions on the device. */
    UQUAD            first_device_sector;  /* where the volume starts */
    UQUAD            total_sectors;        /* how many it has */
    UQUAD            fat_start;            /* absolute, = first + fat_offset */
    UQUAD            heap_start;           /* absolute, = first + heap_offset */

    /* Geometry exactly as validated from the boot sector. */
    struct exfat_geometry geo;

    /* Cluster domain: 32-bit by definition. */
    ULONG            cluster_count;
    ULONG            root_cluster;

    /* Derived sizes, cached to keep the hot paths shift-free. */
    ULONG            sector_size;
    ULONG            cluster_size;
    UBYTE            sector_shift;
    UBYTE            cluster_shift;        /* sectors per cluster, as a shift */

    /* The root directory, a FAT-chained stream of unknown length (spec F4). */
    struct exfat_stream root;

    /* Phase-1 metadata loaded from the root directory. */
    UBYTE            *bitmap;
    UQUAD             bitmap_length;
    struct exfat_stream bitmap_stream;
    UWORD            *upcase;          /* 65536 host-endian UTF-16 entries */
    ULONG             free_clusters;
    ULONG             lock_count;
    struct exfat_lock *locks;
    struct exfat_lock *write_handle;
    BOOL              online;

    /* Validated main-boot-sector state.  A volume which arrived dirty stays
       readable, but is not eligible for writes or for clearing that flag. */
    UWORD             volume_flags;
    /* The device itself refuses writes (write-protect switch, read-only unit).
       Probed once per medium, so a swapped-in disk gets its own answer. */
    BOOL              device_write_protected;
    UWORD             write_transaction_original_flags;
    BOOL              write_transaction_active;
    BOOL              write_transaction_failed;
    BOOL              write_transaction_changed;
    BOOL              allocation_changed;
#ifdef EXFAT_TEST_FAILPOINTS
    ULONG             test_fail_after_sync;
#endif

    struct VolumeIdentity volume;
};

/*
 * Why this volume cannot be written, or 0 when it can.
 *
 * A device that refuses writes is reported as write-protected, which is what a
 * caller can act on: the medium is intact and complete, it simply may not be
 * changed. An inherited dirty or media-failure flag is a different statement -
 * the volume's own consistency is unproven - and keeps the stricter contract,
 * under which even clearing that flag is refused.
 */
static inline LONG exfat_write_refusal(const struct FSSuper *sb)
{
    if (sb->device_write_protected)
        return ERROR_DISK_WRITE_PROTECTED;
    if ((sb->volume_flags
            & (EXFAT_VOLUMEFLAG_DIRTY | EXFAT_VOLUMEFLAG_MEDIAFAIL)) != 0)
        return ERROR_DISK_NOT_VALIDATED;
    return 0;
}

/* A validated file entry set, independent of the directory buffer. */
struct exfat_entry
{
    struct exfat_stream stream;
    UWORD name[EXFAT_MAX_NAME];
    UWORD name_length;
    UWORD attributes;
    ULONG directory_index;
    ULONG modify_timestamp;
    UBYTE modify_10ms;
    BYTE  modify_utc;
};

/* Locks and file handles use the same DOS-visible prefix as FileLock. */
struct exfat_lock
{
    BPTR             fl_Link;
    IPTR             fl_Key;
    LONG             fl_Access;
    struct MsgPort  *fl_Task;
    BPTR             fl_Volume;

    struct exfat_lock *next;
    ULONG            magic;
    struct FSSuper  *sb;
    struct exfat_entry entry;
    struct exfat_stream parent;
    UQUAD            position;
    ULONG            enum_index;
    BOOL             writable;
    BOOL             write_transaction;
    BOOL             contents_modified;
    UWORD            path_length;
    UBYTE            path[EXFAT_MAX_PATH + 1];
};

#define EXFAT_LOCK_MAGIC 0x58464c4bUL /* 'XFLK' */
#define EXFAT_ATTR_READONLY  0x0001
#define EXFAT_ATTR_HIDDEN    0x0002
#define EXFAT_ATTR_SYSTEM    0x0004
#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_ATTR_ARCHIVE   0x0020

#define EXFAT_DATE_MODIFIED  0x01U
#define EXFAT_DATE_ACCESSED  0x02U

#ifdef EXFAT_TEST_FAILPOINTS
#define ACTION_EXFAT_ARM_FAILPOINT 0x58464650L /* 'XFFP' */
#define EXFAT_FAILPOINT_COOKIE     0x45584654UL /* 'EXFT' */
#endif

/*
 * Handler-wide state. Only what the transport layer needs; the rest arrives
 * with the mount and packet code.
 */
struct Globals
{
    struct ExecBase     *gl_SysBase;
    struct DosLibrary   *gl_DOSBase;
    struct Library      *gl_UtilityBase;
    struct LocaleBase   *gl_LocaleBase; /* optional; timestamps still work */

    struct Task         *ourtask;
    struct MsgPort      *ourport;
    APTR                 mempool;

    struct DosList      *devnode;
    struct FileSysStartupMsg *fssm;

    /* Device transport */
    struct IOExtTD      *diskioreq;
    struct IOExtTD      *diskchgreq;
    struct MsgPort      *diskport;
    ULONG                diskchgsig_bit;
    UWORD                readcmd;
    UWORD                writecmd;
    /*
     * TRUE only when the device answered a TD64 or NSD 64-bit probe. A 32-bit
     * command takes the low half of the offset and ignores the high word in
     * io_Actual, so a request past 4 GB on such a device does not fail: it
     * reads somewhere else. Every access checks this.
     */
    BOOL                 dev_64bit;

    /* Result of the last mount attempt, so a refusal is reportable. */
    LONG                 mount_error;

    /*
     * Last out-of-range sector reported, so a runaway caller does not produce
     * one requester per request. UQUAD because it is compared against and
     * assigned from a sector number: a ULONG here would alias every sector
     * above 2^32 onto its low half and suppress genuinely distinct errors.
     */
    UQUAD                last_num;

    struct FSSuper      *sb;            /* current volume, or NULL */
    struct MinList       sblist;        /* volumes with outstanding locks */

    LONG                 disk_inhibited;
    BOOL                 disk_inserted;
    BOOL                 restart_timer;
    BOOL                 quit;

    struct IntData {
        struct Interrupt Interrupt;
        struct ExecBase *SysBase;
        struct Task *task;
        ULONG signal;
    } DiskChangeIntData;
};

#define SysBase     (glob->gl_SysBase)
#define DOSBase     (glob->gl_DOSBase)
#define UtilityBase (glob->gl_UtilityBase)

/*
 * First sector of a cluster. Spec S4: the shift is evaluated in UQUAD, and
 * the caller must already have validated the cluster as 2 ..= cluster_count
 * + 1. This macro is NOT defensive against an invalid cluster; the range
 * check is the whole of that protection.
 */
#define EXFAT_SECTOR_FROM_CLUSTER(sb, cl) \
    ((((UQUAD)(cl) - 2) << (sb)->cluster_shift) + (sb)->heap_start)

static inline int exfat_cluster_valid(const struct FSSuper *sb, ULONG cl)
{
    return cl >= 2 && (UQUAD)cl <= (UQUAD)sb->cluster_count + 1;
}

#endif /* EXFAT_FS_H */
