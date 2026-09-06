/* The initial stack must be 16-byte aligned at _start whatever the length of
 * argv, envp and the apple strings above it: an odd total once left Steam's
 * loader on a misaligned stack under the emulator, and SSE code in libSystem
 * then faults or wanders.  Prints rsp mod 16 as seen inside main (the ABI
 * makes it 0 here when _start was aligned) plus the environment size, and
 * exercises a 16-byte aligned stack store the way clang emits it. */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{
    g_u64 sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp));
    g_u64 envlen = 0;
    for (char **e = envp; *e; e++) envlen += g_strlen_local(*e) + 1;
    g_puts("rsp mod 16 = "); g_putu64(sp & 15);
    /* an aligned SSE spill on this stack must not fault */
    __attribute__((aligned(16))) g_u64 buf[4] = { 1, 2, 3, 4 };
    __asm__ __volatile__("movaps (%0), %%xmm0\n\tmovaps %%xmm0, 16(%0)" : : "r"(buf) : "xmm0", "memory");
    g_puts("movaps ok "); g_putu64(buf[2] == 1 && buf[3] == 2);
    (void)envlen;
    return 0;
}
