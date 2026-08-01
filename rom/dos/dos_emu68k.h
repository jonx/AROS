#ifndef DOS_EMU68K_H
#define DOS_EMU68K_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: 68k hunk seglist launch routing (non-m68k targets)
*/

#include <libraries/emu68k.h>

/* Route a 68k hunk seglist launch. Returns DOSTRUE if the router ran the
 * program (its result in *result_out); DOSFALSE if it declined, in which
 * case the caller keeps its existing not-executable behavior. */
LONG Emu68k_RouteSegList(struct Emu68kLaunchCtx *ctx, LONG *result_out,
                         struct DosLibrary *DOSBase);

#endif /* DOS_EMU68K_H */
