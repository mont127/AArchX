/*
 * The ordered-memory forms of loads that sign-extend, of stores and loads
 * through a base, an index and a displacement, and of adjacent 16-byte moves.
 *
 * Every instruction under test is written in assembly so that the compiler
 * cannot choose another, and runs at every offset from 0 to 63 of a buffer
 * whose bytes have their top bits set, three times over: an acquire-load or a
 * release-store that straddles a 16-byte boundary faults the first time, is
 * patched to an out-of-line arm, and has to give the same answer from the arm
 * as it gave inline.  The displacements are chosen to fall inside and outside
 * the nine signed bits the ordered forms can carry, on both sides of zero.
 */
#include "gsys.h"

__asm__(
    ".text\n"
    ".globl _t_sxd\n_t_sxd: movslq 5(%rdi,%rsi,4), %rax\n ret\n"
    ".globl _t_sxd0\n_t_sxd0: movslq (%rdi,%rsi,1), %rax\n ret\n"
    ".globl _t_sxw64\n_t_sxw64: movswq 3(%rdi,%rsi,2), %rax\n ret\n"
    ".globl _t_sxw32\n_t_sxw32: movq $-1, %rax\n movswl -2(%rdi,%rsi,1), %eax\n ret\n"
    ".globl _t_sxb64\n_t_sxb64: movsbq 300(%rdi,%rsi,1), %rax\n ret\n"
    ".globl _t_sxb32\n_t_sxb32: movq $-1, %rax\n movsbl -300(%rdi,%rsi,1), %eax\n ret\n"
    ".globl _t_sxd_far\n_t_sxd_far: movslq 4100(%rdi,%rsi,1), %rax\n ret\n"
    ".globl _t_sxd_self\n_t_sxd_self: movq %rsi, %rax\n movslq (%rdi,%rax,1), %rax\n ret\n"
    ".globl _t_st4\n_t_st4: movl %edx, 1(%rdi,%rsi,1)\n ret\n"
    ".globl _t_st8\n_t_st8: movq %rdx, -3(%rdi,%rsi,1)\n ret\n"
    ".globl _t_st2\n_t_st2: movw %dx, 300(%rdi,%rsi,1)\n ret\n"
    ".globl _t_st1\n_t_st1: movb %dl, -300(%rdi,%rsi,1)\n ret\n"
    ".globl _t_ld4\n_t_ld4: movl 2(%rdi,%rsi,1), %eax\n ret\n"
    ".globl _t_ld8\n_t_ld8: movq -5(%rdi,%rsi,1), %rax\n ret\n"
    ".globl _t_loop\n_t_loop: xorl %eax, %eax\n xorl %ecx, %ecx\n"
    "1: movslq (%rdi,%rcx,1), %rdx\n addq %rdx, %rax\n movswq 1(%rdi,%rcx,1), %rdx\n"
    " xorq %rdx, %rax\n movsbq 7(%rdi,%rcx,1), %rdx\n addq %rdx, %rax\n rolq $3, %rax\n"
    " incq %rcx\n cmpq %rsi, %rcx\n jne 1b\n ret\n"
    ".globl _t_vpair\n_t_vpair: movups (%rdi,%rsi,1), %xmm0\n movups 16(%rdi,%rsi,1), %xmm1\n"
    " movups %xmm0, (%rdx,%rsi,1)\n movups %xmm1, 16(%rdx,%rsi,1)\n ret\n"
);

extern g_u64 t_sxd(const char *, g_u64), t_sxd0(const char *, g_u64), t_sxw64(const char *, g_u64);
extern g_u64 t_sxw32(const char *, g_u64), t_sxb64(const char *, g_u64), t_sxb32(const char *, g_u64);
extern g_u64 t_sxd_far(const char *, g_u64), t_sxd_self(const char *, g_u64);
extern void t_st4(char *, g_u64, g_u64), t_st8(char *, g_u64, g_u64), t_st2(char *, g_u64, g_u64);
extern void t_st1(char *, g_u64, g_u64);
extern g_u64 t_ld4(const char *, g_u64), t_ld8(const char *, g_u64), t_loop(const char *, g_u64);
extern void t_vpair(const char *, g_u64, char *);

#define AREA 8192
static char area[AREA + 64];
static char out[AREA + 64];

static g_u64 mix(g_u64 h, g_u64 v)
{
    h ^= v;
    h *= 0x100000001b3ull;
    return (h << 7) | (h >> 57);
}

static void fill(char *p, g_u64 n, g_u64 seed)
{
    for (g_u64 i = 0; i < n; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        p[i] = (char)(0x80 | (seed >> 56));
    }
}

int main(void)
{
    char *mid = area + 2048;
    g_u64 h;

    fill(area, sizeof area, 7);
    h = 0;
    for (int round = 0; round < 3; round++)
        for (g_u64 o = 0; o < 64; o++) {
            h = mix(h, t_sxd(mid, o));
            h = mix(h, t_sxd0(mid, o));
            h = mix(h, t_sxw64(mid, o));
            h = mix(h, t_sxw32(mid, o));
            h = mix(h, t_sxb64(mid, o));
            h = mix(h, t_sxb32(mid, o));
            h = mix(h, t_sxd_far(area, o));
            h = mix(h, t_sxd_self(mid, o));
            h = mix(h, t_ld4(mid, o));
            h = mix(h, t_ld8(mid, o));
        }
    g_puts("loads  ");
    g_puthex64(h);

    h = 0;
    for (int round = 0; round < 3; round++)
        for (g_u64 n = 1; n < 80; n += 13)
            h = mix(h, t_loop(mid + round, n));
    g_puts("loop   ");
    g_puthex64(h);

    h = 0;
    for (int round = 0; round < 3; round++)
        for (g_u64 o = 0; o < 64; o++) {
            g_u64 v = 0x8877665544332211ull * (o + 1) + (g_u64)round;
            t_st4(mid, o, v);
            t_st8(mid, o + 100, v ^ 0x5a5a5a5a5a5a5a5aull);
            t_st2(mid, o, v >> 9);
            t_st1(mid, o, v >> 3);
            h = mix(h, t_ld8(mid, o + 5));
            h = mix(h, t_ld8(mid, o + 102));
        }
    for (g_u64 i = 0; i < sizeof area; i++)
        h = mix(h, (unsigned char)area[i]);
    g_puts("stores ");
    g_puthex64(h);

    fill(area, sizeof area, 99);
    for (g_u64 i = 0; i < sizeof out; i++) out[i] = 0;
    for (int round = 0; round < 3; round++)
        for (g_u64 o = 0; o < 64; o++)
            t_vpair(mid, o * 33, out + 1024);
    h = 0;
    for (g_u64 i = 0; i < sizeof out; i++)
        h = mix(h, (unsigned char)out[i]);
    g_puts("pairs  ");
    g_puthex64(h);
    sys_exit(0);
    return 0;
}
