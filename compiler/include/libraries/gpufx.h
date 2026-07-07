#ifndef LIBRARIES_GPUFX_H
#define LIBRARIES_GPUFX_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Public interface of gpufx.library -- explicit GPU-accelerated 2D
          (scale, YUV->RGB) on the hosted (darwin/cocoa) port. The library is
          the AROS-native front door to the cocoametal display shim's GPU
          compute section (cm_gpu_*); every op falls back to a CPU reference
          when the GPU path is unavailable, so a call always succeeds and
          software stays the baseline.
*/

#include <exec/types.h>

/*
    The request structs mirror the shim's cm_gpu_* ABI (v2) field-for-field,
    so the library forwards the pointer to the host with no translation. Pass
    a struct by pointer (not a long argument list) on purpose: an AROS->host
    call misreads arguments past the 8th register slot, so one pointer arg is
    the safe shape (see docs/features/host-bridge on aros-aarch64).

    Buffers are caller-owned CPU memory, 4 bytes/pixel for RGBA; strides are in
    bytes. `filter`: 0 = nearest, 1 = bilinear. `fullRange`: 0 = BT.601 limited
    (video) range, 1 = full range.
*/

struct GfxFxScaleReq
{
    CONST_APTR  src;
    APTR        dst;
    LONG        srcStride, sw, sh;
    LONG        dstStride, dw, dh;
    LONG        filter;
};

struct GfxFxYuvReq
{
    CONST_APTR  y, u, v;
    APTR        rgba;
    LONG        yStride, uStride, vStride;
    LONG        w, h;
    LONG        dstStride;
    LONG        fullRange;
};

#endif /* LIBRARIES_GPUFX_H */
