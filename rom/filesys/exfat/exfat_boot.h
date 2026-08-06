/*
 * exfat-handler - Main Boot Sector validation
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#ifndef EXFAT_BOOT_H
#define EXFAT_BOOT_H

#include "exfat_bounds.h"

/*
 * Implements docs/features/exfat/spec.md section 3.1 and 3.2. Free of AROS
 * dependencies so the host tests exercise this code rather than a copy: the
 * caller supplies UQUAD/ULONG/UWORD/UBYTE and a sector buffer.
 *
 * Every multi-byte field is read byte-wise (spec A1 and A2). This is a raw
 * little-endian byte buffer straight off the device: neither its base
 * alignment nor any C structure layout over it can be assumed. Casting a
 * pointer into it and dereferencing would be an unaligned access on m68k and
 * would depend on the compiler's padding everywhere else.
 */

/* Main Boot Sector field offsets */
#define EXFAT_BOOT_JUMPBOOT        0
#define EXFAT_BOOT_FSNAME          3
#define EXFAT_BOOT_MUSTBEZERO      11
#define EXFAT_BOOT_MUSTBEZERO_LEN  53
#define EXFAT_BOOT_PARTITIONOFF    64
#define EXFAT_BOOT_VOLUMELENGTH    72
#define EXFAT_BOOT_FATOFFSET       80
#define EXFAT_BOOT_FATLENGTH       84
#define EXFAT_BOOT_HEAPOFFSET      88
#define EXFAT_BOOT_CLUSTERCOUNT    92
#define EXFAT_BOOT_ROOTCLUSTER     96
#define EXFAT_BOOT_VOLUMESERIAL    100
#define EXFAT_BOOT_REVISION        104
#define EXFAT_BOOT_VOLUMEFLAGS     106
#define EXFAT_BOOT_SECTORSHIFT     108
#define EXFAT_BOOT_CLUSTERSHIFT    109
#define EXFAT_BOOT_NUMBEROFFATS    110
#define EXFAT_BOOT_DRIVESELECT     111
#define EXFAT_BOOT_PERCENTINUSE    112
#define EXFAT_BOOT_SIGNATURE       510

#define EXFAT_VOLUMEFLAG_ACTIVEFAT 0x0001
#define EXFAT_VOLUMEFLAG_DIRTY     0x0002
#define EXFAT_VOLUMEFLAG_MEDIAFAIL 0x0004
#define EXFAT_VOLUMEFLAG_CLEARZERO 0x0008

/* The first sector the FAT may occupy: the boot region is 24 sectors. */
#define EXFAT_BOOT_REGION_SECTORS  24

enum exfat_boot_result
{
    EXFAT_BOOT_OK = 0,
    EXFAT_BOOT_NOT_EXFAT,      /* -> ERROR_NOT_A_DOS_DISK */
    EXFAT_BOOT_WRONG_VERSION,  /* -> ERROR_OBJECT_WRONG_TYPE */
    EXFAT_BOOT_TEXFAT,         /* -> ERROR_OBJECT_WRONG_TYPE */
    EXFAT_BOOT_BAD_GEOMETRY,   /* -> ERROR_BAD_NUMBER */
    EXFAT_BOOT_BAD_CHECKSUM    /* -> ERROR_DISK_NOT_VALIDATED */
};

struct exfat_geometry
{
    UQUAD partition_offset;
    UQUAD volume_length;       /* sectors */
    ULONG fat_offset;          /* sectors from volume start */
    ULONG fat_length;          /* sectors */
    ULONG heap_offset;         /* sectors from volume start */
    ULONG cluster_count;
    ULONG root_cluster;
    ULONG volume_serial;
    UWORD volume_flags;
    UWORD sector_size;         /* bytes */
    UBYTE sector_shift;
    UBYTE cluster_shift;       /* sectors per cluster, as a shift */
    UBYTE percent_in_use;
};

/* Byte-wise little-endian readers (spec A2). */
static inline UWORD exfat_rd16(const UBYTE *p, unsigned off)
{
    return (UWORD)((UWORD)p[off] | ((UWORD)p[off + 1] << 8));
}

