/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Prove an async reactor can wait on a socket AND a pipe in a single
          WaitSelect. WaitSelect is socket-only, so pipe read-readiness is
          delivered as an exec signal via the pipe handler's ACTION_PIPE_READ_NOTIFY
          (level-triggered). That signal rides in WaitSelect's sigmask beside socket
          readiness, so one Wait wakes for whichever is ready first -- and the
          reactor then does the read itself, exactly the epoll/mio shape.

          Needs a localhost echo server on 127.0.0.1:12345 and PIPE: mounted.
*/

#include <proto/exec.h>
#include <proto/dos.h>

#include <exec/ports.h>
#include <dos/dosextens.h>
#include <dos/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>

#include <aros/libcall.h>
typedef struct fd_set fd_set;            /* opaque, for the WaitSelect macro */
#include <defines/bsdsocket.h>

/* Must match pipe-handler.h. Arg1 = read fh's fh_Arg1, Arg2 = signal mask,
   Arg3 = task to signal (Arg2 = 0 deregisters). */
#define ACTION_PIPE_READ_NOTIFY 0x50524E31L

struct Library *SocketBase;

/* AmiTCP fd_set / timeval are binary { long }: FD_SETSIZE=64 -> one 64-bit word. */
struct fds { long w; };
#define FZERO(p)    ((p)->w = 0)
#define FSET(n, p)  ((p)->w |= (1L << (n)))
#define FISSET(n,p) ((p)->w &  (1L << (n)))
struct tvl { long sec, usec; };

static int connect_echo(void)
{
    struct sockaddr_in sa;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_port = 0x3930;                 /* 12345 */
    sa.sin_addr.s_addr = 0x0100007f;      /* 127.0.0.1 */
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { CloseSocket(s); return -1; }
    return s;
}

int main(void)
{
    BPTR wfh, rfh;
    struct FileHandle *rh;
    ULONG pipeSig;
    BYTE  sigbit;
    int   sock, fail = 0;
    LONG  rc;

    SocketBase = OpenLibrary("bsdsocket.library", 0);
    if (!SocketBase) { Printf("[RCT] FAIL: no bsdsocket.library\n"); return 20; }

    /* Hold both ends of a named pipe open -> no rendezvous lifetime race. */
    wfh = Open("PIPE:react", MODE_NEWFILE);
    if (!wfh) { Printf("[RCT] FAIL: open pipe write end, err %ld\n", (long)IoErr()); return 20; }
    rfh = Open("PIPE:react", MODE_OLDFILE);
    if (!rfh) { Printf("[RCT] FAIL: open pipe read end, err %ld\n", (long)IoErr()); Close(wfh); return 20; }
    rh = BADDR(rfh);

    sigbit = AllocSignal(-1);
    if (sigbit == -1) { Printf("[RCT] FAIL: AllocSignal\n"); Close(rfh); Close(wfh); return 20; }
    pipeSig = 1UL << sigbit;

    /* Register read-readiness notify: the handler signals pipeSig whenever the
       pipe is readable (and immediately if it already is). */
    rc = DoPkt(rh->fh_Type, ACTION_PIPE_READ_NOTIFY,
               (SIPTR)rh->fh_Arg1, (SIPTR)pipeSig, (SIPTR)FindTask(NULL), 0, 0);
    if (rc != 1) { Printf("[RCT] FAIL: register notify rc=%ld err=%ld\n", (long)rc, (long)IoErr()); fail = 1; }
    else           Printf("[RCT] PASS: registered pipe read-readiness notify\n");

    sock = connect_echo();
    if (sock < 0) { Printf("[RCT] FAIL: connect echo, errno %ld\n", (long)Errno()); Close(rfh); Close(wfh); return 20; }

    /* ---- Test A: the PIPE wakes the composed WaitSelect -------------------- */
    {
        struct fds r; struct tvl t; ULONG sigs; char b[8]; LONG n;

        SetSignal(0, pipeSig);                              /* clear any stale */
        Write(wfh, "P", 1);                                 /* becomes readable -> Signal(pipeSig) */
        FZERO(&r); FSET(sock, &r); t.sec = 3; t.usec = 0; sigs = pipeSig;
        rc = WaitSelect(sock + 1, (fd_set *)&r, (fd_set *)0, (fd_set *)0,
                        (struct timeval *)&t, &sigs);
        if ((sigs & pipeSig) && !FISSET(sock, &r))
        {
            n = Read(rfh, b, 1);                            /* read the 1 byte we wrote */
            if (n == 1 && b[0] == 'P')
                Printf("[RCT] PASS: WaitSelect woke on PIPE readiness (rc=%ld), read 'P'\n", (long)rc);
            else { Printf("[RCT] FAIL: pipe read n=%ld\n", (long)n); fail = 1; }
        }
        else { Printf("[RCT] FAIL: A woke wrong (sigs=%lx sockready=%ld)\n",
                      (unsigned long)sigs, (long)FISSET(sock, &r)); fail = 1; }
    }

    /* ---- Test B: the SOCKET wakes the same composed WaitSelect ------------- */
    {
        struct fds r; struct tvl t; ULONG sigs;

        SetSignal(0, pipeSig);                              /* pipe drained -> not readable */
        send(sock, "S", 1, 0);                              /* echo replies -> socket readable */
        FZERO(&r); FSET(sock, &r); t.sec = 3; t.usec = 0; sigs = pipeSig;
        rc = WaitSelect(sock + 1, (fd_set *)&r, (fd_set *)0, (fd_set *)0,
                        (struct timeval *)&t, &sigs);
        if (FISSET(sock, &r) && !(sigs & pipeSig))
            Printf("[RCT] PASS: WaitSelect woke on SOCKET readiness (rc=%ld), pipe idle\n", (long)rc);
        else { Printf("[RCT] FAIL: B woke wrong (sigs=%lx sockready=%ld)\n",
                      (unsigned long)sigs, (long)FISSET(sock, &r)); fail = 1; }
    }

    CloseSocket(sock);
    DoPkt(rh->fh_Type, ACTION_PIPE_READ_NOTIFY, (SIPTR)rh->fh_Arg1, 0, 0, 0, 0);  /* deregister */
    FreeSignal(sigbit);
    Close(rfh); Close(wfh);
    CloseLibrary(SocketBase);

    Printf(fail ? "[RCT] SUMMARY: FAILED\n" : "[RCT] SUMMARY: PASSED\n");
    return fail ? 20 : 0;
}
