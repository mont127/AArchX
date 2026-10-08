/*
 * test and cmp fused with their jcc (src/jit.c emit_cmp_test_jcc): every
 * condition test can drive (e/ne, be/a, s/ns, l/ge, le/g) at each size, against
 * a register, an immediate (one bit and several) and memory, adjacent to the
 * jcc and with a load of a stack slot between them (stack_gap_load_ok).  Then
 * the gap load faults: the lazy flags are first set by popf to the opposite
 * answer, the handler maps the page and returns, and the resumed jcc must
 * still see the compare's flags (written out before the gap).  Prints each
 * outcome, against Rosetta.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CCS(X) X(e) X(ne) X(be) X(a) X(s) X(ns) X(l) X(ge) X(le) X(g)

#define TESTS(cc)                                                                             \
    static unsigned t_##cc(uint64_t x, uint64_t y)                                            \
    {                                                                                         \
        unsigned r = 0;                                                                       \
        uint64_t slot[2] = { 0x5555, 0x6666 }, t;                                             \
        __asm__ volatile(                                                                     \
            "test %[x], %[y]\n\t"        "j" #cc " 1f\n\t" "or $1, %[r]\n"   "1:\n\t"         \
            "test %k[x], %k[y]\n\t"      "j" #cc " 1f\n\t" "or $2, %[r]\n"   "1:\n\t"         \
            "test %w[x], %w[y]\n\t"      "j" #cc " 1f\n\t" "or $4, %[r]\n"   "1:\n\t"         \
            "test %b[x], %b[y]\n\t"      "j" #cc " 1f\n\t" "or $8, %[r]\n"   "1:\n\t"         \
            "test $0x80000000, %k[x]\n\t" "j" #cc " 1f\n\t" "or $16, %[r]\n" "1:\n\t"         \
            "test $-0x7fff0000, %[x]\n\t" "j" #cc " 1f\n\t" "or $32, %[r]\n" "1:\n\t"         \
            "test %[x], %[y]\n\t"   "mov 8(%%rsp), %[t]\n\t" "j" #cc " 1f\n\t" "or $64, %[r]\n"  "1:\n\t" \
            "test %k[x], %k[y]\n\t" "mov 8(%%rsp), %k[t]\n\t" "j" #cc " 1f\n\t" "or $128, %[r]\n" "1:\n\t" \
            "test $0x8000, %k[x]\n\t" "mov (%%rsp), %[t]\n\t" "j" #cc " 1f\n\t" "or $256, %[r]\n" "1:\n\t" \
            "cmp %[x], %[y]\n\t"    "mov 8(%%rsp), %[t]\n\t" "j" #cc " 1f\n\t" "or $512, %[r]\n" "1:\n\t" \
            "cmp $-5, %k[x]\n\t"    "mov (%%rsp), %[t]\n\t"  "j" #cc " 1f\n\t" "or $1024, %[r]\n" "1:\n\t" \
            "cmp $0, %[y]\n\t"      "mov (%%rsp), %[t]\n\t"  "j" #cc " 1f\n\t" "or $2048, %[r]\n" "1:\n\t" \
            "testq $7, %[m]\n\t"    "j" #cc " 1f\n\t" "or $4096, %[r]\n" "1:\n\t"             \
            "testl %k[x], %[m]\n\t" "j" #cc " 1f\n\t" "or $8192, %[r]\n" "1:"                 \
            : [r] "+r"(r), [t] "=&r"(t)                                                       \
            : [x] "r"(x), [y] "r"(y), [m] "m"(slot[0])                                        \
            : "cc");                                                                          \
        return r;                                                                             \
    }
CCS(TESTS)

typedef unsigned (*tfn)(uint64_t, uint64_t);
#define ADDR(cc) t_##cc,
static const tfn tfns[] = { CCS(ADDR) };
#define NAME(cc) #cc,
static const char *const names[] = { CCS(NAME) };

/*
 * The fault: rsp sits 8 to 24 bytes under a PROT_NONE page, the gap loads 0x18(%rsp).
 * The page is a whole 16 KB, the host's page size: ocerz protects host pages.
 */
static uint8_t *g_page;
static volatile int g_faults;
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    g_faults++;
    mprotect(g_page, 0x4000, PROT_READ | PROT_WRITE);
}

#define FAULTS(cc)                                                                            \
    static unsigned f_##cc(uint64_t x, uint64_t y, uint8_t *sp)                               \
    {                                                                                         \
        unsigned r;                                                                           \
        __asm__ volatile(                                                                     \
            "mov %%rsp, %%r11\n\t"                                                            \
            "mov %[sp], %%rsp\n\t"                                                            \
            "pushq $0xcd7\n\t" "popfq\n\t"                                                    \
            "xor %[r], %[r]\n\t"                                                              \
            "test %[x], %[y]\n\t" "mov 0x18(%%rsp), %%rax\n\t" "j" #cc " 1f\n\t" "or $1, %[r]\n" "1:\n\t" \
            "mov %%r11, %%rsp"                                                                \
            : [r] "=&r"(r)                                                                    \
            : [x] "r"(x), [y] "r"(y), [sp] "r"(sp)                                            \
            : "rax", "r11", "cc", "memory");                                                  \
        return r;                                                                             \
    }
CCS(FAULTS)
typedef unsigned (*ffn)(uint64_t, uint64_t, uint8_t *);
#undef ADDR
#define ADDR(cc) f_##cc,
static const ffn ffns[] = { CCS(ADDR) };

int main(void)
{
    static const uint64_t v[] = { 0, 1, 0xff, 0x80, 0x8000, 0x80000000ull, 0x8000000000000000ull,
                                  0x7fffffffffffffffull, 0xffffffffffffffffull, 0x0123456789abcdefull,
                                  0x80008000ull, 0x00ff00ff00ff00ffull };
    const unsigned nv = sizeof v / sizeof v[0];
    for (unsigned c = 0; c < sizeof tfns / sizeof tfns[0]; c++) {
        unsigned long long acc = 0;
        for (int pass = 0; pass < 40; pass++)
            for (unsigned i = 0; i < nv; i++)
                for (unsigned j = 0; j < nv; j++) {
                    unsigned r = tfns[c](v[i], v[j]);
                    if (pass == 0) acc = acc * 16411 + r;
                }
        printf("%s %016llx sample %04x %04x %04x\n", names[c], acc, tfns[c](0, 0), tfns[c](v[6], v[8]), tfns[c](v[9], 1));
    }

    stack_t ss = { .ss_sp = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0),
                   .ss_size = 65536, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    uint8_t *region = mmap((void *)0x2f0200000, 0x10000, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (region == MAP_FAILED)
        region = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_page = (uint8_t *)(((uintptr_t)region + 0x8000) & ~(uintptr_t)0x3fff);
    for (unsigned c = 0; c < sizeof ffns / sizeof ffns[0]; c++) {
        unsigned bits = 0;
        for (unsigned i = 0; i < 6; i++) {
            mprotect(g_page, 0x4000, PROT_NONE);
            /* the faulting slot moves each time: ocerz ends a guest that faults on one address in a row */
            bits = bits << 1 | ffns[c](v[i * 2], v[(i * 5 + 3) % nv], g_page - 0x18 + (c * 6 + i) % 3 * 8);
        }
        printf("fault %s %02x\n", names[c], bits);
    }
    printf("faults %d\n", g_faults);
    return 0;
}
