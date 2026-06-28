/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: TestLib <library> [VER] -- try to OpenLibrary() a named library and
          report success + version, or failure.

          Unlike `Version`, which only reads the file off disk, this actually
          loads AND initialises the library through Exec/the loader. So it
          distinguishes "the file is present and readable" from "OpenLibrary()
          fails" -- the latter being an init / resident-registration / missing-
          dependency failure that Version cannot see. Pair it with the lddemon
          loader trace (set __lddemon_trace) to see *why* a load fails.

          Bring-up diagnostic; see docs/features/control-harness.
*/

#include <proto/exec.h>
#include <proto/dos.h>

#include <exec/libraries.h>
#include <dos/dos.h>

int main(void)
{
    struct RDArgs *rda;
    IPTR args[2] = { (IPTR)NULL, (IPTR)NULL };   /* LIB/A , VER/N */
    LONG rc = RETURN_FAIL;

    rda = ReadArgs("LIB/A,VER/N", args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "TestLib");
        return RETURN_FAIL;
    }

    {
        CONST_STRPTR name    = (CONST_STRPTR)args[0];
        ULONG        version = args[1] ? (ULONG)*(LONG *)args[1] : 0;
        struct Library *lib  = OpenLibrary(name, version);

        if (lib)
        {
            Printf("OK: \"%s\" opened -- version %ld.%ld\n",
                   (IPTR)name, (LONG)lib->lib_Version, (LONG)lib->lib_Revision);
            CloseLibrary(lib);
            rc = RETURN_OK;
        }
        else
        {
            Printf("FAIL: OpenLibrary(\"%s\", %lu) returned NULL.\n"
                   "  The file may exist (try `Version`) but its init / resident\n"
                   "  registration / a dependency failed. Check the lddemon trace.\n",
                   (IPTR)name, (IPTR)version);
            rc = RETURN_WARN;
        }
    }

    FreeArgs(rda);
    return rc;
}