static inline ULONG exfat_rd32(const UBYTE *p, unsigned off)
{
    return (ULONG)p[off]
        | ((ULONG)p[off + 1] << 8)
        | ((ULONG)p[off + 2] << 16)
        | ((ULONG)p[off + 3] << 24);
}

static inline UQUAD exfat_rd64(const UBYTE *p, unsigned off)
{
    return (UQUAD)exfat_rd32(p, off)
        | ((UQUAD)exfat_rd32(p, off + 4) << 32);
}

/* Byte-wise little-endian writers for raw on-disk metadata. */
static inline void exfat_wr16(UBYTE *p, unsigned off, UWORD value)
{
    p[off] = (UBYTE)value;
    p[off + 1] = (UBYTE)(value >> 8);
}

static inline void exfat_wr32(UBYTE *p, unsigned off, ULONG value)
{
    p[off] = (UBYTE)value;
    p[off + 1] = (UBYTE)(value >> 8);
    p[off + 2] = (UBYTE)(value >> 16);
    p[off + 3] = (UBYTE)(value >> 24);
}

static inline void exfat_wr64(UBYTE *p, unsigned off, UQUAD value)
{
    exfat_wr32(p, off, (ULONG)value);
    exfat_wr32(p, off + 4, (ULONG)(value >> 32));
}

/* Preserve status and reserved bits across a base-exFAT transaction.
   ClearToZero must be cleared before mutation.  A dirty indication which
   predates the mount belongs to a repair tool and must remain set. */
static inline UWORD exfat_volume_flags_begin_write(UWORD flags)
{
    return (UWORD)((flags & ~EXFAT_VOLUMEFLAG_CLEARZERO)
        | EXFAT_VOLUMEFLAG_DIRTY);
}

static inline UWORD exfat_volume_flags_finish_write(UWORD flags,
    int was_dirty, int success)
{
    if (success && !was_dirty)
        flags &= (UWORD)~EXFAT_VOLUMEFLAG_DIRTY;
    return flags;
}

/* Sectors 0 to 10 inclusive are covered by the boot region checksum. */
#define EXFAT_BOOT_CHECKSUM_SECTORS  11
/* Sector 11 holds the checksum, repeated to fill it. */
#define EXFAT_BOOT_CHECKSUM_SECTOR   11

/*
 * Boot region checksum (spec 3.2). Call for sector_index 0 through 10 in
 * order, carrying the running sum.
 *
 * The exclusions are positional and belong to sector 0 alone: bytes 106, 107
 * and 112 there are VolumeFlags and PercentInUse, which mutate in normal use.
 * The same offsets in the later sectors are ordinary data and are covered.
 *
 * This takes the sector index rather than an is_first flag deliberately. A
 * caller feeding one buffer repeatedly can pass a true flag more than once,
 * silently applying the exclusion where it does not belong and weakening the
 * checksum. An index makes that a visible mistake instead of a plausible one.
 */
static inline ULONG exfat_boot_checksum(ULONG sum, const UBYTE *sector,
    ULONG sector_size, ULONG sector_index)
{
    ULONG i;

    for (i = 0; i < sector_size; i++)
    {
        if (sector_index == 0 && (i == EXFAT_BOOT_VOLUMEFLAGS
                               || i == EXFAT_BOOT_VOLUMEFLAGS + 1
                               || i == EXFAT_BOOT_PERCENTINUSE))
            continue;

        sum = ((sum & 1) ? 0x80000000u : 0u) + (sum >> 1) + (ULONG)sector[i];
    }

    return sum;
}

/*
 * Spec B3: sector 11 is filled with repeated copies of the checksum, one per
 * four bytes. Every copy is verified, not just the first: a sector whose
 * copies disagree is self-inconsistent, which is evidence of damage even when
 * the first happens to match.
 *
 * Reading the sector belongs to the mount path; deciding whether it is right
 * does not, so it lives here where it can be tested.
 */
static inline int exfat_verify_boot_checksum(const UBYTE *checksum_sector,
    ULONG sector_size, ULONG expected)
{
    ULONG off;

    if (sector_size < 4 || (sector_size & 3) != 0)
        return 0;

    for (off = 0; off < sector_size; off += 4)
        if (exfat_rd32(checksum_sector, off) != expected)
            return 0;

    return 1;
}

