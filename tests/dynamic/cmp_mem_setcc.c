/*
 * cmp/test with a memory first operand followed by two setcc or cmovcc, as
 * src/jit.c forwards their NZCV (nzcv_fuse_producer, emit_rmw_mem): every
 * condition first and second, a sibling consumer that writes the compare's
 * register or its memory base before the second reads the flags, 32- and
 * 64-bit, register and immediate sources, and the comparator qsort calls.
 * Prints each result, against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CCS(X) X(o, no) X(no, b) X(b, ae) X(ae, e) X(e, ne) X(ne, be) X(be, a) X(a, s) \
               X(s, ns) X(ns, p) X(p, np) X(np, l) X(l, ge) X(ge, le) X(le, g) X(g, o)

/* cmp dword [m], eax ; set<c1> al ; set<c2> dl: the first consumer overwrites the compare's eax. */
#define PAIR32(c1, c2)                                                                   \
    static unsigned p32_##c1(int32_t a, volatile int32_t *m)                             \
    {                                                                                    \
        uint32_t ax = (uint32_t)a, dx = 0x5a5a5a5a;                                      \
        __asm__ volatile("cmpl %%eax, (%2)\n\t"                                          \
                         "set" #c1 " %%al\n\t"                                           \
                         "set" #c2 " %%dl"                                               \
                         : "+a"(ax), "+d"(dx) : "r"(m) : "cc");                          \
        return (ax & 0xffff) << 16 | (dx & 0xffff);                                      \
    }
CCS(PAIR32)

/* cmp qword [rbx], imm ; cmov<c1> rbx, rsi ; set<c2> al: the cmov overwrites the memory base. */
#define BASE64(c1, c2)                                                                   \
    static uint64_t b64_##c1(volatile int64_t *m, uint64_t alt)                          \
    {                                                                                    \
        uint64_t bx = (uint64_t)(uintptr_t)m, ax = 0;                                    \
        __asm__ volatile("cmpq $-7, (%%rbx)\n\t"                                         \
                         "cmov" #c1 " %%rsi, %%rbx\n\t"                                  \
                         "set" #c2 " %%al"                                               \
                         : "+b"(bx), "+a"(ax) : "S"(alt) : "cc");                        \
        return (bx == alt) << 1 | ax;                                                    \
    }
CCS(BASE64)

/* test dword [m], ecx ; set<c1> cl ; cmov<c2> edx, esi. */
#define TEST32(c1, c2)                                                                   \
    static unsigned t32_##c1(uint32_t a, volatile uint32_t *m)                           \
    {                                                                                    \
        uint32_t cx = a, dx = 0x11, si = 0x22;                                           \
        __asm__ volatile("testl %%ecx, (%3)\n\t"                                         \
                         "set" #c1 " %%cl\n\t"                                           \
                         "cmov" #c2 " %%esi, %%edx"                                      \
                         : "+c"(cx), "+d"(dx) : "S"(si), "r"(m) : "cc");                 \
        return (cx & 0xff) << 8 | dx;                                                    \
    }
CCS(TEST32)

/* test qword [m], imm ; set<c1> al ; set<c2> cl. */
#define TEST64(c1, c2)                                                                   \
    static unsigned t64_##c1(volatile uint64_t *m)                                       \
    {                                                                                    \
        uint64_t ax = 0, cx = 0;                                                         \
        __asm__ volatile("testq $-0x7fffffff, (%2)\n\t"                                   \
                         "set" #c1 " %%al\n\t"                                           \
                         "set" #c2 " %%cl"                                               \
                         : "+a"(ax), "+c"(cx) : "r"(m) : "cc");                          \
        return (unsigned)(ax << 1 | cx);                                                 \
    }
CCS(TEST64)

typedef unsigned (*p32_fn)(int32_t, volatile int32_t *);
typedef uint64_t (*b64_fn)(volatile int64_t *, uint64_t);
typedef unsigned (*t32_fn)(uint32_t, volatile uint32_t *);
typedef unsigned (*t64_fn)(volatile uint64_t *);
#define ADDR(c1, c2) p32_##c1,
static const p32_fn p32s[] = { CCS(ADDR) };
#undef ADDR
#define ADDR(c1, c2) b64_##c1,
static const b64_fn b64s[] = { CCS(ADDR) };
#undef ADDR
#define ADDR(c1, c2) t32_##c1,
static const t32_fn t32s[] = { CCS(ADDR) };
#undef ADDR
#define ADDR(c1, c2) t64_##c1,
static const t64_fn t64s[] = { CCS(ADDR) };

/* winbench's comparator: mov eax,[rdx] ; cmp [rcx],eax ; setg al ; setl dl ; movzx ; movzx ; sub. */
__attribute__((noinline)) static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    static const int32_t v32[] = { 0, 1, -1, 7, -7, INT32_MIN, INT32_MAX, 0x40000000 };
    static const int64_t v64[] = { 0, -7, -8, -6, 7, INT64_MIN, INT64_MAX, 0x80000001 };
    static const uint32_t u32[] = { 0, 1, 0x80000000, 0xffffffff, 0x80000001, 0x7ffffffe };
    unsigned long long acc = 0;
    for (int f = 0; f < 16; f++) {
        for (unsigned i = 0; i < 8; i++) {
            for (unsigned j = 0; j < 8; j++) {
                volatile int32_t m = v32[j];
                unsigned r = p32s[f](v32[i], &m);
                acc = acc * 31 + r;
            }
            volatile int64_t m64 = v64[i];
            uint64_t r = b64s[f](&m64, 0x1234);
            printf("b64 %d %u %llx\n", f, i, (unsigned long long)r);
            volatile uint64_t q = (uint64_t)v64[i];
            printf("t64 %d %u %x\n", f, i, t64s[f](&q));
        }
        for (unsigned i = 0; i < 6; i++)
            for (unsigned j = 0; j < 6; j++) {
                volatile uint32_t m = u32[j];
                acc = acc * 31 + t32s[f](u32[i], &m);
            }
        printf("cc %d acc %llx\n", f, acc);
    }
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            volatile int32_t m = v32[j];
            printf("p32.%d.%d %x %x\n", i, j, p32s[14](v32[i], &m), p32s[12](v32[i], &m));
        }

    static int arr[20000];
    uint32_t s = 12345;
    for (int i = 0; i < 20000; i++) { s = s * 1103515245u + 12345u; arr[i] = (int)s; }
    for (int i = 0; i < 20000; i += 97) arr[i] = INT32_MIN + (i & 3);
    qsort(arr, 20000, sizeof arr[0], cmp_int);
    unsigned long long h = 0;
    int sorted = 1;
    for (int i = 0; i < 20000; i++) { h = h * 1000003 + (unsigned)arr[i]; if (i && arr[i - 1] > arr[i]) sorted = 0; }
    printf("qsort sorted=%d hash %llx\n", sorted, h);
    return 0;
}
