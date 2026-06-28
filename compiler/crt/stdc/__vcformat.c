/*
    Copyright (C) 1995-2025, The AROS Development Team. All rights reserved.

    Function to format a string like printf().
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#ifndef AROS_NO_LIMITS_H
#       include <limits.h>
#else
#       define ULONG_MAX   4294967295UL
#endif
#include <math.h>
#include <float.h>

#include <ctype.h>

/*
 * Always enable floating-point conversions (%[aAeEfFgG]). The sibling engines
 * (__vcscan, __vwformat, __vwscanf) and the kernel _vkprintf are already
 * unconditionally FULL_SPECIFIERS; __vcformat was the lone exception, gated on
 * STDC_STATIC. On targets that force the freestanding libstdc.static.a early in
 * every link (darwin-aarch64, see config/make.cfg.in), that gate let its strong,
 * float-less __vcformat shadow the float-capable StdCBase dispatcher -- so printf
 * from posixc.library / stdcio.library printed "%f" literally. The float math it
 * pulls in (log10/pow/isinf/...) resolves via the weak StdCBase library stubs.
 */
#define FULL_SPECIFIERS

const unsigned char *const __stdc_char_decimalpoint = ".";

/* support macros for FMTPRINTF */
#define FMTPRINTF_TYPE          char
#define FMTPRINTF_UTYPE         unsigned char
#define FMTPRINTF_STR(str)      str
#define FMTPRINTF_STRLEN(str)   strlen(str)
#define FMTPRINTF_DECIMALPOINT  __stdc_char_decimalpoint
#define FMTPRINTF_ISDIGIT(c)    isdigit(c)
#define FMTPRINTF_TOLOWER(c)    tolower(c)
#define FMTPRINTF_OUT(c,ctx)  do                            \
                { if((*outc)((unsigned char)(c),data)==EOF) \
                    return outcount;                        \
                  outcount++;                               \
                }while(0)

#include "fmtprintf_pre.c"

/*****************************************************************************

    NAME */

        int __vcformat (

/*  SYNOPSIS */
        void       * data,
        int       (* outc)(int, void *),
        const char * format,
        va_list      args)

/*  FUNCTION
        Format a list of arguments and call a function for each char
        to print.

    INPUTS
        data - This is passed to the user callback outc as its second argument.
        outc - Call this function for every character that should be
                emitted. The function should return EOF on error and
                > 0 otherwise.
        format - A printf() format string.
        args - A list of arguments for the format string.

    RESULT
        The number of characters written.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

******************************************************************************/
{
#include "fmtprintf.c"
  return outcount;
}
