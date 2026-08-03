/*
 * exfat-handler - sector-domain range arithmetic
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#ifndef EXFAT_BOUNDS_H
#define EXFAT_BOUNDS_H

/*
 * The single implementation of the sector-domain bounds rules from
 * docs/features/exfat/spec.md S1 to S4. It is deliberately free of AROS
 * dependencies so the host test in hosted/exfat-tests/ exercises this code
 * rather than a copy of it. The caller supplies UQUAD and ULONG.
 *
 * Domain split (spec S1): absolute sector positions and volume capacities are
 * UQUAD. A single transfer count stays ULONG, because it is bounded by the
 * cache range size and must end up in a 32-bit io_Length. It is range checked
 * before every conversion.
 */

#define EXFAT_UQUAD_MAX  (~(UQUAD)0)
#define EXFAT_ULONG_MAX  (~(ULONG)0)

enum exfat_range
{
    EXFAT_RANGE_OK = 0,
    EXFAT_RANGE_OUTSIDE,     /* wholly outside the volume */
    EXFAT_RANGE_BADGEOMETRY, /* the volume itself is not representable */
    EXFAT_RANGE_TOOBIG       /* byte range leaves the 64-bit device domain */
};

/*
 * Mount-time geometry check. A volume whose last sector is not representable
 * is rejected outright rather than clipped per request: a partially usable
 * volume is not a thing this handler offers.
 */
static inline int exfat_geometry_ok(UQUAD start, UQUAD total)
{
    if (total == 0)
        return 0;
    /* start + (total - 1) must not overflow, expressed as a subtraction */
    if (total - 1 > EXFAT_UQUAD_MAX - start)
        return 0;
    return 1;
}

/*
 * Last valid sector. Only call once exfat_geometry_ok() has passed, which is
 * what makes this addition bounded rather than a guess.
 */
static inline UQUAD exfat_last_sector(UQUAD start, UQUAD total)
{
    return start + (total - 1);
}

/*
 * Clip [num, num + nblocks) to the volume [start, start + total).
 *
 * Every bound is a subtraction against a value already known to be in range.
 * An addition here would overflow UQUAD exactly as it used to overflow ULONG,
 * and a wrapped bound admits the access it was meant to reject.
 *
 * On EXFAT_RANGE_OK, *num and *nblocks are updated to the clipped request and
 * *skipped is how many sectors were trimmed from the front, so the caller can
 * advance its data pointer.
 */
static inline enum exfat_range exfat_clip(UQUAD *num, ULONG *nblocks,
    UQUAD *skipped, UQUAD start, UQUAD total)
{
    UQUAD n = *num, rel;
    ULONG nb = *nblocks;

    *skipped = 0;

    if (!exfat_geometry_ok(start, total))
        return EXFAT_RANGE_BADGEOMETRY;

    if (nb == 0)
        return EXFAT_RANGE_OUTSIDE;

    if (n < start)
    {
        UQUAD before = start - n;

        if ((UQUAD)nb <= before)
            return EXFAT_RANGE_OUTSIDE;

        nb -= (ULONG)before;      /* before < nb <= ULONG_MAX, so this fits */
        *skipped = before;
        n = start;
    }

    rel = n - start;              /* n >= start here */
    if (rel >= total)
        return EXFAT_RANGE_OUTSIDE;

    if ((UQUAD)nb > total - rel)
        nb = (ULONG)(total - rel);

    *num = n;
    *nblocks = nb;
    return EXFAT_RANGE_OK;
}

/*
 * Byte range for an already-clipped request. Fails rather than truncating if
 * the result leaves the device's 64-bit byte-offset domain, or if the length
 * will not fit a 32-bit io_Length.
 */
static inline enum exfat_range exfat_byte_range(UQUAD num, ULONG nblocks,
    ULONG block_size, UQUAD *off, ULONG *len)
{
    UQUAD o;
    ULONG l;

    if (block_size == 0)
        return EXFAT_RANGE_BADGEOMETRY;

    if (num > EXFAT_UQUAD_MAX / block_size)
        return EXFAT_RANGE_TOOBIG;
    o = num * block_size;

    if (nblocks > EXFAT_ULONG_MAX / block_size)
        return EXFAT_RANGE_TOOBIG;
    l = nblocks * block_size;

    if ((UQUAD)l > EXFAT_UQUAD_MAX - o)
        return EXFAT_RANGE_TOOBIG;

    *off = o;
    *len = l;
    return EXFAT_RANGE_OK;
}

