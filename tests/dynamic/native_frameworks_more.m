#import <AppKit/AppKit.h>
#import <Network/Network.h>
#import <Security/SecureTransport.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <xlocale.h>

static NSInteger by_value(id a, id b, void *ctx)
{
    (*(int *)ctx)++;
    return [(NSNumber *)a compare:(NSNumber *)b];
}

static NSComparisonResult by_tag(__kindof NSView *a, __kindof NSView *b, void *ctx)
{
    (*(int *)ctx)++;
    return [a tag] < [b tag] ? NSOrderedAscending : [a tag] > [b tag] ? NSOrderedDescending : NSOrderedSame;
}

@interface TagView : NSView {
    NSInteger _t;
}
@end
@implementation TagView
- (NSInteger)tag { return _t; }
- (void)setTag:(NSInteger)t { _t = t; }
@end

static void sorts(void)
{
    int calls = 0;
    NSArray *a = @[ @5, @3, @9, @1, @7 ];
    NSArray *s = [a sortedArrayUsingFunction:by_value context:&calls];
    int c1 = calls > 0;
    NSMutableArray *m = [a mutableCopy];
    calls = 0;
    [m sortUsingFunction:by_value context:&calls];
    int c2 = calls > 0;
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    for (NSNumber *n in a) {
        TagView *v = [[TagView alloc] initWithFrame:NSZeroRect];
        [v setTag:n.integerValue];
        [root addSubview:v];
    }
    calls = 0;
    [root sortSubviewsUsingFunction:by_tag context:&calls];
    NSMutableArray *tags = [NSMutableArray array];
    for (NSView *v in root.subviews)
        [tags addObject:@([v tag])];
    printf("sort %s %s %s called=%d%d%d\n", [[s componentsJoinedByString:@","] UTF8String],
           [[m componentsJoinedByString:@","] UTF8String], [[tags componentsJoinedByString:@","] UTF8String], c1, c2,
           calls > 0);
}

static void ciphers(void)
{
    SSLContextRef ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    size_t n = 0;
    OSStatus st = SSLGetNumberSupportedCiphers(ctx, &n);
    SSLCipherSuite *all = calloc(n ? n : 1, sizeof *all);
    size_t got = n;
    OSStatus st2 = SSLGetSupportedCiphers(ctx, all, &got);
    int has = 0;
    for (size_t k = 0; k < got; k++)
        has |= all[k] == TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256;
    SSLCipherSuite want[2] = { TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, TLS_RSA_WITH_AES_128_GCM_SHA256 };
    OSStatus st3 = SSLSetEnabledCiphers(ctx, want, 2);
    SSLCipherSuite back[8] = { 0 };
    size_t nb = 8;
    OSStatus st4 = SSLGetEnabledCiphers(ctx, back, &nb);
    size_t small = 1;
    SSLCipherSuite one[1] = { 0 };
    OSStatus st5 = SSLGetEnabledCiphers(ctx, one, &small);
    printf("ssl status=%d,%d,%d,%d supported=%d same=%d has=%d enabled=%zu %x,%x small=%d\n", (int)st, (int)st2,
           (int)st3, (int)st4, n > 0, got == n, has, nb, (unsigned)back[0], (unsigned)back[1], st5 != noErr);
    free(all);
    CFRelease(ctx);
}

static int scan_l(const char *s, locale_t l, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = vsscanf_l(s, l, f, ap);
    va_end(ap);
    return r;
}

static int print_l(char **o, locale_t l, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = vasprintf_l(o, l, f, ap);
    va_end(ap);
    return r;
}

static void locale_formats(void)
{
    locale_t c = newlocale(LC_ALL_MASK, "C", NULL);
    int a = 0, b = 0;
    double d = 0;
    char w[16] = "";
    int n1 = sscanf_l("42 3.5 word", c, "%d %lf %15s", &a, &d, w);
    int n2 = scan_l("7,9", c, "%d,%d", &b, &a);
    char *s1 = NULL, *s2 = NULL;
    int n3 = asprintf_l(&s1, c, "%s-%05.1f-%x", "ab", 2.5, 255);
    int n4 = print_l(&s2, NULL, "%d|%c", -3, 'z');
    printf("locale %d %d %.2f %s %d %d %d %s %d %s\n", n1, b, d, w, a, n2, n3, s1, n4, s2);
    free(s1);
    free(s2);
    freelocale(c);
}

