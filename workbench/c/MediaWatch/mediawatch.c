/*
    MediaWatch -- mount host media the user granted, while the system runs.

    The host writes one mount description per granted medium into a shared
    directory (default MacRW:.macaros-media) and, to take one back, a
    ".remove-<name>" request beside it. This background task mounts what
    appears, dismounts what is withdrawn, and deletes each request it has
    honoured, so a stick granted from the Mac side shows up without a reboot.

    Started at boot with `Run <NIL: >NIL: C:MediaWatch [directory]`. Built with
    detach=yes: it loops forever, and a non-detached CLI child would keep the
    boot console window open in front of the desktop after EndCLI.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#define DEFAULT_DIR  "MacRW:.macaros-media"
#define POLL_TICKS   40         /* ~0.8s (TICKS_PER_SECOND is 50) */
#define REMOVE_PFX   ".remove-"
#define MAX_TRIES    3          /* stop retrying a description that will not mount */
#define MAX_FAILED   16

static char failed[MAX_FAILED][32];
static UBYTE failures[MAX_FAILED];

static int failure_slot(CONST_STRPTR name)
{
    int i, free_slot = -1;

    for (i = 0; i < MAX_FAILED; i++)
    {
        if (failed[i][0] == '\0')
        {
            if (free_slot < 0)
                free_slot = i;
        }
        else if (strcmp(failed[i], name) == 0)
            return i;
    }

    if (free_slot >= 0)
    {
        strncpy(failed[free_slot], name, sizeof(failed[0]) - 1);
        failed[free_slot][sizeof(failed[0]) - 1] = '\0';
        failures[free_slot] = 0;
    }

    return free_slot;
}

static void forget_failures(CONST_STRPTR name)
{
    int i;

    for (i = 0; i < MAX_FAILED; i++)
        if (failed[i][0] != '\0' && strcmp(failed[i], name) == 0)
            failed[i][0] = '\0';
}

static BOOL is_mounted(CONST_STRPTR name)
{
    struct DosList *dl;
    BOOL found;

    dl = LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);
    found = FindDosEntry(dl, name, LDF_DEVICES | LDF_VOLUMES) != NULL;
    UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);

    return found;
}

/* Run one shell command with no console attached. */
static LONG run_quiet(CONST_STRPTR command)
{
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    LONG rc;

    rc = SystemTags(command, SYS_Input, (IPTR)in, SYS_Output, (IPTR)out, TAG_DONE);

    /* SystemTags owns the handles it was given, whether or not it succeeded. */
    if (rc == -1)
    {
        if (in) Close(in);
        if (out) Close(out);
    }

    return rc;
}

static void mount_description(CONST_STRPTR dir, CONST_STRPTR name)
{
    char command[320];
    int slot;

    if (is_mounted(name))
        return;

    slot = failure_slot(name);
    if (slot >= 0 && failures[slot] >= MAX_TRIES)
        return;

    snprintf(command, sizeof(command), "C:Mount \"%s/%s\"", dir, name);
    if (run_quiet((CONST_STRPTR)command) != 0 && slot >= 0)
        failures[slot]++;
}

static void remove_device(CONST_STRPTR dir, CONST_STRPTR request)
{
    CONST_STRPTR name = request + strlen(REMOVE_PFX);
    char command[320];

    if (*name != '\0' && is_mounted(name))
    {
        snprintf(command, sizeof(command), "C:Assign \"%s:\" DISMOUNT", name);
        run_quiet((CONST_STRPTR)command);
    }
    forget_failures(name);

    snprintf(command, sizeof(command), "%s/%s", dir, request);
    DeleteFile((CONST_STRPTR)command);
}

/* One pass over the shared directory: honour removals first, then mount the
   descriptions that are left, so a withdrawn medium is never remounted. */
static void scan(CONST_STRPTR dir)
{
    struct FileInfoBlock fib;
    BPTR lock;
    int pass;

    lock = Lock(dir, SHARED_LOCK);
    if (!lock)
        return;

    for (pass = 0; pass < 2; pass++)
    {
        if (!Examine(lock, &fib))
            break;

        while (ExNext(lock, &fib))
        {
            BOOL removal;

            if (fib.fib_DirEntryType > 0)
                continue;
            removal = strncmp(fib.fib_FileName, REMOVE_PFX, strlen(REMOVE_PFX)) == 0;
            if (removal != (pass == 0))
                continue;

            if (removal)
                remove_device(dir, (CONST_STRPTR)fib.fib_FileName);
            else if (fib.fib_FileName[0] != '.')
                mount_description(dir, (CONST_STRPTR)fib.fib_FileName);
        }
    }

    UnLock(lock);
}

int main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    const char *dir = DEFAULT_DIR;
    struct RDArgs *args;
    IPTR arg[1] = { (IPTR)NULL };
    APTR window;

    args = ReadArgs((CONST_STRPTR)"DIRECTORY", (IPTR *)arg, NULL);
    if (args && arg[0])
        dir = (const char *)arg[0];

    /* No one can answer a "please insert volume" requester for a detached
       background task: the share may legitimately be absent. */
    window = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;

    for (;;)
    {
        scan((CONST_STRPTR)dir);
        Delay(POLL_TICKS);
    }

    me->pr_WindowPtr = window;
    if (args)
        FreeArgs(args);

    return RETURN_OK;
}
