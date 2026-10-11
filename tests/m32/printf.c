#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static int vsum(char *out, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(out, n, fmt, ap); va_end(ap); return r; }
int main(void) {
    printf("%d %u %x %X %o %c %s|%5s|%-5s|\n", -42, 42u, 255, 255, 8, 'Z', "str", "ab", "cd");
    printf("%lld %llu %lx %zu %p\n", -9000000000ll, 18000000000ull, 0xdeadbeefUL, (size_t)77, (void *)0x1234);
    printf("%f %.3f %e %g %8.2f\n", 1.5, 3.14159, 12345.678, 0.0001, -2.5);
    char buf[64]; int r = snprintf(buf, sizeof buf, "%s=%d", "k", 9); printf("[%s] %d\n", buf, r);
    r = vsum(buf, 8, "%s%s", "truncate", "d"); printf("[%s] %d\n", buf, r);
    sprintf(buf, "%05.1f|%+d", 3.25, 4); puts(buf);
    fprintf(stdout, "to stdout %d\n", 1);
    int a; float f; char w[16]; long l; int got = sscanf("17 2.5 word -5", "%d %f %15s %ld", &a, &f, w, &l);
    printf("sscanf %d %d %.1f %s %ld\n", got, a, f, w, l);
    printf("%s\n", (char *)0);
    fflush(stdout);
    return 0;
}
