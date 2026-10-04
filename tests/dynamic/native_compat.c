#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <err.h>
#include <errno.h>
#include <glob.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xlocale.h>

static void call_verr(int how, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (how == 0)
        verr(3, fmt, ap);
    else if (how == 1)
        verrx(4, fmt, ap);
    else
        verrc(5, ENOENT, fmt, ap);
}

static void call_vwarnc(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnc(EPERM, fmt, ap);
    va_end(ap);
}

static void errs(void)
{
    for (int k = 0; k < 6; k++) {
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) {
            errno = EACCES;
            switch (k) {
            case 0: err(1, "err %d %s", 7, "x");
            case 1: errx(2, "errx %.1f", 2.5);
            case 2: errc(6, EEXIST, "errc %c", 'q');
            default: call_verr(k - 3, "v%d %s", k, "y");
            }
        }
        int st = 0;
        waitpid(p, &st, 0);
        printf("err child %d exit %d\n", k, WEXITSTATUS(st));
    }
    warnc(ENOMEM, "warnc %lu", 42ul);
    call_vwarnc("vwarnc %s", "z");
}

static void long_doubles(void)
{
    volatile long double a = 2.5L, b = 3.0L;
    char *end = NULL;
    long double s = strtold("12.375e2xyz", &end);
    locale_t c = newlocale(LC_ALL_MASK, "C", NULL);
    long double sl = strtold_l("-0.125", NULL, c);
    int e = 0;
    long double fr = frexpl(a * 8, &e);
    long double acc = 0;
    for (int k = 1; k <= 5; k++)
        acc += powl(a, (long double)k) / k;
    printf("long double %.10g %s %.10g %.10g %.10g %.10g %.10g %.10g %.10g %d %lld %.10g\n", (double)s, end,
           (double)sl, (double)powl(a, b), (double)sinl(a), (double)log10l(a * 40), (double)sqrtl(b),
           (double)ldexpl(a, 4), (double)fr, e, llroundl(a), (double)acc);
    freelocale(c);
}

static int glob_errors;

static int on_glob_error(const char *path, int error)
{
    (void)path;
    (void)error;
    glob_errors++;
    return 0;
}

static void globs(const char *dir)
{
    char pat[1024];
    snprintf(pat, sizeof pat, "%s/*.txt", dir);
    glob_t g;
    int r = glob(pat, 0, on_glob_error, &g);
    printf("glob %d %zu", r, g.gl_pathc);
    for (size_t k = 0; k < g.gl_pathc; k++)
        printf(" %s", strrchr(g.gl_pathv[k], '/') + 1);
    printf("\n");
    globfree(&g);
    snprintf(pat, sizeof pat, "%s/nosuch/*", dir);
    r = glob(pat, GLOB_NOCHECK, on_glob_error, &g);
    printf("glob nocheck %d %zu\n", r, g.gl_pathc);
    globfree(&g);
}

static void calendars(void)
{
    CFCalendarRef cal = CFCalendarCreateWithIdentifier(NULL, kCFGregorianCalendar);
    CFTimeZoneRef utc = CFTimeZoneCreateWithTimeIntervalFromGMT(NULL, 0);
    CFCalendarSetTimeZone(cal, utc);
    CFAbsoluteTime at = 0;
    Boolean ok1 = CFCalendarComposeAbsoluteTime(cal, &at, "yMdHms", 2026, 10, 4, 21, 15, 30);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    Boolean ok2 = CFCalendarDecomposeAbsoluteTime(cal, at, "yMdHms", &y, &mo, &d, &h, &mi, &s);
    CFAbsoluteTime later = at;
    Boolean ok3 = CFCalendarAddComponents(cal, &later, 0, "dH", 3, 5);
    int dd = 0, dh = 0;
    Boolean ok4 = CFCalendarGetComponentDifference(cal, at, later, 0, "dH", &dd, &dh);
    printf("calendar %d%d%d%d %.0f %04d-%02d-%02d %02d:%02d:%02d %dd%dh\n", ok1, ok2, ok3, ok4, at, y, mo, d, h, mi,
           s, dd, dh);
    CFRelease(utc);
    CFRelease(cal);
}

static void put_time(const char *tag, CMTime t)
{
    printf(" %s=%lld/%d/%u", tag, t.value, t.timescale, t.flags);
}

static void times(void)
{
    CMTime a = CMTimeMake(3000, 600), b = CMTimeMakeWithSeconds(2.5, 1000);
    CMTimeRange r = CMTimeRangeMake(a, b);
    printf("cmtime");
    put_time("sum", CMTimeAdd(a, b));
    put_time("diff", CMTimeSubtract(a, b));
    put_time("conv", CMTimeConvertScale(b, 600, kCMTimeRoundingMethod_Default));
    put_time("mul", CMTimeMultiplyByRatio(a, 3, 4));
    put_time("end", CMTimeRangeGetEnd(r));
    put_time("clamp", CMTimeClampToRange(CMTimeMake(100, 1), r));
    printf(" cmp=%d,%d secs=%.3f contains=%d\n", CMTimeCompare(a, b), CMTimeCompare(b, a), CMTimeGetSeconds(a),
           CMTimeRangeContainsTime(r, CMTimeMake(6, 1)));
}

static CFStringRef cf_format(CFStringRef fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    CFStringRef s = CFStringCreateWithFormatAndArguments(NULL, NULL, fmt, ap);
    va_end(ap);
    return s;
}

static void cf_append(CFMutableStringRef m, CFStringRef fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    CFStringAppendFormatAndArguments(m, NULL, fmt, ap);
    va_end(ap);
}

static void formats(void)
{
    CFStringRef s = cf_format(CFSTR("%d-%@-%.2f"), 5, CFSTR("cf"), 1.25);
    CFMutableStringRef m = CFStringCreateMutable(NULL, 0);
    cf_append(m, CFSTR("[%s|%x]"), "app", 255);
    char a[64], b[64];
    CFStringGetCString(s, a, sizeof a, kCFStringEncodingUTF8);
    CFStringGetCString(m, b, sizeof b, kCFStringEncodingUTF8);
    printf("cf va_list %s %s\n", a, b);
    CFRelease(s);
    CFRelease(m);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    errs();
    long_doubles();
    globs(argc > 1 ? argv[1] : ".");
    calendars();
    times();
    formats();
    return 0;
}
