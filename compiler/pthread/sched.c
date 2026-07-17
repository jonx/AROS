/*
  Copyright (C) 2014 Szilard Biro
  Copyright (C) 2018 Harry Sintonen

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

#include <proto/exec.h>
#include <exec/execbase.h>


#include "sched.h"
#include "debug.h"

#define PRIO_MAX 2
#define PRIO_MIN -2

int sched_get_priority_max(int policy)
{
    D(bug("%s(%d)\n", __FUNCTION__, policy));

    return PRIO_MAX;
}

int sched_get_priority_min(int policy)
{
    D(bug("%s(%d)\n", __FUNCTION__, policy));

    return PRIO_MIN;
}

int sched_yield(void)
{
#if defined(__MORPHOS__) || defined(__AMIGA__)
    D(bug("%s()\n", __FUNCTION__));
    // calling Permit() will trigger a reschedule
    Forbid();
#if !defined(__MORPHOS__)
    SysBase->SysFlags |= 1<<15; // trigger rescheduling on Permit();
#endif
    Permit();
#else
    D(bug("%s()\n", __FUNCTION__));

    /* Same cheap reschedule as the AmigaOS branch above: raise the pending
     * task-switch flag and let Permit() act on it (rom/exec/permit.c calls
     * KrnSchedule() when FLAG_SCHEDSWITCH_ISSET).
     *
     * This used to be a pair of SetTaskPri() calls ("changing the priority
     * will trigger a reschedule"): SetTaskPri() is a heavyweight operation --
     * it Forbid()s and pulls the task out of the scheduler's priority-sorted
     * ready list and reinserts it -- and sched_yield() is *hot*. Rust's
     * thread::yield_now() maps here, and crossbeam's Backoff::snooze() (used
     * by every channel recv(), i.e. every idle thread-pool worker) calls it in
     * a tight spin loop. Thousands of ready-list re-sorts per second per idle
     * worker burned an entire CPU and churned kernel list state on a machine
     * with one guest CPU. Setting the flag costs a few instructions and asks
     * the scheduler the same question. */
    Forbid();
    SysBase->AttnResched |= ARF_AttnSwitch;
    Permit();
#endif

    return 0;
}
