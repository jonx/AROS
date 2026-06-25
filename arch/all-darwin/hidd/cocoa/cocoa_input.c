/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Cocoa/Metal display driver -- keyboard + mouse input (darwin-aarch64).

    Modelled on the SDL hosted driver (event.c + sdl_kbdclass.c + sdl_mouseclass.c):
    a CocoaKbd and a CocoaMouse HIDD are registered as hardware drivers under the
    system keyboard.hidd / mouse.hidd, and a high-priority task polls the host
    window for events once per VBlank via cm_pump_events, translating each into an
    AROS rawkey / mouse event and feeding it to the respective IrqHandler.

    cm_pump_events hops to the host main thread (it dequeues NSEvents); like every
    other cm_* call here it runs under Forbid()+HostLib_Lock() so the preemptive
    AROS scheduler can't switch tasks while we're blocked in the host syscall.
*/

#include <hidd/hidd.h>
#include <hidd/keyboard.h>
#include <hidd/mouse.h>
#include <hidd/input.h>

#include <exec/tasks.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <devices/inputevent.h>
#include <devices/rawkeycodes.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <proto/hostlib.h>

#include "cocoa_intern.h"

#define DEBUG 1
#include <aros/debug.h>

#define HostLibBase (xsd.hostlib)

#define CM_MAX_EVENTS 64

/* aHidd_Input_* (kbd/mouse New) and aHidd_Mouse_* resolve against these. Obtained
   in cocoa_input_init, separately from the display's attr bases, so that input
   being unavailable can never stop the window from rendering. */
OOP_AttrBase HiddInputAB;
OOP_AttrBase HiddMouseAB;

static struct OOP_ABDescr input_attrbases[] =
{
    { IID_Hidd_Input, &HiddInputAB },
    { IID_Hidd_Mouse, &HiddMouseAB },
    { NULL,           NULL         }
};

/* ------------------------------------------------------------------------- */
/* macOS virtual keycode (kVK_*) -> Amiga rawkey. Unmapped slots stay 0
   (RAWKEY_TILDE); we only emit those that were explicitly set, see below.    */
static const UWORD cocoa_keymap[128] =
{
    [0x00] = RAWKEY_A,        [0x01] = RAWKEY_S,        [0x02] = RAWKEY_D,
    [0x03] = RAWKEY_F,        [0x04] = RAWKEY_H,        [0x05] = RAWKEY_G,
    [0x06] = RAWKEY_Z,        [0x07] = RAWKEY_X,        [0x08] = RAWKEY_C,
    [0x09] = RAWKEY_V,        [0x0B] = RAWKEY_B,        [0x0C] = RAWKEY_Q,
    [0x0D] = RAWKEY_W,        [0x0E] = RAWKEY_E,        [0x0F] = RAWKEY_R,
    [0x10] = RAWKEY_Y,        [0x11] = RAWKEY_T,        [0x12] = RAWKEY_1,
    [0x13] = RAWKEY_2,        [0x14] = RAWKEY_3,        [0x15] = RAWKEY_4,
    [0x16] = RAWKEY_6,        [0x17] = RAWKEY_5,        [0x18] = RAWKEY_EQUAL,
    [0x19] = RAWKEY_9,        [0x1A] = RAWKEY_7,        [0x1B] = RAWKEY_MINUS,
    [0x1C] = RAWKEY_8,        [0x1D] = RAWKEY_0,        [0x1E] = RAWKEY_RBRACKET,
    [0x1F] = RAWKEY_O,        [0x20] = RAWKEY_U,        [0x21] = RAWKEY_LBRACKET,
    [0x22] = RAWKEY_I,        [0x23] = RAWKEY_P,        [0x24] = RAWKEY_RETURN,
    [0x25] = RAWKEY_L,        [0x26] = RAWKEY_J,        [0x27] = RAWKEY_QUOTE,
    [0x28] = RAWKEY_K,        [0x29] = RAWKEY_SEMICOLON, [0x2A] = RAWKEY_BACKSLASH,
    [0x2B] = RAWKEY_COMMA,    [0x2C] = RAWKEY_SLASH,    [0x2D] = RAWKEY_N,
    [0x2E] = RAWKEY_M,        [0x2F] = RAWKEY_PERIOD,   [0x30] = RAWKEY_TAB,
    [0x31] = RAWKEY_SPACE,    [0x32] = RAWKEY_TILDE,    [0x33] = RAWKEY_BACKSPACE,
    [0x35] = RAWKEY_ESCAPE,   [0x37] = RAWKEY_LAMIGA,   [0x38] = RAWKEY_LSHIFT,
    [0x39] = RAWKEY_CAPSLOCK, [0x3A] = RAWKEY_LALT,     [0x3B] = RAWKEY_CONTROL,
    [0x3C] = RAWKEY_RSHIFT,   [0x3D] = RAWKEY_RALT,     [0x3E] = RAWKEY_RAMIGA,
    [0x7B] = RAWKEY_LEFT,     [0x7C] = RAWKEY_RIGHT,    [0x7D] = RAWKEY_DOWN,
    [0x7E] = RAWKEY_UP,
};

