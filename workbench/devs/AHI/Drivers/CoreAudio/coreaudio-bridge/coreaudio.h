#ifndef AHI_Drivers_CoreAudio_bridge_coreaudio_h
#define AHI_Drivers_CoreAudio_bridge_coreaudio_h

#include <exec/types.h>

BOOL COREAUDIO_Init(void);
VOID COREAUDIO_Cleanup(void);

VOID COREAUDIO_MixerInit(APTR *handle, APTR *elem, LONG *min, LONG *max);
VOID COREAUDIO_MixerCleanup(APTR handle);
LONG COREAUDIO_MixerGetVolume(APTR elem);
VOID COREAUDIO_MixerSetVolume(APTR elem, LONG volume);

APTR COREAUDIO_Open(void);
VOID COREAUDIO_DropAndClose(APTR handle);

BOOL COREAUDIO_SetHWParams(APTR handle, ULONG *rate);
IPTR COREAUDIO_Start(APTR handle);
IPTR COREAUDIO_Stop(APTR handle);

LONG COREAUDIO_Write(APTR handle, APTR buffer, ULONG frames);
LONG COREAUDIO_Avail(APTR handle);
VOID COREAUDIO_Prepare(APTR handle);

#endif /* AHI_Drivers_CoreAudio_bridge_coreaudio_h */
