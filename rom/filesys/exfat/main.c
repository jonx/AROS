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
#include <aros/asmcall.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <devices/trackdisk.h>
#include <libraries/locale.h>

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

/* Handler startup and packet reply plumbing. Unsupported packets use
   ERROR_ACTION_NOT_KNOWN: dos64.library distinguishes "unsupported" from
   "answered zero" solely by the secondary result. */

static void ReplyPacket(struct DosPacket *dp, struct ExecBase *SysBase)
{
    struct MsgPort *rp = dp->dp_Port;
    struct Message *mn = dp->dp_Link;

    dp->dp_Port = &((struct Process *)FindTask(NULL))->pr_MsgPort;
    mn->mn_Node.ln_Name = (char *)dp;
    PutMsg(rp, mn);
}

static AROS_INTH1(ExfatDiskChangeInt, struct IntData *, data)
{
    AROS_INTFUNC_INIT

    struct ExecBase *SysBase = data->SysBase;
    Signal(data->task, data->signal);
    return 0;

    AROS_INTFUNC_EXIT
}

/*
 * The exec inlines resolve SysBase from scope, so with the macro gone every
 * function here must supply one by name. Hence the parameter and the locals
 * below rather than glob->gl_SysBase at each call.
 */
static struct Globals *exfat_init(struct Process *proc, struct DosPacket *dp,
    struct ExecBase *SysBase, LONG *err)
{
    struct Globals *glob;
    struct FileSysStartupMsg *fssm;
    struct DosEnvec *de;
    LONG diskbit;
    LONG why = ERROR_NO_FREE_STORE;

    glob = AllocMem(sizeof(struct Globals), MEMF_PUBLIC | MEMF_CLEAR);
    if (glob == NULL)
        return NULL;

    glob->gl_SysBase = SysBase;
    glob->diskchgsig_bit = ~(ULONG)0;
    glob->gl_DOSBase = (struct DosLibrary *)
        TaggedOpenLibrary(TAGGEDOPEN_DOS);
    if (glob->gl_DOSBase == NULL)
        goto fail;
    /* Optional on classic targets.  Without locale.library, exFAT timestamps
       retain their stored local wall time and new offsets are marked unknown. */
    glob->gl_LocaleBase = (struct LocaleBase *)
        OpenLibrary("locale.library", 38);

    glob->ourtask = (struct Task *)proc;
    glob->ourport = &proc->pr_MsgPort;

    glob->devnode = (struct DosList *)BADDR(dp->dp_Arg3);
    fssm = (struct FileSysStartupMsg *)BADDR(dp->dp_Arg2);
    glob->fssm = fssm;

    if (fssm == NULL || fssm->fssm_Environ == BNULL)
    {
        why = ERROR_BAD_NUMBER;     /* nothing to mount from */
        goto fail;
    }

    de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);

    diskbit = AllocSignal(-1);
    if (diskbit < 0)
        goto fail;
    glob->diskchgsig_bit = (ULONG)diskbit;

    glob->diskport = CreateMsgPort();
    if (glob->diskport == NULL)
        goto fail;

    glob->diskioreq = (struct IOExtTD *)
        CreateIORequest(glob->diskport, sizeof(struct IOExtTD));
    if (glob->diskioreq == NULL)
        goto fail;

    if (OpenDevice(AROS_BSTR_ADDR(fssm->fssm_Device), fssm->fssm_Unit,
        (struct IORequest *)glob->diskioreq, fssm->fssm_Flags) != 0)
    {
        why = ERROR_DEVICE_NOT_MOUNTED;
        goto fail;
    }

    /*
     * Establishes readcmd/writecmd. Every access above 4 GB depends on this
     * having run, so it happens before anything is read.
     */
    Probe64BitSupport(glob);

    glob->diskchgreq = AllocVec(sizeof(struct IOExtTD), MEMF_PUBLIC);
    if (glob->diskchgreq == NULL)
        goto fail;
    CopyMem(glob->diskioreq, glob->diskchgreq, sizeof(struct IOExtTD));
    glob->DiskChangeIntData.SysBase = SysBase;
    glob->DiskChangeIntData.task = glob->ourtask;
    glob->DiskChangeIntData.signal = 1UL << glob->diskchgsig_bit;
    glob->DiskChangeIntData.Interrupt.is_Node.ln_Type = NT_INTERRUPT;
    glob->DiskChangeIntData.Interrupt.is_Node.ln_Pri = 0;
    glob->DiskChangeIntData.Interrupt.is_Node.ln_Name = "exFATFS";
    glob->DiskChangeIntData.Interrupt.is_Data = &glob->DiskChangeIntData;
    glob->DiskChangeIntData.Interrupt.is_Code =
        (VOID_FUNC)AROS_ASMSYMNAME(ExfatDiskChangeInt);
    glob->diskchgreq->iotd_Req.io_Command = TD_ADDCHANGEINT;
    glob->diskchgreq->iotd_Req.io_Data = &glob->DiskChangeIntData.Interrupt;
    glob->diskchgreq->iotd_Req.io_Length = sizeof(struct Interrupt);
    glob->diskchgreq->iotd_Req.io_Flags = 0;
    SendIO((struct IORequest *)glob->diskchgreq);

    NewList((struct List *)&glob->sblist);

    D(bug("[exfat] init: device %b unit %ld, %lu-byte blocks\n",
        fssm->fssm_Device, (long)fssm->fssm_Unit,
        (unsigned long)(de->de_SizeBlock << 2)));
    (void)de; /* DEBUG_MISC may compile the only diagnostic use out. */

    return glob;