/* Which slots are real mappings (so VK 0 == 'A' isn't confused with "unmapped"). */
static inline BOOL cocoa_key_mapped(int vk)
{
    return (vk >= 0 && vk < 128 &&
            (cocoa_keymap[vk] != 0 || vk == 0x00 /* A */ || vk == 0x32 /* ` */));
}

/* ------------------------------------------------------------------------- */
/* CocoaKbd -- subclass of CLID_Hidd, registered as a keyboard hardware driver.
   We keep no per-instance data; the single IrqHandler lives in the driver
   globals so the poll task can reach it directly.                            */
OOP_Object *CocoaKbd__Root__New(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    EnterFunc(bug("[Cocoa:Kbd] New\n"));

    if (xsd.kbdhidd)
        return NULL;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (o)
    {
        xsd.kbd_callback     = (APTR)GetTagData(aHidd_Input_IrqHandler, 0, msg->attrList);
        xsd.kbd_callbackdata = (APTR)GetTagData(aHidd_Input_IrqHandlerData, 0, msg->attrList);
        xsd.kbdhidd          = o;
        D(bug("[Cocoa:Kbd] callback 0x%p data 0x%p\n", xsd.kbd_callback, xsd.kbd_callbackdata));
    }
    return o;
}

static struct OOP_MethodDescr CocoaKbd_Root_descr[] =
{
    { (OOP_MethodFunc)CocoaKbd__Root__New, moRoot_New },
    { NULL, 0 }
};
#define NUM_CocoaKbd_Root_METHODS 1

struct OOP_InterfaceDescr CocoaKbd_ifdescr[] =
{
    { CocoaKbd_Root_descr, IID_Root, NUM_CocoaKbd_Root_METHODS },
    { NULL, NULL, 0 }
};

/* ------------------------------------------------------------------------- */
/* CocoaMouse -- subclass of CLID_Hidd, registered as a pointer hardware driver. */
OOP_Object *CocoaMouse__Root__New(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    EnterFunc(bug("[Cocoa:Mouse] New\n"));

    if (xsd.mousehidd)
        return NULL;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (o)
    {
        xsd.mouse_callback     = (APTR)GetTagData(aHidd_Input_IrqHandler, 0, msg->attrList);
        xsd.mouse_callbackdata = (APTR)GetTagData(aHidd_Input_IrqHandlerData, 0, msg->attrList);
        xsd.mousehidd          = o;
        D(bug("[Cocoa:Mouse] callback 0x%p data 0x%p\n", xsd.mouse_callback, xsd.mouse_callbackdata));
    }
    return o;
}

