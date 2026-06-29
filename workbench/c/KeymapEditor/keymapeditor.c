/*
    KeymapEditor -- a small visual keymap editor for AROS.

    Opens a window showing the main alphanumeric block of the keyboard with the
    character each key currently produces (read live through MapRawKey on the
    default keymap). Click a key to select it, then type the character you want
    that key to produce: the change is applied live to the running system (new
    consoles and the running shell pick it up immediately).

    To make edits safe and live it first takes a private, writable copy of the
    default keymap (the built-in default may live in read-only memory) and
    installs it with SetKeyMapDefault; edits then poke the copy's km_LoKeyMap
    in place. Only "plain" keys (not dead/string/nop) are editable this way.

    Built like LoadKeymap: a -noposixc main() command so it loads reliably on the
    hosted darwin-aarch64 port.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <devices/inputevent.h>
#include <devices/keymap.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/keymap.h>

#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *KeymapBase;

static struct KeyMap *gKM;          /* our writable default keymap */
static int            gSel = -1;    /* selected key as (row<<8)|col, or -1 */

/* --- keyboard layout: the main alphanumeric block, by rawkey code ---------- */

static const UBYTE row0[] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C}; /* 1 .. = */
static const UBYTE row1[] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B}; /* q .. ] */
static const UBYTE row2[] = {0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A};      /* a .. ' */
static const UBYTE row3[] = {0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A};           /* z .. / */

struct Row { const UBYTE *keys; int n; int xoff; };
static const struct Row rows[4] =
{
    { row0, 12,  0 },
    { row1, 12, 20 },
    { row2, 11, 30 },
    { row3, 10, 50 },
};

#define KEYW 38
#define KEYH 32
#define GAP   3
#define MX   12
#define MY   30

/* read the character rawkey rk produces under qualifier qual, via our keymap */
static UBYTE keychar(UBYTE rk, UWORD qual)
{
    struct InputEvent ie;
    UBYTE buf[8];
    WORD n;

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class     = IECLASS_RAWKEY;
    ie.ie_Code      = rk;
    ie.ie_Qualifier = qual;
    n = MapRawKey(&ie, (STRPTR)buf, sizeof(buf), gKM);
    return (n == 1 && buf[0] >= 32 && buf[0] < 127) ? buf[0] : 0;
}

/* Take a private, writable copy of the default keymap and install it, so our
   in-place edits are safe (built-in default may be read-only) and live. */
static BOOL takeover(void)
{
    struct KeyMap *old = AskKeyMapDefault();
    struct KeyMap *km;
    IPTR *lo, *hi;

    if (!old) return FALSE;
    km = AllocMem(sizeof(struct KeyMap), MEMF_PUBLIC | MEMF_CLEAR);
    lo = AllocMem(0x40 * sizeof(IPTR), MEMF_PUBLIC);
    hi = AllocMem(0x38 * sizeof(IPTR), MEMF_PUBLIC);
    if (!km || !lo || !hi) return FALSE;

    CopyMem((APTR)old->km_LoKeyMap, lo, 0x40 * sizeof(IPTR));
    CopyMem((APTR)old->km_HiKeyMap, hi, 0x38 * sizeof(IPTR));

    km->km_LoKeyMapTypes = old->km_LoKeyMapTypes;
    km->km_LoKeyMap      = lo;
    km->km_LoCapsable    = old->km_LoCapsable;
    km->km_LoRepeatable  = old->km_LoRepeatable;
    km->km_HiKeyMapTypes = old->km_HiKeyMapTypes;
    km->km_HiKeyMap      = hi;
    km->km_HiCapsable    = old->km_HiCapsable;
    km->km_HiRepeatable  = old->km_HiRepeatable;

    gKM = km;
    SetKeyMapDefault(km);
    return TRUE;
}

/* Set the character a plain key produces under qualifier `qual`. The packed
   mapping holds up to 4 chars (GetMapChar: (m >> ((3-idx)*8)) & 0xFF) and which
   idx MapRawKey reads depends on the key type, so we locate the right byte by
   probing: temporarily mark each byte and see which one comes back. */
static BOOL setkey(UBYTE rk, UWORD qual, UBYTE c)
{
    IPTR *map = (IPTR *)gKM->km_LoKeyMap;   /* our writable copy */
    UBYTE type;
    IPTR  orig;
    int   idx, found = -1;

    if (rk >= 0x40) return FALSE;
    type = gKM->km_LoKeyMapTypes[rk];
    if (type & (KCF_DEAD | KCF_STRING | KCF_NOP)) return FALSE;

    orig = map[rk];
    for (idx = 0; idx < 4; idx++)
    {
        UBYTE marker = (UBYTE)('A' + idx);
        map[rk] = (orig & ~((IPTR)0xFF << ((3 - idx) * 8))) |
                  ((IPTR)marker << ((3 - idx) * 8));
        if (keychar(rk, qual) == marker) { found = idx; break; }
    }
    if (found < 0) { map[rk] = orig; return FALSE; }
    map[rk] = (orig & ~((IPTR)0xFF << ((3 - found) * 8))) |
              ((IPTR)c << ((3 - found) * 8));
    return TRUE;
}

