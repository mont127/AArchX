/*
 * Blocks whose memory operands sit on a few registers, with or without one
 * index register: the shapes src/jit.c's hoist takes in the Wine layout
 * (select_low_hoist).  It checks itself against plain C and prints OK or the
 * first difference:
 *   - three arrays under one index, every access size, with displacements;
 *   - an index counting up from a negative value to zero, off the arrays' ends;
 *   - bases on both sides of 12 GB in one loop, and one so close below it
 *     that the hoist has to give up (both in the Wine layout build only);
 *   - an index that strides 3 GB, past what the hoist allows, mid-loop;
 *   - an index that hops, in one step, from a mapping below 12 GB to one far
 *     above it and back: the address is valid, the hoisted base is not;
 *   - a loop run on a mapping below 12 GB, then on one far above;
 *   - records walked by an offset stepped inside the loop, the array's address
 *     in the index register;
 *   - a list walked by mov (%rdi), %rdi, its nodes on both sides of 12 GB;
 *   - a table followed by index, i = tab[i], where some entries are indices
 *     that reach from the table into a mapping on the other side;
 *   - straight-line code that steps its index, reloads it, and reloads its
 *     base, between operands;
 *   - a cpuid inside the loop, after which the hoisted bases are formed again;
 *   - a store into a protected page: the handler sees the guest address,
 *     opens the page, and the loop goes on.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define N 1024
static uint64_t ga[N + 2], gc[N + 2];
static int g_low;

static int fail(const char *what, uint64_t got, uint64_t want)
{
    printf("%s: %#llx, want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    return 1;
}

static void *map_at(uint64_t want, size_t len)
{
    void *p = mmap(g_low ? (void *)(uintptr_t)want : NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | (g_low ? MAP_FIXED : 0), -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* c[i] = a[i] + b[i] + a[i+1], and a running sum of every size of load */
__attribute__((noinline)) static uint64_t three(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t n)
{
    uint64_t acc = 0, i = 0, t;
    __asm__ volatile(
        "1:\n\t"
        "mov (%[a],%[i],8), %[t]\n\t"
        "add (%[b],%[i],8), %[t]\n\t"
        "add 8(%[a],%[i],8), %[t]\n\t"
        "mov %[t], (%[c],%[i],8)\n\t"
        "movzbl 3(%[b],%[i],8), %%eax\n\t"   "add %%rax, %[acc]\n\t"
        "movswq 6(%[a],%[i],8), %%rax\n\t"   "add %%rax, %[acc]\n\t"
        "mov 4(%[c],%[i],8), %%eax\n\t"      "xor %%rax, %[acc]\n\t"
        "cmp (%[b],%[i],8), %[t]\n\t"        "adc $0, %[acc]\n\t"
        "addq $5, (%[c],%[i],8)\n\t"
        "movups (%[a],%[i],8), %%xmm0\n\t"
        "movups (%[b],%[i],8), %%xmm1\n\t"
        "paddq %%xmm1, %%xmm0\n\t"
        "movq %%xmm0, %%rax\n\t"             "add %%rax, %[acc]\n\t"
        "inc %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jne 1b"
        : [acc] "+&r"(acc), [i] "+&r"(i), [t] "=&r"(t)
        : [a] "r"(a), [b] "r"(b), [c] "r"(c), [n] "r"(n)
        : "rax", "xmm0", "xmm1", "memory", "cc");
    return acc;
}
static uint64_t three_c(const uint64_t *a, const uint64_t *b, uint64_t *c, uint64_t n)
{
    uint64_t acc = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t t = a[i] + b[i] + a[i + 1];
        c[i] = t;
        acc += (uint8_t)(b[i] >> 24);
        acc += (uint64_t)(int64_t)(int16_t)(a[i] >> 48);
        acc ^= (uint32_t)(c[i] >> 32);
        acc += t < b[i];
        c[i] += 5;
        /* a[i], a[i+1] plus b[i], b[i+1] as two quadwords; the low one */
        acc += a[i] + b[i];
    }
    return acc;
}

