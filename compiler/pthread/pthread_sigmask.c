/*
  Copyright (C) 2014 Szilard Biro

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "pthread_intern.h"

#include <aros/debug.h>
#include <errno.h>

/*****************************************************************************

    NAME */

#include <signal.h>

	int pthread_sigmask (

/*  SYNOPSIS */
	int  how,
	const  sigset_t *set,
	sigset_t *oldset)

/*  FUNCTION
        Examine or change the signal mask of the calling thread.

    INPUTS
        how    - how to change the mask: SIG_BLOCK, SIG_UNBLOCK, or
                 SIG_SETMASK
        set    - if non-NULL, the set of signals to apply according to 'how'
        oldset - if non-NULL, receives the previous signal mask

    RESULT
        0 on success, or the error number on failure. Note that unlike
        sigprocmask(), this does not set errno -- pthread functions report
        errors by return value.

    NOTES
        On AROS the signal mask sigprocmask() maintains is already per-task:
        it lives in the caller's PosixCBase, and a pthread is an exec task
        with its own base. Process-wide and thread-local masks are therefore
        the same thing here, so this forwards and only adapts the error
        convention.

        As sigprocmask() notes, AROS does not deliver asynchronous signals,
        so the mask only affects what raise() would do.

    EXAMPLE

    BUGS

    SEE ALSO
        sigprocmask(), pthread_kill(), raise()

    INTERNALS

******************************************************************************/
{
    int saved_errno = errno;
    int error = 0;

    if (sigprocmask(how, set, oldset) != 0)
        error = errno ? errno : EINVAL;

    /* POSIX: pthread_sigmask() returns the error and leaves errno alone. */
    errno = saved_errno;

    return error;
} /* pthread_sigmask */