static struct OOP_MethodDescr CocoaMouse_Root_descr[] =
{
    { (OOP_MethodFunc)CocoaMouse__Root__New, moRoot_New },
    { NULL, 0 }
};
#define NUM_CocoaMouse_Root_METHODS 1

struct OOP_InterfaceDescr CocoaMouse_ifdescr[] =
{
    { CocoaMouse_Root_descr, IID_Root, NUM_CocoaMouse_Root_METHODS },
    { NULL, NULL, 0 }
};

/* ------------------------------------------------------------------------- */
static void cocoa_dispatch(struct CMEvent *e)
{
    switch (e->type)
    {
    case CM_EV_KEY:
        if (xsd.kbd_callback && cocoa_key_mapped(e->code))
        {
            struct pHidd_Kbd_Event kEvt;
            kEvt.kbdevt = cocoa_keymap[e->code & 0x7F];
            if (!e->pressed)
                kEvt.kbdevt |= IECODE_UP_PREFIX;
            xsd.kbd_callback(xsd.kbd_callbackdata, &kEvt);
        }
        break;

    case CM_EV_MOUSEMOVE:
        if (xsd.mouse_callback)
        {
            struct pHidd_Mouse_Event hev;
            hev.type   = vHidd_Mouse_Motion;
            hev.x      = e->x;
            hev.y      = e->y;
            hev.button = vHidd_Mouse_NoButton;
            xsd.mouse_callback(xsd.mouse_callbackdata, &hev);
        }
        break;

    case CM_EV_MOUSEBTN:
        if (xsd.mouse_callback)
        {
            struct pHidd_Mouse_Event hev;
            hev.type = e->pressed ? vHidd_Mouse_Press : vHidd_Mouse_Release;
            hev.x    = e->x;
            hev.y    = e->y;
            switch (e->code)            /* 0=left, 1=right, 2=middle (NSEvent order) */
            {
            case 0:  hev.button = vHidd_Mouse_Button1; break;
            case 1:  hev.button = vHidd_Mouse_Button2; break;
            case 2:  hev.button = vHidd_Mouse_Button3; break;
            default: hev.button = vHidd_Mouse_NoButton; break;
            }
            xsd.mouse_callback(xsd.mouse_callbackdata, &hev);
        }
        break;

    default:
        break;
    }
}

static void cocoa_event_task(struct Task *creator, ULONG sync)
{
    struct CMEvent   evbuf[CM_MAX_EVENTS];
    int              n, i;

    D(bug("[Cocoa:Input] event task starting\n"));

    Signal(creator, sync);

    for (;;)
    {
        /* Poll on the timer (Delay) rather than a VBlank interrupt server: a
           task woken from the host SIGALRM/VBlank interrupt context runs in
           "supervisor mode" under the threaded darwin scheduler, which trips
           every semaphore op. The timer.device wakeup path is the one the rest
           of the boot uses safely. ~20ms = 50Hz, ample for input. */
        Delay(1);

        if (!xsd.ctx || !xsd.cm)        /* window not open yet -- nothing to poll */
            continue;

        Forbid();
        HostLib_Lock();
        n = xsd.cm->cm_pump_events(xsd.ctx, evbuf, CM_MAX_EVENTS);
        AROS_HOST_BARRIER
        HostLib_Unlock();
        Permit();

        {
            static BOOL first = TRUE;
            if (first)
            {
                first = FALSE;
                D(bug("[Cocoa:Input] poll loop live (first cm_pump_events -> %d)\n", n));
            }
        }
        if (n > 0)
            D(bug("[Cocoa:Input] %d host event(s) this tick\n", n));

        for (i = 0; i < n; i++)
            cocoa_dispatch(&evbuf[i]);
    }
}

