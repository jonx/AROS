/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Cocoa/Metal display driver startup (darwin-aarch64 hosted).

    Loaded as a Monitors driver. Loads cocoametal.dylib, builds the CocoaGfx +
    CocoaBM classes at runtime (CLID_HiddMeta), registers the display with
    graphics.library and stays resident. Input (kbd/mouse) is added separately
    once the window renders (INTERFACE.md D5).
*/

#define DEBUG 1

#include <aros/debug.h>
#include <dos/dosextens.h>
#include <hidd/gfx.h>
#include <hidd/hidd.h>
#include <graphics/gfxbase.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/oop.h>

#include "cocoa_intern.h"

struct Library *OOPBase;
struct Library *UtilityBase;

OOP_AttrBase MetaAttrBase;
OOP_AttrBase HiddAttrBase;
OOP_AttrBase HiddPixFmtAttrBase;
OOP_AttrBase HiddBitMapAttrBase;
OOP_AttrBase HiddColorMapAttrBase;
OOP_AttrBase HiddSyncAttrBase;
OOP_AttrBase HiddGfxAttrBase;
OOP_AttrBase HiddDisplayAttrBase;
OOP_AttrBase HiddDMEnumAttrBase;
OOP_AttrBase HiddChunkyBMAttrBase;

static struct OOP_ABDescr attrbases[] =
{
    { IID_Meta,          &MetaAttrBase         },
    { IID_Hidd,          &HiddAttrBase         },
    { IID_Hidd_PixFmt,   &HiddPixFmtAttrBase   },
    { IID_Hidd_BitMap,   &HiddBitMapAttrBase   },
    { IID_Hidd_ColorMap, &HiddColorMapAttrBase },
    { IID_Hidd_Sync,     &HiddSyncAttrBase     },
    { IID_Hidd_Gfx,      &HiddGfxAttrBase      },
    { IID_Hidd_Display,  &HiddDisplayAttrBase  },
    { IID_Hidd_DMEnum,   &HiddDMEnumAttrBase   },
    { IID_Hidd_ChunkyBM, &HiddChunkyBMAttrBase },
    { NULL,              NULL                  }
};

/* The single driver-global state. */
struct cocoahidd xsd = { NULL };

static int cocoa_Startup(struct cocoahidd *xsd)
{
    struct GfxBase *GfxBase;
    ULONG err;

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 41);
    if (!GfxBase)
        return FALSE;

    xsd->basebm = OOP_FindClass(CLID_Hidd_BitMap);

    /* One Cocoa display, registered as a normal (non-boot) display driver so it
       supersedes the headless fallback when present. */
#ifdef COCOA_SKIP_ADDDISPLAY
    err = 0;   /* BISECT: skip registration to isolate crash (dylib vs register) */
    D(bug("[Cocoa] AddDisplayDriverA SKIPPED (bisect) -> %u\n", err));
#else
    err = AddDisplayDriverA(xsd->gfxclass, NULL, NULL);
    D(bug("[Cocoa] AddDisplayDriverA() = %u\n", err));
#endif

    CloseLibrary(&GfxBase->LibNode);
    return err ? FALSE : TRUE;
}

int __nocommandline = 1;

