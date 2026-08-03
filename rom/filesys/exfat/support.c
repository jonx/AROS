/*
 * exfat-handler - support routines
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/dos.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/alib.h>      /* AROS_SLOWSTACKFORMAT_* */

#include <stdarg.h>

#include "exfat_fs.h"
#include "exfat_protos.h"

#define DEBUG DEBUG_MISC
#include "exfat_debug.h"

LONG ErrorMessageArgs(struct Globals *glob, char *options,
    CONST_STRPTR format, ...)
{
    struct IntuitionBase *IntuitionBase;
    LONG answer = 0;

    AROS_SLOWSTACKFORMAT_PRE(format);

    IntuitionBase =
        (struct IntuitionBase *)TaggedOpenLibrary(TAGGEDOPEN_INTUITION);
    if (IntuitionBase)
    {
        struct EasyStruct es =
        {
            sizeof(struct EasyStruct),
            0,
            "exFAT filesystem",
            NULL,
            options
        };

        es.es_TextFormat = format;
        answer = EasyRequestArgs(NULL, &es, NULL,
            AROS_SLOWSTACKFORMAT_ARG(format));
        CloseLibrary((struct Library *)IntuitionBase);
    }

    AROS_SLOWSTACKFORMAT_POST(format);

    return answer;
}
