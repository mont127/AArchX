/*
 * The one Objective-C file of m32: a host call that stops an NSException at the crossing, so m32 can
 * rethrow it into the guest (src/m32_objc_exc.c) instead of unwinding through the emulator.  Built without ARC.
 */
#include <stdint.h>

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
