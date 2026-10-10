/*
 * The string and memory routines src/jit.c answers in place (src/leaf.s), on
 * memory in every place the Wine layout maps differently: the image's own
 * data, which that build puts below 12 GB, malloc's, a mapping at 5 GB, a
 * page that ends at 12 GB, ranges and strings that run across 12 GB, and the
 * top strip.  Under ocerz 12 GB itself is taken, and the run across it falls
 * back to an ordinary pair of pages: ocerz translates no guest access that
 * straddles 12 GB or the top strip's start, in any mode, since each side has
 * its own host base.  The routines are called through pointers, so each is a
 * real call into libsystem_platform, and every answer is checked against byte
 * loops: lengths, the sign of a comparison, the pointer memchr and strchr
 * return, the first argument memcpy, memmove and memset return, and the bytes
 * either side of a destination.  memmove is also given overlapping ranges,
 * which the in-place routine leaves to the translation, and memcpy a
 * destination on a read-only page, whose handler opens it and sees the guest's
 * address.  The same source is built both ways; where the build is not the
 * Wine layout's the placements fall back to ordinary mappings, so the output
 * is the same.  Against Rosetta.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>

static void *(*volatile p_memcpy)(void *, const void *, size_t) = memcpy;
static void *(*volatile p_memmove)(void *, const void *, size_t) = memmove;
static void *(*volatile p_memset)(void *, int, size_t) = memset;
static int (*volatile p_memcmp)(const void *, const void *, size_t) = memcmp;
static int (*volatile p_bcmp)(const void *, const void *, size_t) = bcmp;
static void *(*volatile p_memchr)(const void *, int, size_t) = memchr;
static int (*volatile p_strncmp)(const char *, const char *, size_t) = strncmp;
static size_t (*volatile p_strnlen)(const char *, size_t) = strnlen;
static size_t (*volatile p_strlen)(const char *) = strlen;
static int (*volatile p_strcmp)(const char *, const char *) = strcmp;
static char *(*volatile p_strchr)(const char *, int) = strchr;

static int g_low, g_bad;
static unsigned char g_data[3][0x6000];

static void bad(const char *where, const char *what, long a, long b)
{
    if (g_bad++ < 20) printf("%s: %s %ld, want %ld\n", where, what, a, b);
}

static int sign(int v) { return v < 0 ? -1 : v > 0; }

static void fill(volatile unsigned char *p, size_t n, unsigned s)
{
    for (size_t i = 0; i < n; i++) { s = s * 1103515245u + 12345u; p[i] = (unsigned char)(s >> 16) | 1; }
}

static int ref_cmp(const volatile unsigned char *a, const volatile unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static size_t ref_len(const volatile char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Every routine on a destination and a source of len + 64 bytes each. */
static void exercise(const char *where, unsigned char *dst, unsigned char *src, size_t room)
{
    static const size_t lens[] = { 0, 1, 7, 15, 16, 17, 63, 64, 65, 127, 300, 1000, 4096 };
    for (unsigned k = 0; k < sizeof lens / sizeof lens[0]; k++) {
        size_t n = lens[k];
        if (n + 64 > room) continue;
        for (size_t off = 0; off < 3; off++) {
            unsigned char *d = dst + 16 + off, *s = src + 16 + 2 * off;
            fill(src, room, (unsigned)(n * 7 + off));
            fill(dst, room, (unsigned)(n * 13 + off + 1));
            unsigned char before = d[-1], after = d[n];
            if (p_memcpy(d, s, n) != d) bad(where, "memcpy result", 0, 1);
            if (ref_cmp(d, s, n) || d[-1] != before || d[n] != after) bad(where, "memcpy bytes", (long)n, (long)off);
            if (p_memset(d, 0x5a + (int)n, n) != d) bad(where, "memset result", 0, 1);
            for (size_t i = 0; i < n; i++)
                if (((volatile unsigned char *)d)[i] != (unsigned char)(0x5a + n)) { bad(where, "memset bytes", (long)i, (long)n); break; }
            if (d[-1] != before || d[n] != after) bad(where, "memset edge", (long)n, (long)off);
            p_memcpy(d, s, n);
            if (n) d[n / 2] ^= 0x40;
            int c1 = sign(p_memcmp(d, s, n)), c2 = sign(ref_cmp(d, s, n));
            if (c1 != c2) bad(where, "memcmp", c1, c2);
            if ((p_bcmp(d, s, n) != 0) != (c2 != 0)) bad(where, "bcmp", c1, c2);
            if (n) {
                ((volatile unsigned char *)s)[n - 1] = 0;
                d[n / 2] = 0x80;
                void *got = p_memchr(s, 0, n), *want = s + n - 1;
                if (got != want) bad(where, "memchr", (long)((unsigned char *)got - s), (long)(n - 1));
                if (p_memchr(d, 0x80, n) != d + n / 2) bad(where, "memchr hit", 0, (long)(n / 2));
                if (p_memchr(s, 0, n - 1) != NULL) bad(where, "memchr miss", 1, 0);
                size_t want_len = ref_len((char *)s);
                if (p_strlen((char *)s) != want_len) bad(where, "strlen", (long)p_strlen((char *)s), (long)want_len);
                if (p_strnlen((char *)s, n / 2) != (want_len < n / 2 ? want_len : n / 2)) bad(where, "strnlen short", 0, 0);
                if (p_strnlen((char *)s, SIZE_MAX) != want_len) bad(where, "strnlen whole", 0, 0);
                p_memcpy(d, s, n);
                if (sign(p_strcmp((char *)d, (char *)s)) != 0) bad(where, "strcmp equal", 1, 0);
                if (p_strncmp((char *)d, (char *)s, n + 8) != 0) bad(where, "strncmp equal", 1, 0);
                if (n > 2) {
                    s[n - 2] = 0x41;
                    d[n - 2] = 0x42;
                    if (sign(p_strcmp((char *)d, (char *)s)) != 1) bad(where, "strcmp more", 0, 1);
                    if (sign(p_strncmp((char *)d, (char *)s, n)) != 1) bad(where, "strncmp more", 0, 1);
                    if (p_strncmp((char *)d, (char *)s, n - 2) != 0) bad(where, "strncmp short", 1, 0);
                    char want_c = (char)s[n / 3];
                    char *hit = p_strchr((char *)s, want_c), *ref = (char *)s;
                    while (*ref != want_c) ref++;
                    if (hit != ref) bad(where, "strchr", (long)(hit - (char *)s), (long)(ref - (char *)s));
                }
            }
        }
    }
    /* overlapping moves, both ways, which the in-place memmove leaves to the translation */
    for (size_t n = 8; n + 80 <= room && n <= 2048; n = n * 3 + 5) {
        fill(dst, room, (unsigned)n);
        unsigned char ref[2200];
        for (size_t i = 0; i < n; i++) ref[i] = ((volatile unsigned char *)dst)[i + 16];
        if (p_memmove(dst + 21, dst + 16, n) != dst + 21) bad(where, "memmove result", 0, 1);
        if (ref_cmp(dst + 21, ref, n)) bad(where, "memmove up", (long)n, 0);
        for (size_t i = 0; i < n; i++) ref[i] = ((volatile unsigned char *)dst)[i + 21];
        p_memmove(dst + 3, dst + 21, n);
        if (ref_cmp(dst + 3, ref, n)) bad(where, "memmove down", (long)n, 0);
    }
}

