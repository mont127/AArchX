#include <dispatch/dispatch.h>
#include <stdio.h>
#include <time.h>
static unsigned long long ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }
static int bucket(unsigned long long d, unsigned long long want) { return d + 2 >= want && d <= want * 4 + 50; }
static void fired(void *ctx) { *(unsigned long long *)ctx = ms(); }
int main(void) {
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    unsigned long long t0 = ms();
    long r = dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC));
    printf("semaphore_wait timeout r=%d ~60ms=%d\n", r != 0, bucket(ms() - t0, 60));
    dispatch_group_t g = dispatch_group_create(); dispatch_group_enter(g);
    t0 = ms(); r = dispatch_group_wait(g, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC));
    printf("group_wait timeout r=%d ~50ms=%d\n", r != 0, bucket(ms() - t0, 50));
    unsigned long long at = 0; t0 = ms();
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), q, &at, fired);
    __block unsigned long long bt = 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_MSEC), q, ^{ bt = ms(); dispatch_semaphore_signal(s); });
    dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
    printf("dispatch_after ~40ms=%d\n", bucket(bt - t0, 40));
    while (!at) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, 0); }
    printf("dispatch_after_f ~80ms=%d\n", bucket(at - t0, 80));
    __block int ticks = 0; __block unsigned long long last = 0;
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC), 20 * NSEC_PER_MSEC, 0);
    dispatch_source_set_event_handler(src, ^{ if (++ticks == 3) { last = ms(); dispatch_source_cancel(src); dispatch_semaphore_signal(s); } });
    t0 = ms(); dispatch_resume(src);
    dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
    printf("timer source 3 ticks ~70ms=%d\n", bucket(last - t0, 70));
    t0 = ms();
    dispatch_block_t blk = dispatch_block_create(0, ^{ struct timespec ts = { 0, 200000000 }; nanosleep(&ts, 0); });
    dispatch_async(q, blk);
    r = dispatch_block_wait(blk, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC));
    printf("block_wait timeout r=%d ~30ms=%d\n", r != 0, bucket(ms() - t0, 30));
    dispatch_time_t w = dispatch_walltime(NULL, 0);
    t0 = ms(); r = dispatch_semaphore_wait(s, dispatch_time(w, 25 * NSEC_PER_MSEC));
    printf("walltime deadline r=%d ~25ms=%d\n", r != 0, bucket(ms() - t0, 25));
    dispatch_time_t n = dispatch_time(DISPATCH_TIME_NOW, 0);
    t0 = ms(); r = dispatch_semaphore_wait(s, dispatch_time(n, 35 * NSEC_PER_MSEC));
    printf("derived uptime deadline r=%d ~35ms=%d\n", r != 0, bucket(ms() - t0, 35));
    return 0;
}
