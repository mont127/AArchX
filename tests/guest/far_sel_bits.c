/* A selector slot on the stack is 16 bits wide whatever the slot size: iretq
 * and lretq take the low half and ignore the rest.  Wine's WoW64 layer keeps
 * SegCs and SegSs in DWORD fields that a 16-bit store fills, so the iretq
 * that returns to 32-bit code sees stack garbage above the selector; the
 * emulator once read the whole slot, failed the descriptor lookup and stayed
 * in 64-bit mode.  Goldens come from the native binary. */
#include "gsys.h"

static g_u64 iret_garbage(void)
{
    g_u64 cs;
    __asm__ __volatile__(
        "movq %%rsp, %%rax\n\t"
        "movq $0xdead0023, %%rdx\n\tpushq %%rdx\n\t"       /* SS slot, junk above the selector */
        "pushq %%rax\n\t"                                   /* RSP */
        "pushfq\n\t"                                        /* RFLAGS */
        "movq $0xbeef002b, %%rdx\n\tpushq %%rdx\n\t"       /* CS slot, junk above the selector */
        "leaq 1f(%%rip), %%rdx\n\tpushq %%rdx\n\t"         /* RIP */
        "iretq\n"
        "1:\n\tmovl %%cs, %k0"
        : "=r"(cs) : : "rax", "rdx", "memory", "cc");
    return cs;
}

static g_u64 lret_garbage(void)
{
    g_u64 cs;
    __asm__ __volatile__(
        "movq $0xcafe002b, %%rdx\n\tpushq %%rdx\n\t"       /* CS slot */
        "leaq 1f(%%rip), %%rdx\n\tpushq %%rdx\n\t"         /* RIP */
        "lretq\n"
        "1:\n\tmovl %%cs, %k0"
        : "=r"(cs) : : "rdx", "memory", "cc");
    return cs;
}

int main(int argc, char **argv, char **envp)
{
    g_u64 marker = 0x1234;
    g_puts("iret cs "); g_putu64(iret_garbage());
    g_puts("lret cs "); g_putu64(lret_garbage());
    g_puts("marker "); g_putu64(marker);
    return 0;
}