/* Ranges and strings that end exactly at the end of a mapping. */
static void ends_at(const char *where, unsigned char *end)
{
    static const size_t lens[] = { 1, 2, 15, 16, 17, 64, 65, 333, 4096 };
    unsigned char src[4200];
    for (unsigned k = 0; k < sizeof lens / sizeof lens[0]; k++) {
        size_t n = lens[k];
        unsigned char *s = end - n;
        fill(src, sizeof src, (unsigned)n);
        if (p_memcpy(s, src, n) != s || ref_cmp(s, src, n)) bad(where, "memcpy to the end", (long)n, 0);
        if (p_memcmp(s, src, n) != 0 || p_bcmp(src, s, n) != 0) bad(where, "memcmp to the end", (long)n, 0);
        if (p_memchr(s, src[n - 1], n) != memchr(src, src[n - 1], n) - (void *)src + (void *)s)
            bad(where, "memchr to the end", (long)n, 0);
        ((volatile unsigned char *)s)[n - 1] = 0;
        if (p_strlen((char *)s) != n - 1) bad(where, "strlen to the end", (long)p_strlen((char *)s), (long)n - 1);
        if (p_strnlen((char *)s, n) != n - 1) bad(where, "strnlen to the end", (long)n, 0);
        if (p_strnlen((char *)s, SIZE_MAX) != n - 1) bad(where, "strnlen past the end", (long)n, 0);
        if (p_memchr(s, 0, n) != s + n - 1) bad(where, "memchr nul at the end", (long)n, 0);
        if (p_strncmp((char *)s, (char *)s, n + 100) != 0) bad(where, "strncmp to the end", (long)n, 0);
        if (p_memset(s, 0x11, n) != s) bad(where, "memset to the end", (long)n, 0);
        if (((volatile unsigned char *)s)[n - 1] != 0x11) bad(where, "memset last byte", (long)n, 0);
    }
}

