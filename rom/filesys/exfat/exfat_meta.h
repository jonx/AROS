/* Pure exFAT metadata helpers, shared with the hosted corruption tests. */
#ifndef EXFAT_META_H
#define EXFAT_META_H

static inline ULONG exfat_meta_rotate32(ULONG sum, UBYTE byte)
{
    return ((sum & 1) ? 0x80000000UL : 0) + (sum >> 1) + byte;
}

static inline UWORD exfat_meta_rotate16(UWORD sum, UBYTE byte)
{
    return (UWORD)(((sum & 1) ? 0x8000U : 0) + (sum >> 1) + byte);
}

static inline int exfat_stream_cluster_count(UQUAD length, ULONG cluster_size,
    UQUAD *count)
{
    if (cluster_size == 0)
        return 0;
    *count = length / cluster_size;
    if (length % cluster_size != 0)
        (*count)++;
    return 1;
}

/* A final 0xffff is the U+FFFF mapping itself; elsewhere it introduces an
   identity run. This ambiguity is present in real macOS-authored tables. */
static inline int exfat_expand_upcase(const UBYTE *raw, ULONG length,
    UWORD *out, ULONG capacity, ULONG *written)
{
    ULONG pos = 0, cp = 0, i;

    if ((length & 1) != 0)
        return 0;
    while (pos < length && cp < capacity)
    {
        UWORD value = exfat_rd16(raw, pos);
        pos += 2;
        if (value != 0xffffU || pos == length)
            out[cp++] = value;
        else
        {
            UWORD count = exfat_rd16(raw, pos);
            pos += 2;
            if ((ULONG)count > capacity - cp)
                return 0;
            for (i = 0; i < count; i++, cp++)
                out[cp] = (UWORD)cp;
        }
    }
    *written = cp;
    return pos == length;
}

#endif
