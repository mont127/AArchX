/*
 * The one Objective-C file of m32: a host call that stops an NSException at the crossing, so m32 can
 * rethrow it into the guest (src/m32_objc_exc.c) instead of unwinding through the emulator.  Built without ARC.
 */
#import <objc/NSObject.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>

void ocerz_abi_call_native(const void *fn, const uint64_t *x, const uint64_t *v, const uint64_t *stack,
                           uint64_t stackbytes, void *x8, uint64_t *out_x, uint64_t *out_v);

int m32_call_native_catching(const void *fn, const uint64_t *x, const uint64_t *v, const uint64_t *stack,
                             uint64_t stackbytes, void *x8, uint64_t *out_x, uint64_t *out_v, void **exception)
{
    *exception = 0;
    @try {
        ocerz_abi_call_native(fn, x, v, stack, stackbytes, x8, out_x, out_v);
        return 0;
    } @catch (id e) {
        *exception = (void *)[e retain];
        return 1;
    }
}

/* "name: reason" of a host exception, for OCERZ_M32LOG=throws (ocerz links no Foundation: by message) */
void m32_describe_exception(void *exception, char *buf, unsigned long size)
{
    id e = (id)exception;
    id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
    Class nsexc = objc_getClass("NSException");
    BOOL (*kind)(id, SEL, Class) = (BOOL (*)(id, SEL, Class))objc_msgSend;
    if (nsexc && kind(e, sel_registerName("isKindOfClass:"), nsexc)) {
        id name = send(e, sel_registerName("name")), reason = send(e, sel_registerName("reason"));
        SEL u = sel_registerName("UTF8String");
        snprintf(buf, size, "%s: %s", name ? utf8(name, u) : "?", reason ? utf8(reason, u) : "");
    } else {
        snprintf(buf, size, "%s", object_getClassName(e));
    }
}
