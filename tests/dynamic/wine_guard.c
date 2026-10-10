/*
 * Memory reached through registers on both sides of 12 GB, at the same
 * instructions, the shape src/jit.c's Wine-layout guard (wine_guard) takes:
 * it compares an address with x29, which every block's prologue sets to 12 GB,
 * and lays out the side the address was on when the block was translated, the
 * other side going through an arm.  So each function here is first run on one
 * side and then on the other, and the other way round, with loads and stores
 * of every size, an SSE load and store, read-modify-write and compare forms,
 * and an index.  Between rounds come what must leave x29 alone: calls out to
 * the host (write, getpid), strlen and memcpy (answered in place, with rdi and
 * rsi saved in x16 and x17), a signal handler that runs guest code and touches
 * both sides, and a second thread doing the same work at the same time.  In
 * the Wine layout build the low side is a mapping at 5 GB and the image's own
 * data; the high side is malloc's.  Elsewhere both are ordinary memory, so the
 * output is the same.  Against Rosetta.
 */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CELLS 512
static uint64_t g_image_cells[CELLS];

__attribute__((noinline)) static uint64_t mixed_access(uint64_t *p, uint64_t i, uint64_t v)
{
    uint64_t acc;
    __asm__ volatile(
        "mov (%[p]), %[acc]\n\t"
        "add 8(%[p]), %[acc]\n\t"
        "movzbl 17(%[p]), %%eax\n\t"     "add %%rax, %[acc]\n\t"
        "movswq 22(%[p]), %%rax\n\t"     "add %%rax, %[acc]\n\t"
        "mov 28(%[p]), %%eax\n\t"        "xor %%rax, %[acc]\n\t"
        "mov %[v], 32(%[p])\n\t"
        "movl %k[v], 40(%[p])\n\t"
        "movw %w[v], 46(%[p])\n\t"
        "movb %b[v], 49(%[p])\n\t"
        "addq %[v], 56(%[p])\n\t"
        "xorl %k[v], 64(%[p])\n\t"
        "incq 72(%[p])\n\t"
        "cmpq %[acc], 80(%[p])\n\t"      "adc $0, %[acc]\n\t"
        "mov (%[p],%[i],8), %%rax\n\t"   "add %%rax, %[acc]\n\t"
        "mov %[acc], 8(%[p],%[i],8)\n\t"
        "movdqu 96(%[p]), %%xmm0\n\t"
        "movdqu %%xmm0, 112(%[p])\n\t"
        "movq %%xmm0, %%rax\n\t"         "add %%rax, %[acc]\n\t"
        : [acc] "=&r"(acc) : [p] "r"(p), [i] "r"(i), [v] "r"(v) : "rax", "xmm0", "memory", "cc");
    return acc;
}

static uint64_t sum_cells(const uint64_t *p)
{
    uint64_t s = 0;
    for (int k = 0; k < CELLS; k++) s = s * 31 + p[k];
    return s;
}

static void seed(uint64_t *p, uint64_t s)
{
    for (int k = 0; k < CELLS; k++) { s = s * 6364136223846793005ull + 1442695040888963407ull; p[k] = s ^ (s >> 29); }
}

/* Each side, then the other, then alternating, on the same two functions. */
static uint64_t rounds(uint64_t *lo, uint64_t *hi, uint64_t salt)
{
    uint64_t acc = 0;
    char buf[64];
    for (int r = 0; r < 600; r++) {
        uint64_t *p = (r < 200) ? lo : (r < 400) ? hi : ((r & 1) ? lo : hi);
        uint64_t *q = p == lo ? hi : lo;
        acc += mixed_access(p, (uint64_t)(r % 40) + 12, acc ^ salt ^ (uint64_t)r);
        acc ^= mixed_access(q + 64, (uint64_t)(r % 33) + 12, acc + (uint64_t)r);
        if (r % 97 == 0) {
            snprintf(buf, sizeof buf, "%llx", (unsigned long long)acc);
            acc += strlen(buf) + (uint64_t)getpid() * 0;
            memcpy(q + 200, p + 100, 256);
            memcpy(p + 300, buf, strlen(buf));
        }
    }
    return acc + sum_cells(lo) * 3 + sum_cells(hi);
}

static uint64_t *g_sig_lo, *g_sig_hi;
static volatile uint64_t g_sig_acc;
static void on_alarm(int sig)
{
    (void)sig;
    g_sig_acc += mixed_access(g_sig_lo + 128, 3, 7) + mixed_access(g_sig_hi + 128, 5, 9);
}

struct job { uint64_t *lo, *hi, result; };
static void *worker(void *arg)
{
    struct job *j = arg;
    j->result = rounds(j->lo, j->hi, 0x5555);
    return NULL;
}

/* Memory at a fixed address in the Wine layout's build, if nothing is there; anywhere otherwise. */
static uint64_t *place(uint64_t want, size_t len, int low_build)
{
    mach_vm_address_t a = want;
    if (low_build && mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED) == KERN_SUCCESS)
        return (uint64_t *)(uintptr_t)a;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int main(void)
{
    int low_build = (uintptr_t)g_image_cells - 0x200000000ull < 0x100000000ull;
    size_t len = CELLS * 8 * 2;
    uint64_t *lo5 = place(0x140000000ull, len, low_build), *hi = malloc(len), *hi2 = malloc(len);
    uint64_t *lo6 = place(0x160000000ull, len, low_build);
    if (!lo5 || !hi || !hi2 || !lo6) { printf("map\n"); return 1; }
    fprintf(stderr, "wine_guard: low %p %p image %p, high %p %p\n", (void *)lo5, (void *)lo6, (void *)g_image_cells,
            (void *)hi, (void *)hi2);

    seed(lo5, 1); seed(hi, 2); seed(g_image_cells, 3);
    uint64_t a = rounds(lo5, hi, 0x1234);
    uint64_t b = rounds(g_image_cells, hi, 0x4321);

    g_sig_lo = lo5; g_sig_hi = hi;
    signal(SIGALRM, on_alarm);
    for (int k = 0; k < 20; k++) {
        raise(SIGALRM);
        a += mixed_access(lo5 + 256, 4, (uint64_t)k) ^ mixed_access(hi + 256, 6, (uint64_t)k);
    }

    seed(lo6, 4); seed(hi2, 5);
    struct job j = { lo6, hi2, 0 };
    pthread_t t;
    pthread_create(&t, NULL, worker, &j);
    uint64_t c = rounds(hi, lo5, 0x9999);
    pthread_join(t, NULL);

    printf("rounds %llx %llx %llx thread %llx signal %llx\n", (unsigned long long)a, (unsigned long long)b,
           (unsigned long long)c, (unsigned long long)j.result, (unsigned long long)g_sig_acc);
    return 0;
}
