/*
 * What Intel Mac applications from the 10.5 to 10.8 years reach that newer
 * programs do not, each found in one that native mode could not open
 * (Transmission, Sequel Pro, HandBrake, VLC 2.2): garbage-collector write
 * barriers, NSGradient's color and location pairs, positional format
 * arguments, a guest function stored into a host structure and called from
 * native code, NSMapTable and NSHashTable callbacks passed by value, a guest
 * method taking a union by value sent from guest code, and a superclass ivar
 * read through its _OBJC_IVAR_$ offset.  The arm64 build is the oracle.
 */
#define OBJC_SILENCE_GC_DEPRECATIONS 1
#import <AppKit/AppKit.h>
#include <objc/objc-auto.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

static void barriers(void)
{
    id a = @"alpha", b = @"beta";
    id slot = nil, weak = nil;
    struct { void *isa; id field; } holder = { NULL, nil };
    int global = objc_assign_global(a, &slot) == a && slot == a;
    int cast = objc_assign_strongCast(b, &slot) == b && slot == b;
    int local = objc_assign_threadlocal(a, &slot) == a && slot == a;
    int ivar = objc_assign_ivar(b, (id)(void *)&holder, offsetof(__typeof__(holder), field)) == b && holder.field == b;
    int weakrw = objc_assign_weak(a, &weak) == a && objc_read_weak(&weak) == a;
    char from[8] = "barrier", to[8] = "";
    int moved = objc_memmove_collectable(to, from, sizeof from) == to && !strcmp(to, "barrier");
    printf("gc barriers: global %d strongCast %d threadlocal %d ivar %d weak %d memmove %d collecting %d\n", global,
           cast, local, ivar, weakrw, moved, (int)objc_collectingEnabled());
}

static void gradient(void)
{
    NSGradient *g = [[NSGradient alloc] initWithColorsAndLocations:[NSColor redColor], 0.0, [NSColor greenColor], 0.25,
                                                                   [NSColor blueColor], 1.0, nil];
    NSColor *c = nil;
    CGFloat at = -1;
    [g getColor:&c location:&at atIndex:1];
    NSColor *rgb = [c colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    printf("gradient: stops %ld, second at %.2f, green %.1f\n", (long)g.numberOfColorStops, (double)at,
           (double)rgb.greenComponent);
    [g release];
}

static void positional(void)
{
    NSString *s = [NSString stringWithFormat:@"%2$@ %1$@ %3$d %4$.*3$f", @"one", @"two", 2, 3.14159];
    printf("positional: %s\n", s.UTF8String);
}

/* A guest function a program stores into the host's FILE, as HandBrake's
   logging does to catch everything written to stderr. */
static char g_caught[64];
static int caught_write(void *cookie, const char *buf, int n)
{
    (void)cookie;
    size_t have = strlen(g_caught);
    if (n > 0 && have + (size_t)n < sizeof g_caught)
        memcpy(g_caught + have, buf, (size_t)n);
    return n;
}

static void stored_function(void)
{
    int (*saved)(void *, const char *, int) = stderr->_write;
    stderr->_write = caught_write;
    fputs("into the hook", stderr);
    fflush(stderr);
    stderr->_write = saved;
    printf("stored function: caught '%s'\n", g_caught);
}

static int g_hashes, g_equals, g_describes;
static NSUInteger key_hash(NSMapTable *t, const void *k) { (void)t; g_hashes++; return strlen(k); }
static BOOL key_equal(NSMapTable *t, const void *a, const void *b) { (void)t; g_equals++; return !strcmp(a, b); }
static NSString *key_describe(NSMapTable *t, const void *k) { (void)t; g_describes++; return [NSString stringWithUTF8String:k]; }
static NSUInteger set_hash(NSHashTable *t, const void *k) { (void)t; g_hashes++; return strlen(k); }
static BOOL set_equal(NSHashTable *t, const void *a, const void *b) { (void)t; g_equals++; return !strcmp(a, b); }

static void tables(void)
{
    NSMapTableKeyCallBacks keys = { key_hash, key_equal, NULL, NULL, key_describe, NSNotAPointerMapKey };
    NSMapTable *m = NSCreateMapTable(keys, NSNonOwnedPointerMapValueCallBacks, 0);
    char one[] = "one", two[] = "two", other[] = "two";
    NSMapInsert(m, one, (void *)1);
    NSMapInsert(m, two, (void *)2);
    long found = (long)NSMapGet(m, other);
    NSString *d = NSStringFromMapTable(m);
    NSFreeMapTable(m);
    NSHashTableCallBacks set = { set_hash, set_equal, NULL, NULL, NULL };
    NSHashTable *h = NSCreateHashTableWithZone(set, 4, NULL);
    NSHashInsert(h, one);
    NSHashInsert(h, two);
    int member = NSHashGet(h, other) == two;
    unsigned count = (unsigned)NSCountHashTable(h);
    NSFreeHashTable(h);
    NSHashTable *plain = NSCreateHashTable(NSNonOwnedPointerHashCallBacks, 0);
    NSHashInsert(plain, one);
    int pointer = NSHashGet(plain, one) == one && NSHashGet(plain, other) == NULL;
    NSFreeHashTable(plain);
    printf("tables: map found %ld, described %d, set member %d count %u, pointer set %d, hashed %d, compared %d,"
           " described keys %d\n", found, [d rangeOfString:@"one"].location != NSNotFound, member, count, pointer,
           g_hashes > 0, g_equals > 0, g_describes > 0);
}

typedef union {
    long long i;
    double d;
    struct { int x, y; } pair;
} OcerzValue;

@interface OcerzUnionTaker : NSObject
- (long long)take:(OcerzValue)v kind:(int)kind;
@end

@implementation OcerzUnionTaker
- (long long)take:(OcerzValue)v kind:(int)kind
{
    return kind == 0 ? v.i + 1 : kind == 1 ? (long long)(v.d * 4) : v.pair.x * 100 + v.pair.y;
}
@end

static void union_send(void)
{
    OcerzUnionTaker *t = [[OcerzUnionTaker alloc] init];
    OcerzValue a = { .i = 41 }, b = { .d = 2.5 }, c = { .pair = { 7, 9 } };
    printf("union by value: %lld %lld %lld\n", [t take:a kind:0], [t take:b kind:1], [t take:c kind:2]);
    [t release];
}

#if __x86_64__
typedef long IvarOffset;
#else
typedef int IvarOffset;
#endif
extern IvarOffset style_mask_offset __asm__("_OBJC_IVAR_$_NSWindow._styleMask");

static void ivar_offset(void)
{
    Ivar iv = class_getInstanceVariable([NSWindow class], "_styleMask");
    printf("superclass ivar offset matches the runtime: %d\n", iv && (ptrdiff_t)style_mask_offset == ivar_getOffset(iv));
}

int main(void)
{
    @autoreleasepool {
        barriers();
        gradient();
        positional();
        stored_function();
        tables();
        union_send();
        ivar_offset();
    }
    return 0;
}