/*
 * Spec U6 to U8: the volume must fit inside the partition it was mounted on.
 *
 * VolumeLength is a field in the volume being validated, so it is exactly as
 * trustworthy as the volume. The Mountlist states what was actually
 * allocated. Believing the former without checking it against the latter lets
 * a corrupt VBR claim more space than it owns, and every read past the real
 * end lands in whatever partition follows: a disclosure of another
 * filesystem's contents, not merely a wrong answer.
 *
 * A volume smaller than its partition is legitimate and normal. The trailing
 * blocks are simply not part of the filesystem, and must not become readable
 * through it, which is why the caller takes its access boundary from
 * geo.volume_length and not from the partition size.
 */
static inline enum exfat_boot_result exfat_check_partition_fit(
    const struct exfat_geometry *g, UQUAD part_start, UQUAD part_blocks)
{
    if (!exfat_geometry_ok(part_start, part_blocks))
        return EXFAT_BOOT_BAD_GEOMETRY;

    if (g->volume_length > part_blocks)
        return EXFAT_BOOT_BAD_GEOMETRY;

    return EXFAT_BOOT_OK;
}

/*
 * Spec U3: the exFAT logical sector size must equal the device block size in
 * Phase 1. Carrying a ratio through every byte-offset computation is a second
 * addressing mode reachable only on hardware we cannot test against, so it is
 * refused explicitly rather than approximated.
 */
static inline enum exfat_boot_result exfat_check_sector_units(
    const struct exfat_geometry *g, ULONG device_block_size)
{
    if (device_block_size == 0)
        return EXFAT_BOOT_BAD_GEOMETRY;
    if ((ULONG)g->sector_size != device_block_size)
        return EXFAT_BOOT_BAD_GEOMETRY;
    return EXFAT_BOOT_OK;
}

/*
 * Validate the Main Boot Sector and fill in the geometry.
 *
 * Fields are checked in the order required by spec G1, so that each bound is
 * evaluated against operands already known to be good. Every comparison is in
 * the S2 subtraction form.
 */
