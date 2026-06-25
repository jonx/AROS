/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CocoaBM -- displayable bitmap for the Cocoa/Metal display driver.

    Subclasses CLID_Hidd_ChunkyBM, so the base class owns the BGRA8 framebuffer
    and all drawing. We only override UpdateRect to present the dirty rectangle
    to the host window via cm_upload_rect + cm_present (INTERFACE.md §2a-2).
*/

#include <hidd/hidd.h>
#include <hidd/gfx.h>
#include <oop/oop.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <proto/hostlib.h>

#include "cocoa_intern.h"

#define DEBUG 1
#include <aros/debug.h>

#define HostLibBase (xsd.hostlib)

VOID CocoaBM__Hidd_BitMap__UpdateRect(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_UpdateRect *msg)
{
    /* Let the chunky-BM base finish any pending composition first. */
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);

    /* Present only the front bitmap, and only once the window is open. */
    if (o == xsd.visible && xsd.ctx && xsd.cm)
    {
        APTR  buffer = NULL;
        IPTR  bpr = 0;

        OOP_GetAttr(o, aHidd_ChunkyBM_Buffer,    (IPTR *)&buffer);
        OOP_GetAttr(o, aHidd_BitMap_BytesPerRow, &bpr);

        if (buffer)
        {
            HostLib_Lock();
            xsd.cm->cm_upload_rect(xsd.ctx, buffer, (int)bpr,
                                   msg->x, msg->y, msg->width, msg->height);
            xsd.cm->cm_present(xsd.ctx);
            AROS_HOST_BARRIER
            HostLib_Unlock();
        }
    }
}

static struct OOP_MethodDescr CocoaBM_Hidd_BitMap_descr[] =
{
    { (OOP_MethodFunc)CocoaBM__Hidd_BitMap__UpdateRect, moHidd_BitMap_UpdateRect },
    { NULL, 0 }
};
#define NUM_CocoaBM_Hidd_BitMap_METHODS 1

struct OOP_InterfaceDescr CocoaBM_ifdescr[] =
{
    { CocoaBM_Hidd_BitMap_descr, IID_Hidd_BitMap, NUM_CocoaBM_Hidd_BitMap_METHODS },
    { NULL, NULL, 0 }
};
