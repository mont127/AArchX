/*
 * Indexed operands whose index src/jit.c's hoist leaves unchecked where the
 * Wine layout's low window has its margins (select_low_hoist): an index a
 * loop only steps, checked once before the loop, and one the block forms
 * itself from 32-bit values.  It checks itself against plain C and prints OK
 * or the first difference:
 *   - loops that step their index up, down, and up from a negative value to
 *     zero, loading and storing;
 *   - an index formed in the block by movsxd, a 32-bit move, movzx, lea and a
 *     shift, negative ones included, and one sign-extended from a table each
 *     time round a loop;
 *   - a loop whose counter is a 32-bit register, entered with the counter in
 *     32 bits and with something above them, which the first inc removes;
 *   - a byte compare with that counter as the base of both operands and the
 *     two pointers as their indices;
 *   - an index of 2 GB that reaches from one mapping into another;
 *   - a pointer in the index register under a small offset in the base, the
 *     pointer below 2 GB and above 4 GB;
 *   - an index that starts 2 GB past the end of a mapping which, in the Wine
 *     layout build, ends at 12 GB, so past the window: each address is first
 *     read by an instruction of its own, and the loop and the formed index
 *     must then see what that saw, the same value or a fault at the same
 *     address with the registers as they were; the handler steps over the
 *     instruction and the loop goes on.  Stores are tried there only when
 *     every read faulted, and write nothing;
 *   - the same just past the end, and 128 GB past it with an index of more
 *     than 32 bits: in a stepped index, above a 32-bit counter, and loaded
 *     from a table of 64-bit values.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

static void seed(uint64_t *p, int n, uint64_t s)
{
    for (int i = 0; i < n; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        p[i] = s >> 11;
    }
}

/* Labels inside the loops, for the handler.  .alt_entry keeps each in its function's atom: the linker would otherwise
   split a function at a global label and may fold two functions' identical first halves into one. */
extern const char lm_up_insn[], lm_up_next[], lm_st_insn[], lm_st_next[], lm_form_insn[], lm_form_next[];
extern const char lm_peek_insn[], lm_peek_next[], lm_w32_insn[], lm_w32_next[], lm_g64_insn[], lm_g64_next[];
extern const char lm_fu_insn[], lm_fu_next[], lm_f32_insn[], lm_f32_next[], lm_fm_insn[], lm_fm_next[];

/* acc += b[i] for i in [i, n): the index only stepped, the base left alone.  The loop is entered by an indirect
   jump, here and below, so that its block starts at the loop's head and meets the index as the caller gave it. */
__attribute__((noinline)) static uint64_t walk_up(const uint64_t *b, int64_t i0, int64_t n)
{
    register uint64_t acc asm("rax") = 0;
    register int64_t i asm("rcx") = i0;
    uint64_t t;
    __asm__ volatile(
        "lea 1f(%%rip), %[t]\n\t"
        "jmp *%[t]\n"
        "1:\n"
        ".globl _lm_up_insn\n.alt_entry _lm_up_insn\n_lm_up_insn:\n\t"
        "add (%[b],%[i],8), %[acc]\n"
        ".globl _lm_up_next\n.alt_entry _lm_up_next\n_lm_up_next:\n\t"
        "inc %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jne 1b\n"
        : [acc] "+r"(acc), [i] "+r"(i), [t] "=&r"(t)
        : [b] "r"(b), [n] "r"(n)
        : "cc", "memory");
    return acc;
}

/* acc += b[i] for i from n - 1 down to 0 */
__attribute__((noinline)) static uint64_t walk_down(const uint64_t *b, int64_t n)
{
    uint64_t acc = 0;
    int64_t i = n;
    __asm__ volatile(
        "1:\n\t"
        "sub $1, %[i]\n\t"
        "add (%[b],%[i],8), %[acc]\n\t"
        "test %[i], %[i]\n\t"
        "jnz 1b\n"
        : [acc] "+r"(acc), [i] "+r"(i)
        : [b] "r"(b)
        : "cc", "memory");
    return acc;
}

