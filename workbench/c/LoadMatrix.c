/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: LoadMatrix -- batch OpenLibrary() smoke test for desktop bring-up.
*/

#include <proto/exec.h>
#include <proto/dos.h>

#include <exec/libraries.h>
#include <dos/dos.h>

struct MatrixEntry
{
    CONST_STRPTR name;
    ULONG        version;
};

static const struct MatrixEntry matrix[] =
{
    { "stdc.library",       0  },
    { "posixc.library",     0  },
    { "png.library",        0  },
    { "datatypes.library",  0  },
    { "icon.library",       0  },
    { "intuition.library",  0  },
    { "graphics.library",   0  },
    { "layers.library",     0  },
    { "asl.library",        0  },
    { "cybergraphics.library", 0 },
    { "muimaster.library",  0  },
    { "muiscreen.library",  0  },
    { "picture.datatype",   41 },
    { "png.datatype",       41 },
    { NULL,                 0  }
};

int main(void)
{
    LONG rc = RETURN_OK;
    ULONG pass = 0, fail = 0;
    const struct MatrixEntry *m;

    Printf("LoadMatrix: OpenLibrary desktop smoke test\n");

    for (m = matrix; m->name; m++)
    {
        struct Library *lib = OpenLibrary(m->name, m->version);

        if (lib)
        {
            Printf("OK   %-24s %ld.%ld\n",
                   (IPTR)m->name,
                   (LONG)lib->lib_Version,
                   (LONG)lib->lib_Revision);
            CloseLibrary(lib);
            pass++;
        }
        else
        {
            Printf("FAIL %-24s minver %lu\n",
                   (IPTR)m->name,
                   (IPTR)m->version);
            fail++;
            rc = RETURN_WARN;
        }
    }

    Printf("LoadMatrix: %lu OK, %lu FAIL\n", (IPTR)pass, (IPTR)fail);
    return rc;
}