static Boolean ci_equal(const void *a, const void *b)
{
    return CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

static CFHashCode ci_hash(const void *v)
{
    return (CFHashCode)CFStringGetLength(v);
}

static CFComparisonResult heap_compare(const void *a, const void *b, void *info)
{
    (void)info;
    intptr_t x = (intptr_t)a, y = (intptr_t)b;
    return x < y ? kCFCompareLessThan : x > y ? kCFCompareGreaterThan : kCFCompareEqualTo;
}

static void collections(void)
{
    CFSetCallBacks cb = kCFTypeSetCallBacks;
    cb.equal = ci_equal;
    cb.hash = ci_hash;
    const void *vals[3] = { CFSTR("Alpha"), CFSTR("ALPHA"), CFSTR("beta") };
    CFSetRef set = CFSetCreate(NULL, vals, 3, &cb);
    CFMutableSetRef ms = CFSetCreateMutable(NULL, 0, &cb);
    CFSetAddValue(ms, CFSTR("x"));
    CFSetAddValue(ms, CFSTR("X"));
    CFBinaryHeapCallBacks hcb = { 0, NULL, NULL, NULL, heap_compare };
    CFBinaryHeapRef heap = CFBinaryHeapCreate(NULL, 0, &hcb, NULL);
    intptr_t in[5] = { 40, 10, 50, 20, 30 };
    for (int k = 0; k < 5; k++)
        CFBinaryHeapAddValue(heap, (const void *)in[k]);
    printf("cf set=%ld mutable=%ld heap=%ld min=%ld\n", (long)CFSetGetCount(set), (long)CFSetGetCount(ms),
           (long)CFBinaryHeapGetCount(heap), (long)(intptr_t)CFBinaryHeapGetMinimum(heap));
    CFRelease(set);
    CFRelease(ms);
    CFRelease(heap);
}

@protocol BlockMethods
- (NSInteger)addTo:(NSInteger)x;
- (NSRect)rectWithWidth:(CGFloat)w;
@end

@interface BlockHolder : NSObject
@property NSInteger base;
@end
@implementation BlockHolder
@end

static void block_imps(void)
{
    Class cls = objc_allocateClassPair([BlockHolder class], "OcerzBlockMethods", 0);
    __block int calls = 0;
    IMP add = imp_implementationWithBlock(^NSInteger(BlockHolder *self, NSInteger x) {
        calls++;
        return self.base + x;
    });
    IMP rect = imp_implementationWithBlock(^NSRect(BlockHolder *self, CGFloat w) {
        calls++;
        return NSMakeRect(self.base, 2, w, w * 2);
    });
    class_addMethod(cls, @selector(addTo:), add, "q@:q");
    class_addMethod(cls, @selector(rectWithWidth:), rect, "{CGRect={CGPoint=dd}{CGSize=dd}}@:d");
    objc_registerClassPair(cls);
    BlockHolder *h = [[cls alloc] init];
    h.base = 40;
    id<BlockMethods> d = (id<BlockMethods>)h;
    NSInteger direct = [d addTo:2];
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[h methodSignatureForSelector:@selector(addTo:)]];
    inv.selector = @selector(addTo:);
    NSInteger arg = 5;
    [inv setArgument:&arg atIndex:2];
    [inv invokeWithTarget:h];
    NSInteger invoked = 0;
    [inv getReturnValue:&invoked];
    NSRect r = [d rectWithWidth:3];
    id got = imp_getBlock(add);
    BOOL removed = imp_removeBlock(rect);
    printf("block imps direct=%ld invoked=%ld rect=%.0f,%.0f,%.0f,%.0f calls=%d getBlock=%d removed=%d\n", (long)direct,
           (long)invoked, r.origin.x, r.origin.y, r.size.width, r.size.height, calls, got != nil, removed);
}

static void on_uncaught(NSException *e)
{
    (void)e;
}

static void uncaught_handlers(void)
{
    NSUncaughtExceptionHandler *before = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(on_uncaught);
    NSUncaughtExceptionHandler *mine = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(before);
    printf("uncaught handler before=%d mine=%d restored=%d\n", before != NULL, mine == on_uncaught,
           NSGetUncaughtExceptionHandler() == before);
}

static void bindings(void)
{
    NSString *one = @"1", *two = @"2", *three = @"3";
    NSArray *dicts = @[ NSDictionaryOfVariableBindings(one), NSDictionaryOfVariableBindings(one, two, three),
                        _NSDictionaryOfVariableBindings(@"  x ,y,   z  ", one, two, three, nil),
                        _NSDictionaryOfVariableBindings(@"p,q", one, two, three, nil) ];
    printf("bindings");
    for (NSDictionary *d in dicts) {
        printf(" %lu:", (unsigned long)d.count);
        for (NSString *k in [[d allKeys] sortedArrayUsingSelector:@selector(compare:)])
            printf("%s=%s,", k.UTF8String, [d[k] UTF8String]);
    }
    printf("\n");
}

static void frameworks(void)
{
    UTType *png = [UTType typeWithFilenameExtension:@"png"];
    printf("uttype %s %s conforms=%d\n", [UTTypePNG.identifier UTF8String], [png.identifier UTF8String],
           [png conformsToType:UTTypeImage]);
    nw_path_monitor_t mon = nw_path_monitor_create();
    nw_path_monitor_set_queue(mon, dispatch_get_global_queue(0, 0));
    nw_path_monitor_set_update_handler(mon, ^(nw_path_t path) { (void)nw_path_get_status(path); });
    nw_path_monitor_start(mon);
    nw_path_monitor_cancel(mon);
    printf("network monitor=%d\n", mon != NULL);
}

int main(void)
{
    @autoreleasepool {
        sorts();
        ciphers();
        locale_formats();
        collections();
        frameworks();
        bindings();
        block_imps();
        uncaught_handlers();
    }
    return 0;
}
