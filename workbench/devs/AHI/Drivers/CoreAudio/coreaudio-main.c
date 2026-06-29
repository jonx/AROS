#include <aros/debug.h>
#include <config.h>

#include <devices/ahi.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <libraries/ahi_sub.h>
#include <proto/ahi_sub.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <stddef.h>

#include "library.h"
#include "DriverData.h"
#include "coreaudio-bridge/coreaudio.h"

#define dd ((struct CoreAudioData *) AudioCtrl->ahiac_DriverData)

void SlaveEntry(void);

#ifdef PROCGW
PROCGW(static, void, slaveentry, SlaveEntry);
#else
#define slaveentry SlaveEntry
#endif

static const LONG frequencies[] = {
    8000,
    11025,
    22050,
    44100,
    48000
};

#define FREQUENCIES (sizeof frequencies / sizeof frequencies[0])

ULONG
_AHIsub_AllocAudio(struct TagItem *taglist,
                   struct AHIAudioCtrlDrv *AudioCtrl,
                   struct DriverBase *AHIsubBase)
{
    struct CoreAudioBase *CoreAudioBase = (struct CoreAudioBase *) AHIsubBase;
    ULONG freq = AudioCtrl->ahiac_MixFreq;

    D(bug("[CoreAudio]: AllocAudio enter\n"));

    AudioCtrl->ahiac_DriverData = AllocVec(sizeof(struct CoreAudioData),
                                           MEMF_CLEAR | MEMF_PUBLIC);
    if(dd == NULL)
        return AHISF_ERROR;

    dd->slavesignal = -1;
    dd->mastersignal = AllocSignal(-1);
    dd->mastertask = (struct Process *)FindTask(NULL);
    dd->ahisubbase = CoreAudioBase;

    if(dd->mastersignal == -1)
        return AHISF_ERROR;

    dd->audiohandle = COREAUDIO_Open();
    if(dd->audiohandle == NULL) {
        bug("[CoreAudio]: failed opening CoreAudio host shim\n");
        return AHISF_ERROR;
    }

    if(!COREAUDIO_SetHWParams(dd->audiohandle, &freq)) {
        bug("[CoreAudio]: failed setting CoreAudio hardware parameters\n");
        COREAUDIO_DropAndClose(dd->audiohandle);
        dd->audiohandle = NULL;
        return AHISF_ERROR;
    }

    AudioCtrl->ahiac_MixFreq = freq;

    D(bug("[CoreAudio]: AllocAudio completed\n"));

    return AHISF_KNOWSTEREO | AHISF_MIXING | AHISF_TIMING;
}

void
_AHIsub_FreeAudio(struct AHIAudioCtrlDrv *AudioCtrl,
                  struct DriverBase *AHIsubBase)
{
    if(AudioCtrl->ahiac_DriverData != NULL) {
        COREAUDIO_DropAndClose(dd->audiohandle);
        FreeSignal(dd->mastersignal);
        FreeVec(AudioCtrl->ahiac_DriverData);
        AudioCtrl->ahiac_DriverData = NULL;
    }
}

void
_AHIsub_Disable(struct AHIAudioCtrlDrv *AudioCtrl,
                struct DriverBase *AHIsubBase)
{
    Forbid();
}

void
_AHIsub_Enable(struct AHIAudioCtrlDrv *AudioCtrl,
               struct DriverBase *AHIsubBase)
{
    Permit();
}

ULONG
_AHIsub_Start(ULONG flags, struct AHIAudioCtrlDrv *AudioCtrl,
              struct DriverBase *AHIsubBase)
{
    struct CoreAudioBase *CoreAudioBase = (struct CoreAudioBase *) AHIsubBase;

    AHIsub_Stop(flags, AudioCtrl);

