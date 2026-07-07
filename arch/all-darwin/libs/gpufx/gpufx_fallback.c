/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CPU reference conversions for gpufx.library -- the always-correct
          baseline used when the GPU path is unavailable or refuses a frame.
          The formulas match the shim's Metal kernels (cmshader.metal) and the
          host reference (hosted/cocoametal/gpu_test.c) op for op, so GPU and
          CPU output agree within rounding.
*/

#include <exec/types.h>

#include "gpufx_intern.h"

static inline UBYTE clamp255(float v)
{
    v += 0.5f;
    if (v < 0.0f)   return 0;
    if (v > 255.0f) return 255;
    return (UBYTE)v;
}

void gfxfx_cpu_scale(const struct GfxFxScaleReq *req)
{
    const UBYTE *src = (const UBYTE *)req->src;
    UBYTE       *dst = (UBYTE *)req->dst;
    LONG sw = req->sw, sh = req->sh, dw = req->dw, dh = req->dh;
    LONG ss = req->srcStride, ds = req->dstStride;
    LONG x, y, c;

    for (y = 0; y < dh; y++)
    {
        for (x = 0; x < dw; x++)
        {
            float u = (x + 0.5f) * (float)sw / (float)dw;
            float v = (y + 0.5f) * (float)sh / (float)dh;
            UBYTE *d = dst + (IPTR)y * ds + (IPTR)x * 4;

            if (req->filter == 0)
            {
                LONG ix = (LONG)u, iy = (LONG)v;
                if (ix > sw - 1) ix = sw - 1;
                if (iy > sh - 1) iy = sh - 1;
                const UBYTE *s = src + (IPTR)iy * ss + (IPTR)ix * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                continue;
            }

            {
                float fu = u - 0.5f, fv = v - 0.5f;
                LONG x0 = (LONG)(fu < 0 ? 0 : fu);
                LONG y0 = (LONG)(fv < 0 ? 0 : fv);
                LONG x1, y1;
                float ax, ay;
                if (x0 > sw - 1) x0 = sw - 1;
                if (y0 > sh - 1) y0 = sh - 1;
                x1 = (x0 + 1 < sw) ? x0 + 1 : sw - 1;
                y1 = (y0 + 1 < sh) ? y0 + 1 : sh - 1;
                ax = fu - (float)x0; if (ax < 0) ax = 0; if (ax > 1) ax = 1;
                ay = fv - (float)y0; if (ay < 0) ay = 0; if (ay > 1) ay = 1;
                for (c = 0; c < 4; c++)
                {
                    float s00 = src[(IPTR)y0 * ss + (IPTR)x0 * 4 + c];
                    float s10 = src[(IPTR)y0 * ss + (IPTR)x1 * 4 + c];
                    float s01 = src[(IPTR)y1 * ss + (IPTR)x0 * 4 + c];
                    float s11 = src[(IPTR)y1 * ss + (IPTR)x1 * 4 + c];
                    float top = s00 + (s10 - s00) * ax;
                    float bot = s01 + (s11 - s01) * ax;
                    d[c] = clamp255(top + (bot - top) * ay);
                }
            }
        }
    }
}

void gfxfx_cpu_yuv420(const struct GfxFxYuvReq *req)
{
    const UBYTE *yp = (const UBYTE *)req->y;
    const UBYTE *up = (const UBYTE *)req->u;
    const UBYTE *vp = (const UBYTE *)req->v;
    UBYTE       *dst = (UBYTE *)req->rgba;
    LONG w = req->w, h = req->h;
    LONG ys = req->yStride, us = req->uStride, vs = req->vStride, ds = req->dstStride;
    LONG x, y;

    for (y = 0; y < h; y++)
    {
        LONG crow = (y >> 1);
        for (x = 0; x < w; x++)
        {
            float Y = yp[(IPTR)y * ys + x];
            float U = up[(IPTR)crow * us + (x >> 1)] - 128.0f;
            float V = vp[(IPTR)crow * vs + (x >> 1)] - 128.0f;
            float r, g, b;
            UBYTE *d = dst + (IPTR)y * ds + (IPTR)x * 4;

            if (req->fullRange)
            {
                r = Y + 1.402000f * V;
                g = Y - 0.344136f * U - 0.714136f * V;
                b = Y + 1.772000f * U;
            }
            else
            {
                float C = 1.164383f * (Y - 16.0f);
                r = C + 1.596027f * V;
                g = C - 0.391762f * U - 0.812968f * V;
                b = C + 2.017232f * U;
            }
            d[0] = clamp255(r);
            d[1] = clamp255(g);
            d[2] = clamp255(b);
            d[3] = 255;
        }
    }
}
