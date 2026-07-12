#ifndef KMS_AKMD_H
#define KMS_AKMD_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: .akmd text keymap descriptor support for kms.library.

    The parser (akmdparse.c) is the mkamikeymap descriptor parser, ported to
    run inside the library: whole-file read via dos into a buffer, AllocVec
    for malloc, longjmp for the parser's exit() paths, inline isspace/strtoul.
    It fills a struct akmd_config; akmdload.c builds a native struct KeyMap
    from it (kms_LoadAkmdKeymap). Moved here from workbench/c/LoadKeymap so
    OpenKeymap itself can fall back to text descriptors on ports that cannot
    use the 68K-hunk keymap files (e.g. hosted darwin-aarch64, whose heap
    lives above 4GB where the hunk->native conversion cannot work).
*/

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <devices/keymap.h>
#include <proto/exec.h>
#include <setjmp.h>

struct akmd_config
{
    char        *descriptor;
    char        *keymap;
    int         verbose;
    int         bitorder;
    UBYTE       LoKeyMapTypes[0x40];
    IPTR        LoKeyMap[0x40];
    UBYTE       LoCapsable[0x08];
    UBYTE       LoRepeatable[0x08];
    UBYTE       HiKeyMapTypes[0x38];
    IPTR        HiKeyMap[0x38];
    UBYTE       HiCapsable[0x07];
    UBYTE       HiRepeatable[0x07];
    struct List KeyDesc;
};

/* C-runtime shims for the parser (no stdio/stdlib/ctype dependency) */

extern jmp_buf akmd_errjmp;             /* set by akmd_ParseDescriptor() */

static inline APTR akmd_malloc(IPTR n) { return AllocVec(n, MEMF_ANY | MEMF_CLEAR); }
static inline void akmd_free(APTR p)   { if (p) FreeVec(p); }

#define malloc(n)   akmd_malloc((IPTR)(n))
#define free(p)     akmd_free((APTR)(p))
#define exit(n)     longjmp(akmd_errjmp, (int)(n) ? (int)(n) : -1)

#define isspace(c)  ((c) == ' '  || (c) == '\t' || (c) == '\n' || \
                     (c) == '\r' || (c) == '\f' || (c) == '\v')

unsigned long akmd_strtoul(const char *nptr, char **endptr, int base);
#define strtoul(s, e, b)  akmd_strtoul((s), (e), (b))

/* Silent in the library: a failed parse just makes OpenKeymap return NULL. */
#define D(x)
#define DLINE(x)
#define CONSOUT(fmt, ...)
#define CONSERR(fmt, ...)

/* akmdparse.c */
BOOL akmd_ParseDescriptor(struct akmd_config *cfg);
void akmd_ParseCleanup(void);

/* akmdload.c */
struct kms_base;
struct KeyMapNode *kms_LoadAkmdKeymap(struct kms_base *KMSBase,
                                      CONST_STRPTR path, CONST_STRPTR name);

#endif /* KMS_AKMD_H */
