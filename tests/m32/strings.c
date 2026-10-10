#include <string.h>
#include <stdlib.h>
#include <stdio.h>
static void put_num(long long v) { char b[32]; int i = 31; b[i] = 0; int neg = v < 0; unsigned long long u = neg ? -v : v;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u); if (neg) b[--i] = '-'; puts(b + i); }
/* volatile pointers keep clang from folding the calls away */
static const char *volatile s_hello = "hello, i386", *volatile s_abc = "abc", *volatile s_abd = "abd";
static const char *volatile s_num = "-12345", *volatile s_big = "9000000000", *volatile s_dash = "find-the-dash";
static const char *volatile s_copied = "copied";
static volatile size_t n7 = 7, n3 = 3;
int main(void) {
    char buf[32];
    puts("strings");
    put_num((long long)strlen(s_hello));
    put_num(strcmp(s_abc, s_abd) < 0);
    memcpy(buf, s_copied, n7); puts(buf);
    memset(buf, 'x', n3); buf[3] = 0; puts(buf);
    put_num(atoi(s_num));
    char *end; put_num(strtoll(s_big, &end, 10)); put_num(*end == 0 && end == s_big + 10);
    puts(strchr(s_dash, '-'));      /* a pointer result inside the window stays a guest pointer */
    return 3;
}
