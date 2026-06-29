/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CacheClearE() - Clear the caches with extended control, AArch64.
*/

#include <exec/execbase.h>
#include <exec/types.h>
#include <aros/libcall.h>
#include <proto/exec.h>

/*
 * AArch64 has separate data and instruction cache maintenance requirements for
 * newly generated or relocated executable code. On hosted Darwin this cannot be
 * performed with inline cache-maintenance instructions from AROS code; the host
 * traps them. The hosted executable-memory path therefore performs the real
 * instruction-cache invalidation in KrnSetProtection() when code pages are
 * flipped from writable to executable.
 */
AROS_LH3(void, CacheClearE,
    AROS_LHA(APTR, address, A0),
    AROS_LHA(IPTR, length, D0),
    AROS_LHA(ULONG, caches, D1),
    struct ExecBase *, SysBase, 107, Exec)
{
    AROS_LIBFUNC_INIT

    (void)address;
    (void)length;
    (void)caches;

    AROS_LIBFUNC_EXIT
} /* CacheClearE */
