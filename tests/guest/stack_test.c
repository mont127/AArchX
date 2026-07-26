/* Ordering traps in the JIT's inlined push and pop. */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{

    {
        g_u64 sp_before = 0, pushed = 0;
        __asm__ __volatile__(
            "movq %%rsp, %[before]\n"
            "pushq %%rsp\n"
            "popq  %[pushed]\n"
            : [before] "=&r"(sp_before), [pushed] "=&r"(pushed)
            :
            : "memory");
        if (pushed == sp_before)
            g_puts("push rsp: old rsp\n");
        else
            g_puts("push rsp: WRONG\n");
    }

    {

        static g_u64 cell[2];
        g_u64 target = (g_u64)(g_u64 *)&cell[0];
        g_u64 got = 0;
        cell[0] = 0xfeedfacecafebeefULL;
        __asm__ __volatile__(
            "movq %%rsp, %%r15\n"
            "movq %[tgt], %%rsp\n"
            "popq %%rsp\n"
            "movq %%rsp, %[got]\n"
            "movq %%r15, %%rsp\n"
            : [got] "=&r"(got)
            : [tgt] "r"(target)
            : "r15", "memory");
        if (got == 0xfeedfacecafebeefULL)
            g_puts("pop rsp: popped value\n");
        else
            g_puts("pop rsp: WRONG\n");
    }

    {
        static const g_u64 want[8] = { 0x5a5a, 0x1111, 0x2222, 0x3333,
                                      0x4444, 0x5555, 0x6666, 0x7777 };
        g_u64 out[8];
        int ok = 1;

        __asm__ __volatile__("pushq $0x5a5a\n popq %[o]\n"
                             : [o] "=r"(out[0]) : : "memory");
        __asm__ __volatile__("pushq %%rax\n popq %[o]\n"
                             : [o] "=r"(out[1]) : "a"((g_u64)0x1111) : "memory");
        __asm__ __volatile__("pushq %%rbx\n popq %[o]\n"
                             : [o] "=r"(out[2]) : "b"((g_u64)0x2222) : "memory");
        __asm__ __volatile__("pushq %%rcx\n popq %[o]\n"
                             : [o] "=r"(out[3]) : "c"((g_u64)0x3333) : "memory");
        __asm__ __volatile__("pushq %%rdx\n popq %[o]\n"
                             : [o] "=r"(out[4]) : "d"((g_u64)0x4444) : "memory");
        __asm__ __volatile__("pushq %%rsi\n popq %[o]\n"
                             : [o] "=r"(out[5]) : "S"((g_u64)0x5555) : "memory");
        __asm__ __volatile__("pushq %%rdi\n popq %[o]\n"
                             : [o] "=r"(out[6]) : "D"((g_u64)0x6666) : "memory");
        __asm__ __volatile__("movq $0x7777, %%r12\n pushq %%r12\n popq %[o]\n"
                             : [o] "=r"(out[7]) : : "r12", "memory");

        for (int i = 0; i < 8; i++)
            if (out[i] != want[i])
                ok = 0;
        g_puts(ok ? "roundtrip: ok\n" : "roundtrip: WRONG\n");
    }

    {
        g_u64 before = 0, after = 0;
        __asm__ __volatile__(
            "movq %%rsp, %[b]\n"
            "pushq $1\n pushq $2\n pushq $3\n"
            "popq %%rax\n popq %%rax\n popq %%rax\n"
            "movq %%rsp, %[a]\n"
            : [b] "=&r"(before), [a] "=&r"(after)
            :
            : "rax", "memory");
        g_puts(before == after ? "balance: ok\n" : "balance: WRONG\n");
    }

    return 0;
}
