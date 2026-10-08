/*
 * Loops that address memory through one base register several times before
 * writing it, as src/jit.c's Wine-layout base hoist (select_low_hoist) takes
 * them: a struct array walked by a pointer, a 16-byte SSE scan, negative
 * displacements, the base written in the middle of the loop, flags carried
 * around the back edge, and an array that ends exactly at 12 GB.  Built at an
 * image base below 12 GB, so its data is in the low window.  Each kernel
 * prints a checksum, against Rosetta.
 */
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

struct part { float x, y, z, vx, vy, vz; int id; };
static struct part ps[1024];
static unsigned char text[8192];
static uint64_t words[4096];

__attribute__((noinline)) static unsigned long long structs(int rounds)
{
    unsigned long long acc = 0;
    for (int r = 0; r < rounds; r++)
        for (struct part *p = ps; p < ps + 1024; p++) {
            p->x += p->vx; p->y += p->vy; p->z += p->vz;
            if (p->x > 100.0f) { p->x = 0.0f; p->vx = -p->vx; }
            acc += (unsigned long long)(p->id ^ (int)p->x);
        }
    return acc;
}

/* A memchr-shaped scan: two 16-byte loads off one pointer, the pointer bumped last. */
__attribute__((noinline)) static long scan(const unsigned char *s, unsigned char c, long n)
{
    __m128i want = _mm_set1_epi8((char)c);
    long i = 0;
    for (; i + 32 <= n; i += 32, s += 32) {
        int m0 = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)s), want));
        int m1 = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(s + 16)), want));
        if (m0 | m1) return i + (m0 ? __builtin_ctz((unsigned)m0) : 16 + __builtin_ctz((unsigned)m1));
    }
    return -1;
}

/* Negative displacements off an end pointer, and the base rewritten mid-loop. */
__attribute__((noinline)) static uint64_t backward(const uint64_t *end, int n)
{
    uint64_t acc = 0;
    const uint64_t *p = end;
    for (int i = 0; i < n; i++) {
        uint64_t a, b2, c2;
        __asm__ volatile("mov -8(%3), %0\n\t"
                         "mov -16(%3), %1\n\t"
                         "lea -24(%3), %3\n\t"
                         "mov 0(%3), %2\n\t"
                         : "=&r"(a), "=&r"(b2), "=&r"(c2), "+r"(p) : : "memory");
        acc = acc * 31 + (a ^ b2) + c2;
        if (p <= words + 3) p = end;
    }
    return acc;
}

/* An adc chain: the carry crosses the loop's back edge in the flags. */
__attribute__((noinline)) static uint64_t carry(const uint64_t *w, int n)
{
    uint64_t lo = 0, hi = 0;
    const uint64_t *p = w;
    __asm__ volatile("xor %%ecx, %%ecx\n\t"
                     "clc\n"
                     "1:\n\t"
                     "mov 0(%2), %%rax\n\t"
                     "adc %%rax, %0\n\t"
                     "mov 8(%2), %%rax\n\t"
                     "adc %%rax, %1\n\t"
                     "lea 16(%2), %2\n\t"
                     "inc %%ecx\n\t"
                     "cmp %3, %%ecx\n\t"
                     "jne 1b\n\t"
                     : "+r"(lo), "+r"(hi), "+r"(p) : "r"(n) : "rax", "rcx", "cc", "memory");
    return lo ^ (hi << 1);
}

int main(void)
{
    for (int i = 0; i < 1024; i++) {
        ps[i].x = (float)i; ps[i].y = (float)-i; ps[i].z = 0.5f;
        ps[i].vx = 0.25f; ps[i].vy = -0.5f; ps[i].vz = 0.125f; ps[i].id = i;
    }
    for (int i = 0; i < (int)sizeof text; i++) text[i] = (unsigned char)('a' + i % 23);
    for (int i = 0; i < 4096; i++) words[i] = 0x9e3779b97f4a7c15ull * (uint64_t)(i + 1);
    printf("structs  %llu\n", structs(300));
    long found = 0;
    for (int k = 0; k < 2000; k++) {
        text[100 + (k * 37) % 8000] = 'Z';
        found += scan(text, 'Z', sizeof text);
        text[100 + (k * 37) % 8000] = 'a';
    }
    printf("scan     %ld\n", found);
    printf("backward %llu\n", (unsigned long long)backward(words + 4096, 50000));
    printf("carry    %llu\n", (unsigned long long)carry(words, 2000));

    /*
     * An array that ends exactly at 12 GB, walked by a pointer: the last element's
     * span reaches the edge, so the hoisted block fails its check there, leaves,
     * and is retranslated with a test per access.
     */
    struct part *edge = mmap((void *)(0x300000000ull - 0x4000), 0x4000, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (edge != MAP_FAILED) {
        int ne = (int)(0x4000 / sizeof *edge);
        struct part *first = (struct part *)((char *)edge + 0x4000) - ne;
        for (int i = 0; i < ne; i++) {
            first[i].x = (float)i; first[i].y = 1.0f; first[i].z = 2.0f;
            first[i].vx = 0.5f; first[i].vy = 0.25f; first[i].vz = 0.125f; first[i].id = i * 3;
        }
        unsigned long long acc = 0;
        for (int r = 0; r < 50; r++)
            for (struct part *p = first; p < first + ne; p++) {
                p->x += p->vx; p->y += p->vy; p->z += p->vz;
                acc += (unsigned long long)(p->id ^ (int)p->x);
            }
        printf("edge     %llu\n", acc);
    } else {
        printf("edge     (no mapping)\n");
    }
    return 0;
}
