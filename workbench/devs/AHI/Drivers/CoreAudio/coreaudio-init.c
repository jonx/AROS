#include <aros/debug.h>
#include <config.h>

#include "library.h"
#include "DriverData.h"

#include "coreaudio-bridge/coreaudio.h"

BOOL
DriverInit(struct DriverBase *AHIsubBase)
{
    struct CoreAudioBase *CoreAudioBase = (struct CoreAudioBase *) AHIsubBase;

    D(bug("[CoreAudio]: DriverInit()\n"));

    CoreAudioBase->dosbase = (struct DosLibrary *)OpenLibrary(DOSNAME, 37);
    if(CoreAudioBase->dosbase == NULL) {
        Req("Unable to open 'dos.library' version 37.\n");
        return FALSE;
    }

    if(!COREAUDIO_Init()) {
        CloseLibrary((struct Library *) DOSBase);
        CoreAudioBase->dosbase = NULL;
        return FALSE;
    }

    D(bug("[CoreAudio]: DriverInit() completed\n"));
    return TRUE;
}

VOID
DriverCleanup(struct DriverBase *AHIsubBase)
{
    struct CoreAudioBase *CoreAudioBase = (struct CoreAudioBase *) AHIsubBase;

    COREAUDIO_Cleanup();

    if(CoreAudioBase->dosbase != NULL) {
        CloseLibrary((struct Library *) DOSBase);
        CoreAudioBase->dosbase = NULL;
    }
}
