/* Flags returned across a `ret`.  clang's outliner emits helpers that end in
 * a compare and return its EFLAGS to the caller, which branches right after
 * the call (vImage's init_CGInterfaces checks: `cmpq $0, ptr(%rip); retq`).
 * The JIT once assumed no flags survive a return and painted every Wine
 * window black.  Each helper below ends in a pure flag producer; the caller
 * reads the flags with setcc/jcc without any compare of its own.  Also the
 * counter-case: a tail ending in arithmetic returns a value, not flags, and
 * the caller must not depend on them (it computes its own).  Goldens come
 * from the native binary. */
#include "gsys.h"

static volatile g_u64 g_ptr = 0, g_word = 0x80;
static volatile g_u64 g_a = 5, g_b = 9;

__asm__(
    ".text\n"
    ".globl _h_cmp_ptr\n_h_cmp_ptr:\n\tcmpq $0, (%rdi)\n\tret\n"
    ".globl _h_cmp_regs\n_h_cmp_regs:\n\tcmpq %rsi, %rdi\n\tret\n"
    ".globl _h_test_bit\n_h_test_bit:\n\ttestq $0x80, (%rdi)\n\tret\n"
    ".globl _h_bt\n_h_bt:\n\tbtq $7, (%rdi)\n\tret\n"
    ".globl _h_cmp_after_jcc\n_h_cmp_after_jcc:\n\tcmpq $1, %rdi\n\tjne 1f\n\tcmpq $0, (%rsi)\n1:\n\tret\n"
    ".globl _h_xor_tail\n_h_xor_tail:\n\tcmpq %rsi, %rdi\n\txorl %eax, %eax\n\tret\n"
);
extern void h_cmp_ptr(const volatile g_u64 *p);
extern void h_cmp_regs(g_u64 a, g_u64 b);
extern void h_test_bit(const volatile g_u64 *p);
extern void h_bt(const volatile g_u64 *p);
extern void h_cmp_after_jcc(g_u64 sel, const volatile g_u64 *p);
extern g_u64 h_xor_tail(g_u64 a, g_u64 b);

static g_u64 flags_after(void (*fn)(const volatile g_u64 *), const volatile g_u64 *p)
{
    g_u64 z, c, s, o;
    __asm__ __volatile__("call *%[fn]\n\tsetz %b[z]\n\tsetc %b[c]\n\tsets %b[s]\n\tseto %b[o]\n\t"
                         "movzbq %b[z], %[z]\n\tmovzbq %b[c], %[c]\n\tmovzbq %b[s], %[s]\n\tmovzbq %b[o], %[o]"
                         : [z] "=&r"(z), [c] "=&r"(c), [s] "=&r"(s), [o] "=&r"(o)
                         : [fn] "r"(fn), "D"(p) : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "memory", "cc");
    return z | (c << 1) | (s << 2) | (o << 3);
}

int main(int argc, char **argv, char **envp)
{
    g_u64 r;
    g_puts("cmp_ptr null "); g_putu64(flags_after(h_cmp_ptr, &g_ptr));
    g_ptr = 0x1000;
    g_puts("cmp_ptr set  "); g_putu64(flags_after(h_cmp_ptr, &g_ptr));
    g_puts("test_bit on  "); g_putu64(flags_after(h_test_bit, &g_word));
    g_word = 0x7f;
    g_puts("test_bit off "); g_putu64(flags_after(h_test_bit, &g_word));
    g_word = 0x80;
    g_puts("bt on        "); g_putu64(flags_after(h_bt, &g_word));

    /* two-register compare: branch straight after the call */
    __asm__ __volatile__("call _h_cmp_regs\n\tmovq $0, %[r]\n\tjae 1f\n\tmovq $1, %[r]\n1:"
                         : [r] "=&r"(r) : "D"(g_a), "S"(g_b) : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    g_puts("cmp_regs a<b "); g_putu64(r);
    __asm__ __volatile__("call _h_cmp_regs\n\tmovq $0, %[r]\n\tjae 1f\n\tmovq $1, %[r]\n1:"
                         : [r] "=&r"(r) : "D"(g_b), "S"(g_a) : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    g_puts("cmp_regs b<a "); g_putu64(r);

    /* the compare sits after a jcc inside the helper: flags reach the ret both ways */
    __asm__ __volatile__("call _h_cmp_after_jcc\n\tsetz %b[r]\n\tmovzbq %b[r], %[r]"
                         : [r] "=&r"(r) : "D"((g_u64)1), "S"(&g_ptr) : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    g_puts("after_jcc sel1 "); g_putu64(r);
    __asm__ __volatile__("call _h_cmp_after_jcc\n\tsetz %b[r]\n\tmovzbq %b[r], %[r]"
                         : [r] "=&r"(r) : "D"((g_u64)2), "S"(&g_ptr) : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    g_puts("after_jcc sel2 "); g_putu64(r);

    /* value-returning tail: the caller compares for itself */
    r = h_xor_tail(g_a, g_b);
    g_puts("xor_tail "); g_putu64(r); g_puts(" "); g_putu64(g_a < g_b);
    return 0;
}
