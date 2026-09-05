/* mov r/m, Sreg reads the live selector.  The decoder once folded CS and SS
 * into constants (0x2b/0x23), which holds in long mode but not for the 32-bit
 * side of a WoW64 process, whose CS is an LDT selector: wine's 32-bit
 * RtlCaptureContext stored 0x2b, and the iretq that later restored that
 * context returned to 32-bit code in 64-bit mode (Steam died at its first
 * OutputDebugString).  This test pins the long-mode values and the width
 * rules: a register destination is zero-extended to the operand size, a
 * 16-bit destination and a memory destination touch 16 bits only.  Goldens
 * come from the native binary. */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{
    g_u64 r;
    volatile g_u64 buf;

    __asm__ __volatile__("movq $-1, %0\n\tmovl %%cs, %k0" : "=r"(r));
    g_puts("cs32 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovw %%cs, %w0" : "=r"(r));
    g_puts("cs16 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovq %%cs, %0" : "=r"(r));
    g_puts("cs64 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovl %%ss, %k0" : "=r"(r));
    g_puts("ss32 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovw %%ss, %w0" : "=r"(r));
    g_puts("ss16 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovl %%ds, %k0" : "=r"(r));
    g_puts("ds32 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovl %%es, %k0" : "=r"(r));
    g_puts("es32 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovl %%fs, %k0" : "=r"(r));
    g_puts("fs32 "); g_putu64(r);
    __asm__ __volatile__("movq $-1, %0\n\tmovl %%gs, %k0" : "=r"(r));
    g_puts("gs32 "); g_putu64(r);

    buf = ~(g_u64)0;
    __asm__ __volatile__("movw %%cs, (%0)" : : "r"(&buf) : "memory");
    g_puts("cs mem "); g_putu64(buf);
    buf = ~(g_u64)0;
    __asm__ __volatile__("movw %%ss, 2(%0)" : : "r"(&buf) : "memory");
    g_puts("ss mem+2 "); g_putu64(buf);
    return 0;
}
