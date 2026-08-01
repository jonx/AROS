/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: 68k hunk seglist launch router. Current stage: receives the launch
          context from both execution boundaries, reports it, and declines,
          so callers fail cleanly exactly as before. The execution engine
          attaches here.
*/

#include <aros/debug.h>

#include "dos_intern.h"
#include "dos_emu68k.h"

LONG Emu68k_RouteSegList(struct Emu68kLaunchCtx *ctx, LONG *result_out,
                         struct DosLibrary *DOSBase)
{
    (void)result_out;
    (void)DOSBase;

    if (!ctx || ctx->elc_Version < 1)
        return DOSFALSE;

    bug("[EMU68K] route v%lu origin=%s seglist=%p name=\"%s\" argsize=%lu args=\"%.*s\" proc=%p mode=%lu (no engine: declining)\n",
        (unsigned long)ctx->elc_Version,
        ctx->elc_Origin == EMU68K_LAUNCH_CLI ? "CLI" :
        ctx->elc_Origin == EMU68K_LAUNCH_PROC ? "PROC" : "?",
        BADDR(ctx->elc_SegList),
        ctx->elc_Name ? (const char *)ctx->elc_Name : "",
        (unsigned long)ctx->elc_ArgSize,
        ctx->elc_Args ? (int)ctx->elc_ArgSize : 0,
        ctx->elc_Args ? (const char *)ctx->elc_Args : "",
        ctx->elc_Process,
        (unsigned long)ctx->elc_Mode);

    return DOSFALSE;
}
