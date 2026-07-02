/*
    Copyright (C) 1995-2025, The AROS Development Team. All rights reserved.

    Desc: console.device function CDInputHandler()
*/

#include <proto/exec.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <proto/console.h>
#include <proto/intuition.h>
#include <intuition/intuitionbase.h>

#include <aros/asmcall.h>

#include <devices/inputevent.h>

#include "console_gcc.h"

#define DEBUG 0
#include <aros/debug.h>

/* protos */
static Object *obtainconunit(struct ConsoleBase *ConsoleDevice);
static Object *obtainconunitbywindow(struct Window *win,
    struct ConsoleBase *ConsoleDevice);
static VOID releaseconunit(Object *o, struct ConsoleBase *ConsoleDevice);

/* Hand one event to the console task asynchronously: one message per
   event, freed by the task after processing. NEVER wait for the console
   task from an input handler. These handlers run in the input.device
   task, and the console task renders (RectFill obtains the window's
   layer lock). During an interactive size/drag intuition holds
   LockLayers() across many input events, so waiting for the console
   task while it waits for the layer lock deadlocks all input. Under
   memory pressure the event is dropped instead. */
static VOID cdih_sendevent(Object *unit, struct InputEvent *ie,
    struct ConsoleBase *ConsoleDevice)
{
    struct cdihMessage *message =
        AllocMem(sizeof(struct cdihMessage), MEMF_PUBLIC | MEMF_CLEAR);

    if (message)
    {
        message->msg.mn_Length = sizeof(struct cdihMessage);
        message->unit = unit;
        message->ie = *ie;
        PutMsg(ConsoleDevice->consIHData.inputPort,
            (struct Message *)message);
    }
}

/*************************************************************************

    NAME */
        AROS_LH2I(struct InputEvent *, CDInputHandler,

/*  SYNOPSIS */
        AROS_LHA(struct InputEvent *, events, A0),
        AROS_LHA(APTR, consoleDevice, A1),

/*  LOCATION */
        struct Library *, ConsoleDevice, 7, Console)

/*  FUNCTION

    INPUTS

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT
#undef ConsoleDevice
    struct ConsoleBase *ConsoleDevice = (struct ConsoleBase *)consoleDevice;

    struct InputEvent *ie;

    D(bug("CDInputHandler(events=%p)\n", events));

    for (ie = events; ie; ie = ie->ie_NextEvent)
    {
        /* A rawkey event ? */
        if ((ie->ie_Class == IECLASS_RAWKEY
                && !(ie->ie_Code & IECODE_UP_PREFIX))
            || (ie->ie_Class == IECLASS_SIZEWINDOW)
            || (ie->ie_Class == IECLASS_CLOSEWINDOW)
            || (ie->ie_Class == IECLASS_REFRESHWINDOW)
            || (ie->ie_Class == IECLASS_GADGETDOWN)
            || (ie->ie_Class == IECLASS_GADGETUP)
            || (ie->ie_Class == IECLASS_RAWMOUSE)
            || (ie->ie_Class == IECLASS_TIMER))
        {
            /* What console do we send it to ? */
            Object *unit;

            D(bug("Got some event\n"));
            /* find and prevent deletion of unit */
            unit = obtainconunit(ConsoleDevice);
            if (unit)
            {
                D(bug("Event should be passed to unit %p\n", unit));

                cdih_sendevent(unit, ie, ConsoleDevice);

                /* deletion of unit is now allowed */
                releaseconunit(unit, ConsoleDevice);

            } /* if (RAWKEY event was meant for a console window) */
        }
        else
        {
            D(bug("Ignoring event of ie_Class %d\n", ie->ie_Class));
        }
    } /* for (each event in the chain) */

    ReturnPtr("CDInputHandler", struct InputEvent *, events);

    AROS_LIBFUNC_EXIT
} /* CDInputHandler */

#undef IntuitionBase

