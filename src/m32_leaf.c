/*
 * i386 imports the JIT calls straight from an import stub's block (src/jit.c emit_bridge_fastcall32), with no
 * crossing: the small libc functions Batman: Arkham Asylum's threads call millions of times a second
 * (pthread_[gs]etspecific, wcscmp, memcpy, memcmp, strlen, bzero, the clock), the CGL context calls Feral's
 * IndirectX wraps every OpenGL call in, and OpenGL functions that take only integers, booleans and pointers.
 *
 * This file is built with -mgeneral-regs-only (Makefile): the guest's xmm registers stay pinned in vector registers
 * across the call, so nothing here may touch a SIMD register.  The libc functions called from here
 * (pthread_*specific, clock_gettime_nsec_np, __tolower) are integer code; the CGL and OpenGL leaves call OpenGL,
 * which may not be, so they are marked simd and the JIT saves xmm0-7 around them.
 *
 * Pointer arguments arrive as host addresses (guest base + guest address).  Results are what the m32 specials
 * these stand in for answer (src/m32_libsystem.c, src/m32_thread.c, src/m32_stdio.c), byte for byte.  The copies
 * are plain word loops: a copy of megabytes runs about 8 bytes a cycle, which is fine for what a game copies.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ocerz/bridge.h"
#include "ocerz/m32.h"

static uint64_t leaf_memmove(uint8_t *d, const uint8_t *s, uint32_t n)
{
    if (d == s || !n)
        return 0;
    if (d < s || d >= s + n) {   /* forward */
        while (n >= 8) {
            uint64_t v;
            __builtin_memcpy(&v, s, 8);   /* unaligned 8-byte moves: ldr/str x, no SIMD under -mgeneral-regs-only */
            __builtin_memcpy(d, &v, 8);
            d += 8, s += 8, n -= 8;
        }
        while (n--)
            *d++ = *s++;
    } else {                    /* backward, overlapping */
        d += n, s += n;
        while (n >= 8) {
            uint64_t v;
            d -= 8, s -= 8, n -= 8;
            __builtin_memcpy(&v, s, 8);
            __builtin_memcpy(d, &v, 8);
        }
        while (n--)
            *--d = *--s;
    }
    return 0;
}
static uint64_t leaf_memset(uint8_t *d, uint32_t c, uint32_t n)
{
    uint64_t v = (uint8_t)c * 0x0101010101010101ull;
    for (; n >= 8; d += 8, n -= 8)
        __builtin_memcpy(d, &v, 8);
    while (n--)
        *d++ = (uint8_t)c;
    return 0;
}
static uint64_t leaf_bzero(uint8_t *d, uint32_t n) { return leaf_memset(d, 0, n); }
static uint64_t leaf_memcmp(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (; n; a++, b++, n--)
        if (*a != *b)
            return (uint32_t)((int)*a - (int)*b);
    return 0;
}
static uint64_t leaf_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (uint32_t)(p - s);
}
/* wchar_t is 4 bytes on both; FreeBSD's (and so the host's) wcscmp: the difference as unsigned ints */
static uint64_t leaf_wcscmp(const uint32_t *a, const uint32_t *b)
{
    while (*a == *b++)
        if (*a++ == 0)
            return 0;
    return (uint32_t)(*a - *--b);
}
static uint64_t leaf_getspecific(uint32_t key) { return (uint32_t)(uintptr_t)pthread_getspecific(key); }
static uint64_t leaf_setspecific(uint32_t key, uint32_t v)
{
    m32_tsd_set(key, v);
    return (uint32_t)pthread_setspecific(key, (void *)(uintptr_t)v);
}
static uint64_t leaf_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   /* m32_libsystem.c now_ns */
static uint64_t leaf_tolower(uint32_t c) { return (uint32_t)__tolower((int)c); }

/* the CGL context calls Feral's IndirectX wraps every GL call in (~1M a second): m32_gl.c's specials, as leaves.
 * OpenGL's own code may use SIMD registers (simd = 1 in the table) */
