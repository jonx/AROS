/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: LoadMatrix -- batch loader smoke test for desktop bring-up.
*/

#include <exec/io.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <devices/clipboard.h>
#include <devices/timer.h>
#include <exec/libraries.h>
#include <dos/dos.h>

#define LM_AHINAME    "ahi.device"
#define LM_AHI_NO_UNIT 255U

enum MatrixKind
{
    MATRIX_LIBRARY,
    MATRIX_DEVICE
};

struct AHIMatrixRequest
{
    struct IOStdReq ahir_Std;
    UWORD           ahir_Version;
    UWORD           ahir_Pad1;
    IPTR            ahir_Private[2];
    ULONG           ahir_Type;
    ULONG           ahir_Frequency;
    LONG            ahir_Volume;
    LONG            ahir_Position;
    APTR            ahir_Link;
};

struct MatrixEntry
{
    enum MatrixKind kind;
    CONST_STRPTR name;
    ULONG        version;
    LONG         unit;
    ULONG        reqsize;
    ULONG        ioversion;
};

static const struct MatrixEntry matrix[] =
{
    { MATRIX_LIBRARY, "stdc.library",       0, 0, 0, 0 },
    { MATRIX_LIBRARY, "posixc.library",     0, 0, 0, 0 },
    { MATRIX_LIBRARY, "png.library",        0, 0, 0, 0 },
    { MATRIX_LIBRARY, "datatypes.library",  0, 0, 0, 0 },
    { MATRIX_LIBRARY, "icon.library",       0, 0, 0, 0 },
    { MATRIX_LIBRARY, "intuition.library",  0, 0, 0, 0 },
    { MATRIX_LIBRARY, "graphics.library",   0, 0, 0, 0 },
    { MATRIX_LIBRARY, "layers.library",     0, 0, 0, 0 },
    { MATRIX_LIBRARY, "asl.library",        0, 0, 0, 0 },
    { MATRIX_LIBRARY, "cybergraphics.library", 0, 0, 0, 0 },
    { MATRIX_LIBRARY, "muimaster.library",  0, 0, 0, 0 },
    { MATRIX_LIBRARY, "muiscreen.library",  0, 0, 0, 0 },
    { MATRIX_LIBRARY, "SYS:Classes/datatypes/picture.datatype", 41, 0, 0, 0 },
    { MATRIX_LIBRARY, "SYS:Classes/datatypes/png.datatype", 41, 0, 0, 0 },
    { MATRIX_LIBRARY, "DEVS:AHI/coreaudio.audio", 6, 0, 0, 0 },
    { MATRIX_DEVICE,  TIMERNAME,            0, UNIT_VBLANK, sizeof(struct timerequest), 0 },
    { MATRIX_DEVICE,  LM_AHINAME,           0, LM_AHI_NO_UNIT, sizeof(struct AHIMatrixRequest), 4 },
    { MATRIX_DEVICE,  "input.device",       0, 0, sizeof(struct IOStdReq), 0 },
    { MATRIX_DEVICE,  "keyboard.device",    0, 0, sizeof(struct IOStdReq), 0 },
    { MATRIX_DEVICE,  "clipboard.device",   0, PRIMARY_CLIP, sizeof(struct IOClipReq), 0 },
    { 0, NULL, 0, 0, 0 }
};

static BOOL test_library(const struct MatrixEntry *m)
{
    struct Library *lib = OpenLibrary(m->name, m->version);

    if (lib)
    {
        Printf("OK   LIB %-20s %ld.%ld\n",
               (IPTR)m->name,
               (LONG)lib->lib_Version,
               (LONG)lib->lib_Revision);
        CloseLibrary(lib);
        return TRUE;
    }

    Printf("FAIL LIB %-20s minver %lu\n",
           (IPTR)m->name,
           (IPTR)m->version);
    return FALSE;
}

static BOOL test_device(const struct MatrixEntry *m)
{
    struct MsgPort *port;
    struct IORequest *io;
    BOOL ok = FALSE;

    port = CreateMsgPort();
    if (!port)
    {
        Printf("FAIL DEV %-20s no msgport\n", (IPTR)m->name);
        return FALSE;
    }

    io = CreateIORequest(port, m->reqsize);
    if (!io)
    {
        Printf("FAIL DEV %-20s no ioreq\n", (IPTR)m->name);
        DeleteMsgPort(port);
        return FALSE;
    }

    if (m->ioversion)
        ((struct AHIMatrixRequest *)io)->ahir_Version = m->ioversion;

    if (!OpenDevice(m->name, m->unit, io, 0))
    {
        Printf("OK   DEV %-20s unit %ld\n", (IPTR)m->name, (LONG)m->unit);
        CloseDevice(io);
        ok = TRUE;
    }
    else
    {
        Printf("FAIL DEV %-20s unit %ld ioerr %ld\n",
               (IPTR)m->name, (LONG)m->unit, (LONG)io->io_Error);
    }

    DeleteIORequest(io);
    DeleteMsgPort(port);
    return ok;
}

int main(void)
{
    LONG rc = RETURN_OK;
    ULONG pass = 0, fail = 0;
    const struct MatrixEntry *m;

    Printf("LoadMatrix: desktop loader smoke test\n");

    for (m = matrix; m->name; m++)
    {
        BOOL ok = (m->kind == MATRIX_DEVICE) ? test_device(m) : test_library(m);

        if (ok)
            pass++;
        else
        {
            fail++;
            rc = RETURN_WARN;
        }
    }

    Printf("LoadMatrix: %lu OK, %lu FAIL\n", (IPTR)pass, (IPTR)fail);
    return rc;
}
