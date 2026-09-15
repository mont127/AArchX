#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>

#define PAGE 0x4000
typedef double v4d __attribute__((vector_size(32), aligned(8)));
static unsigned char *g_map;
static volatile int g_faults;

__attribute__((noinline)) static void run_sse2(const double *a, long n, double *out)
{
    double x = 1.5, y = -0.25, s = 0.0, t = 2.0;
    for (long i = 0; i < n; i++) {
        x = x * a[i] + y;
        y = y - x * 0.5;
        t = t * 0.75 + a[i];
        s += a[i] * x - t;
    }
    out[0] = x; out[1] = y; out[2] = s; out[3] = t;
}
__attribute__((noinline, target("avx2,fma"))) static void run_avx(const double *a, long n, double *out)
{
    double x = 1.5, y = -0.25, s = 0.0, t = 2.0;
    for (long i = 0; i < n; i++) {
        x = x * a[i] + y;
        y = y - x * 0.5;
        t = t * 0.75 + a[i];
        s += a[i] * x - t;
    }
    out[0] = x; out[1] = y; out[2] = s; out[3] = t;
}
__attribute__((noinline, target("avx2"))) static void run_ymm(const double *a, long n, double *out)
{
    v4d acc = { 0.5, 0.25, 0.125, 1.0 }, m = { 1.0001, 0.9999, 1.0, 1.00005 }, acc2 = { 1, 2, 3, 4 };
    for (long i = 0; i + 4 <= n; i += 4) {
        v4d v = *(const v4d *)(a + i);
        acc = acc * m + v;
        acc2 = acc2 - v * m;
    }
    *(v4d *)out = acc;
    *(v4d *)(out + 4) = acc2;
}
static void handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    mprotect(g_map + PAGE, PAGE, PROT_READ | PROT_WRITE);
    g_faults++;
}
static void fill(double *a)
{
    for (int i = 0; i < PAGE / 8; i++) a[i] = 0.999 + (double)(i % 16) * 0.0001;
    for (int i = PAGE / 8; i < PAGE / 4; i++) a[i] = 0.0;
}
int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    g_map = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_map == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    double *a = (double *)g_map;
    double *ref = malloc(2 * PAGE);
    long n = PAGE / 8 + 12;
    double want[8] __attribute__((aligned(32))), got[8] __attribute__((aligned(32)));
    int bad = 0;
    fill(ref);
    for (int k = 0; k < 3; k++) {
        fill(a);
        if (k == 0) run_sse2(ref, n, want); else if (k == 1) run_avx(ref, n, want); else run_ymm(ref, n, want);
        mprotect(g_map + PAGE, PAGE, PROT_NONE);
        g_faults = 0;
        if (k == 0) run_sse2(a, n, got); else if (k == 1) run_avx(a, n, got); else run_ymm(a, n, got);
        int nv = k == 2 ? 8 : 4;
        if (g_faults != 1 || memcmp(want, got, (size_t)nv * sizeof(double)) != 0) {
            bad++;
            printf("variant %d faults=%d mismatch\n", k, g_faults);
            for (int i = 0; i < nv; i++) printf("  want %a got %a\n", want[i], got[i]);
        }
    }
    printf(bad ? "FAIL\n" : "OK\n");
    return bad ? 1 : 0;
}
