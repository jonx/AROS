/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Internal definitions for the Cocoa/Metal display HIDD (darwin-aarch64).

    The AROS side of the cocoametal display driver. It loads the host shim
    cocoametal.dylib via hostlib.resource and drives it through the frozen flat-C
    cm_* ABI (v2, 13 symbols) documented in
    docs/features/cocoa-metal-display/INTERFACE.md. AROS owns the framebuffer
    (BGRA8); the shim shows it in an NSWindow via Metal.
*/

#ifndef COCOA_INTERN_H
#define COCOA_INTERN_H

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <resources/kernel.h>
#include <oop/oop.h>

/* ---- Mirror of the host shim's flat-C ABI (cocoametal.h). The shim pulls no
   AROS headers; we pull no Cocoa headers. These structs/signatures must match
   cocoametal.h exactly (INTERFACE.md §1, §2a-1). ---- */

struct CMPixelDesc                       /* BGRA8, LE AArch64 (INTERFACE.md §2a-1) */
{
    int      bytesPerPixel;              /* 4 */
    int      redShift, greenShift, blueShift, alphaShift;   /* 16, 8, 0, 24 */
    unsigned redMask, greenMask, blueMask, alphaMask;
};

struct CMEvent                           /* INTERFACE.md §5 */
{
    int      type;                       /* CMEventType */
    int      x, y;                       /* logical, top-left */
    int      code;                       /* macOS VK / button index */
    int      pressed;
    unsigned mods;                       /* CM_MOD_* */
};

/* CMEventType values (mirror cocoametal.h's enum order). */
#define CM_EV_NONE       0
#define CM_EV_MOUSEMOVE  1
#define CM_EV_MOUSEBTN   2
#define CM_EV_KEY        3
#define CM_EV_CLOSE      4
#define CM_EV_RESIZE     5
#define CM_EV_SETTING    6
/* CMEvent.mods bits */
#define CM_MOD_SHIFT     (1u << 0)
#define CM_MOD_CONTROL   (1u << 1)
#define CM_MOD_ALT       (1u << 2)
#define CM_MOD_CMD       (1u << 3)

/* CMOption values that the AROS side consumes from CM_EV_SETTING. Keep these
   numerically mirrored with hosted/cocoametal/cocoametal.h. */
#define CM_OPT_REQUEST_MODE_W   0x10
#define CM_OPT_REQUEST_MODE_H   0x11
#define CM_OPT_KEYMAP           0x12
#define CM_OPT_AUDIO_VOLUME     0x13
#define CM_OPT_CLIPBOARD_SHARE  0x14
#define CM_OPT_AUDIO_DEVICE     0x15
#define CM_OPT_VOLUME_ADD       0x16
#define CM_OPT_VOLUME_REMOVE    0x17
#define CM_OPT_POWER            0x18

#define CM_POWER_REQUEST_DOWN   0
#define CM_POWER_RESET          1
#define CM_POWER_FORCE_DOWN     2
#define CM_POWER_FORCE_QUIT     3

typedef struct CMContext CMContext;      /* opaque host window handle */

static inline BOOL cocoa_can_lock_hostlib(void)
{
    return FALSE;
}

/* The 13 cm_* host functions, in cocoametal.dylib symbol order (ABI v2,
   INTERFACE.md §1a). Order is the HostLib_GetInterface contract: append-only. */
struct CMInterface
{
    CMContext *(*cm_open)(int w, int h, const struct CMPixelDesc *fmt, const char *title);
    void       (*cm_close)(CMContext *);
    void       (*cm_upload_rect)(CMContext *, const void *src, int srcStride, int x, int y, int w, int h);
    void       (*cm_present)(CMContext *);
    int        (*cm_set_effect)(CMContext *, int effect);
    int        (*cm_pump_events)(CMContext *, struct CMEvent *out, int maxEvents);
    int        (*cm_readback)(CMContext *, void *dst, int dstStride, int w, int h);
    int        (*cm_target_size)(CMContext *, int *outW, int *outH, int *outScale);
    int        (*cm_render_effect_readback)(CMContext *, int effect, void *dst, int dstStride, int w, int h);
    int        (*cm_abi_version)(void);
    int        (*cm_set_option)(CMContext *, int key, long value);
    int        (*cm_get_option)(CMContext *, int key, long *value);
    int        (*cm_open_settings)(CMContext *);
};

#define CM_ABI_VERSION 2                 /* INTERFACE.md §7 (host reports 2) */
#define CM_DYLIB_NAME  "cocoametal.dylib"

/* ---- libpasteboard.dylib ABI (the clipboard bridge, cocoa_clipboard.c). The 6
   host_pb_* symbols the bridge uses, in symbol order = the HostLib_GetInterface
   contract. Mirror of hosted/clipboard/pasteboard.h; the AROS side pulls no Cocoa
   headers. size_t is 8 bytes on arm64 (== unsigned long). ---- */
struct PBInterface
{
    int  (*host_pb_get_text)(char **out, unsigned long *len);
    long (*host_pb_set_text)(const char *utf8, unsigned long len);
    long (*host_pb_change_count)(void);
    void (*host_pb_free)(void *p);
    int  (*host_latin1_to_utf8)(const unsigned char *latin1, unsigned long len, char **out, unsigned long *out_len);
    int  (*host_utf8_to_latin1)(const char *utf8, unsigned long len, int translit, unsigned char **out, unsigned long *out_len);
};
#define PB_DYLIB_NAME  "libpasteboard.dylib"

/* Default logical display mode (one mode keeps the sync/mode taglists trivial). */
#define COCOA_WIDTH    800
#define COCOA_HEIGHT   600

