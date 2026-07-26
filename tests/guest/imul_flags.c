/* Gate for the inlined IMUL and its flags. */
#include "gsys.h"

#define ARITH_MASK 0x0cd5ULL

#define CFOF_MASK  0x0801ULL

static g_u64 sum_arch;
static g_u64 sum_pinned;

static void obs(g_u64 r, g_u64 f)
{
    sum_arch = sum_arch * 1000003 + (r ^ ((f & CFOF_MASK) * 0x100000001ULL));
    sum_pinned = sum_pinned * 1000003 + (r ^ ((f & ARITH_MASK) * 0x100000001ULL));
}

static void cap_imul64_rr(g_u64 a, g_u64 b)
{
    g_u64 r, f;
    __asm__ volatile("imulq %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "r"(b) : "cc");
    obs(r, f);
}

static void cap_imul32_rr(g_u32 a, g_u32 b)
{
    g_u64 r = 0, f;
    __asm__ volatile("imull %k3, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    obs(r, f);
}

static void cap_imul64_rri8(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("imulq $-5, %2, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"(a) : "cc");
    obs(r, f);
}

static void cap_imul64_rri32(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("imulq $0x7fffffff, %2, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"(a) : "cc");
    obs(r, f);
    __asm__ volatile("imulq $-2147483648, %2, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"(a) : "cc");
    obs(r, f);
}

static void cap_imul32_rri8(g_u32 a)
{
    g_u64 r = 0, f;
    __asm__ volatile("imull $-5, %k2, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"((g_u64)a) : "cc");
    obs(r, f);
}

static void cap_imul32_rri32(g_u32 a)
{
    g_u64 r = 0, f;
    __asm__ volatile("imull $0x80000000, %k2, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"((g_u64)a) : "cc");
    obs(r, f);
}

static void cap_imul64_square(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("imulq %0, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a) : "cc");
    obs(r, f);
}

static void cap_imul16_rr(unsigned short a, unsigned short b)
{
    g_u64 r = 0, f;
    __asm__ volatile("imulw %w3, %w0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    obs(r, f);
}

static void cap_imul64_rm(g_u64 a, g_u64 *p)
{
    g_u64 r, f;
    __asm__ volatile("imulq %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "m"(*p) : "cc");
    obs(r, f);
}

static void cap_imul64_1op(g_u64 a, g_u64 b)
{
    g_u64 lo, hi, f;
    __asm__ volatile("imulq %3\n\tpushfq\n\tpop %2"
                     : "=a"(lo), "=d"(hi), "=r"(f) : "r"(b), "0"(a) : "cc");
    obs(lo, f);
    obs(hi, 0);
}

static void cap_mul64_1op(g_u64 a, g_u64 b)
{
    g_u64 lo, hi, f;
    __asm__ volatile("mulq %3\n\tpushfq\n\tpop %2"
                     : "=a"(lo), "=d"(hi), "=r"(f) : "r"(b), "0"(a) : "cc");
    obs(lo, f);
    obs(hi, 0);
}

#define S64_MIN 0x8000000000000000ULL
#define S64_MAX 0x7fffffffffffffffULL

static const g_u64 vals64[] = {
    0, 1, 2, 3,
    0xffffffffffffffffULL,
    S64_MIN, S64_MAX,
    S64_MIN + 1,
    0x100000000ULL,
    0x80000000ULL,
    0x7fffffffULL,
    0x5555555555555555ULL,
    0xaaaaaaaaaaaaaaaaULL,
    0x0f0f0f0f0f0f0f0fULL,
    0x00000000000000ffULL,
    0x000000000000007fULL,
    0x0000000100000001ULL,
    0xdeadbeefcafebabeULL,
};
#define N64 (sizeof(vals64) / sizeof(vals64[0]))

static const g_u32 vals32[] = {
    0, 1, 2, 3,
    0xffffffffu,
    0x80000000u, 0x7fffffffu,
    0x80000001u,
    0x00010000u,
    0x00007fffu,
    0x00008000u,
    0x0000ffffu,
    0x55555555u,
    0xaaaaaaaau,
    0x000000ffu,
    0x0000007fu,
    0xcafebabeu,
};
#define N32 (sizeof(vals32) / sizeof(vals32[0]))

int main(void)
{
    g_u64 mem;

    for (unsigned i = 0; i < N64; i++) {
        for (unsigned j = 0; j < N64; j++) {
            g_u64 a = vals64[i], b = vals64[j];
            cap_imul64_rr(a, b);
            cap_imul64_1op(a, b);
            cap_mul64_1op(a, b);
            mem = b;
            cap_imul64_rm(a, &mem);
        }
        cap_imul64_rri8(vals64[i]);
        cap_imul64_rri32(vals64[i]);
        cap_imul64_square(vals64[i]);
    }

    for (unsigned i = 0; i < N32; i++) {
        for (unsigned j = 0; j < N32; j++) {
            cap_imul32_rr(vals32[i], vals32[j]);
            cap_imul16_rr((unsigned short)vals32[i], (unsigned short)vals32[j]);
        }
        cap_imul32_rri8(vals32[i]);
        cap_imul32_rri32(vals32[i]);
    }

    g_puts("imul_flags arch=");
    g_puthex64(sum_arch);
    g_puts("imul_flags pinned=");
    g_puthex64(sum_pinned);
    return 0;
}
