/* Pure exFAT metadata helpers, shared with the hosted corruption tests. */
#ifndef EXFAT_META_H
#define EXFAT_META_H

#define EXFAT_MAX_DIRECTORY_BYTES (256ULL * 1024 * 1024)

static inline ULONG exfat_meta_rotate32(ULONG sum, UBYTE byte)
{
    return ((sum & 1) ? 0x80000000UL : 0) + (sum >> 1) + byte;
}

static inline UWORD exfat_meta_rotate16(UWORD sum, UBYTE byte)
{
    return (UWORD)(((sum & 1) ? 0x8000U : 0) + (sum >> 1) + byte);
}

static inline UWORD exfat_entry_set_checksum(const UBYTE *set,
    UBYTE secondary_count)
{
    ULONG i;
    ULONG bytes = ((ULONG)secondary_count + 1) * 32;
    UWORD sum = 0;

    for (i = 0; i < bytes; i++)
        if (i != 2 && i != 3)
            sum = exfat_meta_rotate16(sum, set[i]);
    return sum;
}

static inline UWORD exfat_name_hash(const UWORD *upcase, const UWORD *name,
    UWORD length)
{
    UWORD hash = 0, i, c;

    for (i = 0; i < length; i++)
    {
        c = upcase[name[i]];
        hash = exfat_meta_rotate16(hash, (UBYTE)c);
        hash = exfat_meta_rotate16(hash, (UBYTE)(c >> 8));
    }
    return hash;
}

static inline int exfat_bitmap_address(ULONG cluster, ULONG cluster_count,
    ULONG *byte_index, UBYTE *mask)
{
    ULONG bit;

    if (cluster < 2
        || (UQUAD)cluster > (UQUAD)cluster_count + 1)
        return 0;
    bit = cluster - 2;
    *byte_index = bit >> 3;
    *mask = (UBYTE)(1U << (bit & 7));
    return 1;
}

static inline int exfat_bitmap_bit(const UBYTE *bitmap, ULONG cluster,
    ULONG cluster_count)
{
    ULONG byte_index;
    UBYTE mask;

    if (!exfat_bitmap_address(cluster, cluster_count, &byte_index, &mask))
        return -1;
    return (bitmap[byte_index] & mask) != 0;
}

static inline int exfat_bitmap_range_valid(ULONG first, ULONG count,
    ULONG cluster_count)
{
    if (count == 0 || first < 2)
        return 0;
    return (UQUAD)first + count <= (UQUAD)cluster_count + 2;
}

static inline int exfat_bitmap_range_is(const UBYTE *bitmap, ULONG first,
    ULONG count, ULONG cluster_count, int allocated)
{
    ULONG i;

    if (!exfat_bitmap_range_valid(first, count, cluster_count))
        return 0;
    for (i = 0; i < count; i++)
        if (exfat_bitmap_bit(bitmap, first + i, cluster_count) != allocated)
            return 0;
    return 1;
}

static inline void exfat_bitmap_set_range(UBYTE *bitmap, ULONG first,
    ULONG count, int allocated)
{
    ULONG i, bit;

    for (i = 0; i < count; i++)
    {
        bit = first + i - 2;
        if (allocated)
            bitmap[bit >> 3] |= (UBYTE)(1U << (bit & 7));
        else
            bitmap[bit >> 3] &= (UBYTE)~(1U << (bit & 7));
    }
}

/* Find a physically contiguous free run.  The hint may wrap the search, but
   a result never wraps the cluster heap boundary. */
static inline int exfat_bitmap_find_free_run(const UBYTE *bitmap,
    ULONG cluster_count, ULONG hint, ULONG needed, ULONG *first)
{
    ULONG begin, end, i, run;
    int pass;

    if (needed == 0 || needed > cluster_count)
        return 0;
    begin = hint >= 2 && (UQUAD)hint <= (UQUAD)cluster_count + 1
        ? hint - 2 : 0;

    for (pass = 0; pass < 2; pass++)
    {
        i = pass == 0 ? begin : 0;
        end = pass == 0 ? cluster_count : begin;
        run = 0;
        for (; i < end; i++)
        {
            if ((bitmap[i >> 3] & (1U << (i & 7))) == 0)
            {
                run++;
                if (run == needed)
                {
                    *first = i + 3 - needed;
                    return 1;
                }
            }
            else
                run = 0;
        }
        if (begin == 0)
            break;
    }
    return 0;
}

static inline UBYTE exfat_percent_in_use(ULONG free_clusters,
    ULONG cluster_count)
{
    UQUAD allocated;

    if (cluster_count == 0 || free_clusters > cluster_count)
        return 0xff;
    allocated = (UQUAD)cluster_count - free_clusters;
    return (UBYTE)((allocated * 100) / cluster_count);
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

/* Check directory growth without forming old_length + count * cluster_size.
   Besides enforcing the format's 256 MiB ceiling, this makes every operand
   boundary independently testable on 32-bit targets. */
static inline int exfat_directory_can_grow(UQUAD old_length,
    ULONG cluster_size, ULONG count)
{
    if (cluster_size == 0 || count == 0
        || old_length > EXFAT_MAX_DIRECTORY_BYTES)
        return 0;
    return (UQUAD)count
        <= (EXFAT_MAX_DIRECTORY_BYTES - old_length) / cluster_size;
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
