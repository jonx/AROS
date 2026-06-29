#ifndef AHI_Drivers_CoreAudio_bridge_hostlib_h
#define AHI_Drivers_CoreAudio_bridge_hostlib_h

#include <exec/types.h>

struct CAStats {
    unsigned long pushed;
    unsigned long consumed;
    unsigned long underruns;
    unsigned long rtAROSCalls;
};

struct coreaudio_func {
    APTR (*ca_open)(int ringFrames);
    int (*ca_set_format)(APTR ctx, unsigned *inOutRateHz);
    int (*ca_enable_live_output)(APTR ctx, int enabled);
    int (*ca_start)(APTR ctx);
    void (*ca_stop)(APTR ctx);
    void (*ca_close)(APTR ctx);
    int (*ca_ring_push)(APTR ctx, const short *src, int frames);
    int (*ca_ring_space)(APTR ctx);
    int (*ca_ring_capacity)(APTR ctx);
    void (*ca_get_stats)(APTR ctx, struct CAStats *out);
};

extern struct coreaudio_func coreaudio_func;

#define CACALL(func, ...) (coreaudio_func.func(__VA_ARGS__))

BOOL COREAUDIO_HostLib_Init(void);
VOID COREAUDIO_HostLib_Cleanup(void);

#endif /* AHI_Drivers_CoreAudio_bridge_hostlib_h */