/* the index runs from -n up to zero, off pointers to the arrays' ends */
__attribute__((noinline)) static uint64_t negative(uint64_t *aend, uint64_t *bend, int64_t n)
{
    uint64_t acc = 0;
    int64_t i = -n;
    __asm__ volatile(
        "1:\n\t"
        "mov (%[a],%[i],8), %%rax\n\t"
        "imul $3, %[acc], %[acc]\n\t"
        "add %%rax, %[acc]\n\t"
        "xor %%rax, (%[b],%[i],8)\n\t"
        "add (%[b],%[i],8), %[acc]\n\t"
        "inc %[i]\n\t"
        "jnz 1b"
        : [acc] "+&r"(acc), [i] "+&r"(i)
        : [a] "r"(aend), [b] "r"(bend)
        : "rax", "memory", "cc");
    return acc;
}
static uint64_t negative_c(const uint64_t *aend, uint64_t *bend, int64_t n)
{
    uint64_t acc = 0;
    for (int64_t i = -n; i; i++) {
        acc = acc * 3 + aend[i];
        bend[i] ^= aend[i];
        acc += bend[i];
    }
    return acc;
}

/*
 * Bytes at a stride, two bases: the index goes wherever from, by, and below
 * say.  One copy of the loop per use, since a loop that made the hoist give
 * up is translated without it from then on.
 */
#define STRIDE(name) \
__attribute__((noinline)) static uint64_t name(uint8_t *p, uint8_t *q, uint64_t from, uint64_t by, uint64_t below) \
{ \
    uint64_t acc = 0, i = from; \
    __asm__ volatile( \
        "1:\n\t" \
        "movzbl (%[p],%[i],1), %%eax\n\t" \
        "add %%rax, %[acc]\n\t" \
        "movzbl 1(%[p],%[i],1), %%eax\n\t" \
        "rol $7, %[acc]\n\t" \
        "add %%rax, %[acc]\n\t" \
        "movb %b[acc], (%[q],%[i],1)\n\t" \
        "movzbl 1(%[q],%[i],1), %%eax\n\t" \
        "xor %%rax, %[acc]\n\t" \
        "add %[by], %[i]\n\t" \
        "cmp %[below], %[i]\n\t" \
        "jb 1b" \
        : [acc] "+&r"(acc), [i] "+&r"(i) \
        : [p] "r"(p), [q] "r"(q), [by] "r"(by), [below] "r"(below) \
        : "rax", "memory", "cc"); \
    return acc; \
}
STRIDE(stride_far)
STRIDE(stride_across)
STRIDE(stride_sides)
STRIDE(stride_sides2)
static uint64_t stride_c(const uint8_t *p, uint8_t *q, uint64_t from, uint64_t by, uint64_t below)
{
    uint64_t acc = 0, i = from;
    do {
        acc += p[i];
        acc = (acc << 7) | (acc >> 57);
        acc += p[i + 1];
        q[i] = (uint8_t)acc;
        acc ^= q[i + 1];
        i += by;
    } while (i < below);
    return acc;
}

/* The index moves by a register each time round, n times: it can hop anywhere. */
#define HOP(name) \
__attribute__((noinline)) static uint64_t name(uint8_t *p, uint8_t *q, uint64_t from, uint64_t by, uint64_t n) \
{ \
    uint64_t acc = 0, i = from; \
    __asm__ volatile( \
        "1:\n\t" \
        "movzbl (%[p],%[i],1), %%eax\n\t" \
        "add %%rax, %[acc]\n\t" \
        "movzbl 1(%[p],%[i],1), %%eax\n\t" \
        "rol $7, %[acc]\n\t" \
        "add %%rax, %[acc]\n\t" \
        "movb %b[acc], (%[q],%[i],1)\n\t" \
        "add %[by], %[i]\n\t" \
        "dec %[n]\n\t" \
        "jnz 1b" \
        : [acc] "+&r"(acc), [i] "+&r"(i), [n] "+&r"(n) \
        : [p] "r"(p), [q] "r"(q), [by] "r"(by) \
        : "rax", "memory", "cc"); \
    return acc; \
}
HOP(hop_up)
HOP(hop_down)
HOP(hop_moved)
static uint64_t hop_c(const uint8_t *p, uint8_t *q, uint64_t from, uint64_t by, uint64_t n)
{
    uint64_t acc = 0, i = from;
    do {
        acc += p[i];
        acc = (acc << 7) | (acc >> 57);
        acc += p[i + 1];
        q[i] = (uint8_t)acc;
        i += by;
    } while (--n);
    return acc;
}