/* acc += e[i] for i from -n up to 0, e the array's end; a 32-bit load and a byte load ride along */
__attribute__((noinline)) static uint64_t walk_neg(const uint64_t *e, int64_t n)
{
    uint64_t acc = 0, t;
    int64_t i = -n;
    __asm__ volatile(
        "1:\n\t"
        "add (%[e],%[i],8), %[acc]\n\t"
        "mov 4(%[e],%[i],8), %k[t]\n\t"
        "add %[t], %[acc]\n\t"
        "movzbl 1(%[e],%[i],8), %k[t]\n\t"
        "add %[t], %[acc]\n\t"
        "inc %[i]\n\t"
        "jnz 1b\n"
        : [acc] "+r"(acc), [i] "+r"(i), [t] "=&r"(t)
        : [e] "r"(e)
        : "cc", "memory");
    return acc;
}

/* d[i] = 3 * i + 7 for i in [i, n), the sum of what was stored */
__attribute__((noinline)) static uint64_t store_up(uint64_t *d, int64_t i0, int64_t n)
{
    register uint64_t acc asm("rax") = 0;
    register int64_t i asm("rcx") = i0;
    uint64_t t;
    __asm__ volatile(
        "1:\n\t"
        "lea 7(%[i],%[i],2), %[t]\n"
        ".globl _lm_st_insn\n.alt_entry _lm_st_insn\n_lm_st_insn:\n\t"
        "mov %[t], (%[d],%[i],8)\n"
        ".globl _lm_st_next\n.alt_entry _lm_st_next\n_lm_st_next:\n\t"
        "add %[t], %[acc]\n\t"
        "inc %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jne 1b\n"
        : [acc] "+r"(acc), [i] "+r"(i), [t] "=&r"(t)
        : [d] "r"(d), [n] "r"(n)
        : "cc", "memory");
    return acc;
}

/* One load at an address formed in full, as every layout checks it. */
__attribute__((noinline)) static uint64_t peek(const uint64_t *p)
{
    register uint64_t v asm("rax") = 0;
    __asm__ volatile(
        ".globl _lm_peek_insn\n.alt_entry _lm_peek_insn\n_lm_peek_insn:\n\t"
        "mov (%[p]), %[v]\n"
        ".globl _lm_peek_next\n.alt_entry _lm_peek_next\n_lm_peek_next:\n"
        : [v] "+r"(v) : [p] "r"(p) : "memory");
    return v;
}
static uint64_t (*volatile peek_ptr)(const uint64_t *) = peek;

/* One block that forms its indices itself: sign and zero extensions, a lea of one, a shift of another. */
__attribute__((noinline)) static uint64_t formed(const uint64_t *b, int32_t si, uint32_t zi, uint16_t wi)
{
    register uint64_t acc asm("rax");
    register int64_t x asm("rcx");
    uint64_t y, z, t;
    __asm__ volatile(
        "movslq %[si], %[x]\n\t"
        "mov (%[b],%[x],8), %[acc]\n\t"
        "mov %[zi], %k[y]\n\t"
        "add (%[b],%[y],8), %[acc]\n\t"
        "movzwl %[wi], %k[z]\n\t"
        "mov 4(%[b],%[z],4), %k[t]\n\t"
        "add %[t], %[acc]\n\t"
        "lea 3(%[x],%[x],2), %[t]\n\t"
        "movzbl (%[b],%[t],1), %k[t]\n\t"
        "add %[t], %[acc]\n\t"
        "shl $2, %[y]\n\t"
        "add (%[b],%[y],2), %[acc]\n\t"
        "movsbq %b[zi], %[z]\n\t"
        "add 64(%[b],%[z],8), %[acc]\n"
        : [acc] "=&r"(acc), [x] "=&r"(x), [y] "=&r"(y), [z] "=&r"(z), [t] "=&r"(t)
        : [b] "r"(b), [si] "r"(si), [zi] "r"(zi), [wi] "r"(wi)
        : "cc", "memory");
    return acc;
}
static uint64_t (*volatile formed_ptr)(const uint64_t *, int32_t, uint32_t, uint16_t) = formed;

static uint64_t formed_c(const uint64_t *b, int32_t si, uint32_t zi, uint16_t wi)
{
    const uint8_t *p = (const uint8_t *)b;
    uint64_t acc = b[si] + b[zi], w;
    uint32_t d;
    memcpy(&d, p + 4 + 4 * (uint64_t)wi, 4);
    acc += d;
    acc += p[3 + 3 * (int64_t)si];
    memcpy(&w, p + 8 * (uint64_t)zi, 8);
    acc += w;
    acc += b[8 + (int8_t)zi];
    return acc;
}

