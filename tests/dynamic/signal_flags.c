/*
 * A compare's flags that a block leaves as a record for the block after it survive a signal delivered between the
 * two.  The jne below goes to a block of its own whose jbe reads the compare's flags; a timer signal delivered at
 * that seam used to be given rflags as they were before the compare, which a failed close() leaves with CF and ZF
 * set, so sigreturn brought those back and the jbe was taken.  Steam's webhelper died of it in a heap's bounds check.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static volatile long delivered;

static void on_alarm(int sig)
{
    (void)sig;
    delivered++;
}

static long run(long n)
{
    long bad = 0;
    __asm__ volatile(
        "1:\n\t"
        "mov $1, %%rax\n\t"
        "xor %%esi, %%esi\n\t"
        "cmp %%rsi, %%rax\n\t"
        "jne 2f\n\t"
        "jmp 4f\n\t"
        "2:\n\t"
        "jbe 3f\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n\t"
        "jmp 4f\n\t"
        "3:\n\t"
        "inc %[bad]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n\t"
        "4:\n\t"
        : [n] "+r"(n), [bad] "+r"(bad)
        :
        : "rax", "rsi", "cc");
    return bad;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, 0);
    struct itimerval on = { { 0, 50 }, { 0, 50 } }, off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &on, 0);
    long bad = 0;
    for (int i = 0; i < 400 && delivered < 3000; i++) {
        close(-1);
        bad += run(1000000);
    }
    setitimer(ITIMER_REAL, &off, 0);
    if (bad || delivered < 100)
        printf("bad=%ld delivered=%ld\n", bad, delivered);
    else
        puts("OK");
    return 0;
}
