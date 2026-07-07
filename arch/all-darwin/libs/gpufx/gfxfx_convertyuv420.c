/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <proto/exec.h>
#include <proto/hostlib.h>

#include "gpufx_intern.h"

#define HostLibBase gfxfx_hostlib

/*****************************************************************************

    NAME */
        AROS_LH1(LONG, GfxFx_ConvertYUV420,

/*  SYNOPSIS */
        AROS_LHA(const struct GfxFxYuvReq *, req, A0),

/*  LOCATION */
        struct Library *, GfxFxBase, 7, GfxFx)

/*  FUNCTION
        Convert a planar YUV 4:2:0 frame to RGBA8888 (BT.601, limited or full
        range). GPU when available, else the CPU reference. Returns 0 on
        success, -1 on a bad request. This is the per-frame video hot path
        (libswscale's job).

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    if (!req || !req->y || !req->u || !req->v || !req->rgba
        || req->w < 1 || req->h < 1)
        return -1;

    if (gfxfx_gpu)
    {
        int rc = gfxfx_gpu->cm_gpu_convert_yuv420(req);
        AROS_HOST_BARRIER
        if (rc == 0)
            return 0;
        /* GPU refused this frame -- fall through to the CPU path. */
    }

    gfxfx_cpu_yuv420(req);
    return 0;

    AROS_LIBFUNC_EXIT
}
