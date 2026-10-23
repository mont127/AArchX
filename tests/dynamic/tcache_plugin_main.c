/*
 * Loads the bundle named on its command line with dlopen and checks what its
 * plugin_work returns against the same computation done here, so a translation
 * cache test can see code that only a dlopen brings in.  Prints OK when they
 * agree.
 */
#include <dlfcn.h>
#include <stdio.h>

typedef unsigned long ul;

static ul ref_depth(ul n) { return n < 2 ? n + 1 : ref_depth(n - 1) + ref_depth(n - 2) % 1000003; }

static ul ref_work(ul n)
{
    ul acc = 1;
    for (ul i = 0; i < n; i++) {
        switch (i % 3) {
        case 0: acc = acc + i * 3; break;
        case 1: acc = (acc ^ i) + 7; break;
        default: acc = (acc << (i & 15)) | (acc >> (64 - (i & 15) - 1) >> 1); break;
        }
        acc %= 0xfffffffbul;
    }
    return acc + ref_depth(18);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    void *h = dlopen(argv[1], RTLD_NOW);
    ul (*work)(ul) = h ? (ul (*)(ul))dlsym(h, "plugin_work") : NULL;
    if (!work) {
        printf("cannot load %s: %s\n", argv[1], dlerror());
        return 1;
    }
    ul got = work(200000), want = ref_work(200000);
    printf(got == want ? "OK\n" : "MISMATCH %lu %lu\n", got, want);
    return got == want ? 0 : 1;
}