/* A string and a range that run across the end of a mapping into the next one. */
static void across(const char *where, unsigned char *edge)
{
    unsigned char *s = edge - 100;
    fill(s - 64, 512, 77);
    ((volatile unsigned char *)s)[300] = 0;
    if (p_strlen((char *)s) != 300) bad(where, "strlen across", (long)p_strlen((char *)s), 300);
    if (p_strnlen((char *)s, 200) != 200) bad(where, "strnlen across", (long)p_strnlen((char *)s, 200), 200);
    if (p_strnlen((char *)s, 1000) != 300) bad(where, "strnlen across end", (long)p_strnlen((char *)s, 1000), 300);
    if (p_memchr(s, 0, 400) != s + 300) bad(where, "memchr across", 0, 300);
    unsigned char out[600];
    if (p_memcpy(out, s, 260) != out || ref_cmp(out, s, 260)) bad(where, "memcpy from across", 0, 0);
    if (p_memcmp(out, s, 260) != 0) bad(where, "memcmp across", 1, 0);
    out[250] ^= 1;
    if (sign(p_memcmp(s, out, 260)) != sign(ref_cmp(s, out, 260))) bad(where, "memcmp across differs", 0, 0);
    if (p_strncmp((char *)s, (char *)s, 1000) != 0) bad(where, "strncmp across", 1, 0);
    if (p_strcmp((char *)s, (char *)s) != 0) bad(where, "strcmp across", 1, 0);
    if (p_memset(s, 0x33, 250) != s) bad(where, "memset across", 0, 1);
    for (int i = 0; i < 250; i++)
        if (((volatile unsigned char *)s)[i] != 0x33) { bad(where, "memset across bytes", i, 0x33); break; }
    fill(out, sizeof out, 5);
    if (p_memcpy(s, out, 220) != s || ref_cmp(s, out, 220)) bad(where, "memcpy into across", 0, 0);
}

/*
 * Memory exactly where the Wine layout's build asks for it, when nothing is
 * there (a fixed allocation that may not replace anything, so Rosetta's own
 * mappings are safe), and anywhere otherwise.
 */
static void *map_at(uint64_t want, size_t len, const char *what)
{
    mach_vm_address_t a = want;
    void *p;
    if (g_low && mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED) == KERN_SUCCESS) {
        p = (void *)(uintptr_t)a;
    } else {
        p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return NULL;
    }
    fprintf(stderr, "leaf_low: %s at %p%s\n", what, p, (uintptr_t)p == want ? "" : " (not where asked)");
    return p;
}

static volatile uintptr_t g_fault_at;
static unsigned char *g_ro;

