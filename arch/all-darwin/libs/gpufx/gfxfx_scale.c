/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <proto/exec.h>
#include <proto/hostlib.h>

#include "gpufx_intern.h"

#define HostLibBase gfxfx_hostlib

/*****************************************************************************

    NAME */
        AROS_LH1(LONG, GfxFx_Scale,

/*  SYNOPSIS */
        AROS_LHA(const struct GfxFxScaleReq *, req, A0),

/*  LOCATION */
        struct Library *, GfxFxBase, 6, GfxFx)

/*  FUNCTION
        Scale a 4-bpp image (nearest or bilinear). GPU when available, else the
        CPU reference; either way the output is written and 0 is returned on
        success. Returns -1 only on a bad request (NULL / non-positive dims).

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    if (!req || !req->src || !req->dst
        || req->sw < 1 || req->sh < 1 || req->dw < 1 || req->dh < 1)
        return -1;

    if (gfxfx_gpu)
    {
        int rc = gfxfx_gpu->cm_gpu_scale(req);
        AROS_HOST_BARRIER
        if (rc == 0)
            return 0;
        /* GPU refused this frame -- fall through to the CPU path. */
    }

    gfxfx_cpu_scale(req);
    return 0;

    AROS_LIBFUNC_EXIT
}
