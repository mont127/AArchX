#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_int spin_ready;
static atomic_int helper_done;
static volatile unsigned long spins;
static mach_port_t main_port;
static int main_result = 1;

static void *spinner(void *arg)
{
    (void)arg;
    atomic_store(&spin_ready, 1);
    for (;;)
        spins++;
    return NULL;
}

static int check(const char *who, mach_port_t th)
{
    kern_return_t kr = thread_suspend(th);
    if (kr != KERN_SUCCESS) {
        printf("BAD %s thread_suspend %d\n", who, kr);
        return 1;
    }
    x86_thread_state64_t s64 = { 0 };
    mach_msg_type_number_t c64 = x86_THREAD_STATE64_COUNT;
    kern_return_t k64 = thread_get_state(th, x86_THREAD_STATE64, (thread_state_t)&s64, &c64);
    x86_thread_state_t st = { 0 };
    mach_msg_type_number_t cst = x86_THREAD_STATE_COUNT;
    kern_return_t kst = thread_get_state(th, x86_THREAD_STATE, (thread_state_t)&st, &cst);
    kern_return_t kres = thread_resume(th);
    if (k64 != KERN_SUCCESS || c64 != x86_THREAD_STATE64_COUNT || s64.__rsp == 0) {
        printf("BAD %s x86_THREAD_STATE64 kr=%d count=%u rsp=%#llx\n", who, k64, c64,
               (unsigned long long)s64.__rsp);
        return 1;
    }
    if (kst != KERN_SUCCESS || cst != x86_THREAD_STATE_COUNT || st.tsh.flavor != x86_THREAD_STATE64) {
        printf("BAD %s x86_THREAD_STATE kr=%d count=%u flavor=%d\n", who, kst, cst, st.tsh.flavor);
        return 1;
    }
    if (kres != KERN_SUCCESS) {
        printf("BAD %s thread_resume %d\n", who, kres);
        return 1;
    }
    return 0;
}

static void *suspend_main(void *arg)
{
    (void)arg;
    main_result = check("main thread", main_port);
    atomic_store(&helper_done, 1);
    return NULL;
}

int main(void)
{
    int fails = 0;
    pthread_t t;
    pthread_create(&t, NULL, spinner, NULL);
    while (!atomic_load(&spin_ready))
        usleep(1000);
    fails += check("pthread", pthread_mach_thread_np(t));

    dispatch_semaphore_t got = dispatch_semaphore_create(0);
    dispatch_semaphore_t release = dispatch_semaphore_create(0);
    static mach_port_t wq;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        wq = pthread_mach_thread_np(pthread_self());
        dispatch_semaphore_signal(got);
        dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
    });
    dispatch_semaphore_wait(got, DISPATCH_TIME_FOREVER);
    fails += check("workqueue", wq);
    dispatch_semaphore_signal(release);

    main_port = pthread_mach_thread_np(pthread_self());
    pthread_t h;
    pthread_create(&h, NULL, suspend_main, NULL);
    unsigned long busy = 0;
    while (!atomic_load(&helper_done))
        busy++;
    (void)busy;
    pthread_join(h, NULL);
    fails += main_result;
    if (!fails)
        printf("OK\n");
    return fails != 0;
}