/*
 * Window-event handler, added BELOW intuition (priority < 50).
 *
 * Intuition delivers window events for IDCMP-less windows (all
 * console-managed windows: con-handler opens them with IDCMP 0) by
 * GENERATING input events from its deferred-action handling
 * (ih_fire_intuimessage). Generated events are appended to the chain
 * intuition's own handler (priority 50) returns, so only handlers below
 * priority 50 ever see them. The priority-51 handler above can never
 * receive IECLASS_SIZEWINDOW / REFRESHWINDOW / CLOSEWINDOW /
 * GADGETDOWN / GADGETUP; without this handler the console never learns
 * about a resize and stops redrawing (stale layout after a size-gadget
 * drag).
 *
 * SIZEWINDOW / REFRESHWINDOW / CLOSEWINDOW carry the window in
 * ie_EventAddress, so the unit is matched by window (works for
 * inactive windows, and events from non-console windows simply match
 * nothing). GADGETDOWN / GADGETUP carry the gadget, so they fall back
 * to the active-window match.
 */
AROS_UFH2(struct InputEvent *, consoleWinEventHandler,
    AROS_UFHA(struct InputEvent *, events, A0),
    AROS_UFHA(struct ConsoleBase *, ConsoleDevice, A1))
{
    AROS_USERFUNC_INIT

    struct InputEvent *ie;

    for (ie = events; ie; ie = ie->ie_NextEvent)
    {
        Object *unit = NULL;

        switch (ie->ie_Class)
        {
        case IECLASS_SIZEWINDOW:
        case IECLASS_REFRESHWINDOW:
        case IECLASS_CLOSEWINDOW:
            unit = obtainconunitbywindow((struct Window *)ie->ie_EventAddress,
                ConsoleDevice);
            break;

        case IECLASS_GADGETDOWN:
        case IECLASS_GADGETUP:
            unit = obtainconunit(ConsoleDevice);
            break;
        }

        if (unit)
        {
            cdih_sendevent(unit, ie, ConsoleDevice);
            releaseconunit(unit, ConsoleDevice);
        }
    }

    ReturnPtr("consoleWinEventHandler", struct InputEvent *, events);

    AROS_USERFUNC_EXIT
}

/***********************
**  Support funtions  **
***********************/

/* Obtains a conunit object, and locks it, so that it's
   not deleted while we work on it
*/

static Object *obtainconunit(struct ConsoleBase *ConsoleDevice)
{
    struct IntuitionBase *IntuitionBase =
        (APTR) ConsoleDevice->cb_IntuitionBase;
    struct Window *activewin;
    Object *o, *ostate;
    ULONG lock;
    struct Node *node;

    D(bug("obtainconunit()\n"));

    ForeachNode(&ConsoleDevice->unitList, node)
    {
        D(bug("Node: %p\n", node));
    }

    /* Lock the console list */
    ObtainSemaphoreShared(&ConsoleDevice->unitListLock);

    /* What is the currently active window ? */
    D(bug("Obtaining IBase\n"));
    lock = LockIBase(0UL);

    activewin = IntuitionBase->ActiveWindow;

    UnlockIBase(lock);
    D(bug("Released IBase, active win=%p\n", activewin));

    /* Try to find the correct unit object for taht window */
    ostate = (Object *) ConsoleDevice->unitList.mlh_Head;

    D(bug("Searching for con unit\n"));
    while ((o = NextObject(&ostate)))
    {
        D(bug("Trying unit %p, win=%p\n", o, CU(o)->cu_Window));
        /* Is this console the currently active window ? */
        if (CU(o)->cu_Window == activewin)
        {
            D(bug("Unit found: %p\n", o));
            /* Delay deltion of this console object */
            ICU(o)->conFlags |= CF_DELAYEDDISPOSE;
            break;
        }
    }

    /* Unlock the console list */
    ReleaseSemaphore(&ConsoleDevice->unitListLock);

    ReturnPtr("obtainconunit", Object *, o);
}

/* Like obtainconunit, but matches the unit by its window instead of the
   currently active window. Returns NULL if the window is not console-managed. */
static Object *obtainconunitbywindow(struct Window *win,
    struct ConsoleBase *ConsoleDevice)
{
    struct IntuitionBase *IntuitionBase =
        (APTR) ConsoleDevice->cb_IntuitionBase;
    Object *o, *ostate;

    if (!win)
        return NULL;

