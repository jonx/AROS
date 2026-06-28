#if defined(__aarch64__)
#include <setjmp.h>

__asm__(
    ".text\n"
    ".balign 16\n"
    ".globl vfork\n"
    ".type vfork,%function\n"
    "vfork:\n"
    "    mov x9, x30\n"
    "    sub sp, sp, #256\n"
    "    mov x0, sp\n"
    "    bl setjmp\n"
    "    str x9, [sp, #0]\n"
    "    add x10, sp, #256\n"
    "    str x10, [sp, #104]\n"
    "    mov x0, sp\n"
    "    b __vfork\n"
);
#else
#error vfork() has to be implemented for each cpu
#endif