/*
 * 40-byte records, the array's address in the index register and a byte
 * offset as the base: the offset is stepped before the operands (walk_top,
 * entered at -40) or between them (walk_mid).
 */
struct rec40 { uint64_t a, b, c; double d; uint64_t e; };
__attribute__((noinline)) static uint64_t walk_top(struct rec40 *tab, uint64_t bytes)
{
    uint64_t acc = 0, off = (uint64_t)-40, t;
    __asm__ volatile(
        "1:\n\t"
        "add $40, %[off]\n\t"
        "mov (%[off],%[tab],1), %[t]\n\t"
        "add 8(%[off],%[tab],1), %[t]\n\t"
        "mov %[t], 16(%[off],%[tab],1)\n\t"
        "movsd 24(%[off],%[tab],1), %%xmm0\n\t"
        "addsd %%xmm0, %%xmm0\n\t"
        "movsd %%xmm0, 24(%[off],%[tab],1)\n\t"
        "xor 32(%[off],%[tab],1), %[t]\n\t"
        "rol $3, %[acc]\n\t"
        "add %[t], %[acc]\n\t"
        "cmp %[last], %[off]\n\t"
        "jne 1b"
        : [acc] "+&r"(acc), [off] "+&r"(off), [t] "=&r"(t)
        : [tab] "r"(tab), [last] "r"(bytes - 40)
        : "xmm0", "memory", "cc");
    return acc;
}
__attribute__((noinline)) static uint64_t walk_mid(struct rec40 *tab, uint64_t bytes)
{
    uint64_t acc = 0, off = 0, t;
    __asm__ volatile(
        "1:\n\t"
        "mov (%[off],%[tab],1), %[t]\n\t"
        "add 8(%[off],%[tab],1), %[t]\n\t"
        "add $40, %[off]\n\t"
        "mov %[t], -24(%[off],%[tab],1)\n\t"
        "movsd -16(%[off],%[tab],1), %%xmm0\n\t"
        "addsd %%xmm0, %%xmm0\n\t"
        "movsd %%xmm0, -16(%[off],%[tab],1)\n\t"
        "xor -8(%[off],%[tab],1), %[t]\n\t"
        "rol $3, %[acc]\n\t"
        "add %[t], %[acc]\n\t"
        "cmp %[bytes], %[off]\n\t"
        "jne 1b"
        : [acc] "+&r"(acc), [off] "+&r"(off), [t] "=&r"(t)
        : [tab] "r"(tab), [bytes] "r"(bytes)
        : "xmm0", "memory", "cc");
    return acc;
}
static uint64_t walk_c(struct rec40 *tab, uint64_t bytes)
{
    uint64_t acc = 0;
    for (uint64_t k = 0; k < bytes / 40; k++) {
        uint64_t t = tab[k].a + tab[k].b;
        tab[k].c = t;
        tab[k].d += tab[k].d;
        t ^= tab[k].e;
        acc = ((acc << 3) | (acc >> 61)) + t;
    }
    return acc;
}

/* A list walked through its first word, which the load at the loop's end writes into its own base. */
struct node { struct node *next; uint64_t v, w; };
__attribute__((noinline)) static uint64_t chase(struct node *p)
{
    uint64_t acc = 0;
    __asm__ volatile(
        "1:\n\t"
        "add 8(%[p]), %[acc]\n\t"
        "rol $5, %[acc]\n\t"
        "xor 16(%[p]), %[acc]\n\t"
        "addq $1, 16(%[p])\n\t"
        "mov (%[p]), %[p]\n\t"
        "test %[p], %[p]\n\t"
        "jne 1b"
        : [acc] "+&r"(acc), [p] "+&r"(p) : : "memory", "cc");
    return acc;
}
static uint64_t chase_c(struct node *p)
{
    uint64_t acc = 0;
    for (; p; p = p->next) {
        acc += p->v;
        acc = (acc << 5) | (acc >> 59);
        acc ^= p->w;
        p->w++;
    }
    return acc;
}

