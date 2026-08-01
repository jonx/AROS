/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: RunSeg - load an executable with LoadSeg() and run it as a new
          synchronous process, taking the created-process entry path rather
          than the shell's RunCommand() path.
*/

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dostags.h>

const TEXT version[] = "$VER: RunSeg 1.0 (1.8.2026)";

int main(void)
{
    IPTR args[1] = { 0 };
    struct RDArgs *rda;
    BPTR seg;
    struct Process *pr;

    rda = ReadArgs("FILE/A", args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "RunSeg");
        return RETURN_FAIL;
    }

    seg = LoadSeg((CONST_STRPTR)args[0]);
    if (!seg)
    {
        PrintFault(IoErr(), "RunSeg");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* The child owns and frees the seglist (NP_FreeSeglist defaults TRUE). */
    pr = CreateNewProcTags(NP_Seglist,     (IPTR)seg,
                           NP_Name,        (IPTR)"RunSeg target",
                           NP_Synchronous, (IPTR)TRUE,
                           TAG_DONE);
    if (!pr)
    {
        Printf("RunSeg: could not create the process\n");
        UnLoadSeg(seg);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    FreeArgs(rda);
    return RETURN_OK;
}
