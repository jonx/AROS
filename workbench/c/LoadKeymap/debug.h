#ifndef LK_DEBUG_H
#define LK_DEBUG_H

/* Debug macros for the ported mkamikeymap parser. Debug is off (D/DLINE are
   no-ops); CONSOUT/CONSERR go to the shell via dos.library Printf. */

#include <proto/dos.h>

#define D(x)
#define DLINE(x)
#define CONSOUT(fmt, ...) Printf((CONST_STRPTR)(fmt), ##__VA_ARGS__)
#define CONSERR(fmt, ...) Printf((CONST_STRPTR)(fmt), ##__VA_ARGS__)

#endif /* LK_DEBUG_H */
