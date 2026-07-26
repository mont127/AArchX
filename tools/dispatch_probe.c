/* Exercises one libdispatch primitive per run, so a hang pins the broken one. Build x86_64 and run under ocerz; see notes/wine_bringup.md UPDATE #30. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

static int wait_sem(dispatch_semaphore_t s, int ms)
{
    return dispatch_semaphore_wait(s,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC)) == 0;
}

int main(int argc, char **argv)
{
    const char *w = argc > 1 ? argv[1] : "async";
    dispatch_queue_t gq = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    int ok = 0;

    if (!strcmp(w, "async")) {
        dispatch_async(gq, ^{ dispatch_semaphore_signal(s); });
        ok = wait_sem(s, 4000);
    } else if (!strcmp(w, "after")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), gq,
                       ^{ dispatch_semaphore_signal(s); });
        ok = wait_sem(s, 4000);
    } else if (!strcmp(w, "timer") || !strcmp(w, "interval")) {
        uint64_t iv = strcmp(w, "interval") ? DISPATCH_TIME_FOREVER : 200 * NSEC_PER_MSEC;
        dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gq);
        dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), iv, 0);
        dispatch_source_set_event_handler(t, ^{ dispatch_semaphore_signal(s); });
        dispatch_resume(t);
        ok = wait_sem(s, 4000);
    } else if (!strcmp(w, "read")) {
        int fds[2];
        if (pipe(fds) != 0)
            return 2;
        int rfd = fds[0];
        dispatch_source_t r = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, rfd, 0, gq);
        dispatch_source_set_event_handler(r, ^{
            char b[8];
            ssize_t n = read(rfd, b, sizeof b);
            (void)n;
            dispatch_semaphore_signal(s);
        });
        dispatch_resume(r);
        if (write(fds[1], "x", 1) != 1)
            return 2;
        ok = wait_sem(s, 4000);
    } else if (!strcmp(w, "serial")) {
        dispatch_queue_t sq = dispatch_queue_create("ocerz.probe.serial", NULL);
        dispatch_async(sq, ^{ dispatch_semaphore_signal(s); });
        ok = wait_sem(s, 4000);
    } else {
        fprintf(stderr, "usage: %s async|after|timer|interval|read|serial\n", argv[0]);
        return 64;
    }

    printf("%s: %s\n", w, ok ? "ok" : "TIMED OUT");
    return ok ? 0 : 1;
}