/* acc += b[tab[k]] for k in [0, n): the index sign-extended from a 32-bit load each time round */
__attribute__((noinline)) static uint64_t gather(const uint64_t *b, const int32_t *tab, int64_t n)
{
    register uint64_t acc asm("rax") = 0;
    register int64_t x asm("rcx");
    int64_t k = 0;
    __asm__ volatile(
        "1:\n\t"
        "movslq (%[tab],%[k],4), %[x]\n"
        ".globl _lm_form_insn\n.alt_entry _lm_form_insn\n_lm_form_insn:\n\t"
        "add (%[b],%[x],8), %[acc]\n"
        ".globl _lm_form_next\n.alt_entry _lm_form_next\n_lm_form_next:\n\t"
        "inc %[k]\n\t"
        "cmp %[n], %[k]\n\t"
        "jne 1b\n"
        : [acc] "+r"(acc), [x] "=&r"(x), [k] "+r"(k)
        : [b] "r"(b), [tab] "r"(tab), [n] "r"(n)
        : "cc", "memory");
    return acc;
}

/* acc += b[tab[k]] for k in [0, n), the index a 64-bit load: nothing says how large it is */
__attribute__((noinline)) static uint64_t gather64(const uint64_t *b, const int64_t *tab, int64_t n)
{
    register uint64_t acc asm("rax") = 0;
    register int64_t x asm("rcx");
    int64_t k = 0;
    __asm__ volatile(
        "1:\n\t"
        "mov (%[tab],%[k],8), %[x]\n"
        ".globl _lm_g64_insn\n.alt_entry _lm_g64_insn\n_lm_g64_insn:\n\t"
        "add (%[b],%[x],8), %[acc]\n"
        ".globl _lm_g64_next\n.alt_entry _lm_g64_next\n_lm_g64_next:\n\t"
        "inc %[k]\n\t"
        "cmp %[n], %[k]\n\t"
        "jne 1b\n"
        : [acc] "+r"(acc), [x] "=&r"(x), [k] "+r"(k)
        : [b] "r"(b), [tab] "r"(tab), [n] "r"(n)
        : "cc", "memory");
    return acc;
}

/* acc += b[i] for i from i0 until the 32-bit counter reaches n; what is above the counter's 32 bits goes at the first inc */
__attribute__((noinline)) static uint64_t walk32(const uint64_t *b, uint64_t i0, uint64_t n)
{
    register uint64_t acc asm("rax") = 0;
    register uint64_t i asm("rcx") = i0;
    uint64_t t;
    __asm__ volatile(
        "lea 1f(%%rip), %[t]\n\t"
        "jmp *%[t]\n"
        "1:\n"
        ".globl _lm_w32_insn\n.alt_entry _lm_w32_insn\n_lm_w32_insn:\n\t"
        "add (%[b],%[i],8), %[acc]\n"
        ".globl _lm_w32_next\n.alt_entry _lm_w32_next\n_lm_w32_next:\n\t"
        "inc %k[i]\n\t"
        "cmp %k[n], %k[i]\n\t"
        "jne 1b\n"
        : [acc] "+r"(acc), [i] "+r"(i), [t] "=&r"(t)
        : [b] "r"(b), [n] "r"(n)
        : "cc", "memory");
    return acc;
}

/*
 * Loops of their own for the indices no check lets through, so that each is translated for small indices first and
 * then entered with a large one, having met nothing in between: acc += b[i], then i stepped by 1, i's low half
 * stepped by 1, or i's low half stepped and d added to all of it.
 */
