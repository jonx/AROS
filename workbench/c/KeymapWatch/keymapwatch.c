/*
    KeymapWatch -- apply a host-requested keymap change live.

    The hosted darwin-aarch64 port keeps the chosen keymap in ENVARC:Keymap
    (= SYS:Prefs/Env-Archive/Keymap, a host-visible file). The boot applies it
    once via `LoadKeymap RESTORE`. This tiny background task polls that file and,
    whenever its contents change (e.g. the Mac side writes a new keymap name via
    `aros-ctl keymap <name>`), re-applies it with `LoadKeymap RESTORE` so the
    keyboard switches live, without rebooting and without injecting keystrokes
    (which would themselves be remapped by the active keymap).

    It watches a second file in the host share as well. A packaged host
    application may have no writable system directory at all, so its settings
    window cannot write ENVARC:; the share is the one place both sides can
    write. That request names the layout directly and is applied without being
    persisted, leaving ENVARC: as the boot-time choice.

    Started at boot with `Run <NIL: >NIL: C:KeymapWatch`. Built with detach=yes
    (detach.o): it loops forever, and a non-detached CLI child would keep the
    boot console window open in front of the desktop after EndCLI.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#define REQFILE "SYS:Prefs/Env-Archive/Keymap"
#define SHAREFILE "MacRW:.macaros-keymap"
#define POLL_TICKS 40           /* ~0.8s (TICKS_PER_SECOND is 50) */

/* read the trimmed keymap name from `path` into out (<= len); empty if none */
static void readreq(CONST_STRPTR path, char *out, int len)
{
    BPTR f = Open(path, MODE_OLDFILE);
    out[0] = '\0';
    if (f)
    {
        LONG n = Read(f, out, len - 1);
        Close(f);
        if (n > 0)
        {
            char *p;
            out[n] = '\0';
            for (p = out; *p && *p != '\n' && *p != '\r'; p++) ;
            *p = '\0';
        }
        else
            out[0] = '\0';
    }
}

/* Run one shell command with no console attached. */
static void run_quiet(CONST_STRPTR command)
{
    BPTR in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);

    /* System() closes the supplied handles. */
    SystemTags(command, SYS_Input, (IPTR)in, SYS_Output, (IPTR)out, TAG_DONE);
}

int main(void)
{
    char last[80], cur[80];
    char shared_last[80], shared[80];
    char command[128];

    /* Seed with the current values so we do not re-apply what the boot already
       loaded; only react to later changes. */
    readreq(REQFILE, last, sizeof(last));
    readreq(SHAREFILE, shared_last, sizeof(shared_last));

    for (;;)
    {
        Delay(POLL_TICKS);

        readreq(REQFILE, cur, sizeof(cur));
        if (cur[0] && strcmp(cur, last) != 0)
        {
            /* RESTORE re-reads ENVARC:Keymap (= REQFILE) and SetKeyMapDefault()s it. */
            run_quiet((CONST_STRPTR)"C:LoadKeymap RESTORE");
            strcpy(last, cur);
        }

        readreq(SHAREFILE, shared, sizeof(shared));
        if (shared[0] && strcmp(shared, shared_last) != 0)
        {
            /* NOPERSIST: the request is the running choice, not the stored one,
               and the stored one may live somewhere we cannot write. */
            snprintf(command, sizeof(command), "C:LoadKeymap \"%s\" NOPERSIST", shared);
            run_quiet((CONST_STRPTR)command);
            strcpy(shared_last, shared);
        }
    }
    return 0;
}
