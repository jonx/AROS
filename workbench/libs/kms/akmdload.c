/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Build a native struct KeyMap from a .akmd text descriptor
    (kms_LoadAkmdKeymap, the OpenKeymap fallback for ports that cannot
    use 68K-hunk keymap files). Moved from workbench/c/LoadKeymap.
*/

#include <string.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "kms_intern.h"
#include "kms_akmd.h"

jmp_buf akmd_errjmp;    /* exit() in the parser longjmps here */

unsigned long akmd_strtoul(const char *nptr, char **endptr, int base)
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

/* Build a native struct KeyMap (wrapped in a KeyMapNode) from the parsed
   config. One AllocVec'd block holds the node + the 8 tables + the
   dead/string-key descriptors + the name; it stays allocated for as long as
   the keymap is resident (keymap.resource). */
static struct KeyMapNode *buildKeyMap(struct akmd_config *cfg)
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

    block = AllocVec(blocksize, MEMF_PUBLIC | MEMF_CLEAR);
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

/* Free everything the parser allocated (dead/string descriptor nodes, the
   keymap name if it came from the file, the line buffer). buildKeyMap()
   copied whatever the KeyMapNode needs into its own block. */
static void freeParsed(struct akmd_config *cfg, BOOL name_from_file)
{
    struct Node *n, *n2;

    ForeachNodeSafe(&cfg->KeyDesc, n, n2)
        FreeVec(n);
    NEWLIST(&cfg->KeyDesc);

    if (name_from_file && cfg->keymap)
        FreeVec(cfg->keymap);
    cfg->keymap = NULL;

    akmd_ParseCleanup();
}

/* Parse DEVS:Keymaps/<name>.akmd (or an explicit path) and build a native
   KeyMapNode. Returns NULL if the descriptor is absent or malformed; never
   installs a half-parsed map. The block is AllocVec'd (see OpenKeymap: it is
   FreeVec'd if another copy of the keymap turns out to be resident). */
struct KeyMapNode *kms_LoadAkmdKeymap(struct kms_base *KMSBase,
                                      CONST_STRPTR path, CONST_STRPTR name)
{
    struct akmd_config cfg;
    struct KeyMapNode *kmn = NULL;
    BOOL parsed, populated = FALSE;
    int k;

    memset(&cfg, 0, sizeof(cfg));
    NEWLIST(&cfg.KeyDesc);
    cfg.descriptor = (char *)path;
    cfg.keymap = (char *)name;      /* may be NULL: parser then takes the file's "keymap:" line */

    /* the parser keeps state in statics; serialize it */
    ObtainSemaphore(&KMSBase->akmd_lock);

    parsed = akmd_ParseDescriptor(&cfg);
    if (parsed)
    {
        for (k = 0; k < 0x40; k++)
            if (cfg.LoKeyMapTypes[k]) { populated = TRUE; break; }
    }
    if (parsed && populated)
        kmn = buildKeyMap(&cfg);

    freeParsed(&cfg, name == NULL);

    ReleaseSemaphore(&KMSBase->akmd_lock);

    return kmn;
}
