/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: 68k hunk seglist launch router: completes the launch context with
          the stashed source image from the seg registry and hands it to
          emu68k.library. Without the library (or the image) it reports and
          declines, so callers fail cleanly exactly as before.
*/

#include <aros/debug.h>
#include <proto/exec.h>

#include "dos_intern.h"
#include "internalloadseg.h"
#include "dos_emu68k.h"

/* emu68k.library's one function, called without its proto headers (dos builds
 * before the library's includes exist). The slot is frozen by contract in
 * emu68k.conf: LVO 5 = the first function vector.
 *   LONG Emu68k_RunSeg(struct Emu68kLaunchCtx *ctx (A0), LONG *result (A1))
 * Returns DOSTRUE if the program ran (*result = its return code). */
#define EMU68K_LVO_RUNSEG 5

static LONG call_runseg(struct Library *Emu68kBase, struct Emu68kLaunchCtx *ctx,
                        LONG *result)
{
    return AROS_LVO_CALL2(LONG,
        AROS_LCA(struct Emu68kLaunchCtx *, ctx,    A0),
        AROS_LCA(LONG *,                  result, A1),
        struct Library *, Emu68kBase, EMU68K_LVO_RUNSEG, );
}

LONG Emu68k_RouteSegList(struct Emu68kLaunchCtx *ctx, LONG *result_out,
                         struct DosLibrary *DOSBase)
{
    struct Library *Emu68kBase;
    struct Node *segnode;
    LONG ran = DOSFALSE;

    if (!ctx || ctx->elc_Version < 1)
        return DOSFALSE;

    /* complete the context: the stashed source image rides the registry node */
    ctx->elc_Image = NULL;
    ctx->elc_ImageSize = 0;
    ObtainSemaphoreShared(&((struct IntDosBase *)DOSBase)->segsem);
    ForeachNode(&((struct IntDosBase *)DOSBase)->segdata, segnode)
    {
        if ((BPTR)segnode->ln_Name == ctx->elc_SegList &&
            segnode->ln_Type == SEGTYPE_HUNK)
        {
            struct hunk_segnode *hnode = (struct hunk_segnode *)segnode;
            ctx->elc_Image     = hnode->shn_Image;
            ctx->elc_ImageSize = hnode->shn_ImageSize;
            break;
        }
    }
    ReleaseSemaphore(&((struct IntDosBase *)DOSBase)->segsem);

    if (!ctx->elc_Image)
    {
        bug("[EMU68K] route v%lu origin=%lu name=\"%s\": no stashed image, declining\n",
            (unsigned long)ctx->elc_Version, (unsigned long)ctx->elc_Origin,
            ctx->elc_Name ? (const char *)ctx->elc_Name : "");
        return DOSFALSE;
    }

    /* NOTE: the image pointer is only valid while the seglist stays loaded;
     * both callers hold the seglist across this whole call. */
    Emu68kBase = OpenLibrary(EMU68KNAME, 0);
    if (!Emu68kBase)
    {
        bug("[EMU68K] route v%lu origin=%lu name=\"%s\": no %s, declining\n",
            (unsigned long)ctx->elc_Version, (unsigned long)ctx->elc_Origin,
            ctx->elc_Name ? (const char *)ctx->elc_Name : "", EMU68KNAME);
        return DOSFALSE;
    }

    ran = call_runseg(Emu68kBase, ctx, result_out);
    CloseLibrary(Emu68kBase);
    return ran;
}
