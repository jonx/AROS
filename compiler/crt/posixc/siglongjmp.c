/*
    Copyright (C) 2015, The AROS Development Team. All rights reserved.

    Desc: POSIX.1-2008 function siglongjmp()
*/

/******************************************************************************

    NAME
#include <setjmp.h>

        void siglongjmp (

/*  SYNOPSIS
        jmp_buf env,
        int val)

    FUNCTION
        Save the current context so that you can return to it later.

    INPUTS
        env - The context/environment to restore
        val - This value is returned by setjmp() when you return to the
                saved context. You cannot return 0. If val is 0, then
                setjmp() returns with 1.

    RESULT
        This function doesn't return.

    NOTES

    EXAMPLE
        jmp_buf env;

        ... some code ...

        if (!setjmp (env))
        {
            ... this code is executed after setjmp() returns ...

            // This is no good example on how to use this function
            // You should not do that
            if (error)
                siglongjmp (env, 5);

            ... some code ...
        }
        else
        {
            ... this code is executed if you call siglongjmp(env) ...
        }

    BUGS

    SEE ALSO
        stdc/setjmp()

    INTERNALS

******************************************************************************/

#if defined(__aarch64__)
#include <setjmp.h>

__asm__(
    ".text\n"
    ".balign 16\n"
    ".globl siglongjmp\n"
    ".type siglongjmp,%function\n"
    "siglongjmp:\n"
    "    ldp x19, x20, [x0, #8]\n"
    "    ldp x21, x22, [x0, #24]\n"
    "    ldp x23, x24, [x0, #40]\n"
    "    ldp x25, x26, [x0, #56]\n"
    "    ldp x27, x28, [x0, #72]\n"
    "    ldp x29, x30, [x0, #88]\n"
    "    ldr x2, [x0, #104]\n"
    "    mov sp, x2\n"
    "    ldp d8,  d9,  [x0, #112]\n"
    "    ldp d10, d11, [x0, #128]\n"
    "    ldp d12, d13, [x0, #144]\n"
    "    ldp d14, d15, [x0, #160]\n"
    "    cmp w1, #0\n"
    "    mov w0, w1\n"
    "    cinc w0, w0, eq\n"
    "    ret\n"
);
#else
#error siglongjmp has to be implemented for each cpu
#endif
