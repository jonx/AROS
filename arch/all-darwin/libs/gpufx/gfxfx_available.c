/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <proto/exec.h>

#include "gpufx_intern.h"

/*****************************************************************************

    NAME */
        AROS_LH0(LONG, GfxFx_Available,

/*  LOCATION */
        struct Library *, GfxFxBase, 5, GfxFx)

/*  FUNCTION
        Returns non-zero when the GPU compute path is up (the shim's cm_gpu_*
        bound and opened). When it returns 0, GfxFx_Scale/ConvertYUV420 still
        work -- they run the CPU fallback -- so this is only a hint for callers
        that want to choose an entirely different strategy.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    return gfxfx_gpu != NULL ? 1 : 0;

    AROS_LIBFUNC_EXIT
}
