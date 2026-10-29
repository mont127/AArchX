/* What a program learns about the machine and the process, which native mode answers as an Intel Mac would (page
   size, sysconf, the CPU), and the seeded random generators, whose sequences a program may depend on. */
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <fenv.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <wchar.h>
#include <wctype.h>

#include "probe.h"

int main(void)
{
    CASE("sysconf") {
        static const struct { const char *n; int v; } names[] = {
            { "PAGESIZE", _SC_PAGESIZE }, { "OPEN_MAX", _SC_OPEN_MAX }, { "CHILD_MAX", _SC_CHILD_MAX },
            { "CLK_TCK", _SC_CLK_TCK }, { "ARG_MAX", _SC_ARG_MAX }, { "NGROUPS_MAX", _SC_NGROUPS_MAX },
            { "LINE_MAX", _SC_LINE_MAX }, { "IOV_MAX", _SC_IOV_MAX }, { "HOST_NAME_MAX", _SC_HOST_NAME_MAX },
            { "THREAD_STACK_MIN", _SC_THREAD_STACK_MIN }, { "THREAD_KEYS_MAX", _SC_THREAD_KEYS_MAX },
            { "GETPW_R_SIZE_MAX", _SC_GETPW_R_SIZE_MAX }, { "VERSION", _SC_VERSION }, { "STREAM_MAX", _SC_STREAM_MAX },
            { "TZNAME_MAX", _SC_TZNAME_MAX }, { "ATEXIT_MAX", _SC_ATEXIT_MAX } };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            out_key(names[i].n);
            out_d(sysconf(names[i].v));
            out_end();
        }
        out_key("getpagesize"); out_d(getpagesize()); out_end();
        out_key("PTHREAD_STACK_MIN"); out_d(PTHREAD_STACK_MIN); out_end();
    }
    CASE("sysctl") {
        static const char *const names[] = { "hw.pagesize", "hw.cputype", "hw.cpusubtype", "hw.byteorder",
                                             "hw.optional.x86_64", "hw.optional.sse4_2", "hw.optional.avx2_0",
                                             "hw.optional.arm64", "sysctl.proc_translated", "hw.cachelinesize",
                                             "kern.ostype", "machdep.cpu.vendor", "machdep.cpu.brand_string",
                                             "hw.machine", "kern.osproductversion" };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            union { int64_t i; char s[128]; } v;
            memset(&v, 0, sizeof v);
            size_t n = sizeof v;
            int r = sysctlbyname(names[i], &v, &n, NULL, 0);
            out_key(names[i]);
            if (r != 0) {
                out_s("<error>");
            } else if (n == 4) {
                out_d((int32_t)v.i);
            } else if (n == 8 && !strchr("sm", names[i][0])) {   /* an integer; kern.* and machdep.* are text */
                out_d(v.i);
            } else {
                out_q(v.s, strnlen(v.s, n));
            }
            out_end();
        }
        struct utsname u;
        uname(&u);
        out_key("uname machine"); out_s(u.machine); out_end();
        out_key("uname sysname"); out_s(u.sysname); out_end();
    }
    CASE("rlimits") {
        static const int res[] = { RLIMIT_STACK, RLIMIT_NOFILE, RLIMIT_DATA, RLIMIT_CORE };
        for (int i = 0; i < 4; i++) {
            struct rlimit rl;
            char k[16];
            snprintf(k, sizeof k, "%d", res[i]);
            out_key(k);
            out_d(getrlimit(res[i], &rl));
            out_s(" cur ");
            out_u(rl.rlim_cur);
            out_end();
        }
    }
    CASE("rand") {
        srand(1);
        out_key("rand");
        for (int i = 0; i < 6; i++) { out_d(rand()); out_s(" "); }
        out_end();
        srandom(7);
        out_key("random");
        for (int i = 0; i < 6; i++) { out_d(random()); out_s(" "); }
        out_end();
        srand48(3);
        out_key("lrand48");
        for (int i = 0; i < 4; i++) { out_d(lrand48()); out_s(" "); }
        out_end();
        out_key("drand48");
        for (int i = 0; i < 3; i++) { out_dbl(drand48()); out_s(" "); }
        out_end();
        out_key("mrand48"); out_d(mrand48()); out_end();
        unsigned short xs[3] = { 1, 2, 3 };
        out_key("erand48"); out_dbl(erand48(xs)); out_s(" "); out_d(nrand48(xs)); out_end();
        unsigned seed = 5;
        out_key("rand_r"); out_d(rand_r(&seed)); out_s(" "); out_d(rand_r(&seed)); out_end();
    }
    CASE("ctype C") {
        static int (*const f[])(int) = { isalnum, isalpha, isblank, iscntrl, isdigit, isgraph, islower, isprint,
                                         ispunct, isspace, isupper, isxdigit };
        static const char *const fn[] = { "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print",
                                          "punct", "space", "upper", "xdigit" };
        for (int k = 0; k < 12; k++) {
            out_key(fn[k]);
            for (int c = -1; c < 256; c++)
                out_s(f[k](c) ? "1" : "0");
            out_end();
        }
        out_key("toupper");
        for (int c = 0; c < 256; c++)
            out_x((uint8_t)toupper(c), 2);
        out_end();
    }
    CASE("ctype UTF-8") {
        setlocale(LC_ALL, "en_US.UTF-8");
        static const int wc[] = { 0xe9, 0xc9, 0x3b1, 0x4e2d, 0x2028, 0xa0, 0x660, 0x1f600, 0xff21 };
        for (int i = 0; i < 9; i++) {
            char k[16];
            snprintf(k, sizeof k, "%x", wc[i]);
            out_key(k);
            out_d(iswalpha(wc[i]) != 0); out_d(iswupper(wc[i]) != 0); out_d(iswspace(wc[i]) != 0);
            out_d(iswdigit(wc[i]) != 0); out_d(iswpunct(wc[i]) != 0); out_d(iswprint(wc[i]) != 0);
            out_s(" upper "); out_x((uint32_t)towupper(wc[i]), 5);
            out_s(" lower "); out_x((uint32_t)towlower(wc[i]), 5);
            out_s(" width "); out_d(wcwidth(wc[i]));
            out_end();
        }
    }
    CASE("localeconv") {
        static const char *const loc[] = { "C", "en_US.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8", "ja_JP.UTF-8" };
        for (int i = 0; i < 5; i++) {
            setlocale(LC_ALL, loc[i]);
            struct lconv *lc = localeconv();
            out_key(loc[i]);
            out_q(lc->decimal_point, strlen(lc->decimal_point)); out_s(" ");
            out_q(lc->thousands_sep, strlen(lc->thousands_sep)); out_s(" ");
            out_q(lc->grouping, strlen(lc->grouping)); out_s(" ");
            out_q(lc->currency_symbol, strlen(lc->currency_symbol)); out_s(" ");
            out_d(lc->frac_digits); out_s(" "); out_d(lc->p_cs_precedes);
            out_end();
        }
    }
    CASE("fenv") {
        out_key("round"); out_d(fegetround()); out_end();
        feclearexcept(FE_ALL_EXCEPT);
        volatile double z = 0.0, one = 1.0;
        volatile double r = one / z;
        (void)r;
        out_key("divbyzero"); out_d(fetestexcept(FE_DIVBYZERO) != 0); out_end();
        feclearexcept(FE_ALL_EXCEPT);
        r = sqrt(-one);
        out_key("invalid after sqrt(-1)"); out_d(fetestexcept(FE_INVALID) != 0); out_end();
        fesetround(FE_UPWARD);
        volatile double third = one / 3.0;
        out_key("upward 1/3"); out_dbl(third); out_end();
        out_key("upward rint 2.1"); out_dbl(rint(2.1)); out_end();
        fesetround(FE_TONEAREST);
    }
    return 0;
}