/* ---- Driver-global state (single-instance, like the SDL/headless drivers). ---- */
struct cocoahidd
{
    OOP_Class          *gfxclass;        /* CocoaGfx (CLID_Hidd_Gfx subclass)  */
    OOP_Class          *displayclass;    /* CocoaDisplay (CLID_Hidd_Display sub) */
    OOP_Class          *bmclass;         /* CocoaBM (CLID_Hidd_BitMap subclass) */
    OOP_Object         *gfxhidd;         /* the singleton gfx object           */
    OOP_Object         *display;         /* the Hidd_Display object (modes/show) */
    OOP_Object         *dmenum;          /* its display-mode enumerator         */
    OOP_Class          *basebm;          /* CLID_Hidd_BitMap base class         */
    APTR                hostlib;         /* hostlib.resource (HostLibBase macro) */
    APTR                cmHandle;        /* dlopen handle for cocoametal.dylib  */
    struct CMInterface *cm;              /* resolved cm_* interface             */
    CMContext          *ctx;             /* host window (lazy, opened on Show)  */
    OOP_Object         *visible;         /* currently shown bitmap              */
    BOOL                dirty;           /* visible bitmap has pending damage    */
    WORD                dirty_x1, dirty_y1, dirty_x2, dirty_y2; /* x2/y2 exclusive */

    /* ---- input (cocoa_input.c): kbd+mouse HIDDs + the cm_pump_events task --- */
    OOP_Class          *kbdclass;        /* CocoaKbd (CLID_Hidd_Kbd subclass)   */
    OOP_Class          *mouseclass;      /* CocoaMouse (CLID_Hidd_Mouse subcls) */
    OOP_Object         *kbdhidd;         /* the cocoa kbd driver instance       */
    OOP_Object         *mousehidd;       /* the cocoa mouse driver instance     */
    VOID              (*kbd_callback)(APTR, APTR);    /* keyboard.hidd IrqHandler */
    APTR                kbd_callbackdata;
    VOID              (*mouse_callback)(APTR, APTR);  /* mouse.hidd IrqHandler    */
    APTR                mouse_callbackdata;
    struct Task        *eventtask;       /* polls cm_pump_events every VBlank   */

    /* ---- clipboard bridge (cocoa_clipboard.c): NSPasteboard <-> PRIMARY_CLIP --- */
    APTR                pbHandle;        /* dlopen handle for libpasteboard.dylib */
    struct PBInterface *pb;              /* resolved host_pb_* interface          */
    struct Task        *cliptask;        /* the clipboard-sync poll task          */
};

/* gfx class instance data */
struct gfxdata
{
    struct cocoahidd *xsd;
};

/* bitmap class instance data: AROS owns the BGRA8 framebuffer (INTERFACE.md §2). */
struct bmdata
{
    struct cocoahidd *xsd;
    APTR   framebuffer;                  /* AllocMem(width*height*4, MEMF_CLEAR) */
    ULONG  width, height;
    ULONG  bytesperrow;                  /* width * 4 */
    BOOL   onscreen;
};

/* Class IDs for the runtime-created (CLID_HiddMeta) cocoa classes. */
#define CLID_Hidd_Gfx_Cocoa  "hidd.gfx.cocoa"
#define IID_Hidd_Gfx_Cocoa   "hidd.gfx.cocoa"
#define CLID_Hidd_Display_Cocoa "hidd.display.cocoa"
#define IID_Hidd_Display_Cocoa  "hidd.display.cocoa"
#define IID_Hidd_BitMap_Cocoa "hidd.bitmap.cocoa"

/* The single driver-global state (one Cocoa display in the system). */
extern struct cocoahidd xsd;

/* Class interface descriptors (defined in the class .c files). */
extern struct OOP_InterfaceDescr CocoaGfx_ifdescr[];
extern struct OOP_InterfaceDescr CocoaGfx_Display_ifdescr[];
extern struct OOP_InterfaceDescr CocoaBM_ifdescr[];
extern struct OOP_InterfaceDescr CocoaKbd_ifdescr[];
extern struct OOP_InterfaceDescr CocoaMouse_ifdescr[];

/* Standard HIDD attribute bases, defined+obtained in startup.c. The aHidd_*
   attribute macros used by the class methods resolve against these. */
extern OOP_AttrBase MetaAttrBase;
extern OOP_AttrBase HiddAttrBase;
extern OOP_AttrBase HiddPixFmtAttrBase;
extern OOP_AttrBase HiddSyncAttrBase;
extern OOP_AttrBase HiddGfxAttrBase;
extern OOP_AttrBase HiddDisplayAttrBase;
extern OOP_AttrBase HiddDMEnumAttrBase;
extern OOP_AttrBase HiddBitMapAttrBase;
extern OOP_AttrBase HiddChunkyBMAttrBase;
extern OOP_AttrBase HiddColorMapAttrBase;

BOOL cocoa_hostlib_init(struct cocoahidd *xsd);
void cocoa_hostlib_expunge(struct cocoahidd *xsd);

void cocoa_mark_dirty(OOP_Object *bm, WORD x, WORD y, WORD width, WORD height);
void cocoa_present_visible(BOOL force);

/* input.c: create the kbd+mouse HIDDs, register them, start the poll task. */
BOOL cocoa_input_init(struct cocoahidd *xsd);
void cocoa_input_expunge(struct cocoahidd *xsd);

/* clipboard.c: start the NSPasteboard <-> clipboard.device sync task (non-fatal). */
BOOL cocoa_clipboard_init(struct cocoahidd *xsd);
void cocoa_clipboard_set_enabled(BOOL enabled);

#endif /* COCOA_INTERN_H */