#define GL_LIB "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL"
#define CGL_LEAF(name)                                                                                   \
    static uint64_t leaf_##name(uint32_t ctx)                                                            \
    {                                                                                                    \
        static int (*f)(void *);                                                                         \
        if (!f)                                                                                          \
            f = (int (*)(void *))ocerz_bridge_host_symbol(GL_LIB, #name);                                \
        return (uint32_t)(f ? f(m32_host(ctx)) : 10000);   /* kCGLBadAttribute */                        \
    }
CGL_LEAF(CGLSetCurrentContext)
CGL_LEAF(CGLLockContext)
CGL_LEAF(CGLUnlockContext)

static uint64_t leaf_pthread_self(void) { return m32_handle(pthread_self()); }   /* m32_thread.c sp_pthread_self */
/* the float's bits, as it sits on the guest stack */
static uint64_t leaf_isinff(uint32_t bits) { return (bits & 0x7fffffffu) == 0x7f800000u; }
static uint64_t leaf_isnanf(uint32_t bits) { return (bits & 0x7fffffffu) > 0x7f800000u; }

static const M32Leaf g_leaves[] = {
    { "_pthread_self", (void *)leaf_pthread_self, 0, 0, M32_LEAF_I32, 0, 0, 0 },
    { "___isinff", (void *)leaf_isinff, 1, 0, M32_LEAF_I32, 0, 0, 0 },
    { "___isnanf", (void *)leaf_isnanf, 1, 0, M32_LEAF_I32, 0, 0, 0 },
    { "_CGLSetCurrentContext", (void *)leaf_CGLSetCurrentContext, 1, 0, M32_LEAF_I32, 1, 0, 0 },
    { "_CGLLockContext", (void *)leaf_CGLLockContext, 1, 0, M32_LEAF_I32, 1, 0, 0 },
    { "_CGLUnlockContext", (void *)leaf_CGLUnlockContext, 1, 0, M32_LEAF_I32, 1, 0, 0 },
    { "_memcpy", (void *)leaf_memmove, 3, 3, M32_LEAF_ARG0, 0, 0, 0 },
    { "_memmove", (void *)leaf_memmove, 3, 3, M32_LEAF_ARG0, 0, 0, 0 },
    { "_memset", (void *)leaf_memset, 3, 1, M32_LEAF_ARG0, 0, 0, 0 },
    { "___bzero", (void *)leaf_bzero, 2, 1, M32_LEAF_VOID, 0, 0, 0 },
    { "_bzero", (void *)leaf_bzero, 2, 1, M32_LEAF_VOID, 0, 0, 0 },
    { "_memcmp", (void *)leaf_memcmp, 3, 3, M32_LEAF_I32, 0, 0, 0 },
    { "_strlen", (void *)leaf_strlen, 1, 1, M32_LEAF_I32, 0, 0, 0 },
    { "_wcscmp", (void *)leaf_wcscmp, 2, 3, M32_LEAF_I32, 0, 0, 0 },
    { "_pthread_getspecific", (void *)leaf_getspecific, 1, 0, M32_LEAF_I32, 0, 0, 0 },
    { "_pthread_setspecific", (void *)leaf_setspecific, 2, 0, M32_LEAF_I32, 0, 0, 0 },
    { "_mach_absolute_time", (void *)leaf_now, 0, 0, M32_LEAF_I64, 0, 0, 0 },
    { "_UpTime", (void *)leaf_now, 0, 0, M32_LEAF_I64, 0, 0, 0 },
    { "___tolower", (void *)leaf_tolower, 1, 0, M32_LEAF_I32, 0, 0, 0 },
    { "_OSMemoryBarrier", NULL, 0, 0, M32_LEAF_VOID, 0, 0, 0 },   /* the JIT emits the barrier itself */
};

/* any GL function whose guest and host notations agree and that takes 32-bit integers, booleans and pointers and
 * returns an integer or nothing (glEnable, glBindTexture, glVertexAttribPointer, glDrawRangeElementsBaseVertex,
 * glProgramLocalParameters4fvEXT...).  All integers: the host function itself is the leaf; with pointers or
 * booleans: through m32_leaf_tramp.  Floats, 64-bit values and anything with a special keep the crossing. */
static M32Leaf g_gl[8192];
static const M32Leaf *gl_leaf(uint32_t id, const M32Export *e)
{
    if (id >= 8192 || e->kind != M32_EX_FN || e->special || !e->host_fn || strncmp(e->name, "_gl", 3) ||
        !e->gnote || !e->hnote || strcmp(e->gnote, e->hnote))
        return NULL;
    const char *n = e->gnote;
    if ((n[0] != 'v' && n[0] != 'i' && n[0] != 'u') || n[1] != '(')
        return NULL;
    int k = 0;
    unsigned ptrs = 0, bytes = 0;
    for (n += 2; *n && *n != ')'; n++, k++) {
        if (k == 7)
            return NULL;
        if (*n == 'p') ptrs |= 1u << k;
        else if (*n == 'B') bytes |= 1u << k;
        else if (*n != 'i' && *n != 'u') return NULL;
    }
    if (*n != ')')
        return NULL;
    if (!g_gl[id].fn)
        g_gl[id] = (M32Leaf){ e->name, e->host_fn, (uint8_t)k, (uint8_t)ptrs,
                              (uint8_t)(e->gnote[0] == 'v' ? M32_LEAF_VOID : M32_LEAF_I32), 1, (uint8_t)(ptrs || bytes),
                              (uint8_t)bytes };
    return &g_gl[id];
}

uint64_t m32_leaf_tramp(const M32Leaf *l, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6)
{
    uint64_t a[7] = { a0, a1, a2, a3, a4, a5, a6 };
    for (int k = 0; k < l->nargs; k++) {
        if (l->ptrs >> k & 1) a[k] = (uint64_t)(uintptr_t)m32_host((uint32_t)a[k]);
        else if (l->bytes >> k & 1) a[k] &= 0xff;
    }
    typedef uint64_t (*F7)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    return ((F7)l->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
}

const M32Leaf *m32_leaf_for(uint32_t trap)
{
    uint32_t id = trap - OCERZ_DYLDAPI_LO;
    M32Export *e = m32_export_by_id(id);
    if (!e || !e->name)
        return NULL;
    for (size_t i = 0; i < sizeof g_leaves / sizeof g_leaves[0]; i++)
        if (!strcmp(e->name, g_leaves[i].name))
            return &g_leaves[i];
    static int no_gl = -1;
    if (no_gl < 0) no_gl = getenv("OCERZ_NO_M32_GL_LEAF") ? 1 : 0;
    return no_gl ? NULL : gl_leaf(id, e);
}
