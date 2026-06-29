/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>

#include "coreaudio_hostlib.h"
#include "coreaudio.h"

#define RING_FRAMES 8192

BOOL COREAUDIO_Init(void)
{
    return COREAUDIO_HostLib_Init();
}

VOID COREAUDIO_Cleanup(void)
{
    COREAUDIO_HostLib_Cleanup();
}

VOID COREAUDIO_MixerInit(APTR *handle, APTR *elem, LONG *min, LONG *max)
{
    *handle = NULL;
    *elem = NULL;
    *min = 0;
    *max = 0;
}

VOID COREAUDIO_MixerCleanup(APTR handle)
{
    (void)handle;
}

LONG COREAUDIO_MixerGetVolume(APTR elem)
{
    (void)elem;
    return 0;
}

VOID COREAUDIO_MixerSetVolume(APTR elem, LONG volume)
{
    (void)elem;
    (void)volume;
}

APTR COREAUDIO_Open(void)
{
    APTR handle = CACALL(ca_open, RING_FRAMES);
    if(handle == NULL)
        bug("[CoreAudio] ca_open failed\n");
    return handle;
}

VOID COREAUDIO_DropAndClose(APTR handle)
{
    if(handle != NULL) {
        CACALL(ca_stop, handle);
        CACALL(ca_close, handle);
    }
}

BOOL COREAUDIO_SetHWParams(APTR handle, ULONG *rate)
{
    unsigned r;

    if(handle == NULL || rate == NULL)
        return FALSE;

    if(CACALL(ca_enable_live_output, handle, 1) != 0) {
        bug("[CoreAudio] ca_enable_live_output failed\n");
        return FALSE;
    }

    r = *rate;
    if(CACALL(ca_set_format, handle, &r) != 0) {
        bug("[CoreAudio] ca_set_format failed\n");
        return FALSE;
    }

    *rate = r;
    return TRUE;
}

IPTR COREAUDIO_Start(APTR handle)
{
    if(handle == NULL)
        return FALSE;
    return CACALL(ca_start, handle) == 0;
}

IPTR COREAUDIO_Stop(APTR handle)
{
    if(handle != NULL)
        CACALL(ca_stop, handle);
    return TRUE;
}

LONG COREAUDIO_Write(APTR handle, APTR buffer, ULONG frames)
{
    if(handle == NULL || buffer == NULL || frames == 0)
        return 0;
    return CACALL(ca_ring_push, handle, buffer, frames);
}

LONG COREAUDIO_Avail(APTR handle)
{
    if(handle == NULL)
        return 0;
    return CACALL(ca_ring_space, handle);
}

VOID COREAUDIO_Prepare(APTR handle)
{
    (void)handle;
}
