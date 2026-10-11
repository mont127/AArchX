/* An Intel Mac's clocks (src/m32_libsystem.c): mach_absolute_time in nanoseconds with a 1/1 timebase, and
 * CoreServices' UpTime on the same count, so code that assumed either measures a 50 ms sleep as 50 ms. */
#include <CoreServices/CoreServices.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time();
    AbsoluteTime u0 = UpTime();
    usleep(50000);
    uint64_t ms = (mach_absolute_time() - t0) / 1000000;
    Nanoseconds n = AbsoluteToNanoseconds(UpTime()), n0 = AbsoluteToNanoseconds(u0);
    uint64_t ums = ((((uint64_t)n.hi << 32) | n.lo) - (((uint64_t)n0.hi << 32) | n0.lo)) / 1000000;
    AbsoluteTime d = { 1500, 0 };   /* 1.5 microseconds: rounds to -1 */
    printf("timebase %u/%u mach %s uptime %s duration %d\n", tb.numer, tb.denom, ms >= 45 && ms < 500 ? "ok" : "bad",
           ums >= 45 && ums < 500 ? "ok" : "bad", (int)AbsoluteToDuration(d));
    return 0;
}