static BOOL setbase(UBYTE rk, UBYTE c) { return setkey(rk, 0, c); }

/* which key is under (mx,my)? returns (row<<8)|col, or -1 */
static int hittest(struct Window *w, int mx, int my)
{
    int bx = w->BorderLeft + MX, by = w->BorderTop + MY;
    int r, k;

    for (r = 0; r < 4; r++)
    {
        int y = by + r * (KEYH + GAP);
        if (my < y || my > y + KEYH) continue;
        for (k = 0; k < rows[r].n; k++)
        {
            int x = bx + rows[r].xoff + k * (KEYW + GAP);
            if (mx >= x && mx <= x + KEYW) return (r << 8) | k;
        }
    }
    return -1;
}

static void drawkey(struct RastPort *rp, int x, int y, UBYTE rk, BOOL sel)
{
    UBYTE c  = keychar(rk, 0);
    UBYTE sc = keychar(rk, IEQUALIFIER_LSHIFT);
    char lbl[2];

    SetAPen(rp, sel ? 3 : 2);
    RectFill(rp, x + 1, y + 1, x + KEYW - 1, y + KEYH - 1);
    SetAPen(rp, 1);
    Move(rp, x, y); Draw(rp, x + KEYW, y); Draw(rp, x + KEYW, y + KEYH);
    Draw(rp, x, y + KEYH); Draw(rp, x, y);

    SetAPen(rp, sel ? 2 : 1);
    SetDrMd(rp, JAM1);
    if (sc && sc != c) { lbl[0] = sc; Move(rp, x + 6, y + 13);        Text(rp, lbl, 1); }
    if (c)             { lbl[0] = c;  Move(rp, x + 6, y + KEYH - 6);  Text(rp, lbl, 1); }
}

static void drawall(struct Window *w)
{
    struct RastPort *rp = w->RPort;
    int bx = w->BorderLeft + MX, by = w->BorderTop + MY;
    static const char *hint = "Click a key, then type its new character. Live; close to keep.";
    int r, k;

    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);
    Move(rp, bx, w->BorderTop + 16);
    Text(rp, (STRPTR)hint, strlen(hint));

    for (r = 0; r < 4; r++)
    {
        int y = by + r * (KEYH + GAP);
        for (k = 0; k < rows[r].n; k++)
        {
            int x = bx + rows[r].xoff + k * (KEYW + GAP);
            drawkey(rp, x, y, rows[r].keys[k], gSel == ((r << 8) | k));
        }
    }
}

static void closeall(struct Window *w)
{
    if (w) CloseWindow(w);
    if (KeymapBase)    CloseLibrary(KeymapBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
}

int main(void)
{
    struct Window *win;
    BOOL done = FALSE;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    KeymapBase    = OpenLibrary("keymap.library", 0);
    if (!IntuitionBase || !GfxBase || !KeymapBase) { closeall(NULL); return 20; }

    if (!takeover()) { closeall(NULL); return 20; }

    win = OpenWindowTags(NULL,
        WA_Left,   (IPTR)60,
        WA_Top,    (IPTR)40,
        WA_Width,  (IPTR)560,
        WA_Height, (IPTR)210,
        WA_Title,  (IPTR)"Keymap Editor",
        WA_IDCMP,  (IPTR)(IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
                          IDCMP_MOUSEBUTTONS | IDCMP_VANILLAKEY),
        WA_Flags,  (IPTR)(WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                          WFLG_ACTIVATE | WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH),
        TAG_DONE);
    if (!win) { closeall(NULL); return 20; }

    drawall(win);

    while (!done)
    {
        struct IntuiMessage *msg;
        WaitPort(win->UserPort);
        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            ULONG cl   = msg->Class;
            UWORD code = msg->Code;
            UWORD qual = msg->Qualifier;
            WORD  mx   = msg->MouseX, my = msg->MouseY;
            ReplyMsg((struct Message *)msg);

            switch (cl)
            {
            case IDCMP_CLOSEWINDOW:
                done = TRUE;
                break;
            case IDCMP_REFRESHWINDOW:
                BeginRefresh(win);
                drawall(win);
                EndRefresh(win, TRUE);
                break;
            case IDCMP_MOUSEBUTTONS:
                if (code == SELECTDOWN)
                {
                    gSel = hittest(win, mx, my);
                    drawall(win);
                }
                break;
            case IDCMP_VANILLAKEY:
                if (gSel >= 0 && code >= 32 && code < 127)
                {
                    int r = gSel >> 8, k = gSel & 0xFF;
                    UBYTE rk = rows[r].keys[k];
                    if (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT))
                        setkey(rk, IEQUALIFIER_LSHIFT, (UBYTE)code);
                    else
                        setbase(rk, (UBYTE)code);
                    drawall(win);
                }
                break;
            }
        }
    }

    closeall(win);
    return 0;
}
