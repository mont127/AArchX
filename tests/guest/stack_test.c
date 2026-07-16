/*
 * tests/guest/stack_test.c
 *
 * Targets the exact ordering traps in the JIT's inlined PUSH/POP
 * (emit_push_pop in src/jit.c), which the rest of the suite does not reach.
 *
 * The two cases that matter are the ones where the stack pointer is ALSO the
 * operand, because then the order of the memory access and the rsp update is
 * observable rather than an implementation detail:
 *
 *   push %rsp  must push the OLD rsp (the value is read before rsp moves).
 *              Getting it backwards pushes rsp-8 and nothing else notices.
 *   pop  %rsp  must end with rsp = the POPPED VALUE, not the incremented rsp
 *              (the destination write lands after the rsp adjust).
 *
 * Both are written in inline asm so no compiler choice can lower them to some
 * other instruction and quietly stop testing the thing. Everything is checked
 * against values computed with plain arithmetic, and the results print as
 * deterministic text so the golden pins them.
 *
 * Also covers ordinary push/pop round-tripping (including an immediate and the
 * 8 callee-saved-ish GPRs) so a register-slot mix-up in the inlined form shows
 * up as a wrong value rather than a crash.
 */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{
    /* --- push %rsp pushes the OLD rsp --- */
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

    /* --- pop %rsp lands the popped value in rsp --- */
    {
        /* Build a small scratch area, point rsp at a cell holding a known
         * value, pop it into rsp, and read rsp back. The real rsp is saved and
         * restored around the sequence so the program survives. */
        static g_u64 cell[2];
        g_u64 target = (g_u64)(g_u64 *)&cell[0];
        g_u64 got = 0;
        cell[0] = 0xfeedfacecafebeefULL;
        __asm__ __volatile__(
            "movq %%rsp, %%r15\n"     /* save real rsp */
            "movq %[tgt], %%rsp\n"    /* rsp -> &cell[0] */
            "popq %%rsp\n"            /* rsp = cell[0] */
            "movq %%rsp, %[got]\n"
            "movq %%r15, %%rsp\n"     /* restore */
            : [got] "=&r"(got)
            : [tgt] "r"(target)
            : "r15", "memory");
        if (got == 0xfeedfacecafebeefULL)
            g_puts("pop rsp: popped value\n");
        else
            g_puts("pop rsp: WRONG\n");
    }

    /* --- ordinary round-trip through every general register slot --- */
    {
        static const g_u64 want[8] = { 0x5a5a, 0x1111, 0x2222, 0x3333,
                                      0x4444, 0x5555, 0x6666, 0x7777 };
        g_u64 out[8];
        int ok = 1;

        /* One push/pop pair at a time: the point is that each register slot
         * round-trips through memory, and doing them in one asm block only adds
         * register pressure without testing anything more. */
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

    /* --- push/pop keep the stack balanced --- */
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
