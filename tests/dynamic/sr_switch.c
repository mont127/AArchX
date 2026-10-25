/*
 * The single runner (OCERZ_SINGLE_RUNNER=1, src/jit.c, "single runner") keeps a
 * thread's code plain while it is the only one running guest code and has
 * every thread run ordered code while two do.  The main thread here publishes
 * data and then a flag, over and over, in one tight loop; x86 keeps the two
 * stores in order, so a reader that loads the flag and then the data never
 * sees a flag newer than the data.  The reader checks a burst of such pairs and
 * sleeps in nanosleep, which leaves the main thread alone in its loop, plain,
 * and wakes into it again, which must take the main thread out of its plain
 * loop before the reader loads anything.  The loads and stores are written in
 * assembly so the compiler keeps their order and form.  The litmus trips in
 * two runs of three with both threads plain, not always, so
 * run_dynamic_tests.sh also runs it under OCERZ_SR_VERIFY, which samples where
 * the threads are (about 220 plain blocks a run found then), and checks with
 * OCERZ_SR_LOG that the process switched, sampled, found no thread in a plain
 * block while two ran, and never went ordered for good.  With "fork" the
 * program forks first, which shares nothing with its child and must leave the
 * process able to go plain; with "shared" it maps writable shared memory
 * first, which must make it ordered for good.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROUNDS 60000000L

int64_t g_data, g_flag;
static volatile int g_stop;
static long g_bad, g_checks, g_wakes;

static void *reader(void *a)
{
    (void)a;
    struct timespec ts = { 0, 20000 };
    while (!g_stop) {
        for (int i = 0; i < 40000; i++) {
            int64_t f, d;
            __asm__ volatile("movq _g_flag(%%rip), %0\n\tmovq _g_data(%%rip), %1" : "=r"(f), "=r"(d) :: "memory");
            if (d < f)
                g_bad++;
        }
        g_checks += 40000;
        nanosleep(&ts, NULL);
        g_wakes++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fork")) {
        pid_t pid = fork();
        if (pid == 0)
            _exit(0);
        if (pid < 0 || waitpid(pid, NULL, 0) != pid) { printf("fork\n"); return 1; }
    }
    if (argc > 1 && !strcmp(argv[1], "shared")) {
        volatile int *m = (volatile int *)mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { printf("mmap\n"); return 1; }
        m[0] = 1;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) != 0) { printf("pthread_create\n"); return 1; }
    for (int64_t r = 1; r <= ROUNDS; r++)
        __asm__ volatile("movq %0, _g_data(%%rip)\n\tmovq %0, _g_flag(%%rip)" :: "r"(r) : "memory");
    g_stop = 1;
    pthread_join(t, NULL);
    if (g_bad || g_wakes < 20) {
        printf("bad=%ld checks=%ld wakes=%ld\n", g_bad, g_checks, g_wakes);
        return 1;
    }
    printf("OK\n");
    return 0;
}