/* i = tab[i], twice a round: every operand's index is whatever the last load brought. */
__attribute__((noinline)) static uint64_t follow(uint64_t *tab, uint64_t i, uint64_t n)
{
    uint64_t acc = 0;
    __asm__ volatile(
        "1:\n\t"
        "mov (%[tab],%[i],8), %[i]\n\t"
        "add %[i], %[acc]\n\t"
        "rol $9, %[acc]\n\t"
        "mov 8(%[tab],%[i],8), %[i]\n\t"
        "xor %[i], %[acc]\n\t"
        "dec %[n]\n\t"
        "jnz 1b"
        : [acc] "+&r"(acc), [i] "+&r"(i), [n] "+&r"(n) : [tab] "r"(tab) : "memory", "cc");
    return acc;
}
static uint64_t follow_c(const uint64_t *tab, uint64_t i, uint64_t n)
{
    uint64_t acc = 0;
    do {
        i = *(const uint64_t *)((uintptr_t)tab + i * 8);
        acc += i;
        acc = (acc << 9) | (acc >> 55);
        i = *(const uint64_t *)((uintptr_t)tab + i * 8 + 8);
        acc ^= i;
    } while (--n);
    return acc;
}

/*
 * Straight-line: operands on p and q under i; i stepped twice between them;
 * then i reloaded from memory and p from memory, with operands after each
 * that must go by the new values.  The new index reaches from p into another
 * area, across 12 GB in the Wine layout build, and so does the new base.
 */
__attribute__((noinline)) static uint64_t line(uint64_t *p, uint64_t *q, uint64_t i)
{
    uint64_t acc;
    __asm__ volatile(
        "mov (%[p],%[i],8), %[acc]\n\t"
        "add 8(%[p],%[i],8), %[acc]\n\t"
        "xor (%[q],%[i],8), %[acc]\n\t"
        "add $2, %[i]\n\t"
        "add (%[p],%[i],8), %[acc]\n\t"
        "mov %[acc], -8(%[q],%[i],8)\n\t"
        "sub $1, %[i]\n\t"
        "add 16(%[q],%[i],8), %[acc]\n\t"
        "xor 24(%[p],%[i],8), %[acc]\n\t"
        "mov 32(%[p],%[i],8), %[i]\n\t"
        "add (%[p],%[i],8), %[acc]\n\t"
        "add 8(%[p],%[i],8), %[acc]\n\t"
        "mov 16(%[p],%[i],8), %[p]\n\t"
        "add (%[p]), %[acc]\n\t"
        "add 8(%[p]), %[acc]\n\t"
        "xor 24(%[p]), %[acc]"
        : [acc] "=&r"(acc), [p] "+&r"(p), [i] "+&r"(i)
        : [q] "r"(q)
        : "memory", "cc");
    return acc;
}
/* Called through a pointer, so the translator starts a block at its first instruction instead of running on into it. */
static uint64_t (*volatile line_ptr)(uint64_t *, uint64_t *, uint64_t) = line;
static uint64_t line_c(uint64_t *p, uint64_t *q, uint64_t i)
{
    uint64_t acc = p[i] + p[i + 1];
    acc ^= q[i];
    acc += p[i + 2];
    q[i + 1] = acc;
    acc += q[i + 3];
    acc ^= p[i + 4];
    i = p[i + 5];
    acc += p[i] + p[i + 1];
    p = (uint64_t *)(uintptr_t)p[i + 2];
    acc += p[0] + p[1];
    acc ^= p[3];
    return acc;
}

