/* Pure helpers for the exFAT cache's 32-sector dirty mask. */
#ifndef EXFAT_CACHE_BITS_H
#define EXFAT_CACHE_BITS_H

static inline int exfat_dirty_span(ULONG dirty, ULONG *first, ULONG *count)
{
    ULONG f, n;

    if (dirty == 0)
        return 0;
    for (f = 0; f < 32; f++)
        if ((dirty & ((ULONG)1 << f)) != 0)
            break;
    for (n = 1; f + n < 32; n++)
        if ((dirty & ((ULONG)1 << (f + n))) == 0)
            break;
    *first = f;
    *count = n;
    return 1;
}

static inline ULONG exfat_dirty_span_mask(ULONG first, ULONG count)
{
    if (first == 0 && count == 32)
        return ~(ULONG)0;
    return (((ULONG)1 << count) - 1) << first;
}

#endif