fail:
    if (glob->diskchgreq != NULL)
        FreeVec(glob->diskchgreq);
    if (glob->diskioreq != NULL)
    {
        if (glob->diskioreq->iotd_Req.io_Device != NULL)
            CloseDevice((struct IORequest *)glob->diskioreq);
        DeleteIORequest((struct IORequest *)glob->diskioreq);
    }
    if (glob->diskport != NULL)
        DeleteMsgPort(glob->diskport);
    if (glob->gl_DOSBase != NULL)
        CloseLibrary((struct Library *)glob->gl_DOSBase);
    if (glob->gl_LocaleBase != NULL)
        CloseLibrary((struct Library *)glob->gl_LocaleBase);
    if (glob->diskchgsig_bit != ~(ULONG)0)
        FreeSignal((LONG)glob->diskchgsig_bit);
    FreeMem(glob, sizeof(struct Globals));
    *err = why;
    return NULL;
}

static void exfat_exit(struct Globals *glob)
{
    struct ExecBase *SysBase = glob->gl_SysBase;

    DoDiskRemove(glob);

    glob->diskchgreq->iotd_Req.io_Command = TD_REMCHANGEINT;
    glob->diskchgreq->iotd_Req.io_Data = &glob->DiskChangeIntData.Interrupt;
    glob->diskchgreq->iotd_Req.io_Length = sizeof(struct Interrupt);
    glob->diskchgreq->iotd_Req.io_Flags = 0;
    DoIO((struct IORequest *)glob->diskchgreq);
    FreeVec(glob->diskchgreq);

    if (glob->diskioreq != NULL)
    {
        CloseDevice((struct IORequest *)glob->diskioreq);
        DeleteIORequest((struct IORequest *)glob->diskioreq);
    }
    if (glob->diskport != NULL)
        DeleteMsgPort(glob->diskport);
    if (glob->gl_DOSBase != NULL)
        CloseLibrary((struct Library *)glob->gl_DOSBase);
    if (glob->gl_LocaleBase != NULL)
        CloseLibrary((struct Library *)glob->gl_LocaleBase);
    FreeSignal((LONG)glob->diskchgsig_bit);

    FreeMem(glob, sizeof(struct Globals));
}

LONG handler(struct ExecBase *SysBase)
{
    struct Globals *glob;
    struct Process *proc;
    struct MsgPort *mp;
    struct DosPacket *dp;
    LONG initerr = ERROR_NO_FREE_STORE;

    proc = (struct Process *)FindTask(NULL);
    mp = &proc->pr_MsgPort;
    WaitPort(mp);
    dp = (struct DosPacket *)GetMsg(mp)->mn_Node.ln_Name;

    glob = exfat_init(proc, dp, SysBase, &initerr);
    if (glob == NULL)
    {
        dp->dp_Res1 = DOSFALSE;
        dp->dp_Res2 = initerr;
        ReplyPacket(dp, SysBase);
        return RETURN_FAIL;
    }

    glob->devnode->dol_Task = glob->ourport;

    dp->dp_Res1 = DOSTRUE;
    dp->dp_Res2 = 0;
    ReplyPacket(dp, SysBase);

    /* Query the device rather than assuming media is present. */
    ProcessDiskChange(glob);

    while (!glob->quit)
    {
        ULONG sigs = Wait((1UL << glob->ourport->mp_SigBit)
            | (1UL << glob->diskchgsig_bit));
        if (sigs & (1UL << glob->diskchgsig_bit))
            ProcessDiskChange(glob);
        if (sigs & (1UL << glob->ourport->mp_SigBit))
            ExfatProcessPackets(glob);
    }

    exfat_exit(glob);

    return RETURN_OK;
}