    ObtainSemaphoreShared(&ConsoleDevice->unitListLock);

    ostate = (Object *) ConsoleDevice->unitList.mlh_Head;
    while ((o = NextObject(&ostate)))
    {
        if (CU(o)->cu_Window == win)
        {
            /* Delay deletion of this console object */
            ICU(o)->conFlags |= CF_DELAYEDDISPOSE;
            break;
        }
    }

    ReleaseSemaphore(&ConsoleDevice->unitListLock);

    ReturnPtr("obtainconunitbywindow", Object *, o);
}

static VOID releaseconunit(Object *o, struct ConsoleBase *ConsoleDevice)
{
    struct IntuitionBase *IntuitionBase =
        (APTR) ConsoleDevice->cb_IntuitionBase;

    /* Lock all units */
    ObtainSemaphore(&ConsoleDevice->unitListLock);

    /* Needn't prevent the unit from being disposed anymore */
    ICU(o)->conFlags &= ~CF_DELAYEDDISPOSE;

    /* If unit is scheduled for deletion, then delete it */
    if (ICU(o)->conFlags & CF_DISPOSE)
    {
        ULONG mID = OM_REMOVE;

        /* Remove from list */
        DoMethodA(o, (Msg) &mID);

        /* Delete it */
        DisposeObject(o);
    }

    ReleaseSemaphore(&ConsoleDevice->unitListLock);
}

/****************
** initCDIH()  **
****************/
/* This function should be executed on the console.device task's context only,
   so that the inputport is set correctly
*/

struct Interrupt *initCDIH(struct ConsoleBase *ConsoleDevice)
{
    struct Interrupt *cdihandler;
    struct cdihData *cdihdata;

    D(bug("initCDIH(ConsoleDevice=%p)\n", ConsoleDevice));

    cdihandler =
        AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC | MEMF_CLEAR);
    if (cdihandler)
    {
        cdihdata = &ConsoleDevice->consIHData;
        cdihdata->inputPort = CreateMsgPort();
        if (cdihdata->inputPort)
        {
            ConsoleDevice->winEventHandler =
                AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC | MEMF_CLEAR);
            if (ConsoleDevice->winEventHandler)
            {
                /* Initialize Interrupt structs. The rawkey handler goes
                   above intuition, the window-event handler below it
                   (see consoleWinEventHandler). */
                cdihandler->is_Code =
                    (VOID_FUNC) AROS_SLIB_ENTRY(CDInputHandler, Console, 7);
                cdihandler->is_Data = ConsoleDevice;
                cdihandler->is_Node.ln_Pri = 51;
                cdihandler->is_Node.ln_Name = "console.device InputHandler";

                ConsoleDevice->winEventHandler->is_Code =
                    (VOID_FUNC) AROS_ASMSYMNAME(consoleWinEventHandler);
                ConsoleDevice->winEventHandler->is_Data = ConsoleDevice;
                ConsoleDevice->winEventHandler->is_Node.ln_Pri = 0;
                ConsoleDevice->winEventHandler->is_Node.ln_Name =
                    "console.device WindowHandler";

                ReturnPtr("initCDIH", struct Interrupt *, cdihandler);
            }
            DeleteMsgPort(cdihdata->inputPort);
        }
        FreeMem(cdihandler, sizeof(struct Interrupt));
    }
    ReturnPtr("initCDIH", struct Interrupt *, NULL);
}

/****************
** CleanupIIH  **
****************/

VOID cleanupCDIH(struct Interrupt *cdihandler,
    struct ConsoleBase *ConsoleDevice)
{
    struct cdihData *cdihdata;

    cdihdata = &ConsoleDevice->consIHData;

    /* Drop any events still queued for the console task */
    {
        struct Message *msg;
        while ((msg = GetMsg(cdihdata->inputPort)))
            FreeMem(msg, sizeof(struct cdihMessage));
    }
    DeleteMsgPort(cdihdata->inputPort);

    FreeMem(ConsoleDevice->winEventHandler, sizeof(struct Interrupt));
    ConsoleDevice->winEventHandler = NULL;

    FreeMem(cdihandler, sizeof(struct Interrupt));

    return;
}
