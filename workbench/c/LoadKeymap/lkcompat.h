#ifndef LK_COMPAT_H
#define LK_COMPAT_H

/*
    Host C-runtime shims so the mkamikeymap .akmd descriptor parser
    (parsedescriptor.c) builds and runs inside an AROS -noposixc command,
    which has no stdio/stdlib/ctype.

    - malloc/free  -> exec AllocVec/FreeVec
    - exit(n)      -> longjmp back to parseKeyDescriptor (graceful parse abort)
    - isspace      -> inline
    - strtoul      -> small local implementation
    File reading is handled by reading the whole .akmd into a buffer up front
    (see the rewritten readline()/parseKeyDescriptor() in parsedescriptor.c).
*/

#include <exec/types.h>
#include <proto/exec.h>
#include <setjmp.h>

extern jmp_buf lk_errjmp;       /* set by parseKeyDescriptor() */

static inline APTR lk_malloc(IPTR n) { return AllocVec(n, MEMF_ANY | MEMF_CLEAR); }
static inline void lk_free(APTR p)   { if (p) FreeVec(p); }

#define malloc(n)   lk_malloc((IPTR)(n))
#define free(p)     lk_free((APTR)(p))
#define exit(n)     longjmp(lk_errjmp, (int)(n) ? (int)(n) : -1)

#define isspace(c)  ((c) == ' '  || (c) == '\t' || (c) == '\n' || \
                     (c) == '\r' || (c) == '\f' || (c) == '\v')

unsigned long lk_strtoul(const char *nptr, char **endptr, int base);
#define strtoul(s, e, b)  lk_strtoul((s), (e), (b))

#endif /* LK_COMPAT_H */
