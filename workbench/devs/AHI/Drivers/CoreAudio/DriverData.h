#ifndef AHI_Drivers_CoreAudio_DriverData_h
#define AHI_Drivers_CoreAudio_DriverData_h

#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "DriverBase.h"

struct CoreAudioBase {
    struct DriverBase driverbase;
    struct DosLibrary *dosbase;
};

#define DRIVERBASE_SIZEOF (sizeof(struct CoreAudioBase))

#define DOSBase (*(struct DosLibrary **) &CoreAudioBase->dosbase)

struct CoreAudioData {
    struct DriverData driverdata;
    UBYTE flags;
    UBYTE pad1;
    BYTE mastersignal;
    BYTE slavesignal;
    struct Process *mastertask;
    struct Process *slavetask;
    struct CoreAudioBase *ahisubbase;
    APTR mixbuffer;
    APTR audiohandle;
};

#endif /* AHI_Drivers_CoreAudio_DriverData_h */