#define FAR_LOOP(name, lbl, step) \
__attribute__((noinline)) static uint64_t name(const uint64_t *b, uint64_t i0, uint64_t d, uint64_t n) \
{ \
    register uint64_t acc asm("rax") = 0; \
    register uint64_t i asm("rcx") = i0; \
    uint64_t t; \
    __asm__ volatile( \
        "lea 1f(%%rip), %[t]\n\tjmp *%[t]\n1:\n" \
        ".globl _lm_" lbl "_insn\n.alt_entry _lm_" lbl "_insn\n_lm_" lbl "_insn:\n\t" \
        "add (%[b],%[i],8), %[acc]\n" \
        ".globl _lm_" lbl "_next\n.alt_entry _lm_" lbl "_next\n_lm_" lbl "_next:\n\t" \
        step "\n\tdec %[n]\n\tjnz 1b\n" \
        : [acc] "+r"(acc), [i] "+r"(i), [n] "+r"(n), [t] "=&r"(t) \
        : [b] "r"(b), [d] "r"(d) \
        : "cc", "memory"); \
    return acc; \
}
FAR_LOOP(far_up, "fu", "inc %[i]")
FAR_LOOP(far32, "f32", "inc %k[i]")
FAR_LOOP(far_mix, "fm", "lea 1(%[i]), %k[i]\n\tadd %[d], %[i]")

/* the same with bytes, so that a counter above 32 bits can still name memory */
__attribute__((noinline)) static uint64_t bytes32(const uint8_t *b, uint64_t i0, uint64_t n)
{
    uint64_t acc = 0, i = i0, t;
    __asm__ volatile(
        "lea 1f(%%rip), %[t]\n\t"
        "jmp *%[t]\n"
        "1:\n\t"
        "movzbl (%[b],%[i],1), %k[t]\n\t"
        "add %[t], %[acc]\n\t"
        "inc %k[i]\n\t"
        "cmp %k[n], %k[i]\n\t"
        "jne 1b\n"
        : [acc] "+r"(acc), [i] "+r"(i), [t] "=&r"(t)
        : [b] "r"(b), [n] "r"(n)
        : "cc", "memory");
    return acc;
}

/* How many bytes of p and q agree, up to n: the counter is the base of both operands, the pointers their indices. */
__attribute__((noinline)) static uint64_t match_len(const uint8_t *p, const uint8_t *q, uint64_t n)
{
    uint64_t i = 0, t;
    __asm__ volatile(
        "1:\n\t"
        "movzbl (%[i],%[p],1), %k[t]\n\t"
        "cmp (%[i],%[q],1), %b[t]\n\t"
        "jne 2f\n\t"
        "inc %k[i]\n\t"
        "cmp %k[n], %k[i]\n\t"
        "jne 1b\n"
        "2:\n"
        : [i] "+r"(i), [t] "=&r"(t)
        : [p] "r"(p), [q] "r"(q), [n] "r"(n)
        : "cc", "memory");
    return i;
}

/* acc += the bytes at off + p for p in [p, end): the pointer is the index, a small offset the base */
__attribute__((noinline)) static uint64_t bytes_by_index(uint64_t off, const uint8_t *p, const uint8_t *end)
{
    uint64_t acc = 0, t;
    __asm__ volatile(
        "1:\n\t"
        "movzbl (%[off],%[p],1), %k[t]\n\t"
        "add %[t], %[acc]\n\t"
        "inc %[p]\n\t"
        "cmp %[end], %[p]\n\t"
        "jne 1b\n"
        : [acc] "+r"(acc), [p] "+r"(p), [t] "=&r"(t)
        : [off] "r"(off), [end] "r"(end)
        : "cc", "memory");
    return acc;
}

static volatile int g_nfault;
static uint64_t g_faddr[8], g_fidx[8], g_facc[8];
static const char *volatile g_from, *volatile g_to;

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    int k = g_nfault++;
    (void)sig;
    if (k >= 8 || (const char *)(uintptr_t)uc->uc_mcontext->__ss.__rip != g_from) {
        static const char msg[] = "fault at an instruction that should not fault\n";
        write(1, msg, sizeof msg - 1);
        _exit(1);
    }
    g_faddr[k] = (uint64_t)(uintptr_t)si->si_addr;
    g_fidx[k] = uc->uc_mcontext->__ss.__rcx;
    g_facc[k] = uc->uc_mcontext->__ss.__rax;
    uc->uc_mcontext->__ss.__rip = (uint64_t)(uintptr_t)g_to;
}

#define TAIL 512                    /* elements before the end that the runs past it are based */
#define PAST (1ll << 28)            /* and how many elements past it they start: 2 GB */
#define FAR (1ll << 34)             /* an index no check lets through: 128 GB of elements, past any margin */

