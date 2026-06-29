/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: LoadKeymap -- build a keyboard layout from a .akmd text descriptor at
          runtime and install it as the default keymap (SetKeyMapDefault). No
          compiled 68K-hunk module, LoadSeg, relocation, or hunk->native
          conversion: the .akmd text is parsed and a struct KeyMap is built
          directly in memory. This is the foundation for live, text-based
          keymaps (re-run anytime to swap the layout; new shells/windows pick
          it up immediately).

          Usage:  LoadKeymap            show the current keymap + available layouts
                  LoadKeymap <name>     load DEVS:Keymaps/<name>.akmd and install it
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/keymap.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/keymap.h>

#include <string.h>

#include "config.h"

const TEXT version[] = "$VER: LoadKeymap 1.0 (29.06.2026)\n";

#define ARG_TEMPLATE "NAME,NOPERSIST/S,RESTORE/S"
enum { ARG_NAME = 0, ARG_NOPERSIST, ARG_RESTORE, NOOFARGS };

#define KEYMAPS_DIR "DEVS:Keymaps"
#define CURRENT_ENV "ENV:Keymap"       /* session record, shown by the no-arg display */
#define PERSIST_ENV "ENVARC:Keymap"    /* survives reboot; `LoadKeymap RESTORE` reloads it */

/* the ported .akmd parser */
extern BOOL parseKeyDescriptor(struct config *cfg);

/* strtoul used by the parser (declared in lkcompat.h, defined here) */
unsigned long lk_strtoul(const char *nptr, char **endptr, int base)
{
    unsigned long v = 0;
    const char *s = nptr;
    while (*s == ' ' || *s == '\t') s++;
    if (base == 0)
    {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    }
    else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;)
    {
        int d; char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return v;
}

/* For a dead/string key the parser sets cfg->LoKeyMap[i] to a (struct Node *):
   ln_Pri holds the descriptor byte size, and the descriptor bytes follow the
   node header and the node's name string (same layout writekeymap.c reads). */
static UBYTE *descrOf(IPTR mapval, UWORD *szout)
{
    struct Node *kn = (struct Node *)mapval;
    *szout = (UWORD)(UBYTE)kn->ln_Pri;
    return (UBYTE *)((IPTR)kn + sizeof(struct Node) + strlen((const char *)kn->ln_Name) + 1);
}

/* Build a native struct KeyMap (wrapped in a KeyMapNode) from the parsed config.
   One AllocMem'd block holds the node + the 8 tables + the dead/string-key
   descriptors + the name. It is intentionally not freed: it becomes the live
   default keymap. */
static struct KeyMapNode *buildKeyMap(struct config *cfg)
{
    UWORD i;
    IPTR descrtotal = 0, namelen, blocksize;
    UBYTE *block, *p;
    IPTR *lomap, *himap;
    struct KeyMapNode *kmn;
    CONST_STRPTR nm = cfg->keymap ? (CONST_STRPTR)cfg->keymap : (CONST_STRPTR)"?";

    namelen = strlen((const char *)nm) + 1;

    for (i = 0; i < 0x40; i++)
        if (cfg->LoKeyMapTypes[i] & (KCF_STRING | KCF_DEAD))
            { UWORD s; descrOf(cfg->LoKeyMap[i], &s); descrtotal += (s + 1) & ~1; }
    for (i = 0; i < 0x38; i++)
        if (cfg->HiKeyMapTypes[i] & (KCF_STRING | KCF_DEAD))
            { UWORD s; descrOf(cfg->HiKeyMap[i], &s); descrtotal += (s + 1) & ~1; }

    blocksize = sizeof(struct KeyMapNode)
              + 0x40 + 0x40 * sizeof(IPTR) + 8 + 8          /* Lo */
              + 0x38 + 0x38 * sizeof(IPTR) + 7 + 7          /* Hi */
              + descrtotal + namelen;

    block = AllocMem(blocksize, MEMF_PUBLIC | MEMF_CLEAR);
    if (!block) return NULL;

    kmn = (struct KeyMapNode *)block;
    p = block + sizeof(struct KeyMapNode);

