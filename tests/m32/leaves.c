/* Libc functions an i386 program calls must answer what the host's do (the
 * interpreter's specials call those): overlapping copies both ways, memset/bzero, memcmp's byte difference, strlen,
 * wcscmp on characters with the high bit set, thread-specific data, the clock, tolower, the barrier, pthread_self and
 * the float classifiers (which once answered 0 for every value).  Each runs in a loop so its stub block is translated. */
#include <ctype.h>
#include <libkern/OSAtomic.h>
#include <math.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

static unsigned sum(const unsigned char *p, int n)
{
    unsigned s = 0;
    for (int i = 0; i < n; i++)
        s = s * 31 + p[i];
    return s;
}

/* sizes and strings the compiler cannot see, so these stay real calls through the import stubs (with constants
 * clang expands memcpy/memset/strlen inline or folds them away) */
static volatile int n40 = 40, n30 = 30, n13 = 13, n11 = 11, n9 = 9, n20 = 20, n4 = 4, n0 = 0;
static char hello[] = "hello, leaves", empty[] = "";
static wchar_t wa[] = { 'a', (wchar_t)0x80000001, 'c', 0 }, wb[] = { 'a', 'b', 'c', 0 };
static char abcd[] = "abcd", abcz[] = "abcz";
static volatile float fv[4] = { 1.5f, INFINITY, NAN, -INFINITY };
extern int __isinff(float), __isnanf(float);   /* what isinf/isnan of a float called in the 10.x SDK */

int main(void)
{
    unsigned char buf[96];
    unsigned acc = 0;
    for (int r = 0; r < 50; r++) {
        for (int i = 0; i < 96; i++)
            buf[i] = (unsigned char)(i * 7 + r);
        (memmove)(buf + 3, buf, n40);        /* overlapping, backward */
        (memmove)(buf + 50, buf + 53, n30);  /* overlapping, forward */
        (memcpy)(buf + 10, buf + 60, n13);
        (memset)(buf + 1, 0xa5 + r, n11);
        (bzero)(buf + 80, n9);
        acc = acc * 7 + sum(buf, 96);
        acc += (unsigned)memcmp(buf, buf + 1, n20) + (unsigned)memcmp(abcd, abcz, n4) * 3u + (unsigned)memcmp(buf, buf, n0);
        acc += (unsigned)strlen(hello) + (unsigned)strlen(empty);
        acc += (unsigned)wcscmp(wa, wb) + (unsigned)wcscmp(wb, wa) * 5u + (unsigned)wcscmp(wb, wb);
        acc += (unsigned)tolower('Q' + r % 5) + (unsigned)tolower('!');
        OSMemoryBarrier();
    }
    pthread_key_t key;
    pthread_key_create(&key, NULL);
    for (int r = 0; r < 50; r++) {
        pthread_setspecific(key, (void *)(uintptr_t)(0x1000 + r));
        acc += (unsigned)(uintptr_t)pthread_getspecific(key);
    }
    pthread_t self = pthread_self();
    for (int r = 0; r < 50; r++) {
        acc += (unsigned)pthread_equal(self, pthread_self());
        for (int i = 0; i < 4; i++)
            acc = acc * 3 + (unsigned)__isinff(fv[i]) * 2u + (unsigned)__isnanf(fv[i]);
    }
    uint64_t t0 = mach_absolute_time(), t1 = t0;
    for (int r = 0; r < 50; r++)
        t1 = mach_absolute_time();
    printf("leaves %08x clock %s\n", acc, t1 >= t0 ? "monotonic" : "BACKWARDS");
    return 0;
}
