#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static volatile int handler_flags = -1;

static void expect(const char *what, int ok)
{
    if (!ok) {
        printf("BAD %s\n", what);
        fails++;
    }
}

static void on_usr2(int sig)
{
    stack_t cur;
    (void)sig;
    sigaltstack(NULL, &cur);
    handler_flags = cur.ss_flags;
}

static void *thread_main(void *arg)
{
    stack_t old;
    (void)arg;
    memset(&old, 0xaa, sizeof old);
    expect("thread query", sigaltstack(NULL, &old) == 0);
    expect("a new thread starts with the alternate stack disabled", (old.ss_flags & SS_DISABLE) != 0);
    return NULL;
}

int main(void)
{
    stack_t old, ss;
    memset(&old, 0xaa, sizeof old);
    expect("query", sigaltstack(NULL, &old) == 0);
    expect("the main thread starts with the alternate stack disabled", (old.ss_flags & SS_DISABLE) != 0);

    ss.ss_sp = malloc(SIGSTKSZ);
    ss.ss_size = SIGSTKSZ;
    ss.ss_flags = 0;
    expect("install", sigaltstack(&ss, NULL) == 0);
    memset(&old, 0, sizeof old);
    sigaltstack(NULL, &old);
    expect("the installed stack is reported", old.ss_sp == ss.ss_sp && old.ss_size == ss.ss_size && old.ss_flags == 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr2;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR2);
    for (int i = 0; i < 1000 && handler_flags == -1; i++)
        sched_yield();
    expect("a handler on the alternate stack sees SS_ONSTACK", handler_flags == SS_ONSTACK);

    ss.ss_flags = SS_DISABLE;
    expect("disable", sigaltstack(&ss, NULL) == 0);
    sigaltstack(NULL, &old);
    expect("the disabled stack is reported", (old.ss_flags & SS_DISABLE) != 0);

    pthread_t t;
    pthread_create(&t, NULL, thread_main, NULL);
    pthread_join(t, NULL);

    if (!fails)
        printf("OK\n");
    return fails != 0;
}