int main(void)
{
    int ret = RETURN_FAIL;

    OOPBase = OpenLibrary("oop.library", 42);
    if (!OOPBase)
        return RETURN_FAIL;

    /* Already registered? The user started us twice -- harmless. */
    if (OOP_FindClass(CLID_Hidd_Gfx_Cocoa))
    {
        CloseLibrary(OOPBase);
        return RETURN_OK;
    }

    UtilityBase = OpenLibrary("utility.library", 36);
    if (!UtilityBase)
    {
        CloseLibrary(OOPBase);
        return RETURN_FAIL;
    }

    if (OOP_ObtainAttrBases(attrbases))
    {
        if (cocoa_hostlib_init(&xsd))
        {
            struct TagItem CocoaGfx_tags[] =
            {
                { aMeta_SuperID,        (IPTR)CLID_Hidd_Gfx       },
                { aMeta_InterfaceDescr, (IPTR)CocoaGfx_ifdescr    },
                { aMeta_InstSize,       sizeof(struct gfxdata)    },
                { aMeta_ID,             (IPTR)CLID_Hidd_Gfx_Cocoa },
                { TAG_DONE,             0                         }
            };

            xsd.gfxclass = OOP_NewObject(NULL, CLID_HiddMeta, CocoaGfx_tags);
            if (xsd.gfxclass)
            {
                struct TagItem CocoaBM_tags[] =
                {
                    { aMeta_SuperID,        (IPTR)CLID_Hidd_ChunkyBM },
                    { aMeta_InterfaceDescr, (IPTR)CocoaBM_ifdescr    },
                    { aMeta_InstSize,       sizeof(struct bmdata)    },
                    { TAG_DONE,             0                        }
                };

                struct TagItem CocoaDisplay_tags[] =
                {
                    { aMeta_SuperID,        (IPTR)CLID_Hidd_Display       },
                    { aMeta_InterfaceDescr, (IPTR)CocoaGfx_Display_ifdescr },
                    { aMeta_InstSize,       0                             },
                    { aMeta_ID,             (IPTR)CLID_Hidd_Display_Cocoa },
                    { TAG_DONE,             0                             }
                };

                xsd.gfxclass->UserData = &xsd;

                /* gfx HIDD refactor: CreateObject/Show + the mode DB now live on
                   a Hidd_Display subclass. Register it before the gfx New runs
                   (CocoaGfx::New instantiates one from xsd.displayclass). */
                xsd.displayclass = OOP_NewObject(NULL, CLID_HiddMeta, CocoaDisplay_tags);
                if (xsd.displayclass)
                    xsd.displayclass->UserData = &xsd;

                xsd.bmclass = OOP_NewObject(NULL, CLID_HiddMeta, CocoaBM_tags);
                if (xsd.bmclass)
                {
                    xsd.bmclass->UserData = &xsd;

                    if (cocoa_Startup(&xsd))
                    {
                        struct Process *me = (struct Process *)FindTask(NULL);

                        /* Register the gfx class publicly (double-start guard). */
                        OOP_AddClass(xsd.gfxclass);

                        /* Add keyboard + mouse input. Non-fatal: a display with
                           no input still beats no display. */
                        if (cocoa_input_init(&xsd))
                            D(bug("[Cocoa] input (kbd+mouse) registered\n"));
                        else
                            D(bug("[Cocoa] input registration skipped\n"));

                        /* Start the clipboard bridge (NSPasteboard <-> PRIMARY_CLIP).
                           Non-fatal: no libpasteboard.dylib -> just no clipboard sync. */
                        if (cocoa_clipboard_init(&xsd))
                            D(bug("[Cocoa] clipboard bridge task started\n"));
                        else
                            D(bug("[Cocoa] clipboard bridge not started\n"));

                        /* Stay resident: detach our seglist so exiting this
                           process doesn't unload the driver. */
                        if (me->pr_CLI)
                        {
                            struct CommandLineInterface *cli = BADDR(me->pr_CLI);
                            cli->cli_Module = NULL;
                        }
                        else
                            me->pr_SegList = NULL;

                        D(bug("[Cocoa] driver resident, display registered\n"));
                        return RETURN_OK;
                    }
                    OOP_DisposeObject((OOP_Object *)xsd.bmclass);
                }
                if (xsd.displayclass)
                    OOP_DisposeObject((OOP_Object *)xsd.displayclass);
                OOP_DisposeObject((OOP_Object *)xsd.gfxclass);
            }
            cocoa_hostlib_expunge(&xsd);
        }
        else
            /* Not running on a macOS host (no dylib). Forgive and exit clean. */
            ret = RETURN_OK;

        OOP_ReleaseAttrBases(attrbases);
    }

    CloseLibrary(UtilityBase);
    CloseLibrary(OOPBase);
    return ret;
}