static inline enum exfat_boot_result exfat_validate_boot(const UBYTE *b,
    ULONG buf_len, struct exfat_geometry *out)
{
    static const UBYTE fsname[8] =
        { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    /*
     * Built here and copied out only once every check has passed, so a
     * refused volume never leaves half-validated geometry where a caller
     * that ignored the return value could act on it.
     */
    struct exfat_geometry tmp;
    struct exfat_geometry *g = &tmp;
    ULONG i, sector_size, fat_entries, fat_min;
    UWORD revision, flags;
    UBYTE nfats;

    if (buf_len < 512)
        return EXFAT_BOOT_NOT_EXFAT;

    /* Identity first: anything below is meaningless if this is not exFAT. */
    if (b[EXFAT_BOOT_JUMPBOOT] != 0xEB || b[EXFAT_BOOT_JUMPBOOT + 1] != 0x76
        || b[EXFAT_BOOT_JUMPBOOT + 2] != 0x90)
        return EXFAT_BOOT_NOT_EXFAT;

    for (i = 0; i < 8; i++)
        if (b[EXFAT_BOOT_FSNAME + i] != fsname[i])
            return EXFAT_BOOT_NOT_EXFAT;

    /* The region a legacy FAT driver would read as its BPB (spec G2). */
    for (i = 0; i < EXFAT_BOOT_MUSTBEZERO_LEN; i++)
        if (b[EXFAT_BOOT_MUSTBEZERO + i] != 0)
            return EXFAT_BOOT_NOT_EXFAT;

    if (exfat_rd16(b, EXFAT_BOOT_SIGNATURE) != 0xAA55)
        return EXFAT_BOOT_NOT_EXFAT;

    /* Revision: exactly 1.00 (spec V1). Minor is the low byte. */
    revision = exfat_rd16(b, EXFAT_BOOT_REVISION);
    if (revision != 0x0100)
        return EXFAT_BOOT_WRONG_VERSION;

    /* TexFAT (spec 1.2). */
    nfats = b[EXFAT_BOOT_NUMBEROFFATS];
    if (nfats == 2)
        return EXFAT_BOOT_TEXFAT;
    if (nfats != 1)
        return EXFAT_BOOT_BAD_GEOMETRY;

    flags = exfat_rd16(b, EXFAT_BOOT_VOLUMEFLAGS);
    if (flags & EXFAT_VOLUMEFLAG_ACTIVEFAT)
        return EXFAT_BOOT_WRONG_VERSION;   /* ActiveFat set with one FAT */
    g->volume_flags = flags;

    /* Sector shift, then cluster shift: everything below is in these units. */
    g->sector_shift = b[EXFAT_BOOT_SECTORSHIFT];
    if (g->sector_shift < 9 || g->sector_shift > 12)
        return EXFAT_BOOT_BAD_GEOMETRY;
    sector_size = 1u << g->sector_shift;
    g->sector_size = (UWORD)sector_size;

    g->cluster_shift = b[EXFAT_BOOT_CLUSTERSHIFT];
    if (g->cluster_shift > (UBYTE)(25 - g->sector_shift))
        return EXFAT_BOOT_BAD_GEOMETRY;    /* cluster > 32 MiB */

    g->percent_in_use = b[EXFAT_BOOT_PERCENTINUSE];
    if (g->percent_in_use > 100 && g->percent_in_use != 0xFF)
        return EXFAT_BOOT_BAD_GEOMETRY;

    /* VolumeLength: at least 1 MiB expressed in sectors. */
    g->volume_length = exfat_rd64(b, EXFAT_BOOT_VOLUMELENGTH);
    if (g->volume_length < ((UQUAD)1 << (20 - g->sector_shift)))
        return EXFAT_BOOT_BAD_GEOMETRY;

    g->partition_offset = exfat_rd64(b, EXFAT_BOOT_PARTITIONOFF);

    g->fat_offset = exfat_rd32(b, EXFAT_BOOT_FATOFFSET);
    g->fat_length = exfat_rd32(b, EXFAT_BOOT_FATLENGTH);
    g->heap_offset = exfat_rd32(b, EXFAT_BOOT_HEAPOFFSET);
    g->cluster_count = exfat_rd32(b, EXFAT_BOOT_CLUSTERCOUNT);
    g->root_cluster = exfat_rd32(b, EXFAT_BOOT_ROOTCLUSTER);
    g->volume_serial = exfat_rd32(b, EXFAT_BOOT_VOLUMESERIAL);

    /* The FAT cannot start inside the boot region. */
    if (g->fat_offset < EXFAT_BOOT_REGION_SECTORS)
        return EXFAT_BOOT_BAD_GEOMETRY;

    /* Heap after the FAT, and the whole lot inside the volume. */
    if (g->heap_offset <= g->fat_offset)
        return EXFAT_BOOT_BAD_GEOMETRY;
    if (g->fat_length > g->heap_offset - g->fat_offset)
        return EXFAT_BOOT_BAD_GEOMETRY;
    if ((UQUAD)g->heap_offset >= g->volume_length)
        return EXFAT_BOOT_BAD_GEOMETRY;

    /* The FAT must cover ClusterCount + 2 entries of 4 bytes each. */
    if (g->cluster_count > 0xFFFFFFF5u)
        return EXFAT_BOOT_BAD_GEOMETRY;
    fat_entries = g->cluster_count + 2;
    fat_min = fat_entries / (sector_size / 4);
    if (fat_entries % (sector_size / 4))
        fat_min++;
    if (g->fat_length < fat_min)
        return EXFAT_BOOT_BAD_GEOMETRY;

    /* ClusterCount must match what the heap can actually hold. */
    {
        UQUAD heap_sectors = g->volume_length - (UQUAD)g->heap_offset;
        UQUAD capacity = heap_sectors >> g->cluster_shift;

        if (capacity > 0xFFFFFFF5u)
            capacity = 0xFFFFFFF5u;
        if ((UQUAD)g->cluster_count != capacity)
            return EXFAT_BOOT_BAD_GEOMETRY;
    }

    /* Root directory must name a real cluster. */
    if (g->root_cluster < 2
        || (UQUAD)g->root_cluster > (UQUAD)g->cluster_count + 1)
        return EXFAT_BOOT_BAD_GEOMETRY;

    *out = tmp;
    return EXFAT_BOOT_OK;
}

#endif /* EXFAT_BOOT_H */
