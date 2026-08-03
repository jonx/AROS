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

#endif /* EXFAT_BOUNDS_H */
