/*
 * sigsetjmp returns twice, which Rust cannot represent in a callable ABI.
 * Keep the setjmp frame in C and invoke Rust only from the guarded branch.
 */
#include "ocerz/jit_internal.h"

int jit_flags_guarded(void (*fn)(void *), void *arg)
{
    volatile int ok = 0;
    sigjmp_buf jb;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(jb, 0) == 0) {
        ocerz_jit_decode_recover = &jb;
        fn(arg);
        ok = 1;
    }
    ocerz_jit_decode_recover = prev;
    return ok;
}
