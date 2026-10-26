/*
 * Fork children that keep the parent's translations (src/vm.c, ocerz_vm_atfork_child): the parent runs work(), starts
 * a thread that keeps running translated code, as Ruby's timer thread does, and forks five children, each of which runs work() again, from the code it inherited, and child_only(), which the
 * parent never ran, so the child translates it into the arena's tail and chains it to the inherited blocks around
 * it.  A child that kept a translation it may not execute faults on its first instruction; one whose results differ
 * from the parent's exits 1.  Prints OK when every child agrees.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static long work(int seed)
{
    int v[1000];
    for (int i = 0; i < 1000; i++)
        v[i] = (i * 7919 + seed) % 10007;
    qsort(v, 1000, sizeof v[0], cmp);
    char buf[64];
    long s = 0;
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof buf, "%d-%d", v[i], i);
        s += (long)strlen(buf) + v[i];
    }
    return s;
}

__attribute__((noinline)) static long child_only(long x)
{
    long r = 0;
    for (long i = 1; i <= x % 97 + 50; i++)
        r += (x ^ i) % 13;
    return r;
}

__attribute__((noinline)) static long child_only_check(long x)
{
    long r = 0, n = x % 97 + 50;
    for (long i = n; i >= 1; i--)
        r += (i ^ x) % 13;
    return r;
}

static volatile int g_stop;
static volatile long g_spin;

static void *spinner(void *arg)
{
    (void)arg;
    while (!g_stop)
        g_spin += work((int)(g_spin & 7)) & 1;
    return NULL;
}

int main(void)
{
    long want[5];
    pthread_t t;
    for (int c = 0; c < 5; c++)
        want[c] = work(c);
    if (pthread_create(&t, NULL, spinner, NULL) != 0) { printf("thread failed\n"); return 1; }
    for (int c = 0; c < 5; c++) {
        pid_t p = fork();
        if (p < 0) { printf("fork failed\n"); return 1; }
        if (p == 0) {
            long w = work(c);
            _exit(w == want[c] && child_only(w) == child_only_check(w) ? 0 : 1);
        }
        int st = 0;
        if (waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("child %d: %s %d\n", c, WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
            return 1;
        }
    }
    g_stop = 1;
    pthread_join(t, NULL);
    printf("OK\n");
    return 0;
}
