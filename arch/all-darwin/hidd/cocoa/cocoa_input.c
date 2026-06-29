/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Cocoa/Metal display driver -- keyboard + mouse input (darwin-aarch64).

    Modelled on the SDL hosted driver (event.c + sdl_kbdclass.c + sdl_mouseclass.c):
    a CocoaKbd and a CocoaMouse HIDD are registered as hardware drivers under the
    system keyboard.hidd / mouse.hidd, and a polling task drains host events via
    cm_pump_events, translating each into input.device RAWKEY/RAWMOUSE events.
*/

#include <hidd/hidd.h>
#include <hidd/keyboard.h>
#include <hidd/mouse.h>
#include <hidd/input.h>

#include <exec/tasks.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/pm.h>
#include <hardware/intbits.h>
#include <devices/inputevent.h>
#include <devices/input.h>
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
    [0x35] = RAWKEY_ESCAPE,   [0x36] = RAWKEY_RAMIGA,   [0x37] = RAWKEY_RAMIGA,
    [0x38] = RAWKEY_LSHIFT,
    [0x39] = RAWKEY_CAPSLOCK, [0x3A] = RAWKEY_LALT,     [0x3B] = RAWKEY_CONTROL,
    [0x3C] = RAWKEY_RSHIFT,   [0x3D] = RAWKEY_RALT,     [0x3E] = RAWKEY_CONTROL,
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

/* input.device IORequest used by the poll task for deferred input delivery. */
static struct IOStdReq *g_inputio;
static UWORD g_keyqual;
static UWORD g_mousequal;
static UBYTE g_defer_rmb_pulses;
static BOOL g_defer_rmb_toggle;
static LONG g_defer_rmb_x;
static LONG g_defer_rmb_y;

#define COCOA_SHIFT_QUALIFIERS   (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)
#define COCOA_ALT_QUALIFIERS     (IEQUALIFIER_LALT | IEQUALIFIER_RALT)
#define COCOA_COMMAND_QUALIFIERS (IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND)

static UWORD cocoa_modifier_qualifier(UWORD raw)
{
    switch (raw)
    {
    case RAWKEY_LSHIFT:   return IEQUALIFIER_LSHIFT;
    case RAWKEY_RSHIFT:   return IEQUALIFIER_RSHIFT;
    case RAWKEY_CONTROL:  return IEQUALIFIER_CONTROL;
    case RAWKEY_LALT:     return IEQUALIFIER_LALT;
    case RAWKEY_RALT:     return IEQUALIFIER_RALT;
    case RAWKEY_LAMIGA:   return IEQUALIFIER_LCOMMAND;
    case RAWKEY_RAMIGA:   return IEQUALIFIER_RCOMMAND;
    case RAWKEY_CAPSLOCK: return IEQUALIFIER_CAPSLOCK;
    default:              return 0;
    }
}

static UWORD cocoa_update_qualifiers(UWORD raw, BOOL pressed, unsigned mods)
{
    UWORD q = cocoa_modifier_qualifier(raw);

    if (q)
    {
        /* A modifier KEY transition is authoritative for that key's qualifier:
           set it on key-down, clear it on key-up. */
        if (pressed)
            g_keyqual |= q;
        else
            g_keyqual &= ~q;
    }

    /* The host modifierFlags (mods) may only ADD a qualifier that is logically
       active but whose key-down we never saw (e.g. a modifier already held at
       focus-in). It must NOT clear a held qualifier here: injected key events
       (the control FIFO) legitimately carry mods=0 while a modifier key is still
       down, and clearing would drop the held Shift/Amiga -- e.g. Shift+letter
       would lose its shift and come out lower-case. Qualifiers are cleared only
       by the matching modifier key-up above. */
    if (mods & CM_MOD_SHIFT)   g_keyqual |= IEQUALIFIER_LSHIFT;
    if (mods & CM_MOD_CONTROL) g_keyqual |= IEQUALIFIER_CONTROL;
    if (mods & CM_MOD_ALT)     g_keyqual |= IEQUALIFIER_LALT;
    if (mods & CM_MOD_CMD)     g_keyqual |= IEQUALIFIER_RCOMMAND;

    return g_keyqual;
}

static UWORD cocoa_mouse_button_code(int button)
{
    switch (button)            /* 0=left, 1=right, 2=middle (NSEvent order) */
    {
    case 0:  return IECODE_LBUTTON;
    case 1:  return IECODE_RBUTTON;
    case 2:  return IECODE_MBUTTON;
    default: return IECODE_NOBUTTON;
    }
}

