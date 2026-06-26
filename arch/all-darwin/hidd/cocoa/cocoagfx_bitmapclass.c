/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CocoaBM -- displayable bitmap for the Cocoa/Metal display driver.

    Subclasses CLID_Hidd_ChunkyBM, so the base class owns the BGRA8 framebuffer
    and all drawing. UpdateRect only records damage; the poll task coalesces and
    presents at most once per tick, which avoids showing half-finished draw
    sequences during text selection/window dragging.
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

void cocoa_mark_dirty(OOP_Object *bm, WORD x, WORD y, WORD width, WORD height)
{
    WORD x2, y2;

    if ((bm != xsd.visible) || width <= 0 || height <= 0)
        return;

    x2 = x + width;
    y2 = y + height;

    Forbid();
    if (xsd.dirty)
    {
        if (x < xsd.dirty_x1) xsd.dirty_x1 = x;
        if (y < xsd.dirty_y1) xsd.dirty_y1 = y;
        if (x2 > xsd.dirty_x2) xsd.dirty_x2 = x2;
        if (y2 > xsd.dirty_y2) xsd.dirty_y2 = y2;
    }
    else
    {
        xsd.dirty = TRUE;
        xsd.dirty_x1 = x;
        xsd.dirty_y1 = y;
        xsd.dirty_x2 = x2;
        xsd.dirty_y2 = y2;
    }
    Permit();
}

void cocoa_present_visible(BOOL force)
{
    OOP_Object *bm;
    APTR buffer = NULL;
    IPTR bpr = 0, bw = COCOA_WIDTH, bh = COCOA_HEIGHT;
    WORD x1, y1, x2, y2;

    if (!xsd.visible || !xsd.ctx || !xsd.cm)
        return;

    Forbid();
    if (!force && !xsd.dirty)
    {
        Permit();
        return;
    }

    bm = xsd.visible;
    if (force)
    {
        x1 = 0;
        y1 = 0;
        x2 = COCOA_WIDTH;
        y2 = COCOA_HEIGHT;
        xsd.dirty = FALSE;
    }
    else
    {
        x1 = xsd.dirty_x1;
        y1 = xsd.dirty_y1;
        x2 = xsd.dirty_x2;
        y2 = xsd.dirty_y2;
        xsd.dirty = FALSE;
    }
    Permit();

    OOP_GetAttr(bm, aHidd_ChunkyBM_Buffer,    (IPTR *)&buffer);
    OOP_GetAttr(bm, aHidd_BitMap_BytesPerRow, &bpr);
    OOP_GetAttr(bm, aHidd_BitMap_Width,       &bw);
    OOP_GetAttr(bm, aHidd_BitMap_Height,      &bh);
    if (!buffer || bpr <= 0 || bw <= 0 || bh <= 0)
        return;

    if (force)
    {
        x1 = 0;
        y1 = 0;
        x2 = bw;
        y2 = bh;
    }
    else
    {
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > bw) x2 = bw;
        if (y2 > bh) y2 = bh;
    }

    if (x2 <= x1 || y2 <= y1)
        return;

    Forbid();
    HostLib_Lock();
    /* Upload the WHOLE framebuffer each present, not just the dirty bbox. This is
       REQUIRED by the host's N-buffered fbTex ring: consecutive frames land in
       different ring textures, so every upload must deliver a complete frame or
       the slots diverge. (It also sidesteps any dirty-rect coverage gap.) The bbox
       (x1,y1)-(x2,y2) is still computed above purely for the empty-damage
       early-out. If upload bandwidth ever matters, switch the host ring to
       copy-on-advance and restore a (x1,y1, x2-x1, y2-y1) upload here. */
    (void)x1; (void)y1; (void)x2; (void)y2;
    xsd.cm->cm_upload_rect(xsd.ctx, buffer, (int)bpr, 0, 0, (int)bw, (int)bh);
    xsd.cm->cm_present(xsd.ctx);
    AROS_HOST_BARRIER
    HostLib_Unlock();
    Permit();
}

VOID CocoaBM__Hidd_BitMap__UpdateRect(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_UpdateRect *msg)
{
    /* Let the chunky-BM base finish any pending composition first. */
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);

    {
        static ULONG nupd = 0;
        if (nupd++ < 4)
            D(bug("[Cocoa] UpdateRect #%lu o=0x%p visible=0x%p ctx=0x%p\n",
                  (unsigned long)nupd, o, xsd.visible, xsd.ctx));
    }

    cocoa_mark_dirty(o, msg->x, msg->y, msg->width, msg->height);
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