    if(flags & AHISF_PLAY) {
        struct TagItem proctags[] = {
            { NP_Entry, (IPTR)&slaveentry },
            { NP_Name, (IPTR)LibName },
            { NP_Priority, 127 },
            { TAG_DONE, 0 }
        };

        dd->mixbuffer = AllocVec(AudioCtrl->ahiac_BuffSize,
                                 MEMF_ANY | MEMF_PUBLIC);
        if(dd->mixbuffer == NULL)
            return AHIE_NOMEM;

        if(!COREAUDIO_Start(dd->audiohandle)) {
            FreeVec(dd->mixbuffer);
            dd->mixbuffer = NULL;
            return AHIE_UNKNOWN;
        }

        dd->slavetask = CreateNewProc(proctags);
        if(dd->slavetask != NULL) {
            dd->slavetask->pr_Task.tc_UserData = AudioCtrl;
            Signal((struct Task *)dd->slavetask, SIGF_SINGLE);
            Wait(1L << dd->mastersignal);

            if(dd->slavetask == NULL)
                return AHIE_UNKNOWN;
        } else {
            COREAUDIO_Stop(dd->audiohandle);
            FreeVec(dd->mixbuffer);
            dd->mixbuffer = NULL;
            return AHIE_NOMEM;
        }
    }

    if(flags & AHISF_RECORD)
        return AHIE_UNKNOWN;

    return AHIE_OK;
}

void
_AHIsub_Update(ULONG flags, struct AHIAudioCtrlDrv *AudioCtrl,
               struct DriverBase *AHIsubBase)
{
}

void
_AHIsub_Stop(ULONG flags, struct AHIAudioCtrlDrv *AudioCtrl,
             struct DriverBase *AHIsubBase)
{
    if(flags & AHISF_PLAY) {
        if(dd->slavetask != NULL) {
            if(dd->slavesignal != -1) {
                Signal((struct Task *)dd->slavetask, 1L << dd->slavesignal);
                D(bug("[CoreAudio]: AHIsub_Stop\n"));
            }

            Wait(1L << dd->mastersignal);
        }

        COREAUDIO_Stop(dd->audiohandle);
        FreeVec(dd->mixbuffer);
        dd->mixbuffer = NULL;
    }
}

IPTR
_AHIsub_GetAttr(ULONG attribute, LONG argument, IPTR def,
                struct TagItem *taglist,
                struct AHIAudioCtrlDrv *AudioCtrl,
                struct DriverBase *AHIsubBase)
{
    size_t i;

    switch(attribute) {
    case AHIDB_Bits:
        return 16;

    case AHIDB_Frequencies:
        return FREQUENCIES;

    case AHIDB_Frequency:
        return (LONG)frequencies[argument];

    case AHIDB_Index:
        if(argument <= frequencies[0])
            return 0;
        if(argument >= frequencies[FREQUENCIES - 1])
            return FREQUENCIES - 1;

        for(i = 1; i < FREQUENCIES; i++) {
            if(frequencies[i] > argument) {
                if((argument - frequencies[i - 1]) < (frequencies[i] - argument))
                    return i - 1;
                return i;
            }
        }
        return 0;

    case AHIDB_Author:
        return (IPTR)"The AROS Dev Team";

    case AHIDB_Copyright:
        return (IPTR)"APL";

    case AHIDB_Version:
        return (IPTR)LibIDString;

    case AHIDB_Record:
        return FALSE;

    case AHIDB_Realtime:
        return TRUE;

    case AHIDB_Outputs:
        return 1;

    case AHIDB_Output:
        return (IPTR)"CoreAudio";

    case AHIDB_MinOutputVolume:
        return 0;

    case AHIDB_MaxOutputVolume:
        return 0;

    default:
        return def;
    }
}

ULONG
_AHIsub_HardwareControl(ULONG attribute, LONG argument,
                        struct AHIAudioCtrlDrv *AudioCtrl,
                        struct DriverBase *AHIsubBase)
{
    switch(attribute) {
    case AHIC_OutputVolume:
        return TRUE;

    case AHIC_OutputVolume_Query:
        return 0;
    }

    return 0;
}
