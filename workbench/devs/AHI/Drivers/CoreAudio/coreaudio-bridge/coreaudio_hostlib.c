/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include "coreaudio_hostlib.h"

#include <proto/exec.h>
#include <proto/hostlib.h>

#include <aros/debug.h>

#define LIBCOREAUDIO_SOFILE "libcoreaudio.dylib"

static const char *coreaudio_func_names[] = {
    "ca_open",
    "ca_set_format",
    "ca_enable_live_output",
    "ca_start",
    "ca_stop",
    "ca_close",
    "ca_ring_push",
    "ca_ring_space",
    "ca_ring_capacity",
    "ca_get_stats"
};

#define COREAUDIO_NUM_FUNCS (sizeof(coreaudio_func_names) / sizeof(coreaudio_func_names[0]))

struct coreaudio_func coreaudio_func;

APTR HostLibBase;
static void *libcoreaudiohandle;

static void *hostlib_load_so(const char *sofile, const char **names, int nfuncs,
                             void **funcptr)
{
    void *handle;
    char *err;
    int i;

    if((handle = HostLib_Open(sofile, &err)) == NULL) {
        bug("[CoreAudio] failed to open '%s': %s\n", sofile, err);
        return NULL;
    }

    for(i = 0; i < nfuncs; i++) {
        funcptr[i] = HostLib_GetPointer(handle, names[i], &err);
        if(err != NULL) {
            bug("[CoreAudio] failed to get symbol '%s' (%s)\n", names[i], err);
            HostLib_Close(handle, NULL);
            return NULL;
        }
    }

    return handle;
}

BOOL COREAUDIO_HostLib_Init(void)
{
    HostLibBase = OpenResource("hostlib.resource");
    if(!HostLibBase) {
        bug("[CoreAudio] failed to open hostlib.resource\n");
        return FALSE;
    }

    libcoreaudiohandle = hostlib_load_so(LIBCOREAUDIO_SOFILE, coreaudio_func_names,
                                         COREAUDIO_NUM_FUNCS,
                                         (void **)&coreaudio_func);

    if(!libcoreaudiohandle) {
        bug("[CoreAudio] failed to open " LIBCOREAUDIO_SOFILE "\n");
        return FALSE;
    }

    return TRUE;
}

VOID COREAUDIO_HostLib_Cleanup(void)
{
    if(libcoreaudiohandle != NULL) {
        HostLib_Close(libcoreaudiohandle, NULL);
        libcoreaudiohandle = NULL;
    }
}
