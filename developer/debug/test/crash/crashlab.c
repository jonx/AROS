/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashLab - deliberately provoke each class of fault/error, one per
          argument, so crash-containment behaviour can be measured before and
          after a policy change. Every trigger prints a greppable marker
          ("[CRASHLAB] NAME: ...") BEFORE it fires, so a boot log always records
          exactly what was attempted even when the process dies.

    Usage: CrashLab <TRIGGER>
           CrashLab LIST        (print the trigger table and exit 0)

    This is a TEST tool. It is meant to crash. Do not ship it enabled.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/alerts.h>
#include <exec/tasks.h>
#include <dos/dostags.h>

#include <stdint.h>

static void say(const char *s)
{
    PutStr(s);
    Flush(Output());
}

/* Case-insensitive ASCII compare, no libc dependency. */
static int eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        UBYTE ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ---- individual triggers ---------------------------------------------- */

/* keep the pointer opaque so the compiler can't fold the access away */
static volatile IPTR g_sink;

static void t_nullread(void)
{
    volatile int *p = (volatile int *)(IPTR)0;
    say("[CRASHLAB] NULLREAD: reading address 0\n");
    g_sink = *p;
    say("[CRASHLAB] NULLREAD: SURVIVED (unexpected)\n");
}

static void t_nullwrite(void)
{
    volatile int *p = (volatile int *)(IPTR)0;
    say("[CRASHLAB] NULLWRITE: writing address 0\n");
    *p = 0x1234;
    say("[CRASHLAB] NULLWRITE: SURVIVED (unexpected)\n");
}

static void t_nulljump(void)
{
    void (*f)(void) = (void (*)(void))(IPTR)0;
    say("[CRASHLAB] NULLJUMP: calling a NULL function pointer\n");
    f();
    say("[CRASHLAB] NULLJUMP: SURVIVED (unexpected)\n");
}

static void t_wildwrite(void)
{
    volatile int *p = (volatile int *)(IPTR)0xdeadbee0ULL;
    say("[CRASHLAB] WILDWRITE: writing a wild pointer (0xdeadbee0)\n");
    *p = 0x1234;
    say("[CRASHLAB] WILDWRITE: SURVIVED (unexpected)\n");
}

static void t_illegal(void)
{
    say("[CRASHLAB] ILLEGAL: executing an undefined instruction\n");
    /* udf #0 -> SIGILL on AArch64 (not BRK, which is SIGTRAP). */
    __asm__ __volatile__(".inst 0x00000000");
    say("[CRASHLAB] ILLEGAL: SURVIVED (unexpected)\n");
}

static IPTR smash(volatile IPTR depth)
{
    volatile IPTR pad[16];
    pad[0] = depth;
    /* recurse forever; the volatile pad defeats tail-call folding */
    return pad[0] + smash(depth + 1);
}

static void t_stacksmash(void)
{
    say("[CRASHLAB] STACKSMASH: unbounded recursion (stack overflow)\n");
    g_sink = smash(0);
    say("[CRASHLAB] STACKSMASH: SURVIVED (unexpected)\n");
}

static void t_deadend(void)
{
    say("[CRASHLAB] DEADEND: Alert(AT_DeadEnd) - non-recoverable guru\n");
    Alert(AT_DeadEnd | AG_BadParm | AO_Unknown);
    say("[CRASHLAB] DEADEND: SURVIVED (unexpected)\n");
}

static void t_guru(void)
{
    say("[CRASHLAB] GURU: Alert() recoverable - should return if handled\n");
    Alert(AG_BadParm | AO_Unknown);
    say("[CRASHLAB] GURU: returned (recoverable alert handled)\n");
}

static void t_busyloop(void)
{
    say("[CRASHLAB] BUSYLOOP: infinite loop, task still preemptible\n");
    for (;;)
        g_sink++;
}

static void t_forbidloop(void)
{
    say("[CRASHLAB] FORBIDLOOP: Forbid() + infinite loop - wedges the scheduler\n");
    say("[CRASHLAB] FORBIDLOOP: only a host watchdog can catch this one\n");
    Forbid();
    for (;;)
        g_sink++;
}

/* child process that faults on its own; tests containment of a BACKGROUND task */
static void crashchild(void)
{
    volatile int *p = (volatile int *)(IPTR)0;
    *p = 0x1234;
}

static void t_crashtask(void)
{
    say("[CRASHLAB] CRASHTASK: spawning a child process that faults\n");
    CreateNewProcTags(
        NP_Entry,      (IPTR)crashchild,
        NP_Name,       (IPTR)"CrashLab-child",
        NP_StackSize,  (IPTR)16384,
        TAG_DONE);
    /* give the child time to run and fault */
    Delay(50);
    say("[CRASHLAB] CRASHTASK: parent still alive after child fault (containment works)\n");
}

/* ---- dispatch ---------------------------------------------------------- */

struct Trigger
{
    const char *name;
    void      (*fn)(void);
    const char *desc;
};

static const struct Trigger triggers[] =
{
    { "NULLREAD",   t_nullread,   "read address 0 (SIGSEGV)" },
    { "NULLWRITE",  t_nullwrite,  "write address 0 (SIGSEGV)" },
    { "NULLJUMP",   t_nulljump,   "call a NULL function pointer" },
    { "WILDWRITE",  t_wildwrite,  "write a wild pointer (SIGBUS/SIGSEGV)" },
    { "ILLEGAL",    t_illegal,    "undefined instruction (SIGILL)" },
    { "STACKSMASH", t_stacksmash, "stack overflow via infinite recursion" },
    { "DEADEND",    t_deadend,    "Alert(AT_DeadEnd) - dead-end guru" },
    { "GURU",       t_guru,       "recoverable Alert() guru" },
    { "BUSYLOOP",   t_busyloop,   "infinite loop (task stays preemptible)" },
    { "FORBIDLOOP", t_forbidloop, "Forbid()+loop (needs host watchdog)" },
    { "CRASHTASK",  t_crashtask,  "background child task faults" },
};
#define NTRIG (sizeof(triggers) / sizeof(triggers[0]))

static void list(void)
{
    ULONG i;
    say("CrashLab triggers:\n");
    for (i = 0; i < NTRIG; i++)
    {
        Printf("  %-11s %s\n", (IPTR)triggers[i].name, (IPTR)triggers[i].desc);
    }
    say("\nNote: integer divide-by-zero does NOT trap on AArch64 (UDIV/SDIV\n"
        "return 0), so there is no DIVZERO trigger.\n");
}

int main(int argc, char **argv)
{
    ULONG i;

    if (argc < 2)
    {
        say("usage: CrashLab <TRIGGER> | LIST\n");
        return RETURN_FAIL;
    }

    if (eq(argv[1], "LIST") || eq(argv[1], "-l") || eq(argv[1], "?"))
    {
        list();
        return RETURN_OK;
    }

    for (i = 0; i < NTRIG; i++)
    {
        if (eq(argv[1], triggers[i].name))
        {
            triggers[i].fn();
            /* Only BUSYLOOP/FORBIDLOOP never return; the rest either crashed
             * (we don't get here) or were contained/handled. */
            return RETURN_OK;
        }
    }

    Printf("CrashLab: unknown trigger '%s' (try: CrashLab LIST)\n", (IPTR)argv[1]);
    return RETURN_FAIL;
}
