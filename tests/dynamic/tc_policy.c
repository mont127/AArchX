/*
 * A real-time thread policy in mach_absolute_time units, computed the way
 * CoreAudio's I/O thread computes it: from the timebase the guest is shown.
 * That timebase is 1/1 under ocerz and Rosetta, so the values are
 * nanoseconds, and the kernel must not take them for host ticks: 2 ms of
 * computation read as 2,000,000 ticks is 83 ms and is refused, which left every
 * audio output unit unable to start.  The set must succeed and a get must hand
 * back the same period and constraint in the guest's units.  No audio device
 * is touched.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <stdio.h>

static uint32_t abs_from_ns(double ns, const mach_timebase_info_data_t *tb)
{
    return (uint32_t)(ns * tb->denom / tb->numer);
}

int main(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    thread_time_constraint_policy_data_t p = {
        abs_from_ns(10666667, &tb), abs_from_ns(2000000, &tb), abs_from_ns(10666667, &tb), 1
    };
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        printf("FAIL set kr=%d\n", kr);
        return 0;
    }
    thread_time_constraint_policy_data_t g = { 0 };
    mach_msg_type_number_t n = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def = FALSE;
    kr = thread_policy_get(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                           (thread_policy_t)&g, &n, &def);
    double dp = (double)g.period - p.period, dk = (double)g.constraint - p.constraint;
    if (kr != KERN_SUCCESS || def || dp * dp > (double)p.period * p.period * 1e-6 ||
        dk * dk > (double)p.constraint * p.constraint * 1e-6 || g.computation > g.constraint) {
        printf("FAIL get kr=%d default=%d period %u/%u computation %u constraint %u/%u\n",
               kr, def, g.period, p.period, g.computation, g.constraint, p.constraint);
        return 0;
    }
    printf("OK\n");
    return 0;
}
