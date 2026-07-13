/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: __ieee754_tan shim for the ffmpeg.datatype module link.

    AROS stdc provides the long-double and tanh IEEE internals
    (__ieee754_tanl, __ieee754_tanh) but not the plain-double __ieee754_tan;
    libavutil pulls it in (init paths we never reach when decoding a single
    picture frame). A program link (FFView) resolves it from the fuller libm
    the C:-program rule pulls; a module link does not, so define it here in
    terms of the available double sin/cos. Correct for every finite x where
    cos(x) != 0, which is all this ever needs.
*/

#include <math.h>

double __ieee754_tan(double x)
{
    return sin(x) / cos(x);
}