/* ------------------------------------------------------------------------- */
BOOL cocoa_input_init(struct cocoahidd *xsd_)
{
    struct TagItem kbdmeta[] =
    {
        { aMeta_SuperID,        (IPTR)CLID_Hidd          },
        { aMeta_InterfaceDescr, (IPTR)CocoaKbd_ifdescr   },
        { aMeta_InstSize,       0                        },
        { TAG_DONE,             0                        }
    };
    struct TagItem mousemeta[] =
    {
        { aMeta_SuperID,        (IPTR)CLID_Hidd          },
        { aMeta_InterfaceDescr, (IPTR)CocoaMouse_ifdescr },
        { aMeta_InstSize,       0                        },
        { TAG_DONE,             0                        }
    };
    struct TagItem kbd_tags[] =
    {
        { aHidd_Name,         (IPTR)"CocoaKbd"                  },
        { aHidd_HardwareName, (IPTR)"Cocoa keyboard input"     },
        { aHidd_ProducerName, (IPTR)"AROS darwin-aarch64 graft" },
        { TAG_DONE,           0                                }
    };
    struct TagItem mouse_tags[] =
    {
        { aHidd_Name,         (IPTR)"CocoaMouse"               },
        { aHidd_HardwareName, (IPTR)"Cocoa pointer input"      },
        { aHidd_ProducerName, (IPTR)"AROS darwin-aarch64 graft" },
        { TAG_DONE,           0                                }
    };
    OOP_Object *kbd, *ms;
    OOP_Object *kbddrv = NULL, *msdrv = NULL;
    struct Task *task;
    ULONG sync = SIGF_SINGLE;

    if (!OOP_ObtainAttrBases(input_attrbases))
    {
        D(bug("[Cocoa:Input] no input attr bases (kbd/mouse HIDD not up yet)\n"));
        return FALSE;
    }

    xsd_->kbdclass = OOP_NewObject(NULL, CLID_HiddMeta, kbdmeta);
    if (!xsd_->kbdclass)
        return FALSE;
    xsd_->kbdclass->UserData = xsd_;

    xsd_->mouseclass = OOP_NewObject(NULL, CLID_HiddMeta, mousemeta);
    if (!xsd_->mouseclass)
        return FALSE;
    xsd_->mouseclass->UserData = xsd_;

    /* Register under the system keyboard.hidd / mouse.hidd. AddHardwareDriver
       instantiates our class, passing the device's IrqHandler in the attrs. */
    kbd = OOP_NewObject(NULL, CLID_Hidd_Kbd, NULL);
    ms  = OOP_NewObject(NULL, CLID_Hidd_Mouse, NULL);
    if (kbd)
        kbddrv = HIDD_Input_AddHardwareDriver(kbd, xsd_->kbdclass, kbd_tags);
    if (ms)
        msdrv = HIDD_Input_AddHardwareDriver(ms, xsd_->mouseclass, mouse_tags);

    D(bug("[Cocoa:Input] kbd driver 0x%p, mouse driver 0x%p\n", kbddrv, msdrv));
    if (!kbddrv && !msdrv)
        return FALSE;

    /* Start the poll task. */
    SetSignal(0, sync);
    task = NewCreateTask(TASKTAG_PC,        (IPTR)cocoa_event_task,
                         TASKTAG_NAME,      (IPTR)"cocoa.hidd input",
                         TASKTAG_PRI,       50,
                         TASKTAG_STACKSIZE, AROS_STACKSIZE,
                         TASKTAG_ARG1,      (IPTR)FindTask(NULL),
                         TASKTAG_ARG2,      (IPTR)sync,
                         TAG_DONE);
    if (!task)
        return FALSE;

    Wait(sync);
    xsd_->eventtask = task;
    D(bug("[Cocoa:Input] poll task up (0x%p)\n", task));

    return TRUE;
}

void cocoa_input_expunge(struct cocoahidd *xsd_)
{
    if (xsd_->eventtask)
    {
        RemTask(xsd_->eventtask);
        xsd_->eventtask = NULL;
    }
}