/* cpuid in the loop: a call out of translated code, after which the bases are formed again */
__attribute__((noinline)) static uint64_t callout(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t n)
{
    uint64_t acc = 0, i = 0, t;
    __asm__ volatile(
        "1:\n\t"
        "mov (%[a],%[i],8), %[t]\n\t"
        "add (%[b],%[i],8), %[t]\n\t"
        "mov %[t], (%[c],%[i],8)\n\t"
        "xor %%eax, %%eax\n\t"
        "cpuid\n\t"
        "mov 8(%[a],%[i],8), %[t]\n\t"
        "add %[t], (%[c],%[i],8)\n\t"
        "add (%[c],%[i],8), %[acc]\n\t"
        "inc %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jne 1b"
        : [acc] "+&r"(acc), [i] "+&r"(i), [t] "=&r"(t)
        : [a] "r"(a), [b] "r"(b), [c] "r"(c), [n] "r"(n)
        : "rax", "rbx", "rcx", "rdx", "memory", "cc");
    return acc;
}

static uint8_t *g_prot;
static volatile uintptr_t g_fault_addr;
static volatile int g_faults;
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)ctx;
    g_fault_addr = (uintptr_t)si->si_addr;
    g_faults++;
    if (g_faults > 4 || mprotect(g_prot, 0x4000, PROT_READ | PROT_WRITE) != 0) _exit(3);
}

__attribute__((noinline)) static uint64_t fill(uint64_t *a, uint64_t *b, uint64_t n)
{
    uint64_t acc = 0, i = 0;
    __asm__ volatile(
        "1:\n\t"
        "mov (%[a],%[i],8), %%rax\n\t"
        "lea 1(%%rax,%[i],2), %%rax\n\t"
        "mov %%rax, (%[b],%[i],8)\n\t"
        "add (%[b],%[i],8), %[acc]\n\t"
        "inc %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jne 1b"
        : [acc] "+&r"(acc), [i] "+&r"(i)
        : [a] "r"(a), [b] "r"(b), [n] "r"(n)
        : "rax", "memory", "cc");
    return acc;
}

static void seed(uint64_t *p, size_t n, uint64_t s)
{
    for (size_t i = 0; i < n; i++) { s = s * 6364136223846793005ull + 1442695040888963407ull; p[i] = s ^ (s >> 29); }
}

