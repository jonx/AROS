/*
    KeymapWatch -- apply a host-requested keymap change live.

    The hosted darwin-aarch64 port keeps the chosen keymap in ENVARC:Keymap
    (= SYS:Prefs/Env-Archive/Keymap, a host-visible file). The boot applies it
    once via `LoadKeymap RESTORE`. This tiny background task polls that file and,
    whenever its contents change (e.g. the Mac side writes a new keymap name via
    `aros-ctl keymap <name>`), re-applies it with `LoadKeymap RESTORE` so the
    keyboard switches live, without rebooting and without injecting keystrokes
    (which would themselves be remapped by the active keymap).

    Started at boot with `Run <NIL: >NIL: C:KeymapWatch`. Built with detach=yes
    (detach.o): it loops forever, and a non-detached CLI child would keep the
    boot console window open in front of the desktop after EndCLI.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#define REQFILE "SYS:Prefs/Env-Archive/Keymap"
#define POLL_TICKS 40           /* ~0.8s (TICKS_PER_SECOND is 50) */

/* read the trimmed keymap name from REQFILE into out (<= len); empty if none */
static void readreq(char *out, int len)
{
    BPTR f = Open((CONST_STRPTR)REQFILE, MODE_OLDFILE);
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

int main(void)
{
    char last[80], cur[80];

    /* Seed with the current value so we do not re-apply what the boot already
       loaded; only react to later changes. */
    readreq(last, sizeof(last));

    for (;;)
    {
        Delay(POLL_TICKS);
        readreq(cur, sizeof(cur));
        if (cur[0] && strcmp(cur, last) != 0)
        {
            BPTR in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
            BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
            /* RESTORE re-reads ENVARC:Keymap (= REQFILE) and SetKeyMapDefault()s
               it; System() closes the supplied handles. */
            SystemTags((CONST_STRPTR)"C:LoadKeymap RESTORE",
                       SYS_Input,  (IPTR)in,
                       SYS_Output, (IPTR)out,
                       TAG_DONE);
            strcpy(last, cur);
        }
    }
    return 0;
}
