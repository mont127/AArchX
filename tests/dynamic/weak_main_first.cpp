/*
 * A weak-def lookup takes the first image in load order that has weak
 * definitions and exports the symbol, and the main executable comes first
 * (dyld-1378, Loader::resolveSymbol).  This program defines its own weak
 * operator new(size_t, nothrow_t), which libc++ in the shared cache defines
 * too, so its new (std::nothrow) must reach its own.  Rosetta's dyld does
 * that; ocerz asked the cache first.  Also a template instantiated here and
 * called through a pointer, which binds the same way and only this program
 * defines.  The script builds it twice: with chained fixups, where these are
 * weak-def lookups by ordinal, and with -no_fixup_chains, where they are in
 * the weak-bind stream, which names no library.
 */
#include <cstdio>
#include <cstdlib>
#include <new>

static int own_new;

__attribute__((weak)) void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    own_new++;
    return std::malloc(n);
}

template <typename T> __attribute__((noinline)) T twice(T v) { return v + v; }

int main()
{
    int *p = new (std::nothrow) int(21);
    long (*volatile tw)(long) = twice<long>;
    long r = tw(*p);
    std::free(p);
    if (own_new != 1 || r != 42) {
        std::printf("own operator new used %d time(s), twice(21) = %ld\n", own_new, r);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
