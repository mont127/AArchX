/*
 * The x86 CPU sysctls an x86_64 process sees under Rosetta - hw.optional.* and
 * machdep.cpu.*, by name and through sysctlnametomib() - which must agree with
 * CPUID.  The arm64 kernel has none of these nodes, so passed through, every
 * one of them failed with ENOENT.
 */
#include <cpuid.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static int get(const char *n)
{
    int v = -1;
    size_t s = sizeof v;
    return sysctlbyname(n, &v, &s, NULL, 0) ? -2 : v;
}

int main(void)
{
    unsigned a, b, c, d;
    __cpuid(1, a, b, c, d);
    struct { const char *n; int bit; unsigned reg; } m[] = {
        { "hw.optional.sse2", 26, d },           { "hw.optional.sse3", 0, c },
        { "hw.optional.supplementalsse3", 9, c }, { "hw.optional.sse4_1", 19, c },
        { "hw.optional.sse4_2", 20, c },         { "hw.optional.aes", 25, c },
        { "hw.optional.avx1_0", 28, c },         { "hw.optional.f16c", 29, c },
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
        int want = (int)((m[i].reg >> m[i].bit) & 1), got = get(m[i].n);
        if (got != want) {
            printf("BAD %s=%d but CPUID says %d\n", m[i].n, got, want);
            return 1;
        }
    }
    if (get("hw.optional.x86_64") != 1) {
        printf("BAD hw.optional.x86_64\n");
        return 2;
    }
    char f[512] = { 0 };
    size_t fs = sizeof f - 1;
    if (sysctlbyname("machdep.cpu.features", f, &fs, NULL, 0) != 0 || !strstr(f, "SSE2") ||
        ((c >> 20) & 1) != (strstr(f, "SSE4.2") != NULL)) {
        printf("BAD machdep.cpu.features '%s'\n", f);
        return 3;
    }
    int mib[8], iv = -1;
    size_t ml = 8, is = sizeof iv;
    if (sysctlnametomib("hw.optional.sse4_1", mib, &ml) != 0 ||
        sysctl(mib, (unsigned)ml, &iv, &is, NULL, 0) != 0 || iv != (int)((c >> 19) & 1)) {
        printf("BAD sysctlnametomib path: %d\n", iv);
        return 4;
    }
    printf("OK\n");
    return 0;
}