    kmn->kn_KeyMap.km_LoKeyMapTypes = p; CopyMem(cfg->LoKeyMapTypes, p, 0x40); p += 0x40;
    lomap = (IPTR *)p; kmn->kn_KeyMap.km_LoKeyMap = lomap; p += 0x40 * sizeof(IPTR);
    kmn->kn_KeyMap.km_LoCapsable = p; CopyMem(cfg->LoCapsable, p, 8); p += 8;
    kmn->kn_KeyMap.km_LoRepeatable = p; CopyMem(cfg->LoRepeatable, p, 8); p += 8;
    kmn->kn_KeyMap.km_HiKeyMapTypes = p; CopyMem(cfg->HiKeyMapTypes, p, 0x38); p += 0x38;
    himap = (IPTR *)p; kmn->kn_KeyMap.km_HiKeyMap = himap; p += 0x38 * sizeof(IPTR);
    kmn->kn_KeyMap.km_HiCapsable = p; CopyMem(cfg->HiCapsable, p, 7); p += 7;
    kmn->kn_KeyMap.km_HiRepeatable = p; CopyMem(cfg->HiRepeatable, p, 7); p += 7;

    for (i = 0; i < 0x40; i++)
    {
        UBYTE t = cfg->LoKeyMapTypes[i];
        if (t & (KCF_STRING | KCF_DEAD))
        {
            UWORD sz; UBYTE *d = descrOf(cfg->LoKeyMap[i], &sz);
            CopyMem(d, p, sz);
            lomap[i] = (IPTR)p;
            p += (sz + 1) & ~1;
        }
        else
            lomap[i] = cfg->LoKeyMap[i];
    }
    for (i = 0; i < 0x38; i++)
    {
        UBYTE t = cfg->HiKeyMapTypes[i];
        if (t & (KCF_STRING | KCF_DEAD))
        {
            UWORD sz; UBYTE *d = descrOf(cfg->HiKeyMap[i], &sz);
            CopyMem(d, p, sz);
            himap[i] = (IPTR)p;
            p += (sz + 1) & ~1;
        }
        else
            himap[i] = cfg->HiKeyMap[i];
    }

    CopyMem((APTR)nm, p, namelen);
    kmn->kn_Node.ln_Name = (char *)p;
    return kmn;
}

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

static ULONG doLoad(struct Library *KeymapBase, STRPTR name, BOOL persist)
{
    struct config cfg;
    char path[256];
    struct KeyMapNode *kmn;
    int k, populated = 0;

    memset(&cfg, 0, sizeof(cfg));
    NEWLIST(&cfg.KeyDesc);

    if (strchr((const char *)name, ':') || strchr((const char *)name, '/'))
        { strncpy(path, (const char *)name, sizeof(path) - 1); path[sizeof(path) - 1] = 0; }
    else
        { strcpy(path, KEYMAPS_DIR "/"); strncat(path, (const char *)name, sizeof(path) - 16); strcat(path, ".akmd"); }

    cfg.descriptor = path;
    cfg.keymap = (char *)name;

    if (!parseKeyDescriptor(&cfg))
    {
        Printf("LoadKeymap: could not parse %s (keymap unchanged)\n", (STRPTR)path);
        return RETURN_FAIL;
    }

    /* guard: never install a half-parsed/empty map (it would brick typing) */
    for (k = 0; k < 0x40; k++) if (cfg.LoKeyMapTypes[k]) { populated = 1; break; }
    if (!populated)
    {
        Printf("LoadKeymap: %s has no key data (keymap unchanged)\n", (STRPTR)path);
        return RETURN_FAIL;
    }

    kmn = buildKeyMap(&cfg);
    if (!kmn)
    {
        Printf("LoadKeymap: out of memory (keymap unchanged)\n");
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

static ULONG doRestore(struct Library *KeymapBase)
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
    return doLoad(KeymapBase, (STRPTR)buf, FALSE);  /* already saved; do not rewrite */
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
    Printf("Current keymap: built-in default (no .akmd loaded yet)\n");
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
    struct Library *KeymapBase;
    IPTR args[NOOFARGS] = { 0 };
    struct RDArgs *rda;
    ULONG rc = RETURN_FAIL;

    KeymapBase = OpenLibrary("keymap.library", 0);
    if (!KeymapBase) { Printf("LoadKeymap: cannot open keymap.library\n"); return RETURN_FAIL; }

    rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
    if (rda)
    {
        if (args[ARG_RESTORE])
            rc = doRestore(KeymapBase);
        else if (args[ARG_NAME])
            rc = doLoad(KeymapBase, (STRPTR)args[ARG_NAME], !args[ARG_NOPERSIST]);
        else
        {
            printCurrent();
            printAvailable();
            rc = RETURN_OK;
        }
        FreeArgs(rda);
    }
    else
        PrintFault(IoErr(), "LoadKeymap");

    CloseLibrary(KeymapBase);
    return rc;
}
