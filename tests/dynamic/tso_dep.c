/*
 * Loads that src/jit.c leaves plain in ordered memory because the next load
 * is addressed through what they loaded (dep_plain_mark), in the handoff x86
 * never gets wrong: a writer stores data and then a counter, and a reader
 * that loads the counter and then the data must not see data older than the
 * counter.  The counter is the plain load; between it and the data comes the
 * load that orders it, in each form the translator accepts: a mov, a cmp, an
 * add and a test with a memory source, and an indirect call, addressed
 * through the counter by way of lea, and, a sign extension and a shift; after
 * an arithmetic load of the counter, after a store, and at the end of a chain
 * of three.  The first reader loads the counter and the data with nothing
 * between, where the counter's load must stay an acquire.
 *
 * The counter's address comes out of a chain of multiplies, so that its load
 * is ready long after the data's is, and the data's load is itself left plain
 * by the rule, so only the load between keeps it behind the counter's.  Even
 * so an Apple M5 shows a broken rule a few times in four million rounds and
 * not in every run: the readers take turns sixteen rounds at a time, which is
 * what shows it most, and run_dynamic_tests.sh also runs this program under
 * OCERZ_DEP_PLAIN_CHECK=1, which finds a plain load with no acquire after it
 * in the translated code itself.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t data __attribute__((aligned(128)));
static uint64_t flag __attribute__((aligned(128)));
static uint64_t *flagp __attribute__((aligned(128))) = &flag;
static uint64_t scratch __attribute__((aligned(128)));
static uint64_t table[64] __attribute__((aligned(128)));
enum { ROUNDS = 400000, SHAPES = 10 };
static volatile int stop;

__attribute__((naked)) static void stub(void) { __asm__("ret"); }

static void *writer(void *a)
{
    (void)a;
    for (uint64_t i = 1; !stop; i++) {
        __asm__ volatile("movq %1, %0" : "=m"(data) : "r"(i) : "memory");
        __asm__ volatile("movq %1, %0" : "=m"(flag) : "r"(i) : "memory");
    }
    return NULL;
}

#define READER(name, code) \
    __attribute__((noinline)) static void name(uint64_t *fo, uint64_t *dout) \
    { \
        uint64_t f, d; \
        __asm__ volatile(code \
                         : "=&a"(f), "=&S"(d) \
                         : [fl] "r"(&flag), [da] "r"(&data), [tb] "r"(table), [sc] "r"(&scratch), [pp] "r"(&flagp) \
                         : "rcx", "rdx", "r10", "r11", "cc", "memory"); \
        *fo = f; *dout = d; \
    }
/* r11 = the counter's address, by way of x * 3 * (1/3) four times */
#define M3 "leaq (%%r11,%%r11,2), %%r11\n\timulq %%r10, %%r11\n\t"
#define LATE(p) "movq " p ", %%r11\n\tmovabsq $0xaaaaaaaaaaaaaaab, %%r10\n\t" M3 M3 M3 M3
#define INDEX "andl $0x1f8, %%ecx\n\t"
/* the data's load is plain too, by the same rule, so that only the load between orders it after the counter's */
#define DATA  "movq (%[da]), %%rsi\n\tmovl %%esi, %%ecx\n\t" INDEX "movq (%[tb],%%rcx), %%rdx"
READER(plain_pair, LATE("%[fl]") "movq (%%r11), %%rax\n\t" DATA)
READER(by_mov,     LATE("%[fl]") "movq (%%r11), %%rax\n\tleaq (%%rax,%%rax,2), %%rcx\n\t" INDEX "movq (%[tb],%%rcx), %%rdx\n\t" DATA)
READER(by_cmp,     LATE("%[fl]") "movq (%%r11), %%rax\n\tmovl %%eax, %%ecx\n\t" INDEX "cmpq (%[tb],%%rcx), %%rdx\n\t" DATA)
READER(by_add,     LATE("%[fl]") "movq (%%r11), %%rax\n\tmovl %%eax, %%ecx\n\t" INDEX "addq (%[tb],%%rcx), %%rdx\n\t" DATA)
READER(by_test,    LATE("%[fl]") "movq (%%r11), %%rax\n\tmovl %%eax, %%ecx\n\t" INDEX "testq $1, (%[tb],%%rcx)\n\t" DATA)
READER(by_call,    LATE("%[fl]") "movq (%%r11), %%rax\n\tmovl %%eax, %%ecx\n\t" INDEX "subq $128, %%rsp\n\tcallq *(%[tb],%%rcx)\n\taddq $128, %%rsp\n\t" DATA)
READER(alu_head,   LATE("%[fl]") "xorl %%eax, %%eax\n\taddq (%%r11), %%rax\n\tleaq (,%%rax,8), %%rcx\n\t" INDEX "movq (%[tb],%%rcx), %%rdx\n\t" DATA)
READER(narrow_head,LATE("%[fl]") "movl (%%r11), %%eax\n\tmovslq %%eax, %%rcx\n\tshlq $3, %%rcx\n\t" INDEX "movq (%[tb],%%rcx), %%rdx\n\t" DATA)
READER(past_store, LATE("%[fl]") "movq (%%r11), %%rax\n\tmovq %%rax, (%[sc])\n\tmovl %%eax, %%ecx\n\t" INDEX "movq (%[tb],%%rcx), %%rdx\n\t" DATA)
READER(chain,      LATE("%[pp]") "movq (%%r11), %%r11\n\tmovq (%%r11), %%rax\n\tmovl %%eax, %%ecx\n\t" INDEX "movq (%[tb],%%rcx), %%rdx\n\t" DATA)

int main(void)
{
    static void (*const shape[SHAPES])(uint64_t *, uint64_t *) = { plain_pair, by_mov, by_cmp, by_add, by_test, by_call,
                                                                   alu_head, narrow_head, past_store, chain };
    static const char *const name[SHAPES] = { "plain_pair", "by_mov", "by_cmp", "by_add", "by_test", "by_call",
                                              "alu_head", "narrow_head", "past_store", "chain" };
    for (int i = 0; i < 64; i++) table[i] = (uint64_t)(uintptr_t)stub;
    pthread_t t;
    pthread_create(&t, NULL, writer, NULL);
    uint64_t f = 0, d = 0;
    long bad[SHAPES] = { 0 };
    for (unsigned i = 0; i < ROUNDS * SHAPES; i++) {
        unsigned k = (i >> 4) % SHAPES;
        shape[k](&f, &d);
        if (d < f)
            bad[k]++;
    }
    stop = 1;
    pthread_join(t, NULL);
    int any = 0;
    for (int k = 0; k < SHAPES; k++)
        if (bad[k]) { printf("BAD %s %ld\n", name[k], bad[k]); any = 1; }
    if (!any)
        printf("OK\n");
    return any;
}