static UWORD cocoa_mouse_button_qualifier(int button)
{
    switch (button)            /* 0=left, 1=right, 2=middle (NSEvent order) */
    {
    case 0:  return IEQUALIFIER_LEFTBUTTON;
    case 1:  return IEQUALIFIER_RBUTTON;
    case 2:  return IEQUALIFIER_MIDBUTTON;
    default: return 0;
    }
}

static void cocoa_send_input_event(struct InputEvent *ie)
{
    if (!g_inputio)
        return;

    g_inputio->io_Command = IND_ADDEVENT;
    g_inputio->io_Data    = ie;
    g_inputio->io_Length  = sizeof(*ie);
    DoIO((struct IORequest *)g_inputio);
}

static void cocoa_send_mouse_position(LONG x, LONG y)
{
    struct InputEvent ie;

    /* This driver registers as a normal mouse HIDD, not as a tablet/touch
       device. Keep host coordinates on the classic absolute RAWMOUSE path used
       by the other hosted drivers; NEWPOINTERPOS/NEWTABLET can leave the
       Wanderer pointer visually stuck on this stack. */
    memset(&ie, 0, sizeof ie);
    ie.ie_Class     = IECLASS_RAWMOUSE;
    ie.ie_Code      = IECODE_NOBUTTON;
    ie.ie_Qualifier = g_keyqual | g_mousequal;
    ie.ie_X         = x;
    ie.ie_Y         = y;
    cocoa_send_input_event(&ie);
}

static void cocoa_defer_rmb_menu_pulse(LONG x, LONG y)
{
    g_defer_rmb_pulses = 6;
    g_defer_rmb_toggle = FALSE;
    g_defer_rmb_x = x;
    g_defer_rmb_y = y;
}

static void cocoa_fire_deferred_rmb_menu_pulse(void)
{
    LONG nx;

    if (!g_defer_rmb_pulses)
        return;

    if (!(g_mousequal & IEQUALIFIER_RBUTTON))
    {
        g_defer_rmb_pulses = 0;
        return;
    }

    nx = (g_defer_rmb_x > 0) ? g_defer_rmb_x - 1 : g_defer_rmb_x + 1;
    cocoa_send_mouse_position(g_defer_rmb_toggle ? g_defer_rmb_x : nx, g_defer_rmb_y);
    g_defer_rmb_toggle = !g_defer_rmb_toggle;
    g_defer_rmb_pulses--;
}

static void cocoa_handle_power_setting(LONG request)
{
    switch (request)
    {
    case CM_POWER_REQUEST_DOWN:
        D(bug("[Cocoa:Settings] power request: shutdown\n"));
        ShutdownA(SD_ACTION_POWEROFF);
        D(bug("[Cocoa:Settings] ShutdownA(SD_ACTION_POWEROFF) returned\n"));
        break;

    case CM_POWER_RESET:
        D(bug("[Cocoa:Settings] power request: cold reset\n"));
        ShutdownA(SD_ACTION_COLDREBOOT);
        D(bug("[Cocoa:Settings] ShutdownA(SD_ACTION_COLDREBOOT) returned\n"));
        break;

    case CM_POWER_FORCE_DOWN:
    case CM_POWER_FORCE_QUIT:
        D(bug("[Cocoa:Settings] power request: force %s -> hosted shutdown\n",
              request == CM_POWER_FORCE_QUIT ? "quit" : "down"));
        ShutdownA(SD_ACTION_POWEROFF);
        D(bug("[Cocoa:Settings] forced ShutdownA(SD_ACTION_POWEROFF) returned\n"));
        break;

    default:
        D(bug("[Cocoa:Settings] unknown power request %ld\n", (IPTR)request));
        break;
    }
}

static void cocoa_handle_setting_event(struct CMEvent *e)
{
    switch (e->code)
    {
    case CM_OPT_REQUEST_MODE_W:
    case CM_OPT_REQUEST_MODE_H:
        D(bug("[Cocoa:Settings] display mode request key=0x%lx value=%ld partner=%ld (dynamic modes not wired yet)\n",
              (IPTR)e->code, (IPTR)e->x, (IPTR)e->y));
        break;

    case CM_OPT_KEYMAP:
        D(bug("[Cocoa:Settings] keymap request id=%ld (keymap switching not wired yet)\n",
              (IPTR)e->x));
        break;

    case CM_OPT_AUDIO_VOLUME:
        D(bug("[Cocoa:Settings] audio volume request %ld (host CoreAudio gain mirrored)\n",
              (IPTR)e->x));
        break;

    case CM_OPT_CLIPBOARD_SHARE:
        D(bug("[Cocoa:Settings] clipboard sharing request %ld\n", (IPTR)e->x));
        cocoa_clipboard_set_enabled(e->x != 0);
        break;

    case CM_OPT_AUDIO_DEVICE:
        D(bug("[Cocoa:Settings] audio device request %ld (CoreAudio/AHI not wired yet)\n",
              (IPTR)e->x));
        break;

    case CM_OPT_VOLUME_ADD:
    case CM_OPT_VOLUME_REMOVE:
        D(bug("[Cocoa:Settings] host volume %s request received, but string option ABI v3 is not consumed yet\n",
              e->code == CM_OPT_VOLUME_ADD ? "add" : "remove"));
        break;

    case CM_OPT_POWER:
        cocoa_handle_power_setting(e->x);
        break;

    default:
        D(bug("[Cocoa:Settings] unknown setting key=0x%lx value=%ld partner=%ld\n",
              (IPTR)e->code, (IPTR)e->x, (IPTR)e->y));
        break;
    }
}

