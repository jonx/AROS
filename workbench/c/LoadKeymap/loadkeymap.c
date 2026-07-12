/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: LoadKeymap -- load a keyboard layout by name and install it as the
          default keymap (SetKeyMapDefault). The heavy lifting lives in
          kms.library's OpenKeymap(), which loads a compiled keymap where the
          platform supports it and otherwise builds the keymap from the
          DEVS:Keymaps/<name>.akmd text descriptor at runtime. Re-run anytime
          to swap the layout; new shells/windows pick it up immediately.

          Usage:  LoadKeymap            show the current keymap + available layouts
                  LoadKeymap <name>     load keymap <name> and install it
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/keymap.h>
#include <libraries/kms.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/keymap.h>
#include <proto/kms.h>

#include <string.h>

const TEXT version[] = "$VER: LoadKeymap 1.1 (12.07.2026)\n";

#define ARG_TEMPLATE "NAME,NOPERSIST/S,RESTORE/S"
enum { ARG_NAME = 0, ARG_NOPERSIST, ARG_RESTORE, NOOFARGS };

#define KEYMAPS_DIR "DEVS:Keymaps"
#define CURRENT_ENV "ENV:Keymap"       /* session record, shown by the no-arg display */
#define PERSIST_ENV "ENVARC:Keymap"    /* survives reboot; `LoadKeymap RESTORE` reloads it */

struct KMSLibrary *KMSBase;

static void recordCurrent(STRPTR name)
{
    BPTR f = Open((CONST_STRPTR)CURRENT_ENV, MODE_NEWFILE);
    if (f) { Write(f, name, strlen((const char *)name)); Close(f); }
}

static void recordPersist(STRPTR name)
{
    BPTR f = Open((CONST_STRPTR)PERSIST_ENV, MODE_NEWFILE);
    if (f) { Write(f, name, strlen((const char *)name)); Close(f); }
}

static ULONG doLoad(STRPTR name, BOOL persist)
{
    struct KeyMapNode *kmn;

    kmn = OpenKeymap(name);
    if (!kmn)
    {
        Printf("LoadKeymap: could not load keymap '%s' (keymap unchanged)\n", name);
        return RETURN_FAIL;
    }

    /* guard: never install a degenerate map (it would brick typing) */
    if (!kmn->kn_KeyMap.km_LoKeyMap || !kmn->kn_KeyMap.km_LoKeyMapTypes)
    {
        Printf("LoadKeymap: keymap '%s' looks invalid (keymap unchanged)\n", name);
        return RETURN_FAIL;
    }

    SetKeyMapDefault(&kmn->kn_KeyMap);
    recordCurrent(name);
    if (persist) recordPersist(name);
    Printf("LoadKeymap: installed '%s' live%s\n", (STRPTR)name,
           persist ? (STRPTR)" (saved as the default for next boot)"
                   : (STRPTR)" (this session only)");
    return RETURN_OK;
}

static ULONG doRestore(void)
{
    BPTR f = Open((CONST_STRPTR)PERSIST_ENV, MODE_OLDFILE);
    char buf[64], *q;
    LONG n;
    if (!f) return RETURN_OK;                       /* nothing saved; keep built-in default */
    n = Read(f, buf, sizeof(buf) - 1);
    Close(f);
    if (n <= 0) return RETURN_OK;
    buf[n] = '\0';
    for (q = buf; *q && *q != '\n' && *q != '\r'; q++) ;
    *q = '\0';
    if (!buf[0]) return RETURN_OK;
    return doLoad((STRPTR)buf, FALSE);              /* already saved; do not rewrite */
}

static void printCurrent(void)
{
    BPTR f = Open((CONST_STRPTR)CURRENT_ENV, MODE_OLDFILE);
    if (f)
    {
        char buf[64]; LONG n = Read(f, buf, sizeof(buf) - 1);
        Close(f);
        if (n > 0)
        {
            char *q; buf[n] = '\0';
            for (q = buf; *q && *q != '\n' && *q != '\r'; q++) ;
            *q = '\0';
            if (buf[0]) { Printf("Current keymap: %s\n", (STRPTR)buf); return; }
        }
    }
    Printf("Current keymap: built-in default (no keymap loaded yet)\n");
}

static void printAvailable(void)
{
    BPTR lock = Lock((CONST_STRPTR)KEYMAPS_DIR, SHARED_LOCK);
    struct FileInfoBlock *fib;

    if (!lock) { Printf("Available layouts: (cannot open %s)\n", (STRPTR)KEYMAPS_DIR); return; }

    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib))
    {
        Printf("Available layouts (drop a .akmd text file in %s to add one):\n",
               (STRPTR)KEYMAPS_DIR);
        while (ExNext(lock, fib))
        {
            STRPTR nm = (STRPTR)fib->fib_FileName;
            LONG l = strlen((const char *)nm);
            if (l > 5 && strcmp((const char *)nm + l - 5, ".akmd") == 0)
            {
                nm[l - 5] = '\0';
                Printf("  %s\n", nm);
            }
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
}

int main(void)
{
    IPTR args[NOOFARGS] = { 0 };
    struct RDArgs *rda;
    ULONG rc = RETURN_FAIL;

    KMSBase = (struct KMSLibrary *)OpenLibrary((CONST_STRPTR)"kms.library", 0);
    if (!KMSBase) { Printf("LoadKeymap: cannot open kms.library\n"); return RETURN_FAIL; }

    rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
    if (rda)
    {
        if (args[ARG_RESTORE])
            rc = doRestore();
        else if (args[ARG_NAME])
            rc = doLoad((STRPTR)args[ARG_NAME], !args[ARG_NOPERSIST]);
        else
        {
            printCurrent();
            printAvailable();
            rc = RETURN_OK;
        }
        FreeArgs(rda);
    }
    else
        PrintFault(IoErr(), (CONST_STRPTR)"LoadKeymap");

    CloseLibrary((struct Library *)KMSBase);
    return rc;
}
