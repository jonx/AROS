/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: BevelProbe -- open a screen, draw one GadTools bevel box into it,
          close it again, all natively.

          This exists to answer one question: when a 68k program does this
          through the bridge and the host dies afterwards, is the bridge
          implicated at all? The same sequence run natively either dies the
          same way, and the bridge is exonerated, or it does not, and the
          difference is the crossing. Nothing else in the tree does the
          sequence without a 68k program in front of it.

          Bring-up diagnostic; see docs/features/debug-tools/README.md.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <intuition/screens.h>
#include <libraries/gadtools.h>

int main(void)
{
    struct Library *GadToolsBase;
    struct Screen *scr;
    APTR vi;

    GadToolsBase = OpenLibrary("gadtools.library", 0);
    if (!GadToolsBase)
    {
        Printf("[BEVELPROBE] no gadtools.library\n");
        return 20;
    }

    scr = OpenScreenTags(NULL, TAG_DONE);
    Printf("[BEVELPROBE] OpenScreenTags -> %s\n", (IPTR)(scr ? "ok" : "NULL"));
    if (!scr)
    {
        CloseLibrary(GadToolsBase);
        return 20;
    }

    vi = GetVisualInfoA(scr, NULL);
    Printf("[BEVELPROBE] GetVisualInfo -> %s\n", (IPTR)(vi ? "ok" : "NULL"));

    if (vi)
    {
        struct TagItem beveltags[] =
        {
            { GT_VisualInfo,  (IPTR)vi },
            { GTBB_Recessed,  1        },
            { GTBB_FrameType, 1        },
            { TAG_DONE,       0        }
        };

        Printf("[BEVELPROBE] drawing\n");
        DrawBevelBoxA(&scr->RastPort, 4, 4, 32, 12, beveltags);
        Printf("[BEVELPROBE] drawn\n");
        FreeVisualInfo(vi);
    }

    CloseScreen(scr);
    Printf("[BEVELPROBE] closed\n");
    CloseLibrary(GadToolsBase);
    Printf("[BEVELPROBE] PASS\n");
    return 0;
}
