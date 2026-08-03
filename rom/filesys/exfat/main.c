/*
 * exfat-handler - entry point
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <devices/trackdisk.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "exfat_fs.h"
#include "exfat_protos.h"

/*
 * exfat_fs.h defines SysBase as glob->gl_SysBase for the rest of the
 * handler, where a Globals is always in scope. It is not in scope here: this
 * file receives the ExecBase as an argument before any Globals exists, so the
 * macro has to go. The fat handler does the same for the same reason.
 */
#undef SysBase

#define DEBUG DEBUG_MISC
#include "exfat_debug.h"

/*
 * Phase 1 skeleton.
 *
 * This brings the handler up, mounts the volume through the validated path in
 * volume.c, and answers packets with ERROR_ACTION_NOT_KNOWN until the packet
 * layer lands. Answering that way rather than DOSFALSE with no error is
 * deliberate and is spec R4: dos64.library distinguishes "unsupported" from
 * "answered zero" solely by the secondary result, and three upstream
 * regressions have been traced to handlers that got this wrong.
 */

static void ReplyPacket(struct DosPacket *dp, struct ExecBase *SysBase)
{
    struct MsgPort *rp = dp->dp_Port;
    struct Message *mn = dp->dp_Link;

    dp->dp_Port = &((struct Process *)FindTask(NULL))->pr_MsgPort;
    mn->mn_Node.ln_Name = (char *)dp;
    PutMsg(rp, mn);
}

/*
 * The exec inlines resolve SysBase from scope, so with the macro gone every
 * function here must supply one by name. Hence the parameter and the locals
 * below rather than glob->gl_SysBase at each call.
 */
static struct Globals *exfat_init(struct Process *proc, struct DosPacket *dp,
    struct ExecBase *SysBase)
{
    struct Globals *glob;
    struct FileSysStartupMsg *fssm;
    struct DosEnvec *de;

    glob = AllocMem(sizeof(struct Globals), MEMF_PUBLIC | MEMF_CLEAR);
    if (glob == NULL)
        return NULL;

    glob->gl_SysBase = SysBase;
    glob->gl_DOSBase = (struct DosLibrary *)
        TaggedOpenLibrary(TAGGEDOPEN_DOS);
    if (glob->gl_DOSBase == NULL)
        goto fail;

    glob->ourtask = (struct Task *)proc;
    glob->ourport = &proc->pr_MsgPort;

    glob->devnode = (struct DosList *)BADDR(dp->dp_Arg3);
    fssm = (struct FileSysStartupMsg *)BADDR(dp->dp_Arg2);
    glob->fssm = fssm;

    if (fssm == NULL || fssm->fssm_Environ == BNULL)
        goto fail;

    de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);

    glob->diskport = CreateMsgPort();
    if (glob->diskport == NULL)
        goto fail;

    glob->diskioreq = (struct IOExtTD *)
        CreateIORequest(glob->diskport, sizeof(struct IOExtTD));
    if (glob->diskioreq == NULL)
        goto fail;

    if (OpenDevice(AROS_BSTR_ADDR(fssm->fssm_Device), fssm->fssm_Unit,
        (struct IORequest *)glob->diskioreq, fssm->fssm_Flags) != 0)
        goto fail;

    /*
     * Establishes readcmd/writecmd. Every access above 4 GB depends on this
     * having run, so it happens before anything is read.
     */
    Probe64BitSupport(glob);

    NewList((struct List *)&glob->sblist);

    D(bug("[exfat] init: device %b unit %ld, %lu-byte blocks\n",
        fssm->fssm_Device, (long)fssm->fssm_Unit,
        (unsigned long)(de->de_SizeBlock << 2)));

    return glob;

fail:
    if (glob->diskioreq != NULL)
        DeleteIORequest((struct IORequest *)glob->diskioreq);
    if (glob->diskport != NULL)
        DeleteMsgPort(glob->diskport);
    if (glob->gl_DOSBase != NULL)
        CloseLibrary((struct Library *)glob->gl_DOSBase);
    FreeMem(glob, sizeof(struct Globals));
    return NULL;
}

static void exfat_exit(struct Globals *glob)
{
    struct ExecBase *SysBase = glob->gl_SysBase;

    DoDiskRemove(glob);

    if (glob->diskioreq != NULL)
    {
        CloseDevice((struct IORequest *)glob->diskioreq);
        DeleteIORequest((struct IORequest *)glob->diskioreq);
    }
    if (glob->diskport != NULL)
        DeleteMsgPort(glob->diskport);
    if (glob->gl_DOSBase != NULL)
        CloseLibrary((struct Library *)glob->gl_DOSBase);

    FreeMem(glob, sizeof(struct Globals));
}

/* Answer every packet as not-implemented until the packet layer exists. */
static void ProcessPackets(struct Globals *glob)
{
    struct ExecBase *SysBase = glob->gl_SysBase;
    struct Message *msg;
    struct DosPacket *dp;

    while ((msg = GetMsg(glob->ourport)) != NULL)
    {
        dp = (struct DosPacket *)msg->mn_Node.ln_Name;

        switch (dp->dp_Type)
        {
        case ACTION_DIE:
            glob->quit = TRUE;
            dp->dp_Res1 = DOSTRUE;
            dp->dp_Res2 = 0;
            break;

        case ACTION_IS_FILESYSTEM:
            dp->dp_Res1 = DOSTRUE;
            dp->dp_Res2 = 0;
            break;

        default:
            /* Spec R4: never DOSFALSE with no error. */
            dp->dp_Res1 = DOSFALSE;
            dp->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
            break;
        }

        ReplyPacket(dp, SysBase);
    }
}

LONG handler(struct ExecBase *SysBase)
{
    struct Globals *glob;
    struct Process *proc;
    struct MsgPort *mp;
    struct DosPacket *dp;

    proc = (struct Process *)FindTask(NULL);
    mp = &proc->pr_MsgPort;
    WaitPort(mp);
    dp = (struct DosPacket *)GetMsg(mp)->mn_Node.ln_Name;

    glob = exfat_init(proc, dp, SysBase);
    if (glob == NULL)
    {
        dp->dp_Res1 = DOSFALSE;
        dp->dp_Res2 = ERROR_NO_FREE_STORE;
        ReplyPacket(dp, SysBase);
        return RETURN_FAIL;
    }

    glob->devnode->dol_Task = glob->ourport;

    dp->dp_Res1 = DOSTRUE;
    dp->dp_Res2 = 0;
    ReplyPacket(dp, SysBase);

    /* Mount whatever is already in the drive. */
    glob->disk_inserted = TRUE;
    DoDiskInsert(glob);

    while (!glob->quit)
    {
        Wait(1UL << glob->ourport->mp_SigBit);
        ProcessPackets(glob);
    }

    exfat_exit(glob);

    return RETURN_OK;
}
