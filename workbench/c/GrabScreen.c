/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: GrabScreen <file> -- dump the frontmost public screen to a binary PPM
          (P6) file. Used to make headless screenshots of the AROS display for
          inspection/recording/testing (works with any display driver: the
          headless gfx driver, and later the Cocoa/Metal window).

          PPM is trivial to convert host-side: sips / ImageMagick / a few lines
          of Python turn it into PNG.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/view.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "RAM:shot.ppm";

    /* Make sure there is something to grab: bring up the Workbench screen if no
     * public screen is open yet. OpenWorkBench() just opens the screen (it does
     * not need Wanderer/icon.library), so this works even on a bare GUI boot. */
    struct Screen *scr = LockPubScreen(NULL);
    if (!scr)
    {
        OpenWorkBench();
        Delay(50);   /* ~1s for the screen to come up and render */
        scr = LockPubScreen(NULL);
    }
    if (!scr)
    {
        Printf("GrabScreen: no public screen open (OpenWorkBench failed)\n");
        return RETURN_FAIL;
    }

    LONG w = scr->Width;
    LONG h = scr->Height;
    struct RastPort *rp = &scr->RastPort;
    struct ColorMap *cm = scr->ViewPort.ColorMap;
    UWORD depth = (rp->BitMap) ? GetBitMapAttr(rp->BitMap, BMA_DEPTH) : 8;

    Printf("GrabScreen: screen %ldx%ld depth %ld -> %s\n", w, h, (LONG)depth, (STRPTR)path);

    /* Palette for CLUT screens (depth <= 8). GetRGB32 writes count*3 ULONGs,
     * each colour component left-justified in 32 bits. */
    ULONG ncol = (depth <= 8) ? (1UL << depth) : 0;
    ULONG *clut = NULL;
    if (ncol)
    {
        clut = AllocVec(ncol * 3 * sizeof(ULONG), MEMF_ANY);
        if (clut && cm)
            GetRGB32(cm, 0, ncol, clut);
    }

    UBYTE *row = AllocVec(w * 3, MEMF_ANY);
    if (!row)
    {
        if (clut) FreeVec(clut);
        UnlockPubScreen(NULL, scr);
        Printf("GrabScreen: out of memory\n");
        return RETURN_FAIL;
    }

    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    if (!f)
    {
        FreeVec(row);
        if (clut) FreeVec(clut);
        UnlockPubScreen(NULL, scr);
        Printf("GrabScreen: cannot open %s\n", (STRPTR)path);
        return RETURN_FAIL;
    }

    /* PPM header */
    {
        char hdr[64];
        int n = snprintf(hdr, sizeof hdr, "P6\n%ld %ld\n255\n", w, h);
        Write(f, hdr, n);
    }

    /* Read a whole row of pen values at once via ReadPixelArray8 (vastly faster
     * and safer than per-pixel ReadPixel). It needs a scratch single-row
     * rastport. */
    UBYTE *pens = AllocVec(w + 16, MEMF_ANY | MEMF_CLEAR);
    struct RastPort temprp;
    struct BitMap *tbm = AllocBitMap(w, 1, 8, BMF_CLEAR, rp->BitMap);
    if (pens && tbm)
    {
        InitRastPort(&temprp);
        temprp.BitMap = tbm;

        for (LONG y = 0; y < h; y++)
        {
            ReadPixelArray8(rp, 0, y, w - 1, y, pens, &temprp);
            UBYTE *p = row;
            for (LONG x = 0; x < w; x++)
            {
                ULONG pix = pens[x];
                UBYTE r, g, b;
                if (clut && pix < ncol)
                {
                    r = clut[pix * 3 + 0] >> 24;
                    g = clut[pix * 3 + 1] >> 24;
                    b = clut[pix * 3 + 2] >> 24;
                }
                else
                {
                    r = (pix >> 16) & 0xFF;
                    g = (pix >> 8) & 0xFF;
                    b = pix & 0xFF;
                }
                *p++ = r; *p++ = g; *p++ = b;
            }
            Write(f, row, w * 3);
        }
    }
    if (tbm) FreeBitMap(tbm);
    if (pens) FreeVec(pens);

    Close(f);
    FreeVec(row);
    if (clut) FreeVec(clut);
    UnlockPubScreen(NULL, scr);

    Printf("GrabScreen: done\n");
    return RETURN_OK;
}