int main(void)
{
    g_low = (uintptr_t)ga < 0x300000000ull;
    static uint64_t ref[N + 2];
    uint64_t stack_arr[N + 2];

    /* globals, the stack, and mappings: in the Wine layout build one sits at 4 GB, below 12 GB, and one far above */
    uint64_t *lo4 = map_at(0x100000000ull, sizeof ga), *high = map_at(0x200000000000ull, sizeof ga);
    if (!lo4 || !high) { printf("map\n"); return 1; }
    uint64_t *srcs[4] = { ga, stack_arr, lo4, high };
    for (int rep = 0; rep < 40; rep++)
        for (int x = 0; x < 4; x++)
            for (int y = 0; y < 4; y++) {
                uint64_t *a = srcs[x], *b = srcs[y], *c = srcs[(x + y + rep) & 3];
                if (c == a || c == b) c = gc;
                seed(a, N + 2, 1 + (uint64_t)x + (uint64_t)rep * 16);
                if (b != a) seed(b, N + 2, 77 + (uint64_t)y);
                memset(ref, 0, sizeof ref);
                uint64_t want = three_c(a, b, ref, N);
                memset(c, 0, sizeof ga);
                uint64_t got = three(a, b, c, N);
                if (got != want) return fail("three", got, want);
                if (memcmp(c, ref, N * 8)) return fail("three stores", (uint64_t)x, (uint64_t)y);

                seed(a, N + 2, 5 + (uint64_t)x);
                seed(gc, N + 2, 9 + (uint64_t)y);
                memcpy(ref, gc, sizeof ref);
                want = negative_c(a + N, ref + N, N);
                if (c != gc) memcpy(c, gc, sizeof ga);
                got = negative(a + N, c + N, N);
                if (got != want) return fail("negative", got, want);
                if (memcmp(c, ref, N * 8)) return fail("negative stores", (uint64_t)x, (uint64_t)y);
            }

    /* the index strides 3 GB of a sparse mapping: past 2 GB the hoist's check fails mid-loop */
    size_t big = 0xC0100000ull;
    uint8_t *sp = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    uint8_t *sq = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (sp == MAP_FAILED || sq == MAP_FAILED) { printf("map big\n"); return 1; }
    for (uint64_t i = 0; i < 0xC0000000ull; i += 0x8000000ull) { sp[i] = (uint8_t)(i >> 27) + 3; sp[i + 1] = (uint8_t)(i >> 26) ^ 0x5a; sq[i + 1] = (uint8_t)(i >> 25) + 1; }
    for (int rep = 0; rep < 3; rep++) {
        uint64_t got = stride_far(sp, sq, 0, 0x8000000ull, 0xC0000000ull);
        for (uint64_t i = 0; i < 0xC0000000ull; i += 0x8000000ull) sq[i] = 0;
        uint64_t want = stride_c(sp, sq, 0, 0x8000000ull, 0xC0000000ull);
        if (got != want) return fail("stride", got, want);
    }

    /*
     * Bytes just below 12 GB (Wine layout build): the index could reach past
     * it, so the hoist gives up before the loop.  Then loads at 4 GB with
     * stores far above, and the other way round.
     */
    uint8_t *edge = map_at(0x2FFFF0000ull, 0x10000), *low = (uint8_t *)(void *)lo4, *far = (uint8_t *)(void *)high;
    if (!edge) { printf("map edge\n"); return 1; }
    static uint8_t side[0x10000], side2[0x10000];
    for (int rep = 0; rep < 3; rep++) {
        for (size_t i = 0; i < 0x10000; i++) edge[i] = (uint8_t)(i * 7 + (size_t)rep);
        memset(side, 0, sizeof side); memset(side2, 0, sizeof side2);
        uint64_t got = stride_across(edge, side, 0, 3, 0xfff0), want = stride_c(edge, side2, 0, 3, 0xfff0);
        if (got != want) return fail("below 12 GB", got, want);
        if (memcmp(side, side2, sizeof side)) return fail("below 12 GB stores", 0, 1);

        for (size_t i = 0; i < sizeof ga; i++) low[i] = (uint8_t)(i * 11 + (size_t)rep);
        memset(side2, 0, sizeof side2);
        want = stride_c(low, side2, 0, 5, sizeof ga - 16);
        memset(far, 0, sizeof ga);
        got = stride_sides(low, far, 0, 5, sizeof ga - 16);
        if (got != want) return fail("both sides", got, want);
        if (memcmp(far, side2, sizeof ga)) return fail("both sides stores", 0, 1);
        memset(side2, 0, sizeof side2);
        want = stride_c(far, side2, 16, 5, sizeof ga - 16);
        memset(low, 0, sizeof ga);
        got = stride_sides2(far, low, 16, 5, sizeof ga - 16);
        if (got != want) return fail("both sides, swapped", got, want);
        if (memcmp(low, side2, sizeof ga)) return fail("both sides, swapped, stores", 0, 1);
    }

    /* one step from the mapping at 4 GB to the one far above, and one back: bytes 0, 1 and 0x1000 of each */
    for (int rep = 0; rep < 3; rep++) {
        static uint8_t flat[0x2000];
        for (size_t i = 0; i < sizeof ga; i++) { low[i] = (uint8_t)(i * 5 + (size_t)rep); far[i] = (uint8_t)(i * 9 + 7 + (size_t)rep); }
        uint64_t warm = hop_up(low, low + 0x1000, 0, 1, 0xff0);            /* translated on low addresses first */
        memcpy(flat, low, 0x1000);
        if (warm != hop_c(flat, flat + 0x1000, 0, 1, 0xff0)) return fail("hop warm", warm, 0);
        uint64_t got = hop_up(low, low + 0x1000, 0, (uint64_t)(far - low), 2);
        uint64_t want = (uint64_t)low[0];
        want = ((want << 7) | (want >> 57)) + low[1];
        uint8_t st0 = (uint8_t)want;
        want += far[0]; want = ((want << 7) | (want >> 57)) + far[1];
        if (got != want) return fail("hop up", got, want);
        if (low[0x1000] != st0 || far[0x1000] != (uint8_t)want) return fail("hop up stores", far[0x1000], (uint8_t)want);

        got = hop_down(far, far + 0x1000, 0, (uint64_t)(low - far), 2);
        want = (uint64_t)far[0];
        want = ((want << 7) | (want >> 57)) + far[1];
        st0 = (uint8_t)want;
        want += low[0]; want = ((want << 7) | (want >> 57)) + low[1];
        if (got != want) return fail("hop down", got, want);
        if (far[0x1000] != st0 || low[0x1000] != (uint8_t)want) return fail("hop down stores", low[0x1000], (uint8_t)want);

        /* the same loop on the low mapping, then with both bases moved far above */
        static uint8_t flat2[0x2000];
        memcpy(flat, low, 0x2000); memcpy(flat2, far, 0x2000);
        got = hop_moved(low, low + 0x1000, 0, 1, 0xff0);
        want = hop_c(flat, flat + 0x1000, 0, 1, 0xff0);
        if (got != want || memcmp(low, flat, 0x2000)) return fail("hop, low", got, want);
        got = hop_moved(far, far + 0x1000, 0, 1, 0xff0);
        want = hop_c(flat2, flat2 + 0x1000, 0, 1, 0xff0);
        if (got != want || memcmp(far, flat2, 0x2000)) return fail("hop, moved above", got, want);
    }

    /* records, in the image and far above */
    static struct rec40 recs[200], recs_ref[200];
    struct rec40 *recs_far = (struct rec40 *)(void *)high;
    for (int rep = 0; rep < 6; rep++) {
        struct rec40 *tab = rep & 1 ? recs_far : recs;
        for (int k = 0; k < 200; k++) {
            recs_ref[k].a = (uint64_t)k * 0x9e3779b97f4a7c15ull + (uint64_t)rep; recs_ref[k].b = ~recs_ref[k].a >> 7;
            recs_ref[k].c = 0; recs_ref[k].d = k * 0.5 + rep; recs_ref[k].e = recs_ref[k].a ^ 0x5555;
        }
        memcpy(tab, recs_ref, sizeof recs);
        uint64_t want = walk_c(recs_ref, sizeof recs);
        uint64_t got = rep < 3 ? walk_top(tab, sizeof recs) : walk_mid(tab, sizeof recs);
        if (got != want) return fail(rep < 3 ? "walk, stepped first" : "walk, stepped between", got, want);
        if (memcmp(tab, recs_ref, sizeof recs)) return fail("walk stores", (uint64_t)rep, 0);
    }

    /* lists: all below 12 GB in the Wine layout build, then alternating with nodes far above */
    static struct node nodes[64], nodes_ref[64];
    struct node *nodes_far = (struct node *)(void *)high;
    for (int rep = 0; rep < 6; rep++) {
        struct node *at[64];
        for (int k = 0; k < 64; k++) at[k] = rep >= 3 && (k & 1) ? &nodes_far[k] : &nodes[k];
        for (int k = 0; k < 64; k++) {
            at[k]->next = k < 63 ? at[k + 1] : NULL; at[k]->v = (uint64_t)k * 77 + (uint64_t)rep; at[k]->w = ~(uint64_t)k << 9;
            nodes_ref[k].next = k < 63 ? &nodes_ref[k + 1] : NULL; nodes_ref[k].v = at[k]->v; nodes_ref[k].w = at[k]->w;
        }
        for (int again = 0; again < 3; again++) {
            uint64_t want = chase_c(nodes_ref), got = chase(at[0]);
            if (got != want) return fail("chase", got, want);
        }
        for (int k = 0; k < 64; k++) if (at[k]->w != nodes_ref[k].w) return fail("chase stores", at[k]->w, nodes_ref[k].w);
    }

    /*
     * Tables of indices: 64 cells in one area and 64 in another, each cell
     * naming a cell of either, as an index from the first.  Every pairing of
     * the four areas, so the reach crosses 12 GB both ways in the Wine layout
     * build.
     */
    for (int rep = 0; rep < 64; rep++) {
        uint64_t *ta = srcs[rep & 3], *tb = srcs[(rep >> 2) & 3];
        if (tb == ta) tb = gc;
        uint64_t s = 12345 + (uint64_t)rep, over = (uint64_t)(tb - ta);
        for (int k = 0; k < 66; k++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            ta[k] = ((s >> 33) % 64) + ((s >> 20 & 3) == 0 ? over : 0);
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            tb[k] = ((s >> 33) % 64) + ((s >> 20 & 3) == 0 ? 0 : over);
        }
        uint64_t want = follow_c(ta, (uint64_t)(rep % 64), 5000), got = follow(ta, (uint64_t)(rep % 64), 5000);
        if (got != want) return fail("follow", got, want);
    }

    /* straight-line: p, q, the area the reloaded index reaches into and the reloaded base, each from the four areas */
    for (int rep = 0; rep < 1024; rep++) {
        uint64_t *pp = srcs[rep & 3], *qq = srcs[(rep >> 2) & 3], *tp = srcs[(rep >> 4) & 3], *np = srcs[(rep >> 6) & 3];
        if (qq == pp) qq = gc;
        for (int x = 0; x < 4; x++) seed(srcs[x], 64, 900 + (uint64_t)rep * 4 + (uint64_t)x);
        seed(gc, 64, 77 + (uint64_t)rep);
        uint64_t i0 = (uint64_t)(rep % 7), i1 = (uint64_t)(tp - pp) + 20 + (uint64_t)(rep % 9);
        pp[i0 + 5] = i1;                                          /* the index the block reloads: pp[i1] is tp[20..] */
        pp[i1 + 2] = (uint64_t)(uintptr_t)np;                     /* the base it reloads */
        uint64_t before = qq[i0 + 1];
        uint64_t got = line_ptr(pp, qq, i0);
        uint64_t after = qq[i0 + 1];
        qq[i0 + 1] = before;
        uint64_t want = line_c(pp, qq, i0);
        if (got != want) return fail("line", got, want);
        if (after != qq[i0 + 1]) return fail("line store", after, qq[i0 + 1]);
    }

    for (int rep = 0; rep < 6; rep++) {
        seed(ga, N + 2, 300 + (uint64_t)rep); seed(lo4, N + 2, 400 + (uint64_t)rep);
        uint64_t want = 0;
        for (int i = 0; i < 300; i++) want += ga[i] + lo4[i] + ga[i + 1];
        uint64_t got = callout(ga, lo4, high, 300);
        if (got != want) return fail("callout", got, want);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    uint8_t *area = (uint8_t *)map_at(0x240000000ull, 0xc000);
    if (!area) { printf("map prot\n"); return 1; }
    g_prot = area + 0x4000;
    uint64_t *dst = (uint64_t *)(void *)(area + 0x3000);          /* dst[512] is the protected page's first word */
    for (int rep = 0; rep < 4; rep++) {
        seed(ga, N + 2, 500 + (uint64_t)rep);
        if (rep >= 2) fill(ga, dst, 256);                         /* translated and warm before the fault */
        if (mprotect(g_prot, 0x4000, PROT_NONE) != 0) { printf("mprotect\n"); return 1; }
        g_faults = 0;
        uint64_t got = fill(ga, dst, 1024), want = 0;
        for (uint64_t i = 0; i < 1024; i++) want += ga[i] + 1 + 2 * i;
        if (got != want) return fail("fault sum", got, want);
        if (g_faults != 1) return fail("faults", (uint64_t)g_faults, 1);
        if (g_fault_addr != (uintptr_t)g_prot) return fail("fault address", g_fault_addr, (uintptr_t)g_prot);
    }
    printf("OK\n");
    return 0;
}
