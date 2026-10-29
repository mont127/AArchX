/* The printf family: integer, floating-point, long double, character, string, wide, positional and %n formats, the
   return values, truncation, and the entry points that take a va_list or allocate. */
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include "probe.h"

static void show(const char *buf, int ret)
{
    out_q(buf, strlen(buf));
    out_s(" ret ");
    out_d(ret);
    out_end();
}

static int via_va(char *b, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(b, n, fmt, ap);
    va_end(ap);
    return r;
}

int main(void)
{
    static const char *const ifmt[] = { "%d", "%i", "%u", "%x", "%X", "%o", "%5d", "%-5d|", "%05d", "%+d", "% d",
                                        "%.3d", "%#x", "%#o", "%hhd", "%hd", "%hhu", "%hu", "%c" };
    static const int ival[] = { 0, 1, -1, 42, -42, 255, 256, 65535, INT_MAX, INT_MIN, 0x7f, 0xdeadbeef };
    char b[512];
    for (size_t f = 0; f < sizeof ifmt / sizeof *ifmt; f++) {
        char name[64];
        snprintf(name, sizeof name, "int %s", ifmt[f]);
        CASE(name) {
            for (size_t i = 0; i < sizeof ival / sizeof *ival; i++) {
                char k[16];
                snprintf(k, sizeof k, "%zu", i);
                out_key(k);
                show(b, snprintf(b, sizeof b, ifmt[f], ival[i]));
            }
        }
    }
    static const char *const lfmt[] = { "%ld", "%lu", "%lx", "%lld", "%llu", "%llx", "%jd", "%zd", "%zu", "%td",
                                        "%qd", "%20lld|", "%-20lld|" };
    static const long long lval[] = { 0, -1, LLONG_MAX, LLONG_MIN, 1234567890123LL, 0x8000000000000000ULL };
    for (size_t f = 0; f < sizeof lfmt / sizeof *lfmt; f++) {
        char name[64];
        snprintf(name, sizeof name, "long %s", lfmt[f]);
        CASE(name) {
            for (size_t i = 0; i < sizeof lval / sizeof *lval; i++) {
                char k[16];
                snprintf(k, sizeof k, "%zu", i);
                out_key(k);
                show(b, snprintf(b, sizeof b, lfmt[f], lval[i]));
            }
        }
    }
    static const char *const dfmt[] = { "%f", "%e", "%g", "%a", "%A", "%E", "%G", "%F", "%.0f", "%.17g", "%.3e",
                                         "%10.4f|", "%-10.2e|", "%+g", "% f", "%#g", "%#.0f", "%.30f", "%.0e", "%lf" };
    static const double dval[] = { 0.0, -0.0, 1.0, -1.5, 0.1, 1e-310, 2.2250738585072014e-308, 1e300, -1e300,
                                   123456789.125, 3.141592653589793, 0.5, 1.5, 2.5, 1e21, INFINITY, -INFINITY, NAN };
    for (size_t f = 0; f < sizeof dfmt / sizeof *dfmt; f++) {
        char name[64];
        snprintf(name, sizeof name, "double %s", dfmt[f]);
        CASE(name) {
            for (size_t i = 0; i < sizeof dval / sizeof *dval; i++) {
                char k[16];
                snprintf(k, sizeof k, "%zu", i);
                out_key(k);
                show(b, snprintf(b, sizeof b, dfmt[f], dval[i]));
            }
        }
    }
    static const char *const ldfmt[] = { "%Lf", "%Le", "%Lg", "%La", "%.20Lg", "%.0Lf", "%10.3Lf|" };
    static const long double ldval[] = { 0.0L, 1.0L, -1.5L, 0.1L, 1e-4000L, 1e4000L, 3.14159265358979323846L,
                                         1.0L / 3.0L, (long double)INFINITY, (long double)NAN };
    for (size_t f = 0; f < sizeof ldfmt / sizeof *ldfmt; f++) {
        char name[64];
        snprintf(name, sizeof name, "long double %s", ldfmt[f]);
        CASE(name) {
            for (size_t i = 0; i < sizeof ldval / sizeof *ldval; i++) {
                char k[16];
                snprintf(k, sizeof k, "%zu", i);
                out_key(k);
                show(b, snprintf(b, sizeof b, ldfmt[f], ldval[i]));
            }
        }
    }
    CASE("strings") {
        out_key("%s"); show(b, snprintf(b, sizeof b, "%s", "hello"));
        out_key("%.3s"); show(b, snprintf(b, sizeof b, "%.3s", "hello"));
        out_key("%10s"); show(b, snprintf(b, sizeof b, "%10s|", "hi"));
        out_key("%-10s"); show(b, snprintf(b, sizeof b, "%-10s|", "hi"));
        out_key("%s NULL"); show(b, snprintf(b, sizeof b, "%s", (char *)NULL));
        out_key("%c"); show(b, snprintf(b, sizeof b, "[%c%c%c]", 'a', 0x7f, 'z'));
        out_key("%%"); show(b, snprintf(b, sizeof b, "100%%"));
        out_key("mixed"); show(b, snprintf(b, sizeof b, "%s=%d (%.2f) %c %x", "k", 7, 2.5, 'q', 255u));
        out_key("many"); show(b, snprintf(b, sizeof b, "%d %d %d %d %d %d %d %d %d %d %f %f %f %f %f %f %f %f %f %f",
                                          1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0));
    }
    CASE("positional") {
        out_key("swap"); show(b, snprintf(b, sizeof b, "%2$s %1$s", "world", "hello"));
        out_key("repeat"); show(b, snprintf(b, sizeof b, "%1$d %1$x %2$f", 255, 1.5));
        out_key("width"); show(b, snprintf(b, sizeof b, "%1$*2$d|", 42, 8));
    }
    CASE("star") {
        out_key("width"); show(b, snprintf(b, sizeof b, "%*d|", 6, 42));
        out_key("negative width"); show(b, snprintf(b, sizeof b, "%*d|", -6, 42));
        out_key("precision"); show(b, snprintf(b, sizeof b, "%.*f", 3, 3.14159));
        out_key("both"); show(b, snprintf(b, sizeof b, "%*.*s|", 8, 2, "abcdef"));
    }
    CASE("percent n") {
        int n1 = -1, n2 = -1;
        snprintf(b, sizeof b, "abc%nde%nf", &n1, &n2);
        out_key(NULL);
        out_d(n1);
        out_s(" ");
        out_d(n2);
        out_end();
    }
    CASE("wide") {
        out_key("%ls"); show(b, snprintf(b, sizeof b, "%ls", L"wide"));
        out_key("%lc"); show(b, snprintf(b, sizeof b, "%lc", (wint_t)L'x'));
        out_key("%S"); show(b, snprintf(b, sizeof b, "%S", L"caps"));
        wchar_t w[64];
        int r = swprintf(w, 64, L"%d-%ls-%.1f", 5, L"x", 2.25);
        out_key("swprintf");
        for (int i = 0; i < r && i < 64; i++) {
            out_x((uint32_t)w[i], 4);
            out_s(" ");
        }
        out_s("ret ");
        out_d(r);
        out_end();
    }
    CASE("utf8") {
        setlocale(LC_ALL, "en_US.UTF-8");
        out_key("%ls"); show(b, snprintf(b, sizeof b, "%ls", L"é中"));
        out_key("%lc"); show(b, snprintf(b, sizeof b, "%lc", (wint_t)0x00e9));
        out_key("%'d"); show(b, snprintf(b, sizeof b, "%'d", 1234567));
        out_key("%'.2f"); show(b, snprintf(b, sizeof b, "%'.2f", 1234567.891));
    }
    CASE("truncation") {
        char t[8];
        memset(t, 'Z', sizeof t);
        int r = snprintf(t, 5, "%s", "abcdefgh");
        out_key("snprintf 5"); out_q(t, 8); out_s(" ret "); out_d(r); out_end();
        r = snprintf(NULL, 0, "%d-%s", 12345, "xyz");
        out_key("size only"); out_d(r); out_end();
        memset(t, 'Z', sizeof t);
        r = snprintf(t, 1, "abc");
        out_key("size 1"); out_q(t, 8); out_s(" ret "); out_d(r); out_end();
    }
    CASE("va_list") {
        out_key("vsnprintf"); show(b, via_va(b, sizeof b, "%s %d %.3f %Lf %c", "va", -3, 1.0 / 3, 2.5L, 'v'));
    }
    CASE("asprintf") {
        char *a = NULL;
        int r = asprintf(&a, "%s:%05.1f:%llx", "as", 9.87, 0xabcdefULL);
        out_key(NULL); show(a ? a : "", r);
        free(a);
    }
    CASE("sprintf") {
        int r = sprintf(b, "%08.3f|%-6s|%+i", -3.5, "sp", 7);
        out_key(NULL); show(b, r);
    }
    return 0;
}
