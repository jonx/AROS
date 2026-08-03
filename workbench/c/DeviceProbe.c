/*
    Open a device the way the 68k bridge opens one, from ordinary native code.

    The bridge's OpenDevice crashes for keyboard.device, and the bisection
    proved the crash is in that native call rather than in the port or request
    around it. What that does NOT yet prove is whether OpenDevice is at fault:
    the bridge reconstructs the arguments, and a wrong request size, a stale
    proxy, an unexpected task context or an ABI mismatch would all first become
    visible inside the callee.

    So run the identical sequence from a plain Shell command, where none of
    those reconstructions happen. If this crashes too, the device is the
    problem; if it succeeds, the bridge's calling context is.

    DeviceProbe [devicename] [unit] [flags] [count]
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <dos/dos.h>

#include <aros/debug.h>

#include <stdlib.h>
#include <string.h>

/* bug() rather than stdio: these commands link -noposixc, and the debug stream
 * is what the corpus harness already captures. */
#define OUT(...) bug(__VA_ARGS__)

static int probe(const char *name, ULONG unit, ULONG flags, int pass)
{
    struct MsgPort   *port;
    struct IORequest *req;
    LONG              err;

    port = CreateMsgPort();
    if (!port)
    {
        OUT("[DEVPROBE] pass %d FAIL: no MsgPort\n", pass);
        return 0;
    }
    /* The same size the bridge asks for. A request shorter than the device
       expects is the classic way a device writes past the end of it, and this
       is where that would show. */
    req = (struct IORequest *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (!req)
    {
        OUT("[DEVPROBE] pass %d FAIL: no IORequest\n", pass);
        DeleteMsgPort(port);
        return 0;
    }

    OUT("[DEVPROBE] pass %d before: req=%p size=%u mn_Length=%u "
           "replyport=%p sigtask=%p sigbit=%d task=%p io_Device=%p io_Unit=%p "
           "io_Error=%d\n",
           pass, (void *)req, (unsigned)sizeof(struct IOStdReq),
           (unsigned)req->io_Message.mn_Length,
           (void *)req->io_Message.mn_ReplyPort,
           (void *)port->mp_SigTask, (int)port->mp_SigBit,
           (void *)FindTask(NULL), (void *)req->io_Device,
           (void *)req->io_Unit, (int)req->io_Error);

    err = OpenDevice((CONST_STRPTR)name, unit, req, flags);

    OUT("[DEVPROBE] pass %d after:  rc=%d io_Device=%p io_Unit=%p "
           "io_Error=%d\n", pass, (int)err, (void *)req->io_Device,
           (void *)req->io_Unit, (int)req->io_Error);

    if (err == 0)
        CloseDevice(req);
    DeleteIORequest(req);
    DeleteMsgPort(port);
    return err == 0;
}

/* The bridge runs its OpenDevice on a SWAPPED stack: the 68k run needs a deep
 * one of its own, so emu68k.library calls NewStackSwap and every native call
 * made on the guest's behalf happens there. That is the most conspicuous
 * difference between this oracle and the bridge, so it is worth reproducing
 * exactly rather than assuming it is harmless. */
struct probe_args { const char *name; ULONG unit, flags; int pass; int ok; };

static AROS_UFH3(int, probe_on_swapped_stack,
                 AROS_UFHA(struct probe_args *, pa, A0),
                 AROS_UFHA(APTR, unused1, A1),
                 AROS_UFHA(APTR, unused2, A2))
{
    AROS_USERFUNC_INIT
    pa->ok = probe(pa->name, pa->unit, pa->flags, pa->pass);
    return 0;
    AROS_USERFUNC_EXIT
}

#define SWAP_STACK (512 * 1024)

static int probe_swapped(const char *name, ULONG unit, ULONG flags, int pass)
{
    struct StackSwapStruct sss;
    struct StackSwapArgs   ssa;
    struct probe_args      pa;
    APTR stackmem = AllocMem(SWAP_STACK, MEMF_ANY);

    if (!stackmem)
    {
        OUT("[DEVPROBE] pass %d FAIL: no swap stack\n", pass);
        return 0;
    }
    pa.name = name; pa.unit = unit; pa.flags = flags; pa.pass = pass; pa.ok = 0;
    ssa.Args[0] = (IPTR)&pa;
    sss.stk_Lower   = stackmem;
    sss.stk_Upper   = (IPTR)stackmem + SWAP_STACK;
    sss.stk_Pointer = (APTR)sss.stk_Upper;
    OUT("[DEVPROBE] pass %d on a SWAPPED stack (%d KB)\n", pass,
        SWAP_STACK / 1024);
    NewStackSwap(&sss, probe_on_swapped_stack, &ssa);
    FreeMem(stackmem, SWAP_STACK);
    return pa.ok;
}

int main(int argc, char **argv)
{
    const char *name  = (argc > 1) ? argv[1] : "keyboard.device";
    ULONG       unit  = (argc > 2) ? (ULONG)strtoul(argv[2], NULL, 0) : 0;
    ULONG       flags = (argc > 3) ? (ULONG)strtoul(argv[3], NULL, 0) : 0;
    int         count = (argc > 4) ? atoi(argv[4]) : 1;
    int         swap  = (argc > 5) && argv[5][0] == 's';
    int         i, ok = 0;

    OUT("[DEVPROBE] %s unit=%lu flags=%lu x%d\n", name,
           (unsigned long)unit, (unsigned long)flags, count);
    /* Repeated so a COLD open and a WARM one are distinguishable: a device
       that only fails the first time is an initialisation problem, and one
       that only fails after a previous open is a teardown problem. */
    for (i = 1; i <= count; i++)
        ok += swap ? probe_swapped(name, unit, flags, i)
                   : probe(name, unit, flags, i);

    OUT("[DEVPROBE] %d of %d opened\n", ok, count);
    return ok == count ? RETURN_OK : RETURN_WARN;
}
