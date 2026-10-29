/* Text to numbers and back: the strto* family with their errno and end pointers, the ato* calls, sscanf, and the
   string functions whose answers are more than a copy (strerror, strsignal, strcoll, strxfrm, fnmatch, regex). */
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <stdio.h>
#include <wchar.h>

#include "probe.h"

static const char *const nums[] = { "0", "42", "-42", "  +17xyz", "0x1F", "0X1fp3", "077", "1e10", "-1e-5", "1.5e308",
                                     "1e309", "-1e309", "4.9e-324", "1e-330", "inf", "-Infinity", "nan", "nan(0x123)",
                                     "0x1.8p1", ".5", "5.", "", "abc", "9223372036854775807", "9223372036854775808",
                                     "-9223372036854775809", "18446744073709551615", "18446744073709551616",
                                     "2147483648", "0b101", "1,5", "  \t\n12" };

int main(void)
{
    for (size_t i = 0; i < sizeof nums / sizeof *nums; i++) {
        char name[64];
        snprintf(name, sizeof name, "strto %zu %s", i, nums[i]);
        CASE(name) {
            const char *s = nums[i];
            char *end;
            static const int bases[] = { 0, 10, 16, 8, 2, 36 };
            for (size_t b = 0; b < sizeof bases / sizeof *bases; b++) {
                char k[32];
                errno = 0;
                long long v = strtoll(s, &end, bases[b]);
                snprintf(k, sizeof k, "strtoll %d", bases[b]);
                out_key(k); out_d(v); out_s(" end "); out_d(end - s); out_s(" errno "); out_d(errno); out_end();
                errno = 0;
                unsigned long long u = strtoull(s, &end, bases[b]);
                snprintf(k, sizeof k, "strtoull %d", bases[b]);
                out_key(k); out_u(u); out_s(" end "); out_d(end - s); out_s(" errno "); out_d(errno); out_end();
            }
            errno = 0;
            long l = strtol(s, &end, 0);
            out_key("strtol"); out_d(l); out_s(" end "); out_d(end - s); out_s(" errno "); out_d(errno); out_end();
            errno = 0;
            unsigned long ul = strtoul(s, &end, 0);
            out_key("strtoul"); out_u(ul); out_s(" end "); out_d(end - s); out_s(" errno "); out_d(errno); out_end();
            errno = 0;
            double d = strtod(s, &end);
            out_key("strtod"); out_dbl(d); out_s(" end "); out_d(end - s); out_s(" errno "); out_d(errno); out_end();
            errno = 0;
            float f = strtof(s, &end);
            out_key("strtof"); out_flt(f); out_s(" end "); out_d(end - s); out_s(" errno "); out_d(errno); out_end();
            errno = 0;
            long double ld = strtold(s, &end);
            out_key("strtold"); out_ldbl(ld); out_s(" end "); out_d(end - s); out_s(" errno "); out_d(errno); out_end();
            out_key("atoi"); out_d(atoi(s)); out_end();
            out_key("atol"); out_d(atol(s)); out_end();
            out_key("atof"); out_dbl(atof(s)); out_end();
        }
    }
    CASE("sscanf") {
        int a = -1, b2 = -1, n = -1;
        double d = -1;
        char w[16] = "", c = 0;
        unsigned x = 0;
        long long ll = 0;
        int r = sscanf("  12 abc 3.5 ff 9876543210 z", "%d %15s %lf %x %lld %c%n", &a, w, &d, &x, &ll, &c, &n);
        out_key("mixed"); out_d(r); out_s(" "); out_d(a); out_s(" "); out_s(w); out_s(" "); out_dbl(d); out_s(" ");
        out_u(x); out_s(" "); out_d(ll); out_s(" "); out_d(c); out_s(" n "); out_d(n); out_end();
        r = sscanf("7,8", "%d,%d", &a, &b2);
        out_key("comma"); out_d(r); out_s(" "); out_d(a); out_s(" "); out_d(b2); out_end();
        char set[16] = "";
        r = sscanf("abc123def", "%15[a-z]", set);
        out_key("set"); out_d(r); out_s(" "); out_s(set); out_end();
        float f = 0;
        r = sscanf("1e-3", "%f", &f);
        out_key("float"); out_d(r); out_s(" "); out_flt(f); out_end();
        long double ld = 0;
        r = sscanf("3.25", "%Lf", &ld);
        out_key("long double"); out_d(r); out_s(" "); out_ldbl(ld); out_end();
        r = sscanf("", "%d", &a);
        out_key("empty"); out_d(r); out_end();
        int p1 = 0, p2 = 0;
        r = sscanf("5 6", "%2$d %1$d", &p1, &p2);
        out_key("positional"); out_d(r); out_s(" "); out_d(p1); out_s(" "); out_d(p2); out_end();
        short h = 0;
        signed char hh = 0;
        r = sscanf("-300 -5", "%hd %hhd", &h, &hh);
        out_key("narrow"); out_d(r); out_s(" "); out_d(h); out_s(" "); out_d(hh); out_end();
    }
    CASE("strerror") {
        for (int e = 0; e <= 107; e++) {
            char k[16];
            snprintf(k, sizeof k, "%d", e);
            out_key(k); out_s(strerror(e)); out_end();
        }
        char buf[8];
        int r = strerror_r(ENOENT, buf, sizeof buf);
        out_key("strerror_r short"); out_d(r); out_s(" "); out_q(buf, strnlen(buf, sizeof buf)); out_end();
    }
    CASE("strsignal") {
        for (int s = 0; s <= 32; s++) {
            char k[16];
            snprintf(k, sizeof k, "%d", s);
            out_key(k); out_s(strsignal(s)); out_end();
        }
    }
    CASE("collation") {
        static const char *const loc[] = { "C", "en_US.UTF-8", "de_DE.UTF-8", "sv_SE.UTF-8" };
        for (size_t i = 0; i < 4; i++) {
            out_key(loc[i]);
            out_s(setlocale(LC_ALL, loc[i]) ? "set " : "unset ");
            out_d(strcoll("apple", "Banana") > 0 ? 1 : strcoll("apple", "Banana") < 0 ? -1 : 0);
            out_s(" ");
            out_d(strcoll("\xc3\xa4", "z") > 0 ? 1 : -1);
            char x[64];
            size_t n = strxfrm(x, "Hello", sizeof x);
            out_s(" xfrm ");
            out_u(n);
            out_end();
        }
        setlocale(LC_ALL, "C");
    }
    CASE("fnmatch") {
        static const char *const pairs[][2] = { { "*.c", "probe.c" }, { "*.c", "dir/probe.c" }, { "[a-c]?x", "bzx" },
                                                { "\\*", "*" }, { "*", ".hidden" }, { "a*b*c", "aXXbYYc" } };
        for (size_t i = 0; i < 6; i++) {
            out_key(pairs[i][0]);
            out_d(fnmatch(pairs[i][0], pairs[i][1], 0));
            out_s(" ");
            out_d(fnmatch(pairs[i][0], pairs[i][1], FNM_PATHNAME | FNM_PERIOD));
            out_end();
        }
    }
    CASE("regex") {
        regex_t re;
        regmatch_t m[3];
        int r = regcomp(&re, "([a-z]+)-([0-9]+)", REG_EXTENDED);
        out_key("compile"); out_d(r); out_s(" nsub "); out_u(re.re_nsub); out_end();
        r = regexec(&re, "xx abc-123 yy", 3, m, 0);
        out_key("exec"); out_d(r);
        for (int i = 0; i < 3; i++) {
            out_s(" ");
            out_d(m[i].rm_so);
            out_s("-");
            out_d(m[i].rm_eo);
        }
        out_end();
        regfree(&re);
        r = regcomp(&re, "(unclosed", REG_EXTENDED);
        char eb[64];
        regerror(r, &re, eb, sizeof eb);
        out_key("error"); out_d(r); out_s(" "); out_s(eb); out_end();
    }
    CASE("wide strings") {
        setlocale(LC_ALL, "en_US.UTF-8");
        wchar_t w[16];
        size_t n = mbstowcs(w, "a\xc3\xa9\xe4\xb8\xad", 16);
        out_key("mbstowcs"); out_d((int64_t)n);
        for (size_t i = 0; i < n && i < 16; i++) {
            out_s(" ");
            out_x((uint32_t)w[i], 4);
        }
        out_end();
        char mb[16];
        n = wcstombs(mb, L"éx", sizeof mb);
        out_key("wcstombs"); out_d((int64_t)n); out_s(" "); out_q(mb, n == (size_t)-1 ? 0 : n); out_end();
        out_key("wcwidth"); out_d(wcwidth(0x4e2d)); out_s(" "); out_d(wcwidth(0x301)); out_s(" "); out_d(wcwidth('a'));
        out_end();
        out_key("wcstol"); out_d(wcstol(L"  -77z", NULL, 10)); out_end();
        out_key("wcstod"); out_dbl(wcstod(L"2.5e1", NULL)); out_end();
        out_key("MB_CUR_MAX"); out_u(MB_CUR_MAX); out_end();
    }
    return 0;
}
