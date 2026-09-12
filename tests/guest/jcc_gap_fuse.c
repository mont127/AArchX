/* A superblock side exit fused over a flag-neutral gap: cmp ; lea|mov ; jcc.
 * The gap used to be emitted after the compare, and emit_lea's pointer-rsp
 * form loads its base through JT0 - the temp that held the compared byte
 * for the cbz/cbnz - so `cmp byte [p],0 ; lea r15,[rsp+0x290] ; jne` took
 * the branch on rsp instead of the byte.  In libcef that copied an
 * unengaged optional's garbage and crashed Steam's CEF browser on its first
 * navigation (2026-09-06).  Every shape below runs with the byte 0 and 1 (or
 * the register equal and not); golden from the native run.
 *
 * The other shape is libSystem's backward UTF-8 scan, where the gap writes the
 * compare's index register: the NZCV-forwarding fallback used to re-load the
 * byte with the new index and spun forever.
 */
#include "gsys.h"

static volatile unsigned char flag0 = 0, flag1 = 1;
static volatile g_u64 v5 = 5, v9 = 9;

#define SHAPE(name, asmtext, ...)                                                   \
    static __attribute__((noinline)) g_u64 name(void)                                \
    {                                                                                \
        g_u64 r = 7;                                                                 \
        __asm__ __volatile__(asmtext                                                 \
                             "movq $0, %[r]\n\tjmp 2f\n\t1:\n\tmovq $1, %[r]\n\t2:\n\t" \
                             : [r] "+r"(r) : __VA_ARGS__ : "cc", "memory", "r15", "r14", "rax");  \
        return r;                                                                    \
    }

SHAPE(s_lea_rsp_jne, "cmpb $0, (%[p])\n\tleaq 0x290(%%rsp), %%r15\n\tjne 1f\n\t", [p] "r"(&flag0))
SHAPE(s_lea_rsp_jne1, "cmpb $0, (%[p])\n\tleaq 0x290(%%rsp), %%r15\n\tjne 1f\n\t", [p] "r"(&flag1))
SHAPE(s_lea_reg_je0, "cmpb $0, (%[p])\n\tleaq 0x10(%[p]), %%r15\n\tje 1f\n\t", [p] "r"(&flag0))
SHAPE(s_lea_reg_je1, "cmpb $0, (%[p])\n\tleaq 0x10(%[p]), %%r15\n\tje 1f\n\t", [p] "r"(&flag1))
SHAPE(s_movimm_jne0, "cmpb $0, (%[p])\n\tmovq $0x1234, %%r15\n\tjne 1f\n\t", [p] "r"(&flag0))
SHAPE(s_movimm_jne1, "cmpb $0, (%[p])\n\tmovq $0x1234, %%r15\n\tjne 1f\n\t", [p] "r"(&flag1))
SHAPE(s_movreg_je0, "cmpb $0, (%[p])\n\tmovq %%rsp, %%r15\n\tje 1f\n\t", [p] "r"(&flag0))
SHAPE(s_movreg_je1, "cmpb $0, (%[p])\n\tmovq %%rsp, %%r15\n\tje 1f\n\t", [p] "r"(&flag1))
SHAPE(s_cmpreg_lea_je_eq, "cmpq %[b], %[a]\n\tleaq 0x40(%%rsp), %%r15\n\tje 1f\n\t", [a] "r"(v5), [b] "r"(v5))
SHAPE(s_cmpreg_lea_je_ne, "cmpq %[b], %[a]\n\tleaq 0x40(%%rsp), %%r15\n\tje 1f\n\t", [a] "r"(v5), [b] "r"(v9))
SHAPE(s_cmpreg_lea_jb_lt, "cmpq %[b], %[a]\n\tleaq 0x40(%%rsp), %%r14\n\tjb 1f\n\t", [a] "r"(v5), [b] "r"(v9))
SHAPE(s_cmpreg_lea_jb_gt, "cmpq %[b], %[a]\n\tleaq 0x40(%%rsp), %%r14\n\tjb 1f\n\t", [a] "r"(v9), [b] "r"(v5))
SHAPE(s_test_lea_jne0, "testb $0x90, (%[p])\n\tleaq 8(%%rsp), %%r15\n\tjne 1f\n\t", [p] "r"(&flag0))
SHAPE(s_test_lea_jne1, "testb $0x90, (%[p])\n\tleaq 8(%%rsp), %%r15\n\tjne 1f\n\t", [p] "r"(&v9))
SHAPE(s_testreg_lea_je0, "testq %[a], %[a]\n\tleaq 8(%%rsp), %%r15\n\tje 1f\n\t", [a] "r"(v5))
SHAPE(s_testreg_lea_je1, "testq %[a], %[a]\n\tleaq 8(%%rsp), %%r15\n\tje 1f\n\t", [a] "r"((g_u64)0))
static __attribute__((noinline)) g_u64 lea_value(void)
{
    g_u64 r = 0, sp = 0;
    __asm__ __volatile__("movq %%rsp, %[sp]\n\tcmpb $0, (%[p])\n\tleaq 0x290(%%rsp), %%r15\n\tjne 1f\n\tmovq %%r15, %[r]\n\tjmp 2f\n\t1:\n\tmovq $0, %[r]\n\t2:\n\t"
                         : [r] "=&r"(r), [sp] "=&r"(sp) : [p] "r"(&flag0) : "cc", "memory", "r15");
    return r - sp;
}

