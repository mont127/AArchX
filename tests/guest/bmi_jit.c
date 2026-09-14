#include "gsys.h"

typedef unsigned long long u64;
typedef unsigned int u32;

static u64 vals[12] = { 0, 1, 0x80, 0xffffffffffffffffull, 0x8000000000000000ull, 0x123456789abcdef0ull,
                        0x00000000fedcba98ull, 0xdeadbeef00000000ull, 0x7fffffff, 0x80000000, 0x0f0f0f0f0f0f0f0full, 0xa5a5a5a5a5a5a5a5ull };
static u64 cnts[8] = { 0, 1, 7, 31, 32, 63, 64, 0x1ff };
static u64 out[64];

#define OP2(name, mn) \
__attribute__((noinline)) static void name(const u64 *a, u64 *o) \
{ \
    __asm__ volatile("movq (%0), %%rsi\n\t" mn "q %%rsi, %%rax\n\tmovq %%rax, (%1)\n\t" \
                     "movq $-1, %%rax\n\t" mn "l %%esi, %%eax\n\tmovq %%rax, 8(%1)\n\t" \
                     mn "q (%0), %%rdx\n\tmovq %%rdx, 16(%1)" \
                     :: "r"(a), "r"(o) : "memory", "rax", "rdx", "rsi"); \
}
OP2(t_blsr, "blsr") OP2(t_blsmsk, "blsmsk") OP2(t_blsi, "blsi")

#define OP3(name, mn) \
__attribute__((noinline)) static void name(const u64 *a, const u64 *c, u64 *o) \
{ \
    __asm__ volatile("movq (%0), %%rsi\n\tmovq (%1), %%rdx\n\t" mn "q %%rdx, %%rsi, %%rax\n\tmovq %%rax, (%2)\n\t" \
                     "movq $-1, %%rax\n\t" mn "l %%edx, %%esi, %%eax\n\tmovq %%rax, 8(%2)\n\t" \
                     mn "q %%rdx, (%0), %%rdi\n\tmovq %%rdi, 16(%2)\n\t" \
                     "movq $-1, %%rdi\n\t" mn "l %%edx, (%0), %%edi\n\tmovq %%rdi, 24(%2)\n\t" \
                     "movq %%rsi, %%rax\n\t" mn "q %%rdx, %%rax, %%rax\n\tmovq %%rax, 32(%2)\n\t" \
                     "movq %%rdx, %%rax\n\t" mn "q %%rax, %%rsi, %%rax\n\tmovq %%rax, 40(%2)" \
                     :: "r"(a), "r"(c), "r"(o) : "memory", "rax", "rdx", "rsi", "rdi"); \
}
OP3(t_bzhi, "bzhi") OP3(t_shlx, "shlx") OP3(t_shrx, "shrx") OP3(t_sarx, "sarx")

__attribute__((noinline)) static void t_andn(const u64 *a, const u64 *c, u64 *o)
{
    __asm__ volatile("movq (%0), %%rsi\n\tmovq (%1), %%rdx\n\tandnq %%rdx, %%rsi, %%rax\n\tmovq %%rax, (%2)\n\t"
                     "movq $-1, %%rax\n\tandnl %%edx, %%esi, %%eax\n\tmovq %%rax, 8(%2)\n\t"
                     "andnq (%1), %%rsi, %%rdi\n\tmovq %%rdi, 16(%2)\n\t"
                     "movq %%rsi, %%rax\n\tandnq %%rdx, %%rax, %%rax\n\tmovq %%rax, 24(%2)\n\t"
                     "andnq %%rdx, %%rsi, %%rax\n\tsetz %%al\n\tsetc %%ah\n\tmovzwl %%ax, %%eax\n\tmovq %%rax, 32(%2)\n\t"
                     "andnl %%edx, %%esi, %%eax\n\tsets %%al\n\tseto %%ah\n\tmovzwl %%ax, %%eax\n\tmovq %%rax, 40(%2)"
                     :: "r"(a), "r"(c), "r"(o) : "memory", "rax", "rdx", "rsi", "rdi", "cc");
}
__attribute__((noinline)) static void t_rorx(const u64 *a, u64 *o)
{
    __asm__ volatile("movq (%0), %%rsi\n\trorxq $13, %%rsi, %%rax\n\tmovq %%rax, (%1)\n\t"
                     "movq $-1, %%rax\n\trorxl $7, %%esi, %%eax\n\tmovq %%rax, 8(%1)\n\t"
                     "rorxq $63, (%0), %%rdx\n\tmovq %%rdx, 16(%1)\n\t"
                     "rorxq $0, %%rsi, %%rax\n\tmovq %%rax, 24(%1)\n\t"
                     "movq $-1, %%rax\n\trorxl $0, %%esi, %%eax\n\tmovq %%rax, 32(%1)\n\t"
                     "movq %%rsi, %%rax\n\trorxq $40, %%rax, %%rax\n\tmovq %%rax, 40(%1)"
                     :: "r"(a), "r"(o) : "memory", "rax", "rdx", "rsi");
}
__attribute__((noinline)) static void t_mulx(const u64 *a, const u64 *c, u64 *o)
{
    __asm__ volatile("movq (%0), %%rdx\n\tmovq (%1), %%rsi\n\tmulxq %%rsi, %%r8, %%rax\n\tmovq %%rax, (%2)\n\tmovq %%r8, 8(%2)\n\t"
                     "movq $-1, %%rax\n\tmovq $-1, %%r8\n\tmulxl %%esi, %%r8d, %%eax\n\tmovq %%rax, 16(%2)\n\tmovq %%r8, 24(%2)\n\t"
                     "mulxq (%1), %%r8, %%rax\n\tmovq %%rax, 32(%2)\n\tmovq %%r8, 40(%2)\n\t"
                     "mulxq %%rsi, %%rax, %%rax\n\tmovq %%rax, 48(%2)\n\t"
                     "movq %%rdx, %%rax\n\tmulxq %%rax, %%r8, %%rax\n\tmovq %%rax, 56(%2)"
                     :: "r"(a), "r"(c), "r"(o) : "memory", "rax", "rdx", "rsi", "r8");
}

static void dump(const char *name, int n)
{
    for (int i = 0; i < n; i++) { g_puts(name); g_puts(" "); g_putu64_nonl((u64)i); g_puts(" "); g_puthex64(out[i]); }
}

int main(void)
{
    for (int pass = 0; pass < 2; pass++) {
        int last = pass == 1;
        for (int i = 0; i < 12; i++) {
            t_blsr(&vals[i], out); if (last) dump("blsr", 3);
            t_blsmsk(&vals[i], out); if (last) dump("blsmsk", 3);
            t_blsi(&vals[i], out); if (last) dump("blsi", 3);
            t_rorx(&vals[i], out); if (last) dump("rorx", 6);
            for (int c = 0; c < 8; c++) {
                t_bzhi(&vals[i], &cnts[c], out); if (last) dump("bzhi", 6);
                t_shlx(&vals[i], &cnts[c], out); if (last) dump("shlx", 6);
                t_shrx(&vals[i], &cnts[c], out); if (last) dump("shrx", 6);
                t_sarx(&vals[i], &cnts[c], out); if (last) dump("sarx", 6);
            }
            for (int j = 0; j < 12; j += 3) {
                t_andn(&vals[i], &vals[j], out); if (last) dump("andn", 6);
                t_mulx(&vals[i], &vals[j], out); if (last) dump("mulx", 8);
            }
        }
    }
    g_puts("DONE\n");
    return 0;
}
