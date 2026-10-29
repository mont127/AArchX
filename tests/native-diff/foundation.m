/* Foundation: string formatting and methods, number and date formatters with fixed locales and zones, JSON and
   property lists, scanners, regular expressions, data encodings, URL components, predicates and expressions, and
   descriptions of values a program may parse. */
#import <Foundation/Foundation.h>

#include "probe.h"

static void out_ns(id v)
{
    NSString *s = [v isKindOfClass:[NSString class]] ? v : [v description];
    const char *c = s ? [s UTF8String] : "nil";
    out_q(c, strlen(c));
}

int main(void)
{
    @autoreleasepool {
        CASE("format") {
            out_key("mixed");
            out_ns([NSString stringWithFormat:@"%d|%5.2f|%@|%s|%lx|%C|%%|%g|%qd|%lu", -7, 3.14159, @"obj", "c",
                                              255ul, (unichar)0x4e2d, 1e-7, -9000000000LL, (unsigned long)NSNotFound]);
            out_end();
            out_key("positional"); out_ns([NSString stringWithFormat:@"%2$@ %1$@", @"a", @"b"]); out_end();
            out_key("long double"); out_ns([NSString stringWithFormat:@"%Lf", 2.5L]); out_end();
            out_key("locale"); out_ns([[NSString alloc] initWithFormat:@"%.2f %d" locale:[NSLocale localeWithLocaleIdentifier:@"de_DE"], 1234.5, 1000000]);
            out_end();
            out_key("localized"); out_ns([NSString localizedStringWithFormat:@"%.1f", 2.5]); out_end();
        }
        CASE("strings") {
            NSString *s = @"  Hello, wörld! 中文 Straße  ";
            out_key("trim"); out_ns([s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]); out_end();
            out_key("components"); out_ns([s componentsSeparatedByString:@" "]); out_end();
            out_key("upper"); out_ns([s uppercaseString]); out_end();
            out_key("capitalized"); out_ns([@"hello wORLD foo-bar" capitalizedString]); out_end();
            out_key("range"); out_ns(NSStringFromRange([s rangeOfString:@"WÖRLD" options:NSCaseInsensitiveSearch])); out_end();
            out_key("not found"); out_ns(NSStringFromRange([s rangeOfString:@"zzz"])); out_end();
            out_key("compare numeric"); out_d([@"file10" compare:@"file9" options:NSNumericSearch]); out_end();
            out_key("localized compare"); out_d([@"ä" localizedCompare:@"z"]); out_end();
            out_key("folding"); out_ns([@"Été Straße" stringByFoldingWithOptions:NSDiacriticInsensitiveSearch | NSCaseInsensitiveSearch locale:nil]); out_end();
            out_key("decomposed"); out_d([[@"é" decomposedStringWithCanonicalMapping] length]); out_end();
            out_key("padding"); out_ns([@"ab" stringByPaddingToLength:5 withString:@"xy" startingAtIndex:1]); out_end();
            out_key("int"); out_d([@" -42abc" intValue]); out_s(" "); out_d([@"9999999999" longLongValue]); out_end();
            out_key("double"); out_dbl([@"1.5e3x" doubleValue]); out_end();
            out_key("path"); out_ns([@"/a/b/../c.tar.gz" stringByStandardizingPath]); out_s(" ");
            out_ns([@"/a/b/c.tar.gz" pathExtension]); out_s(" "); out_ns([@"~/x" stringByExpandingTildeInPath].lastPathComponent); out_end();
            NSData *d = [@"héllo 中" dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
            out_key("utf16 bytes"); out_d((int64_t)[d length]); out_end();
            out_key("latin1 lossy"); out_ns([[NSString alloc] initWithData:[@"中é" dataUsingEncoding:NSISOLatin1StringEncoding allowLossyConversion:YES] encoding:NSISOLatin1StringEncoding]); out_end();
        }
        CASE("numbers") {
            out_key("descriptions"); out_ns(@[ @0.1, @(1.0f / 3), @(-0.0), @(1e100), @(LLONG_MAX), @(ULLONG_MAX), @YES, @'c' ]); out_end();
            out_key("stringValue"); out_ns([@(0.1 + 0.2) stringValue]); out_end();
            out_key("objCType"); out_s([@(3.5f) objCType]); out_s(" "); out_s([@YES objCType]); out_s(" "); out_s([@(1L) objCType]); out_end();
            NSDecimalNumber *a = [NSDecimalNumber decimalNumberWithString:@"123.456"];
            NSDecimalNumber *b = [NSDecimalNumber decimalNumberWithString:@"0.1"];
            out_key("decimal"); out_ns([a decimalNumberByMultiplyingBy:b]); out_s(" "); out_ns([a decimalNumberByDividingBy:[NSDecimalNumber decimalNumberWithString:@"7"]]); out_end();
            NSNumberFormatter *nf = [NSNumberFormatter new];
            nf.locale = [NSLocale localeWithLocaleIdentifier:@"en_US"];
            nf.numberStyle = NSNumberFormatterSpellOutStyle;
            out_key("spell out"); out_ns([nf stringFromNumber:@1234]); out_end();
            nf.numberStyle = NSNumberFormatterCurrencyStyle;
            nf.locale = [NSLocale localeWithLocaleIdentifier:@"sk_SK"];
            out_key("currency sk"); out_ns([nf stringFromNumber:@(-1234.5)]); out_end();
            out_key("bytes"); out_ns([NSByteCountFormatter stringFromByteCount:1536000 countStyle:NSByteCountFormatterCountStyleFile]); out_end();
        }
        CASE("dates") {
            NSDate *t = [NSDate dateWithTimeIntervalSince1970:1700000000.25];
            NSDateFormatter *f = [NSDateFormatter new];
            f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
            f.timeZone = [NSTimeZone timeZoneWithName:@"America/Los_Angeles"];
            f.dateFormat = @"yyyy-MM-dd'T'HH:mm:ss.SSSZZZZZ EEE w";
            out_key("pattern"); out_ns([f stringFromDate:t]); out_end();
            out_key("parse"); out_dbl([[f dateFromString:@"2024-02-29T23:59:59.500-08:00 Thu 9"] timeIntervalSince1970]); out_end();
            f.locale = [NSLocale localeWithLocaleIdentifier:@"ja_JP"];
            f.dateStyle = NSDateFormatterFullStyle;
            f.timeStyle = NSDateFormatterShortStyle;
            out_key("styles ja"); out_ns([f stringFromDate:t]); out_end();
            NSISO8601DateFormatter *iso = [NSISO8601DateFormatter new];
            out_key("iso8601"); out_ns([iso stringFromDate:t]); out_end();
            NSCalendar *cal = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
            cal.timeZone = [NSTimeZone timeZoneWithName:@"Asia/Kolkata"];
            NSDateComponents *c = [cal components:NSCalendarUnitYear | NSCalendarUnitWeekOfYear | NSCalendarUnitWeekday | NSCalendarUnitHour fromDate:t];
            out_key("components"); out_d(c.year); out_s(" "); out_d(c.weekOfYear); out_s(" "); out_d(c.weekday); out_s(" "); out_d(c.hour); out_end();
            NSDateComponentsFormatter *dcf = [NSDateComponentsFormatter new];
            dcf.unitsStyle = NSDateComponentsFormatterUnitsStylePositional;
            out_key("interval"); out_ns([dcf stringFromTimeInterval:3725.5]); out_end();
        }
        CASE("json") {
            NSError *err = nil;
            id o = [NSJSONSerialization JSONObjectWithData:[@"{\"a\":[1,2.5,-0.0,1e400,\"\\u00e9\\ud83d\\ude00\",null,true],\"b\":{\"c\":12345678901234567890}}" dataUsingEncoding:NSUTF8StringEncoding] options:0 error:&err];
            out_key("parse"); out_ns(o); out_s(" err "); out_d(err.code); out_end();
            NSData *d = [NSJSONSerialization dataWithJSONObject:@{ @"x" : @[ @0.1, @(1.0 / 3), @YES, @"q\"/" ], @"y" : @{ @"z" : [NSNull null] } }
                                                       options:NSJSONWritingSortedKeys error:&err];
            out_key("write"); out_q(d.bytes, d.length); out_end();
        }
        CASE("plist") {
            NSDictionary *pl = @{ @"n" : @(-1), @"r" : @(0.5), @"s" : @"é", @"d" : [NSDate dateWithTimeIntervalSince1970:0], @"a" : @[ @YES ] };
            NSData *x = [NSPropertyListSerialization dataWithPropertyList:pl format:NSPropertyListXMLFormat_v1_0 options:0 error:nil];
            out_key("xml"); out_q(x.bytes, x.length); out_end();
            NSData *o = [NSPropertyListSerialization dataWithPropertyList:pl format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
            out_key("binary"); out_d((int64_t)o.length); out_s(" "); out_d([[NSPropertyListSerialization propertyListWithData:o options:0 format:NULL error:nil] isEqual:pl]); out_end();
        }
        CASE("scanner and regex") {
            NSScanner *sc = [NSScanner scannerWithString:@"  42 apples, 3.5kg; 0x1F"];
            NSInteger i = 0;
            double d = 0;
            unsigned h = 0;
            NSString *w = nil;
            BOOL r1 = [sc scanInteger:&i], r2 = [sc scanUpToString:@"," intoString:&w];
            [sc scanString:@"," intoString:NULL];
            BOOL r3 = [sc scanDouble:&d];
            [sc scanUpToString:@"0x" intoString:NULL];
            BOOL r4 = [sc scanHexInt:&h];
            out_key("scanner"); out_d(r1); out_d(r2); out_d(r3); out_d(r4); out_s(" "); out_d(i); out_s(" "); out_ns(w);
            out_s(" "); out_dbl(d); out_s(" "); out_u(h); out_s(" at "); out_d((int64_t)sc.scanLocation); out_end();
            NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"(\\w+)@(\\w+)\\.com" options:NSRegularExpressionCaseInsensitive error:nil];
            NSString *text = @"mail Bob@Example.com or ann@site.com";
            out_key("matches"); out_d((int64_t)[re numberOfMatchesInString:text options:0 range:NSMakeRange(0, text.length)]); out_end();
            out_key("replace"); out_ns([re stringByReplacingMatchesInString:text options:0 range:NSMakeRange(0, text.length) withTemplate:@"<$2:$1>"]); out_end();
            NSTextCheckingResult *m = [re firstMatchInString:text options:0 range:NSMakeRange(0, text.length)];
            out_key("ranges"); out_ns(NSStringFromRange([m rangeAtIndex:1])); out_ns(NSStringFromRange([m rangeAtIndex:2])); out_end();
        }
        CASE("data and urls") {
            NSData *d = [@"any carnal pleasé" dataUsingEncoding:NSUTF8StringEncoding];
            out_key("base64"); out_ns([d base64EncodedStringWithOptions:0]); out_end();
            out_key("hex description"); out_ns([[NSData dataWithBytes:"\x01\xab\xff" length:3] description]); out_end();
            NSURLComponents *c = [NSURLComponents componentsWithString:@"http://h.com/p%20q?a=1&b=%C3%A9&a=2#f"];
            out_key("query items"); out_ns(c.queryItems); out_end();
            c.path = @"/x y/é";
            out_key("percent encoded path"); out_ns(c.percentEncodedPath); out_end();
            NSURL *u = [NSURL URLWithString:@"../z?q" relativeToURL:[NSURL URLWithString:@"https://a.b/c/d/e"]];
            out_key("relative"); out_ns(u.absoluteString); out_end();
            out_key("file url"); out_ns([NSURL fileURLWithPath:@"/tmp/a b" isDirectory:NO].absoluteString); out_end();
        }
        CASE("predicates") {
            NSArray *people = @[ @{ @"n" : @"Ann", @"a" : @31 }, @{ @"n" : @"bob", @"a" : @17 }, @{ @"n" : @"Éva", @"a" : @45 } ];
            NSPredicate *p = [NSPredicate predicateWithFormat:@"a > 18 AND n BEGINSWITH[cd] %@", @"e"];
            out_key("filter"); out_ns([people filteredArrayUsingPredicate:p]); out_end();
            out_key("format"); out_ns(p.predicateFormat); out_end();
            NSExpression *e = [NSExpression expressionWithFormat:@"(3 + 4) * 2 ** 3 / 5"];
            out_key("expression"); out_ns([e expressionValueWithObject:nil context:nil]); out_end();
            out_key("kvc"); out_ns([people valueForKeyPath:@"@avg.a"]); out_s(" "); out_ns([people valueForKeyPath:@"@max.n"]); out_end();
            NSSortDescriptor *sd = [NSSortDescriptor sortDescriptorWithKey:@"n" ascending:YES selector:@selector(localizedCaseInsensitiveCompare:)];
            out_key("sorted"); out_ns([[people sortedArrayUsingDescriptors:@[ sd ]] valueForKey:@"n"]); out_end();
        }
        CASE("values") {
            out_key("range"); out_ns([NSValue valueWithRange:NSMakeRange(3, 4)]); out_end();
            out_key("index set"); out_ns([NSIndexSet indexSetWithIndexesInRange:NSMakeRange(2, 5)]); out_end();
            NSMutableIndexSet *is = [NSMutableIndexSet indexSet];
            [is addIndex:NSNotFound - 1];
            out_key("big index"); out_d((int64_t)is.lastIndex); out_end();
            out_key("NSNotFound"); out_d(NSNotFound); out_s(" "); out_d([@[ @1 ] indexOfObject:@2]); out_end();
            out_key("process"); out_ns([NSProcessInfo processInfo].operatingSystemVersionString.length ? @"has version" : @"none");
            out_s(" cpus>0 "); out_d([NSProcessInfo processInfo].processorCount > 0); out_end();
            out_key("archiver");
            NSData *ar = [NSKeyedArchiver archivedDataWithRootObject:@{ @"k" : @[ @1, @"s" ] } requiringSecureCoding:NO error:nil];
            out_ns([NSKeyedUnarchiver unarchiveObjectWithData:ar]);
            out_end();
        }
    }
    return 0;
}
