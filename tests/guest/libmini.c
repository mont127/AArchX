/*
 * tests/guest/libmini.c
 *
 * Minimal freestanding C runtime support for the Ocerz guest tests. Built
 * with -nostdlib there is no libc, yet clang at -O2 still emits calls to
 * memcpy/memset/memmove/bzero (for struct copies, array zeroing, and large
 * literal initializers) and the test programs themselves use the str*
 * helpers. This file provides correct, simple, byte-at-a-time implementations
 * of exactly those functions so the guest binaries link with no undefined
 * symbol.
 *
 * Every memory primitive carries __attribute__((no_builtin)): without it,
 * clang's loop-idiom recognizer would rewrite each function's own byte loop
 * back into a call to the very libcall it is implementing (memset -> bzero,
 * bzero -> bzero, memcpy -> memcpy), producing an unresolvable self-reference
 * at link time. no_builtin keeps the explicit loops intact. The project's
 * fixed test-compile flag set is preserved (no global -fno-builtin), so the
 * test programs are still free to emit memcpy/memset/sqrtsd as intended.
 *
 * The implementations are deliberately scalar and obvious rather than fast:
 * the point is reference correctness when run under Ocerz, and a forward/
 * backward-aware memmove so overlapping copies are right. They are non-static
 * with external linkage because the compiler-generated calls reference them
 * by name. strings.c additionally exercises rep movsb/stosb/scasb directly in
 * inline asm to guarantee those string instructions appear in a guest image.
 * bzero and its internal alias __bzero are provided because clang lowers
 * memset(p,0,n) to the platform __bzero entry on this target.
 */

typedef unsigned long um_size;

__attribute__((no_builtin)) void *memcpy(void *dst, const void *src, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (um_size i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

__attribute__((no_builtin)) void *memset(void *dst, int c, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    for (um_size i = 0; i < n; i++)
        d[i] = v;
    return dst;
}

__attribute__((no_builtin)) void __bzero(void *dst, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    for (um_size i = 0; i < n; i++)
        d[i] = 0;
}

__attribute__((no_builtin)) void bzero(void *dst, um_size n)
{
    __bzero(dst, n);
}

__attribute__((no_builtin)) void *memmove(void *dst, const void *src, um_size n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        for (um_size i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (um_size i = n; i != 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

__attribute__((no_builtin)) int memcmp(const void *a, const void *b, um_size n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (um_size i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

__attribute__((no_builtin)) um_size strlen(const char *s)
{
    um_size n = 0;
    while (s[n])
        n++;
    return n;
}

__attribute__((no_builtin)) int strcmp(const char *a, const char *b)
{
    um_size i = 0;
    while (a[i] != 0 && a[i] == b[i])
        i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}
