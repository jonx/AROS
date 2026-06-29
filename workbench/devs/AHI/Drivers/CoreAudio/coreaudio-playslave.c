/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>
#include <config.h>

#include <devices/ahi.h>
#include <exec/execbase.h>
#include <libraries/ahi_sub.h>

#include "DriverData.h"
#include "library.h"
#include "coreaudio-bridge/coreaudio.h"

#define dd ((struct CoreAudioData *) AudioCtrl->ahiac_DriverData)
#define min(a, b) ((a) < (b) ? (a) : (b))

#undef SysBase

void Slave(struct ExecBase *SysBase);

#include <aros/asmcall.h>

AROS_UFH3(LONG, SlaveEntry,
          AROS_UFHA(STRPTR, argPtr, A0),
          AROS_UFHA(ULONG, argSize, D0),
          AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT
    Slave(SysBase);
    return 0;
    AROS_USERFUNC_EXIT
}

#include <hardware/intbits.h>
#include <proto/timer.h>

AROS_INTH1(AHITimerTickCode, struct Task *, task)
{
    AROS_INTFUNC_INIT
    Signal(task, SIGBREAKF_CTRL_F);
    return 0;
    AROS_INTFUNC_EXIT
}

static void SmallDelay(struct ExecBase *SysBase)
{
    struct Interrupt i;

    i.is_Code = (APTR)AHITimerTickCode;
    i.is_Data = FindTask(0);
    i.is_Node.ln_Name = "AROS CoreAudio AHI Timer Tick Server";
    i.is_Node.ln_Pri = 0;
    i.is_Node.ln_Type = NT_INTERRUPT;

    SetSignal(0, SIGBREAKF_CTRL_F);
    AddIntServer(INTB_VERTB, &i);
    Wait(SIGBREAKF_CTRL_F);
    RemIntServer(INTB_VERTB, &i);
}

void
Slave(struct ExecBase *SysBase)
{
    struct AHIAudioCtrlDrv *AudioCtrl;
    struct DriverBase *AHIsubBase;
    BOOL running;
    ULONG signals;
    LONG framesready = 0;
    APTR framesptr = NULL;

    Wait(SIGF_SINGLE);

    AudioCtrl = (struct AHIAudioCtrlDrv *)FindTask(NULL)->tc_UserData;
    AHIsubBase = (struct DriverBase *)dd->ahisubbase;
    dd->slavesignal = AllocSignal(-1);

    if(dd->slavesignal != -1) {
        Signal((struct Task *)dd->mastertask, 1L << dd->mastersignal);

        running = TRUE;
        while(running) {
            signals = SetSignal(0L, 0L);

            if(signals & (SIGBREAKF_CTRL_C | (1L << dd->slavesignal))) {
                running = FALSE;
            } else {
                LONG framesfree = COREAUDIO_Avail(dd->audiohandle);

                if(framesfree <= 0) {
                    SmallDelay(SysBase);
                    continue;
                }

                framesfree = min(framesfree, (LONG)AudioCtrl->ahiac_BuffSamples);

                while(framesfree > 0) {
                    LONG written;

                    if(framesready == 0) {
                        CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);
                        CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, dd->mixbuffer);
                        framesready = AudioCtrl->ahiac_BuffSamples;
                        framesptr = dd->mixbuffer;
                    }

                    written = COREAUDIO_Write(dd->audiohandle, framesptr,
                                              min(framesready, framesfree));
                    if(written > 0) {
                        framesready -= written;
                        framesfree -= written;
                        framesptr += written * 4;
                        CallHookA(AudioCtrl->ahiac_PostTimerFunc,
                                  (Object *)AudioCtrl, 0);
                    } else {
                        break;
                    }
                }

                SmallDelay(SysBase);
            }
        }
    }

    if(dd->slavesignal != -1) {
        FreeSignal(dd->slavesignal);
        dd->slavesignal = -1;
    }

    Forbid();

    Signal((struct Task *)dd->mastertask, 1L << dd->mastersignal);
    dd->slavetask = NULL;
}
