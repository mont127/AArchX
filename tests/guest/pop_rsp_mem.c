/* POP with an (R)SP-relative memory destination.  SDM, POP: "If the ESP
 * register is used as a base register for addressing a destination operand in
 * memory, the POP instruction computes the effective address of the operand
 * after it increments the ESP register."  PUSH reads an (R)SP-relative source
 * with the pre-decrement (R)SP.  V8's TailCallRuntime trampoline moves the
 * return address with `pop qword ptr [rsp+0x98]` right before jumping to
 * CEntry; computing that address with the old rsp leaves CEntry a stale heap
 * pointer as its return address (Steam's renderer then "returned" into a JS
 * object).  Goldens come from the native binary. */
#include "gsys.h"

int main(void)
{
    g_u64 a = 0, b = 0, c = 0, d = 0;

    /* pop [rsp+16]: destination is (rsp+8)+16 = old+24 */
    __asm__ volatile(
        "subq $64, %%rsp\n\t"
        "movq $0x1111, 0(%%rsp)\n\t"
        "movq $0x2222, 8(%%rsp)\n\t"
        "movq $0x3333, 16(%%rsp)\n\t"
        "movq $0x4444, 24(%%rsp)\n\t"
        "movq $0x5555, 32(%%rsp)\n\t"
        "popq 16(%%rsp)\n\t"
        "movq 0(%%rsp), %[a]\n\t"
        "movq 8(%%rsp), %[b]\n\t"
        "movq 16(%%rsp), %[c]\n\t"
        "movq 24(%%rsp), %[d]\n\t"
        "addq $56, %%rsp"
        : [a]"=&r"(a), [b]"=&r"(b), [c]"=&r"(c), [d]"=&r"(d) : : "memory", "cc");
    g_puts("pop [rsp+16]:"); g_puthex64(a); g_puthex64(b); g_puthex64(c); g_puthex64(d);

    /* pop [rsp]: destination is old+8, i.e. the slot right above the popped one */
    __asm__ volatile(
        "subq $32, %%rsp\n\t"
        "movq $0xbbbb, 0(%%rsp)\n\t"
        "movq $0xcccc, 8(%%rsp)\n\t"
        "movq $0xdddd, 16(%%rsp)\n\t"
        "popq (%%rsp)\n\t"
        "movq 0(%%rsp), %[a]\n\t"
        "movq 8(%%rsp), %[b]\n\t"
        "addq $24, %%rsp"
        : [a]"=&r"(a), [b]"=&r"(b) : : "memory", "cc");
    g_puts("pop [rsp]:"); g_puthex64(a); g_puthex64(b);

    /* push [rsp+8]: source read with the pre-decrement rsp */
    __asm__ volatile(
        "subq $32, %%rsp\n\t"
        "movq $0x6666, 0(%%rsp)\n\t"
        "movq $0x7777, 8(%%rsp)\n\t"
        "movq $0x8888, 16(%%rsp)\n\t"
        "pushq 8(%%rsp)\n\t"
        "movq 0(%%rsp), %[a]\n\t"
        "movq 8(%%rsp), %[b]\n\t"
        "movq 16(%%rsp), %[c]\n\t"
        "addq $40, %%rsp"
        : [a]"=&r"(a), [b]"=&r"(b), [c]"=&r"(c) : : "memory", "cc");
    g_puts("push [rsp+8]:"); g_puthex64(a); g_puthex64(b); g_puthex64(c);

    /* the V8 shape: pop [rsp+0x98] then add rsp,0x98 must leave the popped
     * value at the new top of stack */
    __asm__ volatile(
        "subq $0xb0, %%rsp\n\t"
        "movq $0x600d, 0(%%rsp)\n\t"
        "movq $0xbad0, 0x98(%%rsp)\n\t"
        "movq $0xbad1, 0xa0(%%rsp)\n\t"
        "movq $0xbad2, 0xa8(%%rsp)\n\t"
        "popq 0x98(%%rsp)\n\t"
        "addq $0x98, %%rsp\n\t"
        "movq 0(%%rsp), %[a]\n\t"
        "movq 8(%%rsp), %[b]\n\t"
        "addq $0x10, %%rsp"
        : [a]"=&r"(a), [b]"=&r"(b) : : "memory", "cc");
    g_puts("v8 shape:"); g_puthex64(a); g_puthex64(b);
    return 0;
}
