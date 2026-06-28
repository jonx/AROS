/*
 Copyright  1995-2011, The AROS Development Team. All rights reserved.
 
 Desc: Filesystem that accesses an underlying host OS filesystem.
 */

/*********************************************************************************************/

#define DEBUG 0

#include <aros/debug.h>
#include <aros/symbolsets.h>
#include <libraries/expansion.h>
#include <resources/filesysres.h>
#include <proto/arossupport.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include "emul_intern.h"
#include LC_LIBDEFS_FILE

#include <limits.h>
#include <string.h>
#include <stddef.h>

static LONG startup(struct emulbase *emulbase)
{
    APTR ExpansionBase;

    D(bug("[Emulhandler] startup\n"));

    HostLibBase = OpenResource("hostlib.resource");
    D(bug("[EmulHandler] got hostlib.resource %p\n", HostLibBase));
    if (!HostLibBase)
        return FALSE;

    KernelBase = OpenResource("kernel.resource");
    D(bug("[EmulHandler] KernelBase = %p\n", KernelBase));
    if (!KernelBase)
        return FALSE;

    emulbase->mempool = CreatePool(MEMF_ANY|MEMF_SEM_PROTECTED, 4096, 2000);
    if (!emulbase->mempool)
        return FALSE;

    /* Create a BootNode so we can boot from this device */
    if ((ExpansionBase = OpenLibrary("expansion.library", 0)))
    {
        struct DeviceNode *dn;
        IPTR pp[4 + sizeof(struct DosEnvec)/sizeof(IPTR)] = {};

        pp[0]                 = (IPTR)"EMU";
        pp[1]                 = 0;
        pp[2]                 = 0;
        pp[DE_TABLESIZE  + 4] = DE_DOSTYPE;
        /* .... */
        pp[DE_BUFMEMTYPE + 4] = MEMF_PUBLIC;
        pp[DE_MASK       + 4] = -1;
        pp[DE_BOOTPRI    + 4] = 0;
        pp[DE_DOSTYPE    + 4] = AROS_MAKE_ID('E', 'M', 'U', 0);

        dn = MakeDosNode(pp);
        if (dn)
        {
            dn->dn_SegList = CreateSegList(EmulHandlerMain);
            dn->dn_Handler = AROS_CONST_BSTR("emul-handler");
            dn->dn_StackSize = 16384;
            dn->dn_GlobalVec = (BPTR)-1;

            AddDosNode(0, ADNF_STARTPROC, dn);
        }

        CloseLibrary(ExpansionBase);
    }

    return TRUE;
}

/*********************************************************************************************/

/*
 * Launcher hook: mount a host folder named in the AROS_HOST_VOLUME host
 * environment variable. The value is one or more device strings of the same
 * form the handler parses, "<Vol>:<hostpath>[;WRITE]" — e.g. "Mac:~/Amiga"
 * (read-only) or "Mac:~/Amiga;WRITE" (read/write) — separated by newlines, so
 * several host volumes (e.g. the same folder mounted read-only AND read/write)
 * can be set up at once. This is OUR mount path: we build fssm_Device directly,
 * so our read-only-default policy and the ;WRITE keyword are honoured without
 * ever going through (and being mangled by) the Mount command. A spec without
 * the keyword stays read-only, like any host volume.
 *
 * Runs at a LATER init priority than host_startup (emul_host_unix.c, pri 0) so
 * the host libc interface used by GetHostEnv is already up.
 */
static void mount_one_hostvol(APTR ExpansionBase, char *spec)
{
    struct DeviceNode *dn;
    IPTR pp[4 + sizeof(struct DosEnvec)/sizeof(IPTR)] = {};
    char nodename[40];
    int i;

    if (!spec[0] || !strchr(spec, ':'))
        return;

    /* node name = the volume name (chars before the first ':') */
    for (i = 0; spec[i] && spec[i] != ':' && i < (int)sizeof(nodename) - 1; i++)
        nodename[i] = spec[i];
    nodename[i] = 0;

    pp[0]                 = (IPTR)nodename;
    pp[1]                 = (IPTR)spec;     /* fssm_Device "<Vol>:<path>[;WRITE]" */
    pp[2]                 = 0;
    pp[DE_TABLESIZE  + 4] = DE_DOSTYPE;
    pp[DE_BUFMEMTYPE + 4] = MEMF_PUBLIC;
    pp[DE_MASK       + 4] = -1;
    pp[DE_BOOTPRI    + 4] = 0;
    pp[DE_DOSTYPE    + 4] = AROS_MAKE_ID('E', 'M', 'U', 0);

    dn = MakeDosNode(pp);
    if (dn)
    {
        dn->dn_SegList   = CreateSegList(EmulHandlerMain);
        dn->dn_Handler   = AROS_CONST_BSTR("emul-handler");
        dn->dn_StackSize = 16384;
        dn->dn_GlobalVec = (BPTR)-1;

        AddDosNode(0, ADNF_STARTPROC, dn);
    }
}

static LONG mount_hostvol(struct emulbase *emulbase)
{
    APTR ExpansionBase;
    char *list = GetHostEnv(emulbase, "AROS_HOST_VOLUME");
    char *spec, *nl;

    if (!list || !list[0])
        return TRUE;

    if ((ExpansionBase = OpenLibrary("expansion.library", 0)))
    {
        /* one "<Vol>:<path>[;WRITE]" per newline-separated line */
        for (spec = list; spec && *spec; spec = nl)
        {
            nl = strchr(spec, '\n');
            if (nl)
                *nl++ = 0;
            mount_one_hostvol(ExpansionBase, spec);
        }

        CloseLibrary(ExpansionBase);
    }

    return TRUE;
}

ADD2INITLIB(mount_hostvol, 10)

ADD2INITLIB(startup, -10)

/*********************************************************************************************/

static LONG cleanup(struct emulbase *emulbase)
{
    if (emulbase->mempool)
        DeletePool(emulbase->mempool);

    return TRUE;
}

ADD2EXPUNGELIB(cleanup, 10);