/*
 * Mountlist geometry, promoted safely.
 *
 * de_Surfaces * de_BlocksPerTrack is a 32-bit product on a 32-bit target and
 * overflows on a large disk, and de_SizeBlock << 2 shifts a value that came
 * from a Mountlist and is therefore not trusted. Both are computed here in
 * UQUAD with the result range checked, rather than at the call site where the
 * promotion is easy to place one operator too late.
 *
 * Returns the partition extent in device blocks and the block size in bytes.
 */
static inline enum exfat_range exfat_mountlist_extent(
    ULONG low_cyl, ULONG high_cyl, ULONG surfaces, ULONG blocks_per_track,
    ULONG size_block_longs,
    UQUAD *start, UQUAD *blocks, ULONG *block_size)
{
    UQUAD cylinder, cyl_count, bsize;

    if (surfaces == 0 || blocks_per_track == 0 || size_block_longs == 0)
        return EXFAT_RANGE_BADGEOMETRY;
    if (high_cyl < low_cyl)
        return EXFAT_RANGE_BADGEOMETRY;

    /* Promote before multiplying, not after. */
    cylinder = (UQUAD)surfaces * (UQUAD)blocks_per_track;
    if (cylinder == 0)
        return EXFAT_RANGE_BADGEOMETRY;

    cyl_count = (UQUAD)high_cyl - (UQUAD)low_cyl + 1;

    /* start = low_cyl * cylinder, refused rather than wrapped */
    if ((UQUAD)low_cyl > EXFAT_UQUAD_MAX / cylinder)
        return EXFAT_RANGE_TOOBIG;
    *start = (UQUAD)low_cyl * cylinder;

    if (cyl_count > EXFAT_UQUAD_MAX / cylinder)
        return EXFAT_RANGE_TOOBIG;
    *blocks = cyl_count * cylinder;

    /* A block size is a power of two between 512 and 32768 bytes. */
    bsize = (UQUAD)size_block_longs << 2;
    if (bsize < 512 || bsize > 32768 || (bsize & (bsize - 1)) != 0)
        return EXFAT_RANGE_BADGEOMETRY;
    *block_size = (ULONG)bsize;

    if (!exfat_geometry_ok(*start, *blocks))
        return EXFAT_RANGE_BADGEOMETRY;

    return EXFAT_RANGE_OK;
}

/*
 * A device without TD64 or NSD 64-bit support takes only the low 32 bits of
 * the offset: the high word is placed in io_Actual and a 32-bit CMD_READ
 * ignores it, so the transfer silently lands somewhere else entirely. Refuse
 * instead.
 */
static inline int exfat_offset_fits_32(UQUAD off, ULONG len)
{
    if (off > 0xFFFFFFFFull)
        return 0;
    if (len == 0)
        return 1;
    /* off + len - 1 must still fit, expressed without the addition. */
    return (UQUAD)(len - 1) <= 0xFFFFFFFFull - off;
}

/*
 * Everything a transfer needs decided in one place: the byte range, and
 * whether this device can actually address it.
 *
 * Both callers go through here rather than each performing its own checks.
 * The boot-region read bypasses the cache, and therefore bypassed the guard
 * that lived in AccessDisk(), which meant a 32-bit-only device with a
 * partition above 4 GB read a truncated address and appeared to succeed. A
 * guard that is not on every path reads as covering more than it does.
 */
static inline enum exfat_range exfat_prepare_transfer(UQUAD block,
    ULONG count, ULONG block_size, int dev_64bit, UQUAD *off, ULONG *len)
{
    enum exfat_range r = exfat_byte_range(block, count, block_size, off, len);

    if (r != EXFAT_RANGE_OK)
        return r;

    if (!dev_64bit && !exfat_offset_fits_32(*off, *len))
        return EXFAT_RANGE_TOOBIG;

    return EXFAT_RANGE_OK;
}

#endif /* EXFAT_BOUNDS_H */
