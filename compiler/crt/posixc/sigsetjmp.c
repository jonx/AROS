#if defined(__aarch64__)
#include <setjmp.h>

__asm__(
    ".text\n"
    ".balign 16\n"
    ".globl sigsetjmp\n"
    ".type sigsetjmp,%function\n"
    "sigsetjmp:\n"
    "    stp x19, x20, [x0, #8]\n"
    "    stp x21, x22, [x0, #24]\n"
    "    stp x23, x24, [x0, #40]\n"
    "    stp x25, x26, [x0, #56]\n"
    "    stp x27, x28, [x0, #72]\n"
    "    stp x29, x30, [x0, #88]\n"
    "    mov x1, sp\n"
    "    str x1, [x0, #104]\n"
    "    stp d8,  d9,  [x0, #112]\n"
    "    stp d10, d11, [x0, #128]\n"
    "    stp d12, d13, [x0, #144]\n"
    "    stp d14, d15, [x0, #160]\n"
    "    str x30, [x0, #0]\n"
    "    mov w0, #0\n"
    "    ret\n"
);
#else
#error sigsetjmp has to be implemented for each cpu
#endif
