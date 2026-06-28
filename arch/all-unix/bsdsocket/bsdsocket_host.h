/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The two host interfaces this library resolves via hostlib.resource:
          libSystem.dylib (the real BSD sockets) and libbsdsockhost.dylib (our
          kqueue readiness pump + a varargs-safe non-blocking helper).

    Only FIXED-ARG host functions are called directly from AROS: on aarch64 the
    AAPCS64 calling convention matches the host, so no H3 variadic-on-stack shim is
    needed for them. The one varargs call sockets need — fcntl(fd, F_SETFL,
    O_NONBLOCK) — is routed through libbsdsockhost.dylib's hs_set_nonblock/hs_socket
    (compiled host-side, correct ABI), which is why it appears in HostPumpIFace.
*/
#ifndef BSDSOCKET_HOST_H
#define BSDSOCKET_HOST_H

#include <exec/types.h>

/* Readiness directions — MUST match libbsdsockhost.dylib's bsdsock_host.h. */
#define PS_WANT_READ   (1u << 0)
#define PS_WANT_WRITE  (1u << 1)

struct PumpReady { int fd; unsigned ready; };

/* libSystem.dylib BSD sockets (fixed-arg). Field order == the symbol-name array
   passed to HostLib_GetInterface (bsdsocket_init.c). */
struct HostSockIFace
{
    int   (*socket)(int, int, int);
    int   (*bind)(int, const void *, unsigned int);
    int   (*listen)(int, int);
    int   (*accept)(int, void *, unsigned int *);
    int   (*connect)(int, const void *, unsigned int);
    long  (*send)(int, const void *, unsigned long, int);
    long  (*recv)(int, void *, unsigned long, int);
    long  (*sendto)(int, const void *, unsigned long, int, const void *, unsigned int);
    long  (*recvfrom)(int, void *, unsigned long, int, void *, unsigned int *);
    int   (*shutdown)(int, int);
    int   (*setsockopt)(int, int, int, const void *, unsigned int);
    int   (*getsockopt)(int, int, int, void *, unsigned int *);
    int   (*getsockname)(int, void *, unsigned int *);
    int   (*getpeername)(int, void *, unsigned int *);
    int   (*close)(int);
    int * (*__error)(void);     /* macOS errno location (thread-local) */
};

/* libbsdsockhost.dylib — our kqueue pump + varargs-safe non-blocking helpers.
   Field order == the symbol-name array in bsdsocket_init.c. */
struct HostPumpIFace
{
    int   (*pump_start)(void);
    void  (*pump_stop)(void);
    int   (*pump_register)(int, unsigned int, APTR);
    int   (*pump_unregister)(int, APTR);
    int   (*pump_drain)(APTR, struct PumpReady *, int);
    void  (*pump_wake_kqueue)(void);
    APTR  (*ps_create_cb)(void (*)(APTR), APTR);
    void  (*ps_destroy)(APTR);
    int   (*hs_set_nonblock)(int);
    int   (*hs_socket)(int, int, int);
    /* async DNS (getaddrinfo on a detached host thread; AROS timer-polls) */
    APTR  (*hs_resolve_start)(const char *name);
    int   (*hs_resolve_poll)(APTR job, unsigned *ip_net_out);
    void  (*hs_resolve_free)(APTR job);
};

#endif /* BSDSOCKET_HOST_H */
