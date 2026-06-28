/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: bsdsocket.library gethostbyname (LVO 35) — host-passthrough DNS.

    getaddrinfo() can block for seconds; on a single underlying thread (H6) that
    would freeze every AROS task, and a host-thread Signal is unsafe on darwin
    (R-DARWIN-WAKE). So the lookup runs on a detached host pthread (in
    libbsdsockhost.dylib) and this LVO timer-polls the result on a Delay() tick —
    only the *calling* task parks. The returned struct hostent lives in per-task
    storage and is valid until the task's next gethostbyname (the AmiTCP contract).
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/hostlib.h>
#include <sys/errno.h>
#include <netdb.h>

#include "bsdsocket_intern.h"
#include "bsdsocket_util.h"

#define HostLibBase (gb->hostlib)

#define RESOLVE_TIMEOUT_MS 10000        /* bound a wedged resolver */

struct HostentBuf
{
    struct hostent he;
    char    *addrlist[2];
    unsigned addr;                      /* network-order IPv4 */
    char     name[256];
};

AROS_LH1(struct hostent *, gethostbyname,
    AROS_LHA(char *, name, A0),
    struct TaskBase *, taskBase, 35, BSDSocket)
{
    AROS_LIBFUNC_INIT

    struct bsdsocketBase *gb = taskBase->glob;
    APTR DOSBase = gb->DOSBase;
    struct HostentBuf *hb;
    APTR job;
    unsigned ip = 0;
    int st;
    LONG elapsed = 0;

    if (!name)
    {
        SetError(EINVAL, taskBase);
        return NULL;
    }

    /* per-task result buffer, allocated once and reused. */
    hb = (struct HostentBuf *)taskBase->he_buf;
    if (!hb)
    {
        hb = (struct HostentBuf *)AllocPooled(taskBase->pool, sizeof *hb);
        if (!hb) { SetError(ENOMEM, taskBase); return NULL; }
        taskBase->he_buf = hb;
    }

    HostLib_Lock();
    job = gb->pump->hs_resolve_start(name);
    HostLib_Unlock();
    if (!job) { SetError(ENOMEM, taskBase); return NULL; }

    /* timer-poll the async lookup (R-DARWIN-WAKE) — only this task parks. */
    for (;;)
    {
        HostLib_Lock();
        st = gb->pump->hs_resolve_poll(job, &ip);
        HostLib_Unlock();
        if (st != 0)
            break;                      /* done (ok or fail) */
        if (elapsed >= RESOLVE_TIMEOUT_MS)
            break;
        Delay(1);                       /* ~20ms tick */
        elapsed += 20;
    }

    HostLib_Lock();
    gb->pump->hs_resolve_free(job);
    HostLib_Unlock();

    if (st != 1)
    {
        SetError(EINVAL, taskBase);     /* maps roughly to "host not found" */
        return NULL;
    }

    /* build a minimal IPv4 hostent the caller can read sin_addr from. */
    strncpy(hb->name, name, sizeof hb->name - 1);
    hb->name[sizeof hb->name - 1] = '\0';
    hb->addr        = ip;               /* network order */
    hb->addrlist[0] = (char *)&hb->addr;
    hb->addrlist[1] = NULL;
    hb->he.h_name      = hb->name;
    hb->he.h_aliases   = NULL;
    hb->he.h_addrtype  = AF_INET;
    hb->he.h_length    = 4;
    hb->he.h_addr_list = hb->addrlist;

    return &hb->he;

    AROS_LIBFUNC_EXIT
}