static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    g_fault_at = (uintptr_t)si->si_addr;
    mprotect(g_ro, 0x4000, PROT_READ | PROT_WRITE);
}

int main(void)
{
    g_low = (uintptr_t)g_data - 0x200000000ull < 0x100000000ull;

    exercise("image", g_data[0], g_data[1], sizeof g_data[0]);
    unsigned char *h1 = malloc(0x6000), *h2 = malloc(0x6000);
    exercise("malloc", h1, h2, 0x6000);
    exercise("image to malloc", h1, g_data[2], 0x6000);
    exercise("malloc to image", g_data[2], h2, 0x6000);
    unsigned char *at4 = map_at(0x140000000ull, 0x10000, "5 GB");
    if (!at4) { printf("map 5 GB\n"); return 1; }
    exercise("5 GB", at4, at4 + 0x8000, 0x8000);
    exercise("5 GB to malloc", h1, at4, 0x6000);

    /* a page that ends at 12 GB, and in the Wine layout's build the page above it */
    unsigned char *below = map_at(0x300000000ull - 0x8000, 0x8000, "below 12 GB");
    if (!below) { printf("map below 12 GB\n"); return 1; }
    exercise("below 12 GB", below, below + 0x4000, 0x4000 - 64);
    ends_at("12 GB", below + 0x8000);
    unsigned char *above = map_at(0x300000000ull, 0x4000, "12 GB");
    if (!above) { printf("map 12 GB\n"); return 1; }
    if (above == below + 0x8000) {
        across("12 GB", above);
    } else {
        unsigned char *two = mmap(NULL, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        across("12 GB", two + 0x4000);
    }

    unsigned char *top = map_at(0x7ffffe100000ull, 0x10000, "top strip");
    if (!top) { printf("map top strip\n"); return 1; }
    exercise("top strip", top, top + 0x8000, 0x8000);
    exercise("top strip to malloc", h2, top, 0x6000);
    exercise("image to top strip", top, g_data[1], 0x6000);
    across("top strip", top + 0x8000);
    ends_at("5 GB", at4 + 0x10000);

    /* a destination on a read-only page below 12 GB: the handler sees the guest's address */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    g_ro = map_at(0x180000000ull, 0x4000, "read-only page");
    if (!g_ro) { printf("map read-only\n"); return 1; }
    unsigned char src[3000];
    fill(src, sizeof src, 99);
    for (int r = 0; r < 3; r++) {
        mprotect(g_ro, 0x4000, PROT_READ);
        g_fault_at = 0;
        unsigned char *d = g_ro + 0x100 + r * 0x400;
        if (p_memcpy(d, src, 2000 + r) != d || ref_cmp(d, src, 2000 + r)) bad("read-only", "memcpy", r, 0);
        if (g_fault_at < (uintptr_t)g_ro || g_fault_at >= (uintptr_t)g_ro + 0x4000)
            bad("read-only", "fault address offset", (long)(g_fault_at - (uintptr_t)g_ro), 0x100);
        mprotect(g_ro, 0x4000, PROT_READ);
        if (p_memset(d, 7, 900) != d) bad("read-only", "memset", r, 0);
        for (int i = 0; i < 900; i++)
            if (((volatile unsigned char *)d)[i] != 7) { bad("read-only", "memset bytes", i, 7); break; }
    }

    /* the routines' answers for a few fixed inputs */
    char *word = (char *)g_data[2];
    p_memcpy(word, "translation", 12);
    printf("strlen %zu strnlen %zu %zu memchr %ld strchr %ld\n", p_strlen(word), p_strnlen(word, 4),
           p_strnlen(word, 100), (long)((char *)p_memchr(word, 'l', 11) - word), (long)(p_strchr(word, 'i') - word));
    printf("memcmp %d strncmp %d strcmp %d\n", sign(p_memcmp(word, "transit", 7)), sign(p_strncmp(word, "transl", 6)),
           sign(p_strcmp(word, "translator")));
    printf("%s\n", g_bad ? "FAILED" : "OK");
    return g_bad != 0;
}