int main(void)
{
    g_low = (uintptr_t)&g_low < 0x300000000ull;
    /* a: 2 GB and a little, of which only the first and last megabyte are touched */
    size_t alen = (2ull << 30) + (1u << 20);
    uint64_t *a = (uint64_t *)map_at(0x40000000ull, alen);
    uint8_t *lowp = (uint8_t *)map_at(0x10000000ull, 1u << 16);
    /* e: a megabyte that in the Wine layout build ends at 12 GB */
    uint8_t *emap = (uint8_t *)map_at(0x2fff00000ull, 1u << 20);
    if (!a || !lowp || !emap) { printf("map\n"); return 1; }
    uint64_t *end = (uint64_t *)(void *)(emap + (1u << 20));
    uint64_t *far = (uint64_t *)(void *)((uint8_t *)a + (2ull << 30));
    enum { N = 4096 };
    static uint64_t buf[N + 16];

    for (int rep = 0; rep < 8; rep++) {
        seed(a, N + 16, 11 + (uint64_t)rep);
        seed(far, N + 16, 23 + (uint64_t)rep);
        seed(buf, N + 16, 31 + (uint64_t)rep);
        seed(end - N, N, 43 + (uint64_t)rep);
        const uint64_t *srcs[4] = { a, far, buf, end - N };
        for (int x = 0; x < 4; x++) {
            const uint64_t *s = srcs[x];
            uint64_t want = 0, got;
            for (int i = 3; i < N; i++) want += s[i];
            if ((got = walk_up(s, 3, N)) != want) return fail("up", got, want);
            if ((got = walk32(s, 3, N)) != want) return fail("up32", got, want);
            want += s[0] + s[1] + s[2];
            if ((got = walk_down(s, N)) != want) return fail("down", got, want);
            want = 0;
            for (int i = 0; i < N; i++) {
                uint32_t d;
                memcpy(&d, (const uint8_t *)&s[i] + 4, 4);
                want += s[i] + d + ((const uint8_t *)&s[i])[1];
            }
            if ((got = walk_neg(s + N, N)) != want) return fail("neg", got, want);
        }
        uint64_t *dsts[3] = { a, far, end - N };
        for (int x = 0; x < 3; x++) {
            uint64_t want = 0, got = store_up(dsts[x], 5, N);
            for (int i = 5; i < N; i++) want += 3 * (uint64_t)i + 7;
            if (got != want) return fail("store sum", got, want);
            for (int i = 5; i < N; i++)
                if (dsts[x][i] != 3 * (uint64_t)i + 7) return fail("store", dsts[x][i], 3 * (uint64_t)i + 7);
        }
    }

    /* indices the block forms, from the middle of each area so the negative ones stay inside */
    for (int rep = 0; rep < 4096; rep++) {
        uint64_t s = 99 + (uint64_t)rep * 7919;
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        const uint64_t *mids[3] = { a + 2048, buf + 2048, end - 2048 };
        const uint64_t *b = mids[rep % 3];
        int32_t si = (int32_t)((s >> 20) % 1000) - 600;
        uint32_t zi = (uint32_t)((s >> 33) % 255);
        uint16_t wi = (uint16_t)((s >> 44) % 900);
        if (rep == 0) { seed(a, N + 16, 5); seed(buf, N + 16, 6); seed(end - N, N, 7); }
        uint64_t got = formed_ptr(b, si, zi, wi), want = formed_c(b, si, zi, wi);
        if (got != want) return fail("formed", got, want);
    }

    /* a table of indices, some negative, from the middle of each area */
    {
        static int32_t tab[512];
        const uint64_t *mids[3] = { a + 2048, buf + 2048, end - 2048 };
        for (int rep = 0; rep < 12; rep++) {
            const uint64_t *b = mids[rep % 3];
            uint64_t s = 4242 + (uint64_t)rep, want = 0, got;
            for (int k = 0; k < 512; k++) {
                s = s * 6364136223846793005ull + 1442695040888963407ull;
                tab[k] = (int32_t)((s >> 30) % 3000) - 1500;
                want += b[tab[k]];
            }
            if ((got = gather(b, tab, 512)) != want) return fail("gather", got, want);
        }
    }

    /* the 32-bit counter over bytes: entered in range, and 4 GB out, where the first byte is c4's and the rest c0's */
    {
        uint8_t *c0 = g_low ? (uint8_t *)map_at(0x20000000ull, 1u << 16)
                            : (uint8_t *)mmap(NULL, (4ull << 30) + (1u << 16), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        uint8_t *c4 = g_low ? (uint8_t *)map_at(0x120000000ull, 1u << 16) : c0 + (4ull << 30);
        if (!c0 || c0 == MAP_FAILED || !c4) { printf("map c\n"); return 1; }
        for (int i = 0; i < 4096; i++) { c0[i] = (uint8_t)(i * 13 + 5); c4[i] = (uint8_t)(i * 29 + 3); }
        for (int rep = 0; rep < 6; rep++) {
            uint64_t want = 0, got;
            for (int i = 7; i < 3000; i++) want += c0[i];
            if ((got = bytes32(c0, 7, 3000)) != want) return fail("bytes32", got, want);
            want += c4[6];
            if ((got = bytes32(c0, (1ull << 32) + 6, 3000)) != want) return fail("bytes32 above", got, want);
        }
        /* the compare: equal for 1000 bytes, then not; from c0 against a copy in c4 */
        memcpy(c4, c0, 4096);
        c4[1000] ^= 1;
        for (int rep = 0; rep < 6; rep++) {
            uint64_t got = match_len(c0 + rep, c4 + rep, 3000);
            if (got != (uint64_t)(1000 - rep)) return fail("match", got, (uint64_t)(1000 - rep));
        }
    }

    /* an index of 2 GB: from a into far, by a 32-bit index scaled by 8 and by a stepped one that starts there */
    seed(far, N + 16, 77);
    {
        uint64_t got = formed_ptr(a, (int32_t)(1u << 28), 1u << 28, 0), want = formed_c(a, (int32_t)(1u << 28), 1u << 28, 0);
        if (got != want) return fail("formed far", got, want);
        want = 0;
        for (int i = 0; i < N; i++) want += far[i];
        if ((got = walk_up(a, 1ll << 28, (1ll << 28) + N)) != want) return fail("up far", got, want);
    }

    /* the pointer as index: below 2 GB, where the check before the loop passes, and above it, where it cannot */
    for (int rep = 0; rep < 8; rep++) {
        uint8_t *areas[2] = { lowp, (uint8_t *)far };
        for (int x = 0; x < 2; x++) {
            uint8_t *p = areas[x];
            uint64_t want = 0, got;
            for (int i = 0; i < 5000; i++) p[i] = (uint8_t)(i * 7 + rep + x);
            for (int i = 0; i < 4000; i++) want += p[16 + i];
            if ((got = bytes_by_index(16, p, p + 4000)) != want) return fail("bytes", got, want);
        }
    }

    /* an index that reaches from one mapping into another: lowp + 8 * ((a - lowp) / 8 + i) is a[i] */
    seed(a, N + 16, 123);
    {
        uint64_t want = 0, got;
        int64_t i0 = (int64_t)(((uintptr_t)a - (uintptr_t)lowp) / 8);
        for (int i = 0; i < N; i++) want += a[i];
        if ((got = walk_up((const uint64_t *)(void *)lowp, i0, i0 + N)) != want) return fail("up wide", got, want);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    /*
     * Past the end.  What is at those addresses is not this program's, so each
     * is read alone first; twice, the second time with the loops translated
     * for what they met.
     */
    for (int rep = 0; rep < 2; rep++) {
        uint64_t seen[4], want = 0, got;
        int faulted[4], nf = 0;
        for (int k = 0; k < 4; k++) {
            g_nfault = 0; g_from = lm_peek_insn; g_to = lm_peek_next;
            seen[k] = peek_ptr(end + PAST + k);
            faulted[k] = g_nfault;
            if (g_nfault && g_faddr[0] != (uintptr_t)(end + PAST + k)) return fail("peek address", g_faddr[0], (uintptr_t)(end + PAST + k));
            nf += faulted[k];
        }

        g_nfault = 0; g_from = lm_up_insn; g_to = lm_up_next;
        got = walk_up(end - TAIL, TAIL + PAST, TAIL + PAST + 4);
        for (int k = 0, f = 0; k < 4; k++) {
            if (!faulted[k]) { want += seen[k]; continue; }
            if (f >= g_nfault) return fail("past faults", (uint64_t)g_nfault, (uint64_t)nf);
            if (g_faddr[f] != (uintptr_t)(end + PAST + k)) return fail("past address", g_faddr[f], (uintptr_t)(end + PAST + k));
            if (g_fidx[f] != (uint64_t)(TAIL + PAST + k)) return fail("past index", g_fidx[f], (uint64_t)(TAIL + PAST + k));
            if (g_facc[f] != want) return fail("past acc", g_facc[f], want);
            f++;
        }
        if (g_nfault != nf) return fail("past faults", (uint64_t)g_nfault, (uint64_t)nf);
        if (got != want) return fail("past sum", got, want);

        /* the 32-bit counter past the end */
        want = 0;
        g_nfault = 0; g_from = lm_w32_insn; g_to = lm_w32_next;
        got = walk32(end - TAIL, (uint64_t)(TAIL + PAST), (uint64_t)(TAIL + PAST + 4));
        for (int k = 0, f = 0; k < 4; k++) {
            if (!faulted[k]) { want += seen[k]; continue; }
            if (f >= g_nfault) return fail("past32 faults", (uint64_t)g_nfault, (uint64_t)nf);
            if (g_faddr[f] != (uintptr_t)(end + PAST + k)) return fail("past32 address", g_faddr[f], (uintptr_t)(end + PAST + k));
            if (g_fidx[f] != (uint64_t)(TAIL + PAST + k)) return fail("past32 index", g_fidx[f], (uint64_t)(TAIL + PAST + k));
            if (g_facc[f] != want) return fail("past32 acc", g_facc[f], want);
            f++;
        }
        if (g_nfault != nf) return fail("past32 faults", (uint64_t)g_nfault, (uint64_t)nf);
        if (got != want) return fail("past32 sum", got, want);

        /* the table's index: entry 5 names the first element past the end */
        {
            static int32_t tab[16];
            want = 0;
            for (int k = 0; k < 16; k++) { tab[k] = k * 7 - 40; want += (end - 300)[tab[k]]; }
            if ((got = gather(end - 300, tab, 16)) != want) return fail("gather end", got, want);
            want -= (end - 300)[tab[5]];
            tab[5] = (int32_t)(300 + PAST);
            if (!faulted[0]) want += seen[0];
            g_nfault = 0; g_from = lm_form_insn; g_to = lm_form_next;
            got = gather(end - 300, tab, 16);
            if (g_nfault != faulted[0]) return fail("gather past faults", (uint64_t)g_nfault, (uint64_t)faulted[0]);
            if (faulted[0]) {
                if (g_faddr[0] != (uintptr_t)(end + PAST)) return fail("gather past address", g_faddr[0], (uintptr_t)(end + PAST));
                if (g_fidx[0] != (uint64_t)(300 + PAST)) return fail("gather past index", g_fidx[0], (uint64_t)(300 + PAST));
            }
            if (got != want) return fail("gather past", got, want);
        }

        /*
         * Just past the end, where the layout may have something readable, and
         * 128 GB on, further than an unchecked index may reach: a stepped index
         * of more than 32 bits, the same above a 32-bit counter, and one loaded
         * whole from a table.  Each must see what the single load saw.
         */
        {
            const uint64_t *base = end - TAIL;
            int64_t at[5] = { TAIL, TAIL + FAR, TAIL + FAR + 1, FAR + 5, FAR + 6 };
            uint64_t one[5];
            int flt[5];
            for (int k = 0; k < 5; k++) {
                g_nfault = 0; g_from = lm_peek_insn; g_to = lm_peek_next;
                one[k] = peek_ptr(base + at[k]);
                flt[k] = g_nfault;
            }
            /* just past the end: base[TAIL - 2], base[TAIL - 1], then whatever is at the end */
            want = base[TAIL - 2] + base[TAIL - 1] + (flt[0] ? 0 : one[0]);
            g_nfault = 0; g_from = lm_up_insn; g_to = lm_up_next;
            got = walk_up(base, TAIL - 2, TAIL + 1);
            if (g_nfault != flt[0]) return fail("near faults", (uint64_t)g_nfault, (uint64_t)flt[0]);
            if (flt[0] && g_faddr[0] != (uintptr_t)(base + at[0])) return fail("near address", g_faddr[0], (uintptr_t)(base + at[0]));
            if (got != want) return fail("near sum", got, want);

            /*
             * Far: each loop first runs inside the array, then is entered with an index 128 GB of elements on.
             * far_up reads two elements there; far32 reads one, and its counter's low half then brings it
             * back to base[6 .. 11]; far_mix reads base[5] and gets there by its 64-bit add.
             */
            struct { uint64_t (*f)(const uint64_t *, uint64_t, uint64_t, uint64_t); const char *from, *to, *name;
                     uint64_t i0, d, n; int first, count; uint64_t inside; } fl[3] = {
                { far_up,  lm_fu_insn,  lm_fu_next,  "far up",  (uint64_t)(TAIL + FAR), 0, 2, 1, 2, 0 },
                { far32,   lm_f32_insn, lm_f32_next, "far 32",  (uint64_t)(FAR + 5), 0, 7, 3, 1, 0 },
                { far_mix, lm_fm_insn,  lm_fm_next,  "far mix", 5, (uint64_t)FAR, 2, 3, 1, 0 },
            };
            for (int i = 6; i < 12; i++) fl[1].inside += base[i];
            fl[2].inside = base[5];
            fl[2].first = 4;
            for (int w = 0; w < 3; w++) {
                uint64_t warm = 0;
                for (int i = 3; i < 23; i++) warm += base[i];
                g_nfault = 0; g_from = fl[w].from; g_to = fl[w].to;
                if ((got = fl[w].f(base, 3, 0, 20)) != warm) return fail(fl[w].name, got, warm);
                want = fl[w].inside;
                int expect = 0;
                for (int k = fl[w].first; k < fl[w].first + fl[w].count; k++) { if (flt[k]) expect++; else want += one[k]; }
                got = fl[w].f(base, fl[w].i0, fl[w].d, fl[w].n);
                if (g_nfault != expect) return fail(fl[w].name, (uint64_t)g_nfault, (uint64_t)expect);
                for (int k = fl[w].first, f = 0; k < fl[w].first + fl[w].count; k++)
                    if (flt[k] && g_faddr[f++] != (uintptr_t)(base + at[k])) return fail(fl[w].name, g_faddr[f - 1], (uintptr_t)(base + at[k]));
                if (got != want) return fail(fl[w].name, got, want);
            }
            /* the table of 64-bit indices */
            static int64_t tab64[8];
            want = 0;
            for (int k = 0; k < 8; k++) { tab64[k] = k * 5 - 30; want += base[tab64[k]]; }
            if ((got = gather64(base, tab64, 8)) != want) return fail("gather64", got, want);
            want -= base[tab64[3]];
            tab64[3] = at[1];
            if (!flt[1]) want += one[1];
            g_nfault = 0; g_from = lm_g64_insn; g_to = lm_g64_next;
            got = gather64(base, tab64, 8);
            if (g_nfault != flt[1]) return fail("gather64 far faults", (uint64_t)g_nfault, (uint64_t)flt[1]);
            if (flt[1] && g_faddr[0] != (uintptr_t)(base + at[1])) return fail("gather64 far address", g_faddr[0], (uintptr_t)(base + at[1]));
            if (got != want) return fail("gather64 far", got, want);
        }

        if (nf != 4) continue;
        want = 0;
        for (int k = 0; k < 4; k++) want += 3 * (uint64_t)(TAIL + PAST + k) + 7;
        g_nfault = 0; g_from = lm_st_insn; g_to = lm_st_next;
        got = store_up(end - TAIL, TAIL + PAST, TAIL + PAST + 4);
        if (got != want) return fail("store past sum", got, want);
        if (g_nfault != 4) return fail("store past faults", (uint64_t)g_nfault, 4);
        for (int k = 0; k < 4; k++) {
            if (g_faddr[k] != (uintptr_t)(end + PAST + k)) return fail("store past address", g_faddr[k], (uintptr_t)(end + PAST + k));
            if (g_fidx[k] != (uint64_t)(TAIL + PAST + k)) return fail("store past index", g_fidx[k], (uint64_t)(TAIL + PAST + k));
        }
    }
    printf("OK\n");
    return 0;
}
