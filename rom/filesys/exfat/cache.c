/*
    Copyright (C) 2010-2022, The AROS Development Team. All rights reserved.

    Disk cache.
 */

/*
 * This is an LRU copyback cache.
 *
 * The cache consists of a fixed number of cache blocks, each of which can
 * hold a contiguous range of disk blocks (each range is of a fixed
 * length). Each range is addressed by its base block number, calculated by
 * applying a mask to the requested block number.
 *
 * Each cache block contains two Exec list nodes, so it can be part of two
 * lists simultaneously. The first node links a block into a hash table
 * list, while the second links it into either the free list or the dirty
 * list. Initially, all cache blocks are present in the free list, and are
 * absent from the hash table.
 *
 * When a disk block is requested by the client, the range that contains
 * that disk block is calculated. The range is then sought by looking up
 * the hash table. Each position in the hash table holds a list containing
 * all cache blocks (henceforth referred to as a block: individual disk
 * blocks are not used internally) that map to that hash location. Each
 * list in the hash table is typically very short, so look-up time is
 * quick.
 *
 * If the requested range is not in the hash table, a cache block is
 * removed from the head of the free list and from any hash table list it
 * is in, and used to read the appropriate range from disk. This block is
 * then added to the hash table at its new location.
 *
 * Two elements are returned to the client when a requested block is found:
 * an opaque cache block handle, and a pointer to the data buffer.
 *
 * When a range is freed, it remains in the hash table so that it can be
 * found quickly if needed again. Unless dirty, it is also added to the
 * tail of the free list through its second list node. The block then
 * remains in the hash table until reused for a different range.
 *
 * If a disk block is marked dirty by the client, the entire containing
 * range is marked dirty and added to the tail of the dirty list through
 * the cache block's second node. The dirty list is flushed periodically
 * (currently once per second), and additionally whenever the free list
 * becomes empty.
 *
 */

#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include "cache.h"
#include "exfat_cache_bits.h"

#define SysBase (c->sys_base)
#define DOSBase (c->dos_base)

#define RANGE_SHIFT 5
#define RANGE_SIZE (1 << RANGE_SHIFT)
#define RANGE_MASK (RANGE_SIZE - 1)

#define NODE2(A) \
   ((struct BlockRange *)(((A) != NULL) ? \
   (((BYTE *)(A)) - (IPTR)&((struct BlockRange *)NULL)->node2) : NULL))


