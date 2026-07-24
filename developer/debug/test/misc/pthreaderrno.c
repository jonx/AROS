/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Verify per-task C errno: two threads each set errno to a distinct value and
    check it survives a yield to the sibling thread. With a shared (per-process)
    errno the threads clobber each other; with per-task errno each keeps its own.
*/

#include <pthread.h>
#include <errno.h>
#include <proto/dos.h>
#include <proto/exec.h>

static void *worker(void *arg)
{
    long me = (long)arg;
    int i, bad = 0;

    for (i = 0; i < 40; i++)
    {
        errno = (int)me;
        Delay(1);                 /* ~20ms: yield so the sibling runs + sets errno */
        if (errno != (int)me)
            bad++;
    }

    return (void *)(long)bad;
}

int main(void)
{
    pthread_t t1, t2;
    void *r1 = (void *)-1, *r2 = (void *)-1;

    if (pthread_create(&t1, 0, worker, (void *)100) != 0 ||
        pthread_create(&t2, 0, worker, (void *)200) != 0)
    {
        Printf("[ERRNO] FAIL: pthread_create\n");
        return 20;
    }

    pthread_join(t1, &r1);
    pthread_join(t2, &r2);

    if ((long)r1 == 0 && (long)r2 == 0)
        Printf("[ERRNO] PASS: per-thread errno held across interleavings\n");
    else
        Printf("[ERRNO] FAIL: errno clobbered (t1 bad=%ld t2 bad=%ld)\n",
               (long)r1, (long)r2);

    return 0;
}
