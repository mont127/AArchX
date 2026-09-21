/*
 * x86 NaN results in every place a guest thread's floating-point control state
 * comes from.
 *
 * On a processor with FPCR.AH the translator emits SSE arithmetic bare and
 * relies on that bit for the results x86 defines: the negative default NaN for
 * an invalid operation, and the first operand, quieted, when both are NaNs.
 * The bit is per thread and nothing but ocerz sets it, so a thread that starts
 * running guest code by a path that forgot to would compute arm64's answers
 * instead, a positive default NaN and the signalling operand, and nothing else
 * about the program would look wrong.  The main thread of a dynamically linked
 * program was exactly that: it enters through the guest-call loop, which never
 * applied the guest's control state, where a static program's main thread and
 * every other thread enter through the run loop, which did.
 *
 * So the same six values are computed on the main thread, on a pthread, inside
 * a signal handler and after it returns, in a block libdispatch runs on the
 * calling thread and in one it runs on a worker, after MXCSR was rewritten, in
 * a fork child and in its parent afterwards, scalar and packed, and compared
 * with the bits an x86 processor produces.  The interpreter computes them in C
 * and the JIT without FPCR.AH checks for them, so both of those runs pass for
 * other reasons; what this pins is that all three agree everywhere.
 */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xmmintrin.h>

static volatile float g_zero = 0.0f, g_inf = __builtin_inff();
static volatile int g_bad;

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float from_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

__attribute__((noinline)) static void probe(const char *where)
{
    static const uint32_t want[6] = { 0xffc00000u, 0xffc00000u, 0x7fc12345u, 0x7fc54321u, 0xffc00000u, 0x7fc12345u };
    volatile float q = from_bits(0x7fc12345u), s = from_bits(0x7f854321u);
    float packed[4];
    _mm_storeu_ps(packed, _mm_add_ps(_mm_set1_ps(q), _mm_set1_ps(s)));
    uint32_t got[6] = { bits(g_zero / g_zero), bits(g_inf - g_inf), bits(q + s), bits(s * q),
                        bits(__builtin_sqrtf(-1.0f - g_zero)), bits(packed[2]) };
    for (int i = 0; i < 6; i++)
        if (got[i] != want[i]) {
            char line[96];
            int n = snprintf(line, sizeof line, "BAD %s value %d is %08x, x86 gives %08x\n", where, i, got[i], want[i]);
            write(1, line, (size_t)n);
            g_bad = 1;
        }
}

static void *on_thread(void *arg)
{
    probe((const char *)arg);
    return NULL;
}

static void on_usr1(int sig)
{
    (void)sig;
    probe("signal handler");
}

int main(void)
{
    probe("main thread");
    pthread_t t;
    pthread_create(&t, NULL, on_thread, "pthread");
    pthread_join(t, NULL);
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    probe("after the handler");
    dispatch_sync(dispatch_get_global_queue(0, 0), ^{ probe("dispatch_sync"); });
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(0, 0), ^{ probe("dispatch worker"); dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x2000u);
    probe("rounding down");
    _mm_setcsr(_mm_getcsr() & ~0x6000u);
    pid_t pid = fork();
    if (pid == 0) {
        probe("fork child");
        _exit(g_bad);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        g_bad = 1;
    probe("parent after fork");
    if (!g_bad)
        printf("OK\n");
    return g_bad;
}
