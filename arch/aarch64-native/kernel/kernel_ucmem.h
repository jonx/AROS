/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <inttypes.h>

/* Uncached RAM block accessor (kernel_ucmem.c). The bootstrap reserves a
   block of RAM mapped Normal Non-Cacheable for non-coherent bus masters;
   kernel_startup records it here and DMA-capable drivers read it back. */
void krn_ucmem_set(uint64_t base, uint64_t size);
unsigned long long krn_ucmem_base(void);
unsigned long long krn_ucmem_size(void);
