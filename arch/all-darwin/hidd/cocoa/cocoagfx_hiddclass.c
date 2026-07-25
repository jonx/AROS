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
    /* The display mode ladder. The FIRST entry is the default monitor sync.
       The boot mode is not taken from here: with no screenmode.prefs, intuition
       opens the Workbench screen at AROS_NOMINAL_WIDTH x AROS_NOMINAL_HEIGHT
       (a configure-time value), so that size must exist in this ladder. */
    static const UWORD modes[][2] =
    {
        {  800,  600 }, {  640,  480 }, { 1024,  768 }, { 1152,  864 },
        { 1280,  720 }, { 1280,  800 }, { 1280, 1024 }, { 1366,  768 },
        { 1440,  900 }, { 1600,  900 }, { 1600, 1200 }, { 1680, 1050 },
        { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
    };
#define COCOA_NMODES (sizeof(modes) / sizeof(modes[0]))
    static struct TagItem sync_mode[COCOA_NMODES][7];
    static struct TagItem modetags[COCOA_NMODES + 2];
    ULONG i;

    for (i = 0; i < COCOA_NMODES; i++)
    {
        ULONG w = modes[i][0], h = modes[i][1];
        struct TagItem st[7] =
        {
            { aHidd_Sync_PixelClock, 60UL * w * h        },
            { aHidd_Sync_HDisp,      w                   },
            { aHidd_Sync_VDisp,      h                   },
            { aHidd_Sync_HTotal,     w                   },
            { aHidd_Sync_VTotal,     h                   },
            { aHidd_Sync_Description,(IPTR)"Cocoa:%hx%v" },
            { TAG_DONE,              0                   }
        };
        CopyMem(st, sync_mode[i], sizeof(st));
        modetags[i + 1].ti_Tag  = aHidd_DMEnum_SyncTags;
        modetags[i + 1].ti_Data = (IPTR)sync_mode[i];
    }
    modetags[0].ti_Tag  = aHidd_DMEnum_PixFmtTags;
    modetags[0].ti_Data = (IPTR)pftags;
    modetags[COCOA_NMODES + 1].ti_Tag  = TAG_DONE;
    modetags[COCOA_NMODES + 1].ti_Data = 0;
    struct TagItem msgtags[] =
    {
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
    {
        /* gfx HIDD refactor: the mode database, CreateObject and Show moved off
           the Gfx class onto a separate Hidd_Display subclass. Create our
           display object, hand it the mode tags, and cache its DMEnumerator. */
        struct TagItem displaytags[] =
        {
            { aHidd_Display_GfxHidd,  (IPTR)o        },
            { aHidd_Display_ModeTags, (IPTR)modetags },
            { TAG_DONE,               0              }
        };

        xsd.gfxhidd = o;
        xsd.display = OOP_NewObject(xsd.displayclass, NULL, displaytags);
        if (xsd.display)
            OOP_GetAttr(xsd.display, aHidd_Display_DMEnumerator, (IPTR *)&xsd.dmenum);
    }

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
        case aoHidd_Gfx_DisplayDefault:
            *msg->storage = (IPTR)xsd.display;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/* Hand the base class our bitmap class for every bitmap tied to a display mode.
   The graphics core creates the real front framebuffer with FrameBuffer=TRUE,
   which implies Displayable only later inside the base class, so checking only
   Displayable here misses exactly the bitmap that receives UpdateRect(). */
OOP_Object *CocoaGfx__Hidd_Display__CreateObject(OOP_Class *cl, OOP_Object *o, struct pHidd_Display_CreateObject *msg)
{
    OOP_Object *object;

    if (msg->cl == xsd.basebm)
    {
        struct TagItem tags[2] =
        {
            { TAG_IGNORE, 0                   },
            { TAG_MORE,   (IPTR)msg->attrList }
        };
        struct pHidd_Display_CreateObject p;

        if (GetTagData(aHidd_BitMap_ModeID, vHidd_ModeID_Invalid, msg->attrList) != vHidd_ModeID_Invalid)
        {
            tags[0].ti_Tag  = aHidd_BitMap_ClassPtr;
            tags[0].ti_Data = (IPTR)xsd.bmclass;
            D(bug("[Cocoa] CreateObject: mode bitmap -> CocoaBM\n"));
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

/* Lazy window open on first Show; record the actual front framebuffer returned
   by the gfx base class for the present hook. In direct-FB mode this is not the
   source screen bitmap passed in msg->bitMap. */
OOP_Object *CocoaGfx__Hidd_Display__Show(OOP_Class *cl, OOP_Object *o, struct pHidd_Display_Show *msg)
{
    struct pHidd_Display_Show mymsg = { msg->mID, msg->bitMap, msg->flags };
    OOP_Object *shown;

    D(bug("[Cocoa] CocoaGfx::Show(0x%p)\n", msg->bitMap));

    if (msg->bitMap && xsd.cm)
    {
        IPTR w = COCOA_WIDTH, h = COCOA_HEIGHT;
        BOOL lockHost = cocoa_can_lock_hostlib();

        OOP_GetAttr(msg->bitMap, aHidd_BitMap_Width,  &w);
        OOP_GetAttr(msg->bitMap, aHidd_BitMap_Height, &h);

        /* Forbid task-switching across the host call. On darwin this call
         * blocks the AROS thread in a host syscall (the cm_* main-thread hop
         * dispatch_syncs to the window thread); HostLib_Lock is only a
         * semaphore, so without Forbid the preemptive scheduler can switch a
         * task at the syscall boundary and save its SP on the host stack ->
         * "out of stack limits" when restored. */
        if (!xsd.ctx)
        {
            if (lockHost)
            {
                Forbid();
                HostLib_Lock();
            }
            xsd.ctx = xsd.cm->cm_open((int)w, (int)h, &cocoa_fmt, "AROS");
            AROS_HOST_BARRIER
            if (lockHost)
            {
                HostLib_Unlock();
                Permit();
            }
            if (xsd.ctx)
            {
                xsd.ctx_w = (LONG)w;
                xsd.ctx_h = (LONG)h;
            }
            D(bug("[Cocoa] cm_open(%ld,%ld) -> 0x%p\n", w, h, xsd.ctx));
        }
        else if ((LONG)w != xsd.ctx_w || (LONG)h != xsd.ctx_h)
        {
            int r;

            if (lockHost)
            {
                Forbid();
                HostLib_Lock();
            }
            r = xsd.cm->cm_set_mode(xsd.ctx, (int)w, (int)h);
            AROS_HOST_BARRIER
            if (lockHost)
            {
                HostLib_Unlock();
                Permit();
            }
            if (r == 0)
            {
                xsd.ctx_w = (LONG)w;
                xsd.ctx_h = (LONG)h;
            }
            D(bug("[Cocoa] cm_set_mode(%ld,%ld) -> %d\n", w, h, r));
        }
    }

    shown = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&mymsg);
    xsd.visible = shown;
    xsd.dirty = FALSE;
    cocoa_present_visible(TRUE);

    return shown;
}

static struct OOP_MethodDescr CocoaGfx_Root_descr[] =
{
    { (OOP_MethodFunc)CocoaGfx__Root__New, moRoot_New },
    { (OOP_MethodFunc)CocoaGfx__Root__Get, moRoot_Get },
    { NULL, 0 }
};
#define NUM_CocoaGfx_Root_METHODS 2

static struct OOP_MethodDescr CocoaGfx_Hidd_Display_descr[] =
{
    { (OOP_MethodFunc)CocoaGfx__Hidd_Display__CreateObject, moHidd_Display_CreateObject },
    { (OOP_MethodFunc)CocoaGfx__Hidd_Display__Show,         moHidd_Display_Show         },
    { NULL, 0 }
};
#define NUM_CocoaGfx_Hidd_Display_METHODS 2

struct OOP_InterfaceDescr CocoaGfx_ifdescr[] =
{
    { CocoaGfx_Root_descr, IID_Root, NUM_CocoaGfx_Root_METHODS },
    { NULL, NULL, 0 }
};

struct OOP_InterfaceDescr CocoaGfx_Display_ifdescr[] =
{
    { CocoaGfx_Hidd_Display_descr, IID_Hidd_Display, NUM_CocoaGfx_Hidd_Display_METHODS },
    { NULL, NULL, 0 }
};
