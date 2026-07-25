/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Uncached RAM block accessor. The bootstrap reserves a block of
          RAM and maps it Normal Non-Cacheable for bus masters that are
          not cache coherent (the BCM2711 PCIe bridge). The block is
          passed in the kernel boot taglist and read back by drivers
          through KrnGetSystemAttr.
*/

#include "kernel_ucmem.h"

static uint64_t ucmem_base, ucmem_size;

void krn_ucmem_set(uint64_t base, uint64_t size)
{
    ucmem_base = base;
    ucmem_size = size;
}

unsigned long long krn_ucmem_base(void) { return ucmem_base; }
unsigned long long krn_ucmem_size(void) { return ucmem_size; }
