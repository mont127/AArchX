/*
 * Keep the cache failure recorder's signal recovery boundary in C: sigsetjmp
 * must return to the same C frame after a fault while copying guest bytes.
 */
#include "ocerz/jit_internal.h"

#include <string.h>

void jit_cache_copy8_recover(void *dst, const void *src)
{
    sigjmp_buf bb, *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(bb, 0) == 0) {
        ocerz_jit_decode_recover = &bb;
        memcpy(dst, src, 8);
    }
    ocerz_jit_decode_recover = prev;
}
