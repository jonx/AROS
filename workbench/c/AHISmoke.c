/*
    AHISmoke - direct ahi.device playback smoke test.

    This intentionally avoids datatypes and file I/O: it generates one
    quiet 16-bit stereo tone and plays it through the CoreAudio AHI mode
    used by the Darwin/aarch64 hosted port.
*/

#include <devices/ahi.h>
#include <dos/dos.h>
#include <exec/memory.h>

#include <proto/ahi.h>
#include <proto/dos.h>
#include <proto/exec.h>

#define COREAUDIO_MODE_ID 0x00450002UL
#define MIX_FREQ         44100UL
#define SECONDS          1UL
#define FRAMES           (MIX_FREQ * SECONDS)
#define TONE_HZ          440UL
#define TABLE_SIZE       32UL
#define TABLE_STEP       ((TONE_HZ * TABLE_SIZE * 65536UL) / MIX_FREQ)

struct Library *AHIBase = NULL;

static const WORD ToneTable[TABLE_SIZE] = {
    0, 1170, 2296, 3334, 4242, 4988, 5543, 5880,
    5993, 5880, 5543, 4988, 4242, 3334, 2296, 1170,
    0, -1170, -2296, -3334, -4242, -4988, -5543, -5880,
    -5993, -5880, -5543, -4988, -4242, -3334, -2296, -1170
};

static void FillTone(WORD *sample)
{
    ULONG i;
    ULONG phase = 0;

    for(i = 0; i < FRAMES; ++i) {
        WORD value = ToneTable[(phase >> 16) & (TABLE_SIZE - 1)];
        sample[i * 2 + 0] = value;
        sample[i * 2 + 1] = value;
        phase += TABLE_STEP;
    }
}

int main(void)
{
    struct MsgPort *port;
    struct AHIRequest *io;
    struct AHIAudioCtrl *actrl = NULL;
    WORD *sample = NULL;
    int rc = RETURN_FAIL;

    port = CreateMsgPort();
    if(port == NULL) {
        Printf("AHISmoke: failed to create message port\n");
        return RETURN_FAIL;
    }

    io = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if(io == NULL) {
        Printf("AHISmoke: failed to create IO request\n");
        DeleteMsgPort(port);
        return RETURN_FAIL;
    }

    io->ahir_Version = 4;
    if(OpenDevice(AHINAME, AHI_NO_UNIT, (struct IORequest *)io, 0) != 0) {
        Printf("AHISmoke: failed to open %s v4+\n", AHINAME);
        goto out;
    }
    AHIBase = (struct Library *)io->ahir_Std.io_Device;

    sample = AllocVec(FRAMES * 2 * sizeof(WORD), MEMF_PUBLIC);
    if(sample == NULL) {
        Printf("AHISmoke: failed to allocate sample buffer\n");
        goto out;
    }
    FillTone(sample);

    actrl = AHI_AllocAudio(AHIA_AudioID,  COREAUDIO_MODE_ID,
                           AHIA_MixFreq,  MIX_FREQ,
                           AHIA_Channels, 1,
                           AHIA_Sounds,   1,
                           TAG_DONE);
    if(actrl == NULL) {
        Printf("AHISmoke: failed to allocate CoreAudio mode 0x%08lx\n", COREAUDIO_MODE_ID);
        goto out;
    }

    {
        struct AHISampleInfo info = { AHIST_S16S, sample, FRAMES };

        if(AHI_LoadSound(0, AHIST_SAMPLE, &info, actrl) != AHIE_OK) {
            Printf("AHISmoke: failed to load sample\n");
            goto out;
        }
    }

    if(AHI_ControlAudio(actrl, AHIC_Play, TRUE, TAG_DONE) != AHIE_OK) {
        Printf("AHISmoke: failed to start playback\n");
        goto out;
    }

    AHI_Play(actrl,
             AHIP_BeginChannel, 0,
             AHIP_Sound,        0,
             AHIP_Freq,         MIX_FREQ,
             AHIP_Vol,          0x10000,
             AHIP_Pan,          0x8000,
             AHIP_EndChannel,   0,
             TAG_DONE);

    Delay(50 * SECONDS);
    AHI_ControlAudio(actrl, AHIC_Play, FALSE, TAG_DONE);

    Printf("AHISmoke: OK CoreAudio tone played for %ld second\n", SECONDS);
    rc = RETURN_OK;

out:
    if(actrl != NULL) {
        AHI_FreeAudio(actrl);
    }
    if(sample != NULL) {
        FreeVec(sample);
    }
    if(AHIBase != NULL) {
        CloseDevice((struct IORequest *)io);
    }
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);

    return rc;
}
