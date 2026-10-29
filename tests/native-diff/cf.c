/* CoreFoundation: strings (formatting, comparison, conversion, case, transforms), numbers, dates and calendars in
   fixed zones, URLs, property lists, and the number and date formatters with fixed locales. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

#include "probe.h"

static void out_cf(CFTypeRef v)
{
    if (!v) {
        out_s("NULL");
        return;
    }
    CFStringRef s = CFGetTypeID(v) == CFStringGetTypeID() ? CFRetain(v) : CFCopyDescription(v);
    char b[4096];
    if (s && CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8))
        out_q(b, strlen(b));
    else
        out_s("<unconvertible>");
    if (s)
        CFRelease(s);
}

static CFStringRef str(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

int main(void)
{
    CASE("format") {
        CFStringRef f = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d|%5.2f|%@|%s|%x|%ld|%c|%%|%e|%lld"), -7, 3.14159,
                                                 CFSTR("obj"), "cstr", 255, 1234567890L, 'Q', 1e-7, -9000000000LL);
        out_key("mixed"); out_cf(f); out_end();
        CFRelease(f);
        f = CFStringCreateWithFormat(NULL, NULL, CFSTR("%2$@ %1$@"), CFSTR("a"), CFSTR("b"));
        out_key("positional"); out_cf(f); out_end();
        CFRelease(f);
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &(double){ 2.5 });
        f = CFStringCreateWithFormat(NULL, NULL, CFSTR("[%@]"), n);
        out_key("number object"); out_cf(f); out_end();
        CFRelease(f);
        CFRelease(n);
    }
    CASE("compare") {
        static const char *const pairs[][2] = { { "apple", "Apple" }, { "file10", "file9" }, { "\xc3\xa9", "e" },
                                                { "stra\xc3\x9f" "e", "strasse" }, { "abc", "abd" } };
        static const struct { const char *n; CFStringCompareFlags f; } flags[] = {
            { "plain", 0 }, { "case", kCFCompareCaseInsensitive }, { "numeric", kCFCompareNumerically },
            { "diacritic", kCFCompareDiacriticInsensitive }, { "nonliteral", kCFCompareNonliteral },
            { "localized", kCFCompareLocalized } };
        for (int i = 0; i < 5; i++)
            for (int k = 0; k < 6; k++) {
                char key[64];
                snprintf(key, sizeof key, "%s %s %s", pairs[i][0], pairs[i][1], flags[k].n);
                CFStringRef a = str(pairs[i][0]), b = str(pairs[i][1]);
                out_key(key); out_d(CFStringCompare(a, b, flags[k].f)); out_end();
                CFRelease(a);
                CFRelease(b);
            }
    }
    CASE("convert") {
        CFStringRef s = str("Gr\xc3\xbc\xc3\x9f" "e \xe4\xb8\xad\xf0\x9f\x98\x80");
        out_key("length"); out_d(CFStringGetLength(s)); out_end();
        char b[64];
        out_key("latin1"); out_d(CFStringGetCString(s, b, sizeof b, kCFStringEncodingISOLatin1)); out_end();
        CFIndex used = 0;
        CFIndex got = CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingASCII, '?', false,
                                       (UInt8 *)b, sizeof b, &used);
        out_key("lossy ascii"); out_d(got); out_s(" "); out_q(b, (size_t)used); out_end();
        CFMutableStringRef m = CFStringCreateMutableCopy(NULL, 0, s);
        CFStringUppercase(m, NULL);
        out_key("upper"); out_cf(m); out_end();
        CFStringNormalize(m, kCFStringNormalizationFormD);
        out_key("NFD length"); out_d(CFStringGetLength(m)); out_end();
        CFRelease(m);
        m = CFStringCreateMutableCopy(NULL, 0, CFSTR("Straße Ångström 中文"));
        CFStringTransform(m, NULL, kCFStringTransformToLatin, false);
        CFStringTransform(m, NULL, kCFStringTransformStripCombiningMarks, false);
        out_key("transform"); out_cf(m); out_end();
        CFRelease(m);
        out_key("double value"); out_dbl(CFStringGetDoubleValue(CFSTR(" 12.75e2xyz"))); out_end();
        out_key("int value"); out_d(CFStringGetIntValue(CFSTR("-42abc"))); out_end();
        CFRelease(s);
    }
    CASE("numbers") {
        SInt8 c = -5;
        SInt64 big = 0x7fffffffffffffffLL;
        float f = 0.1f;
        double d = 1.0 / 3.0;
        CFNumberRef n[4] = { CFNumberCreate(NULL, kCFNumberSInt8Type, &c), CFNumberCreate(NULL, kCFNumberSInt64Type, &big),
                             CFNumberCreate(NULL, kCFNumberFloatType, &f), CFNumberCreate(NULL, kCFNumberDoubleType, &d) };
        for (int i = 0; i < 4; i++) {
            char k[8];
            snprintf(k, sizeof k, "%d", i);
            out_key(k);
            out_d(CFNumberGetType(n[i]));
            out_s(" ");
            out_d(CFNumberIsFloatType(n[i]));
            SInt32 v32 = 0;
            out_s(" lossless32 ");
            out_d(CFNumberGetValue(n[i], kCFNumberSInt32Type, &v32));
            out_s(" ");
            out_d(v32);
            double dv = 0;
            CFNumberGetValue(n[i], kCFNumberDoubleType, &dv);
            out_s(" ");
            out_dbl(dv);
            out_s(" ");
            out_cf(n[i]);
            out_end();
        }
        out_key("compare"); out_d(CFNumberCompare(n[2], n[3], NULL)); out_end();
        for (int i = 0; i < 4; i++)
            CFRelease(n[i]);
    }
    CASE("dates") {
        CFTimeZoneRef tz = CFTimeZoneCreateWithName(NULL, CFSTR("Europe/Bratislava"), true);
        CFAbsoluteTime t = 721692000.0;   /* 2023-11-14 22:00:00 UTC */
        CFGregorianDate g = CFAbsoluteTimeGetGregorianDate(t, tz);
        out_key("gregorian"); out_d(g.year); out_s("-"); out_d(g.month); out_s("-"); out_d(g.day); out_s(" ");
        out_d(g.hour); out_s(":"); out_d(g.minute); out_s(":"); out_dbl(g.second); out_end();
        out_key("offset"); out_dbl(CFTimeZoneGetSecondsFromGMT(tz, t)); out_end();
        out_key("dst"); out_d(CFTimeZoneIsDaylightSavingTime(tz, t)); out_end();
        out_key("next transition"); out_dbl(CFTimeZoneGetNextDaylightSavingTimeTransition(tz, t)); out_end();
        CFCalendarRef cal = CFCalendarCreateWithIdentifier(NULL, kCFGregorianCalendar);
        CFCalendarSetTimeZone(cal, tz);
        int y = 0, mo = 0, dd = 0, wd = 0;
        CFCalendarDecomposeAbsoluteTime(cal, t, "yMdE", &y, &mo, &dd, &wd);
        out_key("decompose"); out_d(y); out_s(" "); out_d(mo); out_s(" "); out_d(dd); out_s(" "); out_d(wd); out_end();
        CFAbsoluteTime later = t;
        CFCalendarAddComponents(cal, &later, 0, "Md", 3, 20);
        out_key("add"); out_dbl(later); out_end();
        CFAbsoluteTime made = 0;
        CFCalendarComposeAbsoluteTime(cal, &made, "yMdHms", 2024, 2, 29, 12, 30, 15);
        out_key("compose"); out_dbl(made); out_end();
        CFLocaleRef loc = CFLocaleCreate(NULL, CFSTR("de_DE"));
        CFDateFormatterRef df = CFDateFormatterCreate(NULL, loc, kCFDateFormatterFullStyle, kCFDateFormatterLongStyle);
        CFDateFormatterSetProperty(df, kCFDateFormatterTimeZone, tz);
        CFStringRef s = CFDateFormatterCreateStringWithAbsoluteTime(NULL, df, t);
        out_key("formatter de"); out_cf(s); out_end();
        CFRelease(s);
        CFDateFormatterSetFormat(df, CFSTR("yyyy-MM-dd'T'HH:mm:ss.SSSZZZZZ EEEE"));
        s = CFDateFormatterCreateStringWithAbsoluteTime(NULL, df, t + 0.125);
        out_key("formatter pattern"); out_cf(s); out_end();
        CFAbsoluteTime parsed = 0;
        Boolean ok = CFDateFormatterGetAbsoluteTimeFromString(df, s, NULL, &parsed);
        out_key("parse back"); out_d(ok); out_s(" "); out_dbl(parsed); out_end();
        CFRelease(s);
        CFRelease(df);
        CFRelease(loc);
        CFRelease(cal);
        CFRelease(tz);
    }
    CASE("number formatter") {
        static const char *const locs[] = { "en_US", "de_DE", "fr_FR", "ja_JP", "ar_EG" };
        double v = -1234567.891;
        for (int i = 0; i < 5; i++) {
            CFStringRef ln = str(locs[i]);
            CFLocaleRef loc = CFLocaleCreate(NULL, ln);
            static const CFNumberFormatterStyle styles[] = { kCFNumberFormatterDecimalStyle,
                                                             kCFNumberFormatterCurrencyStyle,
                                                             kCFNumberFormatterPercentStyle,
                                                             kCFNumberFormatterScientificStyle,
                                                             kCFNumberFormatterSpellOutStyle };
            for (int k = 0; k < 5; k++) {
                CFNumberFormatterRef nf = CFNumberFormatterCreate(NULL, loc, styles[k]);
                CFStringRef s = CFNumberFormatterCreateStringWithValue(NULL, nf, kCFNumberDoubleType,
                                                                       k == 4 ? &(double){ 42 } : &v);
                char key[32];
                snprintf(key, sizeof key, "%s %d", locs[i], k);
                out_key(key); out_cf(s); out_end();
                CFRelease(s);
                CFRelease(nf);
            }
            CFRelease(loc);
            CFRelease(ln);
        }
    }
    CASE("urls") {
        CFURLRef u = CFURLCreateWithString(NULL, CFSTR("https://user:pw@example.com:8080/a/b%20c/../d.txt?q=1&r=%C3%A9#frag"),
                                           NULL);
        out_key("scheme"); out_cf(CFURLCopyScheme(u)); out_end();
        out_key("host"); out_cf(CFURLCopyHostName(u)); out_end();
        out_key("port"); out_d(CFURLGetPortNumber(u)); out_end();
        out_key("path"); out_cf(CFURLCopyPath(u)); out_end();
        out_key("file system path"); out_cf(CFURLCopyFileSystemPath(u, kCFURLPOSIXPathStyle)); out_end();
        out_key("query"); out_cf(CFURLCopyQueryString(u, NULL)); out_end();
        out_key("fragment"); out_cf(CFURLCopyFragment(u, NULL)); out_end();
        CFURLRef abs = CFURLCopyAbsoluteURL(u);
        out_key("absolute"); out_cf(CFURLGetString(abs)); out_end();
        CFURLRef rel = CFURLCreateWithString(NULL, CFSTR("../x?y"), u);
        out_key("relative"); out_cf(CFURLGetString(CFURLCopyAbsoluteURL(rel))); out_end();
        CFStringRef un = CFURLCreateStringByReplacingPercentEscapes(NULL, CFSTR("a%20b%C3%A9%zz"), CFSTR(""));
        out_key("unescape"); out_cf(un); out_end();
        CFStringRef esc = CFURLCreateStringByAddingPercentEscapes(NULL, CFSTR("a b/\xc3\xa9?&"), NULL, CFSTR("&"),
                                                                 kCFStringEncodingUTF8);
        out_key("escape"); out_cf(esc); out_end();
        UInt8 buf[256];
        CFURLRef f = CFURLCreateWithFileSystemPath(NULL, CFSTR("/tmp/../usr/./lib"), kCFURLPOSIXPathStyle, true);
        out_key("file url"); out_cf(CFURLGetString(f)); out_end();
        out_key("representation"); out_d(CFURLGetFileSystemRepresentation(f, true, buf, sizeof buf)); out_s(" ");
        out_s((char *)buf); out_end();
    }
    CASE("property lists") {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(d, CFSTR("int"), CFNumberCreate(NULL, kCFNumberSInt64Type, &(SInt64){ -5000000000LL }));
        CFDictionarySetValue(d, CFSTR("real"), CFNumberCreate(NULL, kCFNumberDoubleType, &(double){ 0.1 }));
        CFDictionarySetValue(d, CFSTR("str"), CFSTR("café"));
        CFDictionarySetValue(d, CFSTR("yes"), kCFBooleanTrue);
        CFDictionarySetValue(d, CFSTR("date"), CFDateCreate(NULL, 721692000.5));
        CFDictionarySetValue(d, CFSTR("data"), CFDataCreate(NULL, (const UInt8 *)"\x00\x01\xff", 3));
        CFDataRef x = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        out_key("xml"); out_q((const char *)CFDataGetBytePtr(x), (size_t)CFDataGetLength(x)); out_end();
        CFDataRef bin = CFPropertyListCreateData(NULL, d, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
        out_key("binary length"); out_d(CFDataGetLength(bin)); out_end();
        CFPropertyListFormat fmt = 0;
        CFPropertyListRef back = CFPropertyListCreateWithData(NULL, bin, 0, &fmt, NULL);
        out_key("round trip"); out_d(CFEqual(back, d)); out_s(" format "); out_d(fmt); out_end();
        CFRelease(x);
        CFRelease(bin);
        CFRelease(back);
        CFRelease(d);
    }
    CASE("collections") {
        CFStringRef vals[] = { CFSTR("pear"), CFSTR("Apple"), CFSTR("banana"), CFSTR("apple") };
        CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        for (int i = 0; i < 4; i++)
            CFArrayAppendValue(a, vals[i]);
        CFArraySortValues(a, CFRangeMake(0, 4), (CFComparatorFunction)CFStringCompare, (void *)kCFCompareCaseInsensitive);
        out_key("sorted"); out_cf(a); out_end();
        out_key("bsearch"); out_d(CFArrayBSearchValues(a, CFRangeMake(0, 4), CFSTR("banana"),
                                                       (CFComparatorFunction)CFStringCompare, (void *)kCFCompareCaseInsensitive));
        out_end();
        CFSetRef set = CFSetCreate(NULL, (const void **)vals, 4, &kCFTypeSetCallBacks);
        out_key("set count"); out_d(CFSetGetCount(set)); out_end();
        CFRelease(set);
        CFRelease(a);
        CFCharacterSetRef cs = CFCharacterSetGetPredefined(kCFCharacterSetLetter);
        out_key("letters"); out_d(CFCharacterSetIsLongCharacterMember(cs, 0x4e2d)); out_d(CFCharacterSetIsLongCharacterMember(cs, '1'));
        out_d(CFCharacterSetIsLongCharacterMember(cs, 0x1f600)); out_end();
    }
    return 0;
}
