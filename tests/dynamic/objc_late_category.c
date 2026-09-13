#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>

int main(void)
{
    id (*msg_cstr)(id, SEL, const char *) = (id (*)(id, SEL, const char *))objc_msgSend;
    id heap = msg_cstr((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
                       "a heap string that is too long to be tagged");
    id constant = (id)CFSTR("a constant string");
    SEL sel = sel_registerName("encodeWithCSCoder:");
    if (!heap || class_getInstanceMethod(object_getClass(heap), sel) ||
        class_getInstanceMethod(object_getClass(constant), sel)) {
        printf("BAD before\n");
        return 1;
    }
    if (!dlopen("/System/Library/Frameworks/CoreSpotlight.framework/CoreSpotlight", RTLD_NOW)) {
        printf("BAD dlopen\n");
        return 1;
    }
    if (!class_getInstanceMethod(object_getClass(heap), sel)) {
        printf("BAD heap string\n");
        return 1;
    }
    if (!class_getInstanceMethod(object_getClass(constant), sel)) {
        printf("BAD constant string\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