/* ------------------------------------------------------------------------- */
static void cocoa_dispatch(struct CMEvent *e)
{
    {
        static ULONG ndbg = 0;
        if (ndbg++ < 16)
            D(bug("[Cocoa:Input] ev type=%d code=%d pressed=%d x=%d y=%d\n",
                  e->type, e->code, e->pressed, e->x, e->y));
    }

    switch (e->type)
    {
    case CM_EV_KEY:
        if (g_inputio && cocoa_key_mapped(e->code))
        {
            struct InputEvent ie;
            UWORD raw = cocoa_keymap[e->code & 0x7F];
            UWORD qual = cocoa_update_qualifiers(raw, e->pressed, e->mods) | g_mousequal;
            if (!e->pressed)
                raw |= IECODE_UP_PREFIX;
            /* Deliver through input.device (IND_ADDEVENT) rather than calling
               the keyboard.hidd IrqHandler inline. The IrqHandler's synchronous
               kbdSendQueuedEvents reaches intuition rendering ON THIS poll task,
               which corrupts the gfx OOP dispatch under the threaded scheduler
               (proven: the fault happens between keyCallback START and DONE on
               'cocoa.hidd input'). DoIO blocks us and lets input.device's own
               task do the rendering. */
            memset(&ie, 0, sizeof ie);
            ie.ie_Class     = IECLASS_RAWKEY;
            ie.ie_Code      = raw;
            ie.ie_Qualifier = qual;
            cocoa_send_input_event(&ie);
        }
        break;

    case CM_EV_MOUSEMOVE:
        if (g_inputio)
        {
            cocoa_send_mouse_position(e->x, e->y);
        }
        break;

    case CM_EV_MOUSEBTN:
        if (g_inputio)
        {
            struct InputEvent ie;
            UWORD code = cocoa_mouse_button_code(e->code);
            UWORD qual = cocoa_mouse_button_qualifier(e->code);

            if (code == IECODE_NOBUTTON || !qual)
                break;

            /* Some Intuition paths (notably screen/menu activation via the right
               button) key off the pointer state that precedes the button
               transition. The host event carries x/y on the button event, but the
               raw mouse stack is more reliable if we publish the motion first. */
            cocoa_send_mouse_position(e->x, e->y);

            if (e->pressed)
                g_mousequal |= qual;
            else
            {
                code |= IECODE_UP_PREFIX;
                g_mousequal &= ~qual;
                if ((code & ~IECODE_UP_PREFIX) == IECODE_RBUTTON)
                    g_defer_rmb_pulses = 0;
            }

            memset(&ie, 0, sizeof ie);
            ie.ie_Class     = IECLASS_RAWMOUSE;
            ie.ie_Code      = code;
            ie.ie_Qualifier = g_keyqual | g_mousequal;
            ie.ie_X         = e->x;
            ie.ie_Y         = e->y;
            cocoa_send_input_event(&ie);

            /* Intuition's menu path is level-ish rather than purely edge-ish:
               on this hosted path, a right-button transition alone can leave the
               menu dormant until a later mouse event arrives while RBUTTON is in
               the qualifier state. Queue a short train of tiny absolute moves on
               later poll ticks so "press and hold" behaves like a real Amiga
               menu press, without requiring the user to wiggle the mouse. Firing
               them inline is too early; the menu task may not have entered its
               active state. */
            if (e->pressed && code == IECODE_RBUTTON)
                cocoa_defer_rmb_menu_pulse(e->x, e->y);
        }
        break;

    case CM_EV_RESIZE:
        cocoa_present_visible(TRUE);
        break;

    case CM_EV_CLOSE:
        D(bug("[Cocoa:Input] host close event -> ShutdownA(SD_ACTION_POWEROFF)\n"));
        ShutdownA(SD_ACTION_POWEROFF);
        D(bug("[Cocoa:Input] close ShutdownA returned\n"));
        break;

    case CM_EV_SETTING:
        cocoa_handle_setting_event(e);
        break;

    default:
        break;
    }
}

