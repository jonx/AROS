/*
    Copyright (C) 2012-2013, The AROS Development Team. All rights reserved.

    Get pointer to errno variable in stdc.library libbase.
    This function is in both the static linklib and stdc.library.
*/
#ifndef STDC_STATIC
#include <libraries/stdc.h>
#endif

/* Optional per-task errno hook. errno normally lives in the (per-process) stdc
   libbase, so tasks sharing that base share one errno. A threading layer that
   wants per-task errno (pthread) installs this hook to return the calling task's
   own errno slot; returning NULL (or leaving the hook NULL, the default) falls
   back to the single per-process errno, so non-threaded programs are unchanged. */
int *(*__stdc_errnoptr_hook)(void) = 0;

int *__stdc_geterrnoptr(void)
{
    if (__stdc_errnoptr_hook)
    {
        int *p = __stdc_errnoptr_hook();
        if (p)
            return p;
    }

#ifdef STDC_STATIC
    {
        static int static_errno;

        return &static_errno;
    }
#else
    return &(__aros_getbase_StdCBase()->_errno);
#endif
}
