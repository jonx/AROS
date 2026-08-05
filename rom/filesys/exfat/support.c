/*
 * exfat-handler - support routines
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#include <exec/types.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>
#include <libraries/dos.h>
#include <libraries/locale.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <proto/alib.h>      /* AROS_SLOWSTACKFORMAT_* */

#include <stdarg.h>

#include "exfat_fs.h"
#include "exfat_protos.h"
#include "exfat_time.h"

#define __LOCALE_LIBBASE (glob->gl_LocaleBase)
#include <proto/locale.h>

#define DEBUG DEBUG_MISC
#include "exfat_debug.h"

void ExfatSendEvent(LONG event, struct Globals *glob)
{
    struct MsgPort *port;
    struct IOStdReq *request;
    struct InputEvent *ie;

    (void)glob;
    port = CreateMsgPort();
    if (port == NULL)
        return;
    request = (struct IOStdReq *)CreateIORequest(port,
        sizeof(struct IOStdReq));
    if (request != NULL)
    {
        if (OpenDevice("input.device", 0, (struct IORequest *)request, 0) == 0)
        {
            ie = AllocVec(sizeof(struct InputEvent), MEMF_PUBLIC | MEMF_CLEAR);
            if (ie != NULL)
            {
                ie->ie_Class = event;
                request->io_Command = IND_WRITEEVENT;
                request->io_Data = ie;
                request->io_Length = sizeof(struct InputEvent);
                DoIO((struct IORequest *)request);
                FreeVec(ie);
            }
            CloseDevice((struct IORequest *)request);
        }
        DeleteIORequest((struct IORequest *)request);
    }
    DeleteMsgPort(port);
}

BOOL ExfatGetGMTOffset(struct Globals *glob, LONG *minutes)
{
    struct Locale *locale;

    if (minutes == NULL || glob->gl_LocaleBase == NULL)
        return FALSE;
    locale = OpenLocale(NULL);
    if (locale == NULL)
        return FALSE;
    *minutes = locale->loc_GMTOffset;
    CloseLocale(locale);
    return TRUE;
}

LONG ExfatPackDateStamp(struct Globals *glob, const struct DateStamp *date,
    ULONG *packed, UBYTE *ten_ms, UBYTE *utc)
{
    LONG days, minute, gmt, adjustment = 0;

    if (date == NULL || packed == NULL || ten_ms == NULL || utc == NULL)
        return ERROR_BAD_NUMBER;
    days = date->ds_Days;
    minute = date->ds_Minute;
    if (ExfatGetGMTOffset(glob, &gmt))
    {
        *utc = exfat_time_encode_utc(gmt, &adjustment);
        if (!exfat_time_adjust_minutes(&days, &minute, adjustment))
            return ERROR_BAD_NUMBER;
    }
    else
        *utc = 0; /* UTC unavailable: OffsetValid must be clear. */
    return exfat_time_pack(days, minute, date->ds_Tick, TICKS_PER_SECOND,
        packed, ten_ms) ? 0 : ERROR_BAD_NUMBER;
}

void ExfatCurrentTimestamp(struct Globals *glob, ULONG *packed,
    UBYTE *ten_ms, UBYTE *utc)
{
    struct DateStamp now;

    DateStamp(&now);
    if (ExfatPackDateStamp(glob, &now, packed, ten_ms, utc) != 0)
    {
        /* Machines without a valid real-time clock commonly start at the
           Amiga epoch (1978), while exFAT begins at 1980.  Creation must not
           fail merely because the clock is unset. */
        *packed = (1UL << 21) | (1UL << 16); /* 1980-01-01 00:00:00 */
        *ten_ms = 0;
        *utc = 0;
    }
}

BOOL ExfatUnpackDateStamp(struct Globals *glob, ULONG packed, UBYTE ten_ms,
    UBYTE utc, struct DateStamp *date)
{
    LONG gmt, adjustment;

    if (date == NULL || !exfat_time_unpack(packed, ten_ms,
            TICKS_PER_SECOND, &date->ds_Days, &date->ds_Minute,
            &date->ds_Tick))
        return FALSE;
    if (ExfatGetGMTOffset(glob, &gmt)
        && exfat_time_display_adjustment(utc, gmt, &adjustment)
        && !exfat_time_adjust_minutes(&date->ds_Days, &date->ds_Minute,
            adjustment))
        return FALSE;
    return TRUE;
}

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
