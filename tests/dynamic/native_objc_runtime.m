#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>

static NSInteger value(id self, SEL command)
{
    Ivar ivar = class_getInstanceVariable(object_getClass(self), "_value");
    NSInteger result;
    memcpy(&result, (char *)(void *)self + ivar_getOffset(ivar), sizeof result);
    return result;
}

static double scale(id self, SEL command, double input)
{
    return input * 2.5;
}

static NSRect rectangle(id self, SEL command, NSRect input)
{
    input.origin.x += 7;
    input.size.height *= 3;
    return input;
}

static id label(id self, SEL command)
{
    return @"dynamic";
}

static NSInteger resolved(id self, SEL command, NSInteger input)
{
    return input + 19;
}

static double floating_value(id self, SEL command)
{
    return 7.25;
}

static NSInteger integer_value(id self, SEL command)
{
    return 73;
}

static long double extended_value(id self, SEL command)
{
    return 1.0L;
}

@interface OcerzDynamicResolver : NSObject
@end

@implementation OcerzDynamicResolver
+ (BOOL)resolveInstanceMethod:(SEL)selector
{
    if (selector == sel_registerName("resolvedValue:"))
        return class_addMethod(self, selector, (IMP)resolved, "q@:q");
    return [super resolveInstanceMethod:selector];
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        unsigned bad = 0;
        Class cls = objc_allocateClassPair([NSObject class], "OcerzRuntimeCreated", 0);
        if (argc > 1 && strcmp(argv[1], "unsupported") == 0) {
            class_addMethod(cls, sel_registerName("extendedValue"), (IMP)extended_value, "D@:");
            return 6;
        }
        if (!cls || !class_addIvar(cls, "_value", sizeof(NSInteger), 3, @encode(NSInteger)))
            return 2;
        SEL number = sel_registerName("dynamicValue");
        SEL fp = sel_registerName("scale:");
        SEL rect = sel_registerName("rectangle:");
        SEL text = sel_registerName("dynamicLabel");
        if (!class_addMethod(cls, number, (IMP)value, "q@:") ||
            class_addMethod(cls, number, (IMP)value, "q@:") ||
            !class_addMethod(cls, fp, (IMP)scale, "d@:d") ||
            !class_addMethod(cls, rect, (IMP)rectangle,
                            "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}") ||
            !class_addMethod(object_getClass(cls), text, (IMP)label, "@@:"))
            return 3;
        objc_registerClassPair(cls);
        id object = [[cls alloc] init];
        NSInteger stored = 0x123456789;
        Ivar ivar = class_getInstanceVariable(cls, "_value");
        memcpy((char *)(void *)object + ivar_getOffset(ivar), &stored, sizeof stored);
        if (((NSInteger (*)(id, SEL))objc_msgSend)(object, number) != stored)
            bad |= 1;
        if (((double (*)(id, SEL, double))objc_msgSend)(object, fp, 1.25) != 3.125)
            bad |= 2;
        NSRect input = NSMakeRect(1, 2, 3, 4);
        NSRect result;
#if defined(__x86_64__)
        ((void (*)(NSRect *, id, SEL, NSRect))objc_msgSend_stret)(&result, object, rect, input);
#else
        result = ((NSRect (*)(id, SEL, NSRect))objc_msgSend)(object, rect, input);
#endif
        if (!NSEqualRects(result, NSMakeRect(8, 2, 3, 12)))
            bad |= 4;
        if (![[cls performSelector:text] isEqualToString:@"dynamic"])
            bad |= 8;
        if (class_addMethod(Nil, number, (IMP)value, "q@:") ||
            objc_allocateClassPair([NSObject class], "OcerzRuntimeCreated", 0))
            bad |= 32;
        Class child = objc_allocateClassPair(cls, "OcerzRuntimeChild", 0);
        if (!child)
            return 4;
        objc_registerClassPair(child);
        id inherited = [[child alloc] init];
        if (((NSInteger (*)(id, SEL))objc_msgSend)(inherited, number) != 0)
            bad |= 64;
        if (!class_addMethod(child, number, (IMP)floating_value, "d@:") ||
            ((double (*)(id, SEL))objc_msgSend)(inherited, number) != 7.25)
            bad |= 64;
        [inherited release];
        objc_disposeClassPair(child);
        id resolver = [[OcerzDynamicResolver alloc] init];
        if (((NSInteger (*)(id, SEL, NSInteger))objc_msgSend)(resolver,
                sel_registerName("resolvedValue:"), 23) != 42)
            bad |= 16;
        [resolver release];
        [object release];
        objc_disposeClassPair(cls);
        for (int i = 0; i < 12; i++) {
            Class reused = objc_allocateClassPair([NSObject class], "OcerzRuntimeReused", 0);
            if (!reused || !class_addMethod(reused, number,
                    i & 1 ? (IMP)integer_value : (IMP)floating_value, i & 1 ? "q@:" : "d@:"))
                return 5;
            objc_registerClassPair(reused);
            id instance = [[reused alloc] init];
            if (i & 1) {
                if (((NSInteger (*)(id, SEL))objc_msgSend)(instance, number) != 73)
                    bad |= 128;
            } else if (((double (*)(id, SEL))objc_msgSend)(instance, number) != 7.25) {
                bad |= 128;
            }
            [instance release];
            objc_disposeClassPair(reused);
        }
        if (bad)
            printf("native_objc_runtime bad:%x\n", bad);
        else
            puts("native_objc_runtime ok");
        return bad != 0;
    }
}
