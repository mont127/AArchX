/* A thread spinning in a translated loop must still take its signals.
 *
 * Each loop below runs until a signal handler ends it, and the handler
 * ends it only after 20 signals have reached it, so every one of those
 * signals is delivered while the loop is running.  A translation that
 * delivers signals only when control leaves the loop never ends: the run
 * is cut off by the test's time limit.  The handler ends a loop the way a
 * debugger or Wine's SetThreadContext would, by rewriting the interrupted
 * context: the count register for the counted loops, a flag in memory for
 * the one that waits on memory.  The floating-point loop then checks that
 * the double it carries round the loop and the counter it keeps beside it
 * still agree, which they cannot if any interruption lost or replayed an
 * iteration.
 *
 * The loops first run with no other thread, when the program's memory is
 * plain, and again after a second thread has started, when it is ordered.
 * In between, the second thread spins in a loop of its own while the main
 * thread signals it 20 times with pthread_kill, waiting for each signal to
 * be handled: what Wine's SuspendThread does to a busy thread.  Progress
 * goes to stderr, so a run that hangs shows which loop it hung in. */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#define TICKS 20

enum { END_RCX_ONE, END_RCX_PAST_RAX, END_FLAG };

static volatile int g_ticks;
static volatile int g_end_how;
static volatile int g_flag;

static void on_tick(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ctx;
    if (++g_ticks != TICKS)
        return;
    switch (g_end_how) {
    case END_RCX_ONE:
        uc->uc_mcontext->__ss.__rcx = 1;
        break;
    case END_RCX_PAST_RAX:
        uc->uc_mcontext->__ss.__rcx = uc->uc_mcontext->__ss.__rax + 1;
        break;
    case END_FLAG:
        g_flag = 1;
        break;
    }
}

static void timer_on(int on)
{
    struct itimerval it;
    memset(&it, 0, sizeof it);
    if (on) {
        it.it_interval.tv_usec = 5000;
        it.it_value.tv_usec = 5000;
    }
    setitimer(ITIMER_REAL, &it, NULL);
}

static uint64_t loop_dec(void)
{
    uint64_t rcx = ~0ull;
    __asm__ volatile("1: dec %%rcx\n\tjnz 1b" : "+c"(rcx) :: "cc");
    return rcx;
}

static uint64_t loop_lea_dec(void)
{
    uint64_t rcx = ~0ull, rax = 0;
    __asm__ volatile("1: lea 1(%%rax), %%rax\n\tdec %%rcx\n\tjnz 1b" : "+c"(rcx), "+a"(rax) :: "cc");
    return rax;
}

static uint64_t loop_add_cmp(void)
{
    uint64_t rcx = ~0ull, rax = 0;
    __asm__ volatile("1: add $1, %%rax\n\tcmp %%rcx, %%rax\n\tjne 1b" : "+c"(rcx), "+a"(rax) :: "cc");
    return rax;
}

static uint64_t loop_wait_flag(void)
{
    uint64_t n = 0;
    __asm__ volatile("1: add $1, %%rax\n\tcmpl $0, (%1)\n\tje 1b" : "+a"(n) : "r"(&g_flag) : "cc", "memory");
    return n;
}

static int loop_double(void)
{
    uint64_t rcx = ~0ull, rax = 0;
    double sum = 0.0, one = 1.0;
    __asm__ volatile("1: addsd %3, %2\n\tadd $1, %%rax\n\tdec %%rcx\n\tjnz 1b"
                     : "+c"(rcx), "+a"(rax), "+x"(sum) : "x"(one) : "cc");
    if (sum != (double)rax) {
        printf("BAD double loop: sum %.17g after %llu iterations\n", sum, (unsigned long long)rax);
        return 1;
    }
    return 0;
}

static uint64_t loop_branchy(void)
{
    uint64_t rcx = ~0ull, rax = 0, rdx = 0;
    __asm__ volatile("1: test $1, %%al\n\tjz 2f\n\tadd $3, %%rdx\n"
                     "2: add $1, %%rax\n\tdec %%rcx\n\tjnz 1b"
                     : "+c"(rcx), "+a"(rax), "+d"(rdx) :: "cc");
    return rdx;
}

static uint64_t loop_two_blocks(void)
{
    uint64_t rcx = ~0ull, rax = 0;
    __asm__ volatile("jmp 2f\n"
                     "1: add $1, %%rax\n\tjmp 3f\n"
                     ".p2align 6\n"
                     "2: nop\n"
                     "3: dec %%rcx\n\tjnz 1b"
                     : "+c"(rcx), "+a"(rax) :: "cc");
    return rax;
}

static int run_loops(const char *phase)
{
    int fails = 0;
    static const char *names[] = { "dec", "lea-dec", "add-cmp", "wait-flag", "double", "branchy", "two-blocks" };
    for (int k = 0; k < 7; k++) {
        fprintf(stderr, "loop_signal: %s %s\n", phase, names[k]);
        g_ticks = 0;
        g_flag = 0;
        g_end_how = k == 2 ? END_RCX_PAST_RAX : k == 3 ? END_FLAG : END_RCX_ONE;
        timer_on(1);
        switch (k) {
        case 0: loop_dec(); break;
        case 1: loop_lea_dec(); break;
        case 2: loop_add_cmp(); break;
        case 3: loop_wait_flag(); break;
        case 4: fails += loop_double(); break;
        case 5: loop_branchy(); break;
        case 6: loop_two_blocks(); break;
        }
        timer_on(0);
        if (g_ticks < TICKS) {
            printf("BAD %s %s: loop ended after %d signals\n", phase, names[k], g_ticks);
            fails++;
        }
    }
    return fails;
}

static volatile int g_acks;
static volatile int g_worker_ready;

static void on_usr1(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ctx;
    if (++g_acks == TICKS)
        uc->uc_mcontext->__ss.__rcx = 1;
}

static void *worker(void *arg)
{
    (void)arg;
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
    g_worker_ready = 1;
    loop_dec();
    return NULL;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_tick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_sigaction = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    int fails = run_loops("plain");

    fprintf(stderr, "loop_signal: thread\n");
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    while (!g_worker_ready)
        usleep(1000);
    usleep(20000);
    for (int i = 0; i < TICKS; i++) {
        int want = g_acks + 1;
        pthread_kill(t, SIGUSR1);
        double t0 = now();
        while (g_acks < want && now() - t0 < 2.0)
            usleep(200);
        if (g_acks < want) {
            printf("BAD thread: signal %d not handled within 2 s\n", i + 1);
            fflush(stdout);
            _exit(1);
        }
    }
    pthread_join(t, NULL);

    fails += run_loops("ordered");
    if (!fails)
        printf("OK\n");
    return fails != 0;
}