static BOOL cocoa_event_is_redundant(struct CMEvent *evbuf, int i, int n)
{
    int j;

    if (evbuf[i].type != CM_EV_MOUSEMOVE && evbuf[i].type != CM_EV_RESIZE)
        return FALSE;

    for (j = i + 1; j < n; j++)
    {
        /* Preserve ordering around keys/buttons/settings/close events. */
        if (evbuf[j].type != CM_EV_MOUSEMOVE && evbuf[j].type != CM_EV_RESIZE)
            return FALSE;

        if (evbuf[j].type == evbuf[i].type)
            return TRUE;
    }

    return FALSE;
}

static void cocoa_dispatch_batch(struct CMEvent *evbuf, int n)
{
    int i;
    ULONG skipped = 0;

    for (i = 0; i < n; i++)
    {
        if (cocoa_event_is_redundant(evbuf, i, n))
        {
            skipped++;
            continue;
        }
        cocoa_dispatch(&evbuf[i]);
    }

    if (skipped)
    {
        static ULONG reports = 0;
        if (reports++ < 8)
            D(bug("[Cocoa:Input] coalesced %lu motion/resize event(s)\n", (IPTR)skipped));
    }
}

static void cocoa_event_task(struct Task *creator, ULONG sync)
{
    struct CMEvent   evbuf[CM_MAX_EVENTS];
    int              n;

    D(bug("[Cocoa:Input] event task starting\n"));

    /* Open input.device for deferred keyboard/mouse delivery. The port + request
       belong to this (poll) task so DoIO()'s WaitIO() waits on us, while
       input.device's own task does the actual event processing + rendering. */
    {
        struct MsgPort *port = CreateMsgPort();
        if (port)
        {
            g_inputio = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
            if (g_inputio && OpenDevice("input.device", 0, (struct IORequest *)g_inputio, 0) != 0)
                g_inputio = NULL;
        }
        D(bug("[Cocoa:Input] input.device for delivery: 0x%p\n", g_inputio));
    }

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

        cocoa_fire_deferred_rmb_menu_pulse();

        {
        BOOL lockHost = cocoa_can_lock_hostlib();

        if (lockHost)
        {
            Forbid();
            HostLib_Lock();
        }
        n = xsd.cm->cm_pump_events(xsd.ctx, evbuf, CM_MAX_EVENTS);
        AROS_HOST_BARRIER
        if (lockHost)
        {
            HostLib_Unlock();
            Permit();
        }
        }

        {
            static BOOL first = TRUE;
            if (first)
            {
                first = FALSE;
                D(bug("[Cocoa:Input] poll loop live (first cm_pump_events -> %d)\n", n));
            }
        }
        cocoa_dispatch_batch(evbuf, n);

        cocoa_present_visible(FALSE);
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

    /* keyboard.hidd forwards only the IrqHandler (not its Data) to hardware
       drivers, so the kbd handler context arrives NULL and keyCallback faults on
       the first key (it dereferences its KeyboardBase arg). That data IS just
       keyboard.device's base, so open the device and take io_Device, keeping it
       open so the base stays valid. (The mouse path already gets valid data via
       the New tag list.) */
    if (!xsd_->kbd_callbackdata)
    {
        struct MsgPort *kbport = CreateMsgPort();
        if (kbport)
        {
            struct IORequest *kbio = CreateIORequest(kbport, sizeof(struct IOStdReq));
            if (kbio)
            {
                LONG err = OpenDevice("keyboard.device", 0, kbio, 0);
                D(bug("[Cocoa:Input] OpenDevice(keyboard.device) = %ld, io_Device 0x%p\n",
                      (long)err, err ? NULL : kbio->io_Device));
                if (err == 0)
                    xsd_->kbd_callbackdata = kbio->io_Device;   /* == KeyboardBase */
                /* keep it open so the base stays valid (resident driver) */
            }
        }
    }
    D(bug("[Cocoa:Input] resolved kbd data 0x%p, mouse data 0x%p\n",
          xsd_->kbd_callbackdata, xsd_->mouse_callbackdata));

    /* Start the poll task. */
    SetSignal(0, sync);
    task = NewCreateTask(TASKTAG_PC,        (IPTR)cocoa_event_task,
                         TASKTAG_NAME,      (IPTR)"cocoa.hidd input",
                         TASKTAG_PRI,       50,
                         /* Dispatching a key runs part of the input chain inline
                            (keyCallback -> kbdSendQueuedEvents -> input.device),
                            so this task needs a generous stack. */
                         TASKTAG_STACKSIZE, 128 * 1024,
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
