/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CocoaGfx -- gfx HIDD class for the Cocoa/Metal display driver.

    Mirrors the SDL hosted-driver structure (a program creating CLID_HiddMeta
    classes), but the host backend is cocoametal.dylib driven through the cm_*
    ABI. One fixed logical mode in vHidd_StdPixFmt_BGRA32 (blue lowest byte ==
    the host CMPixelDesc, no swizzle). The window is opened lazily on the first
    Show (INTERFACE.md §2a-2).
*/

#include <hidd/hidd.h>
#include <hidd/gfx.h>
#include <utility/tagitem.h>
#include <oop/oop.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <proto/hostlib.h>

#include "cocoa_intern.h"

#define DEBUG 1
#include <aros/debug.h>

#define LIBBASE (&xsd)
#define HostLibBase (xsd.hostlib)

/* The exact BGRA8 layout both sides agree on (INTERFACE.md §2a-1). */
static const struct CMPixelDesc cocoa_fmt =
{
    .bytesPerPixel = 4,
    .redShift = 16, .greenShift = 8, .blueShift = 0, .alphaShift = 24,
    .redMask = 0x00FF0000, .greenMask = 0x0000FF00, .blueMask = 0x000000FF, .alphaMask = 0xFF000000,
};

OOP_Object *CocoaGfx__Root__New(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    /* AROS-side BGRA32 pixel format -- the canonical values from
       rom/hidds/gfx/stdpixfmts_le.h (B8G8R8A8). */
    struct TagItem pftags[] =
    {
        { aHidd_PixFmt_ColorModel,   vHidd_ColorModel_TrueColor },
        { aHidd_PixFmt_RedShift,     8                          },
        { aHidd_PixFmt_GreenShift,   16                         },
        { aHidd_PixFmt_BlueShift,    24                         },
        { aHidd_PixFmt_AlphaShift,   0                          },
        { aHidd_PixFmt_RedMask,      0x00FF0000                 },
        { aHidd_PixFmt_GreenMask,    0x0000FF00                 },
        { aHidd_PixFmt_BlueMask,     0x000000FF                 },
        { aHidd_PixFmt_AlphaMask,    0xFF000000                 },
        { aHidd_PixFmt_Depth,        24                         },
        { aHidd_PixFmt_BitsPerPixel, 32                         },
        { aHidd_PixFmt_BytesPerPixel,4                          },
        { aHidd_PixFmt_StdPixFmt,    vHidd_StdPixFmt_BGRA32     },
        { aHidd_PixFmt_BitMapType,   vHidd_BitMapType_Chunky    },
        { TAG_DONE,                  0                          }
    };
    struct TagItem sync_mode[] =
    {
        { aHidd_Sync_PixelClock, 60UL * COCOA_WIDTH * COCOA_HEIGHT },
        { aHidd_Sync_HDisp,      COCOA_WIDTH                       },
        { aHidd_Sync_VDisp,      COCOA_HEIGHT                      },
        { aHidd_Sync_HTotal,     COCOA_WIDTH                       },
        { aHidd_Sync_VTotal,     COCOA_HEIGHT                      },
        { aHidd_Sync_Description,(IPTR)"Cocoa:%hx%v"               },
        { TAG_DONE,              0                                 }
    };
    struct TagItem modetags[] =
    {
        { aHidd_Gfx_PixFmtTags, (IPTR)pftags    },
        { aHidd_Gfx_SyncTags,   (IPTR)sync_mode },
        { TAG_DONE,             0               }
    };
    struct TagItem msgtags[] =
    {
        { aHidd_Gfx_ModeTags, (IPTR)modetags                       },
        { aHidd_Name,         (IPTR)"cocoa.hidd"                   },
        { aHidd_HardwareName, (IPTR)"Cocoa/Metal Display (macOS)"  },
        { aHidd_ProducerName, (IPTR)"AROS darwin-aarch64 graft"    },
        { TAG_MORE,           (IPTR)msg->attrList                  }
    };
    struct pRoot_New supermsg;

    EnterFunc(bug("[Cocoa] CocoaGfx::New\n"));

    if (xsd.gfxhidd)
        return NULL;                 /* singletone */

    supermsg.mID = msg->mID;
    supermsg.attrList = msgtags;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&supermsg);
    if (o)
        xsd.gfxhidd = o;

    ReturnPtr("[Cocoa] CocoaGfx::New", OOP_Object *, o);
}

