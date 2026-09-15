#include "gsys.h"

typedef unsigned long long u64;
u64 g_buf[8] __attribute__((used, aligned(32)));
u64 g_out[8] __attribute__((used, aligned(32)));

__asm__(
    ".text\n"
    ".globl _zero_upper\n"
    "_zero_upper:\n"
    "    vzeroupper\n"
    "    ret\n"
    ".globl _touch_only\n"
    "_touch_only:\n"
    "    addq $1, %rax\n"
    "    ret\n"
    ".globl _fill_ymm\n"
    "_fill_ymm:\n"
    "    leaq _g_buf(%rip), %rax\n"
    "    vmovdqu (%rax), %ymm0\n"
    "    vmovdqu 32(%rax), %ymm5\n"
    "    vpaddq %ymm5, %ymm0, %ymm7\n"
    "    ret\n"
    ".globl _dump_ymm\n"
    "_dump_ymm:\n"
    "    leaq _g_out(%rip), %rax\n"
    "    vmovdqu %ymm0, (%rax)\n"
    "    vmovdqu %ymm7, 32(%rax)\n"
    "    ret\n");
extern void zero_upper(void);
extern void touch_only(void);
extern void fill_ymm(void);
extern void dump_ymm(void);

static void show(const char *tag)
{
    g_puts(tag);
    for (int i = 0; i < 8; i++) { g_puts(" "); g_puthex64(g_out[i]); }
}

int main(void)
{
    void (*volatile fz)(void) = zero_upper;
    void (*volatile ff)(void) = fill_ymm;
    void (*volatile fd)(void) = dump_ymm;
    void (*volatile ft)(void) = touch_only;
    for (int i = 0; i < 8; i++) g_buf[i] = 0x1010101010101010ull * (u64)(i + 1);
    ff(); fd(); show("filled");
    fz(); fd(); show("zeroed");
    ff(); fz(); fz(); fd(); show("twice");
    ff(); ft(); fz(); ft(); fd(); show("gap");
    for (int r = 0; r < 3; r++) { ff(); fz(); fd(); show("loop"); }
    fill_ymm(); zero_upper(); dump_ymm(); show("inline");
    return 0;
}