static unsigned char utf[64];
static __attribute__((noinline)) g_u64 scan_back(g_u64 n)
{
    g_u64 r;
    __asm__ __volatile__(
        "1:\n\t"
        "leaq -1(%%rcx), %%rbx\n\t"
        "cmpb $0xc0, -1(%[p],%%rcx,1)\n\t"
        "movq %%rbx, %%rcx\n\t"
        "jb 2f\n\t"
        "testq %%rcx, %%rcx\n\t"
        "jne 1b\n\t"
        "2:\n\t"
        "movq %%rcx, %[r]\n\t"
        : [r] "=r"(r), "+c"(n) : [p] "r"(utf) : "cc", "memory", "rbx");
    return r;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    for (int pass = 0; pass < 3; pass++) {
        g_putu64_nonl(s_lea_rsp_jne()); g_puts(" "); g_putu64_nonl(s_lea_rsp_jne1()); g_puts(" ");
        g_putu64_nonl(s_lea_reg_je0()); g_puts(" "); g_putu64_nonl(s_lea_reg_je1()); g_puts(" ");
        g_putu64_nonl(s_movimm_jne0()); g_puts(" "); g_putu64_nonl(s_movimm_jne1()); g_puts(" ");
        g_putu64_nonl(s_movreg_je0()); g_puts(" "); g_putu64_nonl(s_movreg_je1()); g_puts(" ");
        g_putu64_nonl(s_cmpreg_lea_je_eq()); g_puts(" "); g_putu64_nonl(s_cmpreg_lea_je_ne()); g_puts(" ");
        g_putu64_nonl(s_cmpreg_lea_jb_lt()); g_puts(" "); g_putu64_nonl(s_cmpreg_lea_jb_gt()); g_puts(" ");
        g_putu64_nonl(s_test_lea_jne0()); g_puts(" "); g_putu64_nonl(s_test_lea_jne1()); g_puts(" ");
        g_putu64_nonl(s_testreg_lea_je0()); g_puts(" "); g_putu64_nonl(s_testreg_lea_je1()); g_puts(" ");
        g_putu64(lea_value());
    }
    for (int i = 0; i < 64; i++) utf[i] = (unsigned char)(i >= 40 ? 0xe0 : 0x41);
    g_putu64_nonl(scan_back(64)); g_puts(" "); g_putu64_nonl(scan_back(40)); g_puts(" "); g_putu64(scan_back(64));
    return 0;
}
