/* thread_suspend / thread_get_state / thread_resume on guest threads, the
 * way JavaScriptCore's garbage collector scans its mutators.  A thread
 * spinning in translated code must really stop (its counter freezes), its
 * x86 registers must come back with rsp inside its own stack, the suspender
 * must be able to run code it never ran before (which needs the JIT) while
 * the target is stopped, and a thread parked in a blocking read must be
 * suspendable too.  Handed to the host kernel, thread_suspend froze Safari's
 * main thread inside the translator with the JIT lock held, and the x86
 * thread_get_state failed outright. */
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile unsigned long counter;
static volatile int stop;
static char *volatile lo[2], *volatile hi[2];
static int pfd[2];

static void note_stack(int i)
{
    pthread_t self = pthread_self();
    char *top = (char *)pthread_get_stackaddr_np(self);
    lo[i] = top - pthread_get_stacksize_np(self);
    hi[i] = top;
}

/* No libc calls in here: a thread suspended inside snprintf can hold the
 * dtoa or malloc lock, and the suspender's own snprintf then waits for it
 * forever -- natively as much as under ocerz, which is why a collector never
 * allocates while its mutators are stopped. */
static void *spin(void *a)
{
    (void)a;
    note_stack(0);
    while (!stop)
        counter++;
    return NULL;
}

static void *reader(void *a)
{
    (void)a;
    note_stack(1);
    char c;
    return (void *)(long)read(pfd[0], &c, 1);
}

static int state_ok(mach_port_t tp, int i, const char *who)
{
    x86_thread_state64_t s;
    mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(tp, x86_THREAD_STATE64, (thread_state_t)&s, &n);
    if (kr != KERN_SUCCESS || n != x86_THREAD_STATE64_COUNT) {
        printf("BAD %s thread_get_state kr=%d n=%u\n", who, kr, n);
        return 0;
    }
    if ((char *)s.__rsp < lo[i] || (char *)s.__rsp >= hi[i]) {
        printf("BAD %s rsp %#llx outside its stack [%p,%p)\n", who,
               (unsigned long long)s.__rsp, (void *)lo[i], (void *)hi[i]);
        return 0;
    }
    return 1;
}

int main(void)
{
    pthread_t t, r;
    if (pipe(pfd) || pthread_create(&t, NULL, spin, NULL) || pthread_create(&r, NULL, reader, NULL))
        return 1;
    while (!counter || !hi[1])
        usleep(1000);
    usleep(20000);                          /* let the reader reach its read() */
    mach_port_t tp = pthread_mach_thread_np(t), rp = pthread_mach_thread_np(r);

    for (int i = 0; i < 100; i++) {
        if (thread_suspend(tp) != KERN_SUCCESS) {
            printf("BAD suspend\n");
            return 2;
        }
        if (!state_ok(tp, 0, "spinner"))
            return 3;
        unsigned long c0 = counter;
        char buf[64];
        snprintf(buf, sizeof buf, "%d %.3f %s", i, i * 0.25, "fresh code");
        usleep(1000);
        if (counter != c0) {
            printf("BAD spinner ran while suspended (%lu -> %lu)\n", c0, counter);
            return 4;
        }
        if (thread_resume(tp) != KERN_SUCCESS) {
            printf("BAD resume\n");
            return 5;
        }
        while (counter == c0)
            ;
    }
    if (thread_suspend(rp) != KERN_SUCCESS || !state_ok(rp, 1, "reader") ||
        thread_resume(rp) != KERN_SUCCESS)
        return 6;
    if (thread_resume(tp) != KERN_FAILURE) {
        printf("BAD resume of a running thread succeeded\n");
        return 7;
    }
    stop = 1;
    if (write(pfd[1], "x", 1) != 1)
        return 8;
    pthread_join(t, NULL);
    void *rv;
    pthread_join(r, &rv);
    if ((long)rv != 1) {
        printf("BAD reader read %ld\n", (long)rv);
        return 9;
    }
    printf("OK\n");
    return 0;
}