APTR Cache_CreateCache(APTR priv, ULONG hash_size, ULONG block_count,
    ULONG block_size, struct ExecBase *sys_base, struct DosLibrary *dos_base)
{
    struct Cache _c, *c = &_c;
    ULONG i;
    BOOL success = TRUE;
    struct BlockRange *b;

    /* Allocate cache structure */

    c->sys_base = sys_base;
    if((c = AllocVec(sizeof(struct Cache), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
        success = FALSE;

    if(success)
    {
        c->priv = priv;
        c->sys_base = sys_base;
        c->dos_base = dos_base;
        c->block_size = block_size;
        c->block_count = block_count;
        c->hash_size = hash_size;

        /* Allocate hash table */

        c->hash_table = AllocVec(sizeof(struct MinList) * hash_size,
            MEMF_PUBLIC | MEMF_CLEAR);
        if(c->hash_table == NULL)
            success = FALSE;

        /* Initialise each hash location's list, and free and dirty lists */

        for(i = 0; i < hash_size && success; i++)
            NewList((struct List *)&c->hash_table[i]);
        NewList((struct List *)&c->free_list);
        NewList((struct List *)&c->dirty_list);

        /* Allocate cache blocks and add them to the free list */

        c->blocks = AllocVec(sizeof(APTR) * block_count,
            MEMF_PUBLIC | MEMF_CLEAR);
        if(c->blocks == NULL)
            success = FALSE;

        for(i = 0; i < block_count && success; i++)
        {
            b = AllocVec(sizeof(struct BlockRange)
                + (c->block_size << RANGE_SHIFT), MEMF_PUBLIC);

            if(b == NULL)
            {
                success = FALSE;
                break;
            }

            b->use_count = 0;
            b->state = BS_EMPTY;
            b->dirty_mask = 0;
            b->num = 0;
            b->data = (UBYTE *)b + sizeof(struct BlockRange);

            c->blocks[i] = b;
            AddTail((struct List *)&c->free_list, (struct Node *)&b->node2);
        }
    }

    if(!success)
    {
        Cache_DestroyCache(c);
        c = NULL;
    }

    return c;
}


static VOID Cache_FreeCache(APTR cache, BOOL flush)
{
    struct Cache *c = cache;
    ULONG i;

    if (flush)
        Cache_Flush(c);

    for(i = 0; i < c->block_count; i++)
        FreeVec(c->blocks[i]);
    FreeVec(c->blocks);
    FreeVec(c->hash_table);
    FreeVec(c);
}

VOID Cache_DestroyCache(APTR cache)
{
    Cache_FreeCache(cache, TRUE);
}

/* A removed medium must never receive cached writes after another disk has
   been inserted in the same device. */
VOID Cache_DiscardCache(APTR cache)
{
    Cache_FreeCache(cache, FALSE);
}


APTR Cache_GetBlock(APTR cache, UQUAD blockNum, UBYTE **data)
{
    struct Cache *c = cache;
    struct BlockRange *b = NULL, *b2;
    LONG error = 0, data_offset;
    /* Shift the UQUAD, not a truncated copy. The mask bounds the result, so
     * the distribution is unaffected by how large the block number gets. */
    struct MinList *l =
        &c->hash_table[(ULONG)((blockNum >> RANGE_SHIFT)
            & (UQUAD)(c->hash_size - 1))];
    struct MinNode *n;

    /* Change block number to the start block of a range and get byte offset
     * within range. The mask is widened explicitly: ~RANGE_MASK is an int,
     * and relying on its sign extension to clear the upper half of a UQUAD
     * is not something a reader should have to work out. */

    data_offset = (LONG)(blockNum & (UQUAD)RANGE_MASK) * c->block_size;
    blockNum &= ~(UQUAD)RANGE_MASK;

    /* Check existing valid blocks first */

    ForeachNode(l, b2)
    {
        if(b2->num == blockNum)
            b = b2;
    }

    if(b != NULL)
    {
        /* Block found, so increment its usage count and remove it from the
         * free list */

        if(b->use_count++ == 0)
        {
            if(b->state != BS_DIRTY)
                Remove((struct Node *)&b->node2);
        }
    }
    else
    {
        /* Get a free buffer to read block from disk */

        n = (struct MinNode *)RemHead((struct List *)&c->free_list);
        if(n == NULL)
        {
            /* No free blocks, so flush dirty list to try and free up some
             * more blocks, then try again */

            Cache_Flush(c);

            n = (struct MinNode *)RemHead((struct List *)&c->free_list);
        }

        if(n != NULL)
        {
            b = (struct BlockRange *)NODE2(n);

            /* Read the block from disk */

            if (b)
            {
                if(AccessDisk(FALSE, blockNum, RANGE_SIZE, c->block_size,
                    b->data, c->priv) == 0)
                {
                    /* Remove block from its old position in the hash */

                    if(b->state == BS_VALID)
                        Remove((struct Node *)b);

                    /* Add it to the hash at the new location */

                    AddHead((struct List *)l, (struct Node *)&b->node1);
                    b->num = blockNum;
                    b->state = BS_VALID;
                    b->dirty_mask = 0;
                    b->use_count = 1;
                }
                else
                {
                    /* Read failed, so put the block back on the free list */

                    b->state = BS_EMPTY;
                    b->dirty_mask = 0;
                    AddHead((struct List *)&c->free_list,
                        (struct Node *)&b->node2);
                    b = NULL;
                    error = ERROR_UNKNOWN;
                }
            }
            else
                error = ERROR_UNKNOWN;
        }
        else
            error = ERROR_NO_FREE_STORE;
    }

    /* Set data pointer and error, and return cache block handle */

    *data = b ? (b->data + data_offset) : NULL;
    SetIoErr(error);

    return b;
}


VOID Cache_FreeBlock(APTR cache, APTR block)
{
    struct Cache *c = cache;
    struct BlockRange *b = block;

    /* Decrement usage count */

    b->use_count--;

    /* Put an unused block at the end of the free list unless it's dirty */

    if(b->use_count == 0 && b->state != BS_DIRTY)
        AddTail((struct List *)&c->free_list, (struct Node *)&b->node2);

    return;
}


VOID Cache_MarkBlockDirty(APTR cache, APTR block)
{
    struct Cache *c = cache;
    struct BlockRange *b = block;

    if(b->state != BS_DIRTY)
    {
        b->state = BS_DIRTY;
        AddTail((struct List *)&c->dirty_list, (struct Node *)&b->node2);
    }

    b->dirty_mask = ~(ULONG)0;

    return;
}


BOOL Cache_MarkBlockDirtySector(APTR cache, APTR block, UQUAD block_num)
{
    struct Cache *c = cache;
    struct BlockRange *b = block;
    UQUAD offset;

    if (block_num < b->num)
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return FALSE;
    }
    offset = block_num - b->num;
    if (offset >= RANGE_SIZE)
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return FALSE;
    }

    if (b->state != BS_DIRTY)
    {
        b->state = BS_DIRTY;
        AddTail((struct List *)&c->dirty_list, (struct Node *)&b->node2);
    }
    b->dirty_mask |= (ULONG)1 << (ULONG)offset;
    SetIoErr(0);
    return TRUE;
}


BOOL Cache_IsClean(APTR cache)
{
    struct Cache *c = cache;

    return IsMinListEmpty(&c->dirty_list);
}


BOOL Cache_Flush(APTR cache)
{
    struct Cache *c = cache;
    LONG error = 0, td_error;
    struct MinNode *n;
    struct BlockRange *b;

    while((n = (struct MinNode *)RemHead((struct List *)&c->dirty_list))
        != NULL && error == 0)
    {
        /* Write dirty block range to disk */
        b = NODE2(n);
        if (b)
        {
            ULONG first, count;

            td_error = 0;
            while (b->dirty_mask != 0 && td_error == 0)
            {
                (void)exfat_dirty_span(b->dirty_mask, &first, &count);

                td_error = AccessDisk(TRUE, b->num + first, count,
                    c->block_size, b->data + first * c->block_size, c->priv);
                if (td_error == 0)
                    b->dirty_mask &= ~exfat_dirty_span_mask(first, count);
            }

            /* Transfer block range to free list if unused, or put back on dirty
             * list upon an error */

            if(td_error == 0)
            {
                b->state = BS_VALID;
                if(b->use_count == 0)
                    AddTail((struct List *)&c->free_list,
                        (struct Node *)&b->node2);
            }
            else
            {
                AddHead((struct List *)&c->dirty_list, (struct Node *)&b->node2);
                error = ERROR_UNKNOWN;
            }
        }
    }

    SetIoErr(error);
    return error == 0;
}