VOID CocoaGfx__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    ULONG idx;

    if (IS_GFX_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_Gfx_IsWindowed:
            *msg->storage = TRUE;
            return;
        case aoHidd_Gfx_DriverName:
            *msg->storage = (IPTR)"Cocoa";
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/* Hand the base class our bitmap class for displayable bitmaps (those with a
   valid ModeID); everything else falls through to the chunky-BM base. */
OOP_Object *CocoaGfx__Hidd_Gfx__CreateObject(OOP_Class *cl, OOP_Object *o, struct pHidd_Gfx_CreateObject *msg)
{
    OOP_Object *object;

    if (msg->cl == xsd.basebm)
    {
        struct TagItem tags[2] =
        {
            { TAG_IGNORE, 0                   },
            { TAG_MORE,   (IPTR)msg->attrList }
        };
        struct pHidd_Gfx_CreateObject p;

        if (GetTagData(aHidd_BitMap_Displayable, FALSE, msg->attrList))
        {
            tags[0].ti_Tag  = aHidd_BitMap_ClassPtr;
            tags[0].ti_Data = (IPTR)xsd.bmclass;
            D(bug("[Cocoa] CreateObject: displayable -> CocoaBM\n"));
        }
        else
        {
            tags[0].ti_Tag  = aHidd_BitMap_ClassID;
            tags[0].ti_Data = (IPTR)CLID_Hidd_ChunkyBM;
        }

        p.mID = msg->mID;
        p.cl  = msg->cl;
        p.attrList = tags;

        object = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&p);
    }
    else
        object = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);

    return object;
}

/* Lazy window open on first Show; record the front bitmap for the present hook. */
OOP_Object *CocoaGfx__Hidd_Gfx__Show(OOP_Class *cl, OOP_Object *o, struct pHidd_Gfx_Show *msg)
{
    struct pHidd_Gfx_Show mymsg = { msg->mID, msg->bitMap, msg->flags };

    D(bug("[Cocoa] CocoaGfx::Show(0x%p)\n", msg->bitMap));

    if (msg->bitMap)
    {
        if (!xsd.ctx && xsd.cm)
        {
            IPTR w = COCOA_WIDTH, h = COCOA_HEIGHT;
            OOP_GetAttr(msg->bitMap, aHidd_BitMap_Width,  &w);
            OOP_GetAttr(msg->bitMap, aHidd_BitMap_Height, &h);

            HostLib_Lock();
            xsd.ctx = xsd.cm->cm_open((int)w, (int)h, &cocoa_fmt, "AROS");
            AROS_HOST_BARRIER
            HostLib_Unlock();
            D(bug("[Cocoa] cm_open(%ld,%ld) -> 0x%p\n", w, h, xsd.ctx));
        }
        xsd.visible = msg->bitMap;
    }
    else
        xsd.visible = NULL;

    return (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&mymsg);
}

static struct OOP_MethodDescr CocoaGfx_Root_descr[] =
{
    { (OOP_MethodFunc)CocoaGfx__Root__New, moRoot_New },
    { (OOP_MethodFunc)CocoaGfx__Root__Get, moRoot_Get },
    { NULL, 0 }
};
#define NUM_CocoaGfx_Root_METHODS 2

static struct OOP_MethodDescr CocoaGfx_Hidd_Gfx_descr[] =
{
    { (OOP_MethodFunc)CocoaGfx__Hidd_Gfx__CreateObject, moHidd_Gfx_CreateObject },
    { (OOP_MethodFunc)CocoaGfx__Hidd_Gfx__Show,         moHidd_Gfx_Show         },
    { NULL, 0 }
};
#define NUM_CocoaGfx_Hidd_Gfx_METHODS 2

struct OOP_InterfaceDescr CocoaGfx_ifdescr[] =
{
    { CocoaGfx_Root_descr,     IID_Root,     NUM_CocoaGfx_Root_METHODS     },
    { CocoaGfx_Hidd_Gfx_descr, IID_Hidd_Gfx, NUM_CocoaGfx_Hidd_Gfx_METHODS },
    { NULL, NULL, 0 }
};
