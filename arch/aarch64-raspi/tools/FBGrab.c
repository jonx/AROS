/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Write the contents of the display to a file.
*/

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <aros/kernel.h>

/* kernel.resource is a resource, so open it explicitly. */
#define __NOLIBBASE__
#include <proto/kernel.h>

#define DEFAULT_NAME "SYS:screenshot.bmp"

static void put32(UBYTE *p, ULONG v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static void put16(UBYTE *p, UWORD v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

int main(int argc, char **argv)
{
    struct KernelBase *KernelBase;
    const char *name = (argc > 1) ? argv[1] : DEFAULT_NAME;
    UBYTE *fb, *row;
    ULONG width, height, pitch, bpp;
    ULONG y, x, rowbytes;
    UBYTE header[54];
    BPTR file;

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
    {
        PutStr("no kernel.resource\n");
        return RETURN_FAIL;
    }

    fb     = (UBYTE *)KrnGetSystemAttr(KATTR_FrameBuffer);
    width  = KrnGetSystemAttr(KATTR_FrameBufferWidth);
    height = KrnGetSystemAttr(KATTR_FrameBufferHeight);
    bpp    = KrnGetSystemAttr(KATTR_FrameBufferDepth);
    pitch  = KrnGetSystemAttr(KATTR_FrameBufferPitch);

    if (!fb || !width || !height)
    {
        PutStr("no framebuffer\n");
        return RETURN_FAIL;
    }

    if (bpp != 32)
    {
        Printf("unsupported depth %lu\n", (unsigned long)bpp);
        return RETURN_FAIL;
    }

    rowbytes = width * 4;

    row = AllocMem(rowbytes, MEMF_ANY);
    if (!row)
    {
        PutStr("out of memory\n");
        return RETURN_FAIL;
    }

    file = Open((STRPTR)name, MODE_NEWFILE);
    if (!file)
    {
        Printf("cannot open %s\n", (IPTR)name);
        FreeMem(row, rowbytes);
        return RETURN_FAIL;
    }

    /* Uncompressed 32-bit BMP, stored top-down via a negative height. */
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put32(&header[2], sizeof(header) + height * rowbytes);
    put32(&header[10], sizeof(header));
    put32(&header[14], 40);
    put32(&header[18], width);
    put32(&header[22], (ULONG)(-(LONG)height));
    put16(&header[26], 1);
    put16(&header[28], 32);
    put32(&header[34], height * rowbytes);

    Write(file, header, sizeof(header));

    for (y = 0; y < height; y++)
    {
        UBYTE *src = fb + y * pitch;

        /* The surface holds red first; a BMP wants blue first. */
        for (x = 0; x < width; x++)
        {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = 0xff;
        }

        Write(file, row, rowbytes);
    }

    Close(file);
    FreeMem(row, rowbytes);

    Printf("wrote %s (%lux%lu)\n", (IPTR)name,
           (unsigned long)width, (unsigned long)height);

    return RETURN_OK;
}
