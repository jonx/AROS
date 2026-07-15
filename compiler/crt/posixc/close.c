/*
    Copyright (C) 1995-2025, The AROS Development Team. All rights reserved.

    POSIX.1-2008 function close().
*/

#include <unistd.h>
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <errno.h>
#include "__fdesc.h"

/*****************************************************************************

    NAME */
#include <unistd.h>

        int close (

/*  SYNOPSIS */
        int fd)

/*  FUNCTION
        Closes an open file. If this is the last file descriptor
        associated with this file, then all allocated resources
        are freed, too.

    INPUTS
        fd - The result of a successful open()

    RESULT
        -1 for error or zero on success.

    NOTES
        This function must not be used in a shared library or
        in a threaded application.

    EXAMPLE

    BUGS

    SEE ALSO
        open(), read(), write(), fopen()

    INTERNALS

******************************************************************************/
{
    fdesc *fdesc;
    fcb *cblock;
    int lastclose;

    /* Atomic bookkeeping: lookup, unhook the slot and drop the share
       count in one section, so a concurrent close()/dup2() of the same
       fd can't double-free, and no reader can fetch the fdesc after it
       is on its way to the pool (the old code freed the fdesc *before*
       clearing the slot). The blocking DOS Close()/UnLock() stays
       outside the lock. */
    __fdesc_lock();
    if (!(fdesc = __getfdesc(fd)))
    {
        __fdesc_unlock();
        errno = EBADF;

        return -1;
    }
    __setfdesc(fd, NULL);
    cblock = fdesc->fcb;
    lastclose = (--cblock->opencount == 0);
    __fdesc_unlock();
    __free_fdesc(fdesc);

    if (lastclose)
    {
        /* Due to a *stupid* behaviour of the dos.library we cannot handle closing failures cleanly :-(
        if (
            !(fdesc->fcb->privflags & _FCB_DONTCLOSE_FH) &&
            !Close(fdesc->fh)
        )
        {
            fdesc->opencount++;
            errno = __stdc_ioerr2errno(IoErr());

            return -1;
        }
        */
        /* FIXME: Damn dos.library! We cannot report the error code correctly! This oughta change someday... */
        /* Since the dos.library destroys the file handle anyway, even if the closing fails, we cannot
           report the error code correctly, so just close the file and get out of here */

        if (!(cblock->privflags & _FCB_DONTCLOSE_FH))
        {
            // don't close directories because we don't Open() them.
            if (cblock->privflags & _FCB_ISDIR)
            {
                UnLock(cblock->handle);
            }
            else
            {
                Close(cblock->handle);
            }
        }

        FreeVec(cblock);
    }

    return 0;
} /* close */

