/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Internal state of gpufx.library.
*/

#ifndef GPUFX_INTERN_H
#define GPUFX_INTERN_H

#include <exec/types.h>
#include <libraries/gpufx.h>

/* The name of the host dylib that exports the cm_gpu_* compute section. */
#define GPUFX_CM_DYLIB_NAME "cocoametal.dylib"
/* The cm_gpu_* contract version this library was built against (cocoametal.h
   CM_GPU_ABI). init refuses a mismatched dylib. */
#define GPUFX_CM_ABI 2

/*
    The cm_gpu_* subset of the shim, resolved by HostLib_GetInterface in the
    same order as gpufx_gpu_syms[]. cm_gpu_scale/convert take the shim's
    CmGpu*Req, which is byte-identical to GfxFx*Req, so we forward our public
    struct pointer straight through.
*/
struct GfxFxCMInterface
{
    int (*cm_gpu_open)(void);
    int (*cm_gpu_abi)(void);
    int (*cm_gpu_scale)(const struct GfxFxScaleReq *req);
    int (*cm_gpu_convert_yuv420)(const struct GfxFxYuvReq *req);
};

/* Bound once at library init; NULL when the GPU path is unavailable (no host,
   no dylib, ABI mismatch, or cm_gpu_open failed) -- callers then get the CPU
   fallback. The host dylib is process-global, so file statics are correct. */
extern APTR gfxfx_hostlib;
extern struct GfxFxCMInterface *gfxfx_gpu;

/* CPU reference conversions (gpufx_fallback.c) -- the always-correct baseline;
   the same formulas the shim's GPU kernels implement. */
void gfxfx_cpu_scale(const struct GfxFxScaleReq *req);
void gfxfx_cpu_yuv420(const struct GfxFxYuvReq *req);

#endif /* GPUFX_INTERN_H */
