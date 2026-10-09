/*
 * This shim keeps the returns-twice sigsetjmp operation in C while the
 * translation-cache implementation moves to Rust and shares ocerz/jit_internal.h.
 */
#include <setjmp.h>

#include "ocerz/jit_internal.h"

int ocerz_tc_guard(void (*fn)(void *), void *arg);

int ocerz_tc_guard(void (*fn)(void *), void *arg)
{
    volatile int done = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        fn(arg);
        done = 1;
    }
    ocerz_jit_decode_recover = prev;
    return done;
}
