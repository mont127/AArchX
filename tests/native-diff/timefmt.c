/* Time conversion and formatting with the time zone fixed: gmtime, localtime, mktime, timegm, strftime's
   specifiers in two locales, strptime, and the zone names. */
#include <locale.h>
#include <stdio.h>
#include <time.h>

#include "probe.h"

static void tm_out(const struct tm *t)
{
    out_d(t->tm_year); out_s("-"); out_d(t->tm_mon); out_s("-"); out_d(t->tm_mday); out_s(" ");
    out_d(t->tm_hour); out_s(":"); out_d(t->tm_min); out_s(":"); out_d(t->tm_sec);
    out_s(" wday "); out_d(t->tm_wday); out_s(" yday "); out_d(t->tm_yday); out_s(" dst "); out_d(t->tm_isdst);
    out_s(" off "); out_d(t->tm_gmtoff); out_s(" zone "); out_s(t->tm_zone);
}

int main(void)
{
    static const time_t when[] = { 0, 951782400, 1700000000, 2147483647, 2147483648LL, -1, -86400 * 365,
                                   4102444800LL, 1711846800 };
    static const char *const zones[] = { "UTC", "America/New_York", "Europe/Bratislava", "Australia/Lord_Howe",
                                         "Asia/Kolkata" };
    for (int z = 0; z < 5; z++) {
        char name[64];
        snprintf(name, sizeof name, "zone %s", zones[z]);
        CASE(name) {
            setenv("TZ", zones[z], 1);
            tzset();
            out_key("tzname"); out_s(tzname[0]); out_s(" "); out_s(tzname[1]); out_end();
            for (size_t i = 0; i < sizeof when / sizeof *when; i++) {
                char k[32];
                struct tm t;
                snprintf(k, sizeof k, "localtime %lld", (long long)when[i]);
                out_key(k);
                if (localtime_r(&when[i], &t)) tm_out(&t); else out_s("NULL");
                out_end();
                snprintf(k, sizeof k, "mktime %lld", (long long)when[i]);
                out_key(k);
                t.tm_isdst = -1;
                out_d(mktime(&t));
                out_end();
            }
            struct tm amb = { .tm_year = 124, .tm_mon = 10, .tm_mday = 3, .tm_hour = 1, .tm_min = 30, .tm_isdst = -1 };
            out_key("mktime ambiguous"); out_d(mktime(&amb)); out_s(" dst "); out_d(amb.tm_isdst); out_end();
            struct tm gap = { .tm_year = 124, .tm_mon = 2, .tm_mday = 10, .tm_hour = 2, .tm_min = 30, .tm_isdst = -1 };
            out_key("mktime gap"); out_d(mktime(&gap)); out_s(" hour "); out_d(gap.tm_hour); out_end();
        }
    }
    CASE("gmtime") {
        for (size_t i = 0; i < sizeof when / sizeof *when; i++) {
            struct tm t;
            char k[32];
            snprintf(k, sizeof k, "%lld", (long long)when[i]);
            out_key(k);
            if (gmtime_r(&when[i], &t)) {
                tm_out(&t);
                out_s(" timegm ");
                out_d(timegm(&t));
            } else {
                out_s("NULL");
            }
            out_end();
        }
    }
    static const char *const locs[] = { "C", "fr_FR.UTF-8" };
    for (int l = 0; l < 2; l++) {
        char name[64];
        snprintf(name, sizeof name, "strftime %s", locs[l]);
        CASE(name) {
            setlocale(LC_ALL, locs[l]);
            setenv("TZ", "Europe/Bratislava", 1);
            tzset();
            time_t t0 = 1700000000;
            struct tm t;
            localtime_r(&t0, &t);
            static const char *const spec = "aAbBcCdDeFgGhHIjklmMnprRsStTuUVwWxXyYzZ%+";
            for (const char *p = spec; *p; p++) {
                char f[4] = { '%', *p, 0 }, buf[128] = "";
                size_t n = strftime(buf, sizeof buf, f, &t);
                out_key(f);
                out_q(buf, n);
                out_end();
            }
            char buf[64];
            out_key("%Ec %Oy"); out_q(buf, strftime(buf, sizeof buf, "%Ec %Oy", &t)); out_end();
            out_key("too small"); out_d((int64_t)strftime(buf, 4, "%Y-%m-%d", &t)); out_end();
        }
    }
    CASE("strptime") {
        setlocale(LC_ALL, "C");
        struct tm t;
        static const char *const in[][2] = { { "2024-02-29 13:45:01", "%Y-%m-%d %H:%M:%S" },
                                             { "Tue, 14 Nov 2023 22:13:20 +0100", "%a, %d %b %Y %H:%M:%S %z" },
                                             { "12/31/99", "%D" }, { "1700000000", "%s" }, { "garbage", "%Y" } };
        for (int i = 0; i < 5; i++) {
            memset(&t, 0, sizeof t);
            const char *e = strptime(in[i][0], in[i][1], &t);
            out_key(in[i][1]);
            out_s(e ? "ok " : "NULL ");
            tm_out(&t);
            out_end();
        }
    }
    CASE("difftime and asctime") {
        struct tm t;
        time_t t0 = 1700000000;
        gmtime_r(&t0, &t);
        char b[64];
        out_key("asctime"); out_s(asctime_r(&t, b)); out_end();
        out_key("ctime"); setenv("TZ", "UTC", 1); tzset(); out_s(ctime_r(&t0, b)); out_end();
        out_key("difftime"); out_dbl(difftime(t0, 0)); out_end();
    }
    return 0;
}
