/*
 * The in-place string and memory routines, checked against the host's own.
 *
 * Each routine in src/leaf.s is run through ocerz_leaf_call, which places three
 * values where translated code keeps rdi, rsi and rdx and returns what the
 * routine left where rax lives, and its answer is compared with the C library's
 * for the same arguments.  Every call but the ones made to see memmove decline
 * must also leave zero where a routine says whether it declined.  Comparisons are compared by sign, which is all the
 * standard promises, and also by value, because the routines claim the host's
 * convention of returning the difference of the two bytes.
 *
 * The buffers are what make the test worth having.  Two three-page regions are
 * mapped with their last page inaccessible, and every string is placed so that
 * its final byte is at each of the last forty offsets before that page, at
 * every alignment the sixteen-byte reads can meet.  A routine that reads a
 * block reaching past the end of what it was given dies here on a fault and
 * not in a guest.  The second string of a comparison is placed against its own
 * inaccessible page independently, so the two pointers reach a page end at
 * different distances and both halves of the page test are taken.
 *
 * Lengths run from zero through a few blocks.  Differences are planted at
 * every position including past the terminator, where a comparison of strings
 * must not look and a comparison of memory must, and with bytes on both sides
 * of 0x80 so that a signed byte comparison would be caught.  The searched-for
 * byte is absent, present once, present before the pointer in the same block,
 * present only past the limit, and zero.  strnlen and memchr are also given a
 * limit so large that pointer plus limit wraps.
 *
 * memmove and memset are checked for what they leave in memory, not only for
 * the pointer they return.  Every length from zero to a few blocks past the
 * largest unrolled step is copied between every pair of alignments, inside an
 * arena whose every other byte is compared afterwards, so a store one byte
 * outside the destination fails as surely as a wrong byte inside it.  Every
 * length is also moved at every distance from a hundred bytes below the source
 * to a hundred above.  memmove declines a move whose two ranges overlap without
 * being the same range, so there the test expects the declining answer and an
 * arena left exactly as it was, and everywhere else, distance zero included,
 * the answer that it did the work and the bytes the host's memmove leaves.
 * Copies are also made with both buffers ending at an inaccessible page.
 */
#include "ocerz/leaf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 16384u
#define SPAN (2u * PAGE)
#define TAILS 40u
#define MAXLEN 70u

static int g_fail;
static unsigned long g_checks;

static unsigned char *edge_region(void)
{
    unsigned char *p = mmap(NULL, SPAN + PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED || mprotect(p + SPAN, PAGE, PROT_NONE) != 0) {
        fprintf(stderr, "test_leaf: cannot map a guarded region\n");
        exit(2);
    }
    memset(p, 0x5a, SPAN);
    return p;
}

static int sign_of(int64_t v)
{
    return v < 0 ? -1 : v > 0 ? 1 : 0;
}

static void expect(const char *what, uint64_t got, uint64_t want, unsigned a, unsigned b, unsigned c)
{
    g_checks++;
    if (got == want)
        return;
    if (g_fail++ < 20)
        fprintf(stderr, "test_leaf: %s(%u,%u,%u) answered %#llx, the host answers %#llx\n", what, a,
                b, c, (unsigned long long)got, (unsigned long long)want);
}

static uint64_t run(void (*routine)(void), uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    ocerz_leaf_declined = 99;
    uint64_t got = ocerz_leaf_call(routine, rdi, rsi, rdx);
    g_checks++;
    if (ocerz_leaf_declined != 0 && g_fail++ < 20)
        fprintf(stderr, "test_leaf: a routine left %llu where it says whether it declined\n",
                (unsigned long long)ocerz_leaf_declined);
    return got;
}

static unsigned char *place(unsigned char *region, unsigned tail, unsigned bytes)
{
    return region + SPAN - tail - bytes;
}

static void fill_text(unsigned char *s, unsigned len, unsigned seed)
{
    for (unsigned i = 0; i < len; i++)
        s[i] = (unsigned char)(1 + (seed * 7 + i * 13) % 250);
    s[len] = 0;
}

static void check_lengths(unsigned char *ra)
{
    for (unsigned tail = 0; tail < TAILS; tail++)
        for (unsigned len = 0; len < MAXLEN; len++) {
            unsigned char *s = place(ra, tail, len + 1);
            memset(s - 16, 0, 16);
            fill_text(s, len, tail + len);
            expect("strlen", run(ocerz_leaf_strlen, (uint64_t)(uintptr_t)s, 0, 0),
                   strlen((char *)s), tail, len, 0);
            static const uint64_t limits[] = { 0, 1, 15, 16, 17, 31, 32, 33, 64, 200, ~0ull, ~0ull - 7 };
            for (unsigned k = 0; k < sizeof limits / sizeof limits[0]; k++) {
                uint64_t lim = limits[k];
                unsigned char *t = s;
                if (lim < len + 1 && lim > 0) {
                    t = place(ra, tail, (unsigned)lim);
                    memset(t, 'x', (size_t)lim);
                }
                expect("strnlen", run(ocerz_leaf_strnlen, (uint64_t)(uintptr_t)t, lim, 0),
                       strnlen((char *)t, (size_t)lim), tail, len, (unsigned)k);
                if (t != s)
                    fill_text(s, len, tail + len);
            }
        }
}

static void check_search(unsigned char *ra)
{
    for (unsigned tail = 0; tail < TAILS; tail++)
        for (unsigned len = 0; len < MAXLEN; len++) {
            unsigned char *s = place(ra, tail, len + 1);
            memset(s - 16, 0xee, 16);
            fill_text(s, len, tail * 3 + len);
            for (unsigned at = 0; at <= len; at += 1 + len / 9) {
                unsigned char saved = s[at];
                if (at < len)
                    s[at] = 0xee;
                int wanted[] = { 0xee, 0, 0xfb, 0x1ee };
                for (unsigned w = 0; w < 4; w++) {
                    int c = wanted[w];
                    expect("strchr",
                           run(ocerz_leaf_strchr, (uint64_t)(uintptr_t)s,
                                           0xabcdef0000000000ull | (uint64_t)(unsigned)c, 0),
                           (uint64_t)(uintptr_t)strchr((char *)s, c), tail, len, at);
                    static const uint64_t limits[] = { 0, 1, 16, 33, 0, ~0ull };
                    for (unsigned k = 0; k < sizeof limits / sizeof limits[0]; k++) {
                        uint64_t lim = k == 4 ? len + 1 : limits[k];
                        if (lim != ~0ull && lim > len + 1)
                            lim = len + 1;
                        if (lim == ~0ull && !memchr(s, c, len + 1))
                            continue;
                        uint64_t host_lim = lim == ~0ull ? len + 1 : lim;
                        expect("memchr",
                               run(ocerz_leaf_memchr, (uint64_t)(uintptr_t)s,
                                               (uint64_t)(unsigned)c, lim),
                               (uint64_t)(uintptr_t)memchr(s, c, (size_t)host_lim), tail, len, at);
                    }
                }
                s[at] = saved;
            }
        }
}

static void check_compare(unsigned char *ra, unsigned char *rb)
{
    static const unsigned char pairs[][2] = { { 0x01, 0x02 }, { 0x7f, 0x80 }, { 0xff, 0x01 }, { 0x80, 0x7f } };
    for (unsigned ta = 0; ta < TAILS; ta += 3)
        for (unsigned tb = 0; tb < TAILS; tb += 5)
            for (unsigned len = 0; len < MAXLEN; len += 1 + len / 24) {
                unsigned char *a = place(ra, ta, len + 1);
                unsigned char *b = place(rb, tb, len + 1);
                for (unsigned at = 0; at <= len + 1; at += 1 + len / 7) {
                    for (unsigned v = 0; v < 4; v++) {
                        fill_text(a, len, ta + tb);
                        fill_text(b, len, ta + tb);
                        if (at < len) {
                            a[at] = pairs[v][0];
                            b[at] = pairs[v][1];
                        }
                        uint64_t pa = (uint64_t)(uintptr_t)a, pb = (uint64_t)(uintptr_t)b;
                        int64_t got = (int64_t)run(ocerz_leaf_strcmp, pa, pb, 0);
                        int want = strcmp((char *)a, (char *)b);
                        expect("strcmp sign", (uint64_t)sign_of(got), (uint64_t)sign_of(want), ta, tb, len);
                        if (at < len)
                            expect("strcmp value", (uint64_t)got,
                                   (uint64_t)(int64_t)((int)pairs[v][0] - (int)pairs[v][1]), ta, tb, len);
                        static const uint64_t limits[] = { 0, 1, 15, 16, 17, 40, ~0ull };
                        for (unsigned k = 0; k < sizeof limits / sizeof limits[0]; k++) {
                            uint64_t lim = limits[k];
                            got = (int64_t)run(ocerz_leaf_strncmp, pa, pb, lim);
                            want = strncmp((char *)a, (char *)b, (size_t)lim);
                            expect("strncmp sign", (uint64_t)sign_of(got), (uint64_t)sign_of(want), ta,
                                   tb, len);
                        }
                        for (uint64_t lim = 0; lim <= len + 1; lim += 1 + lim / 5) {
                            got = (int64_t)run(ocerz_leaf_memcmp, pa, pb, lim);
                            want = memcmp(a, b, (size_t)lim);
                            expect("memcmp sign", (uint64_t)sign_of(got), (uint64_t)sign_of(want), ta,
                                   tb, len);
                        }
                    }
                }
                fill_text(a, len, 1);
                fill_text(b, len, 1);
                a[len] = 0x33;
                b[len] = 0x99;
                int64_t past = (int64_t)run(ocerz_leaf_memcmp, (uint64_t)(uintptr_t)a,
                                                        (uint64_t)(uintptr_t)b, len + 1);
                expect("memcmp last byte", (uint64_t)past, (uint64_t)(int64_t)(0x33 - 0x99), ta, tb, len);
            }
}

#define ARENA 1024u
#define MOVE_MAX 300u

static void fill_arena(unsigned char *m, unsigned seed)
{
    for (unsigned i = 0; i < ARENA; i++)
        m[i] = (unsigned char)(seed + i * 37 + (i >> 3));
}

static void check_moves(unsigned char *ra, unsigned char *rb)
{
    static unsigned char mine[ARENA], host[ARENA], from[ARENA];
    for (unsigned len = 0; len <= MOVE_MAX; len++)
        for (unsigned da = 0; da < 16; da++)
            for (unsigned sa = 0; sa < 16; sa += len < 80 ? 1 : 5) {
                fill_arena(mine, len);
                fill_arena(host, len);
                fill_arena(from, len + 91);
                uint64_t got = run(ocerz_leaf_memmove, (uint64_t)(uintptr_t)(mine + 64 + da),
                                               (uint64_t)(uintptr_t)(from + 32 + sa), len);
                memmove(host + 64 + da, from + 32 + sa, len);
                expect("memmove result", got, (uint64_t)(uintptr_t)(mine + 64 + da), len, da, sa);
                expect("memmove bytes", (uint64_t)memcmp(mine, host, ARENA), 0, len, da, sa);
            }

    for (unsigned len = 0; len <= MOVE_MAX; len += len < 140 ? 1 : 7)
        for (int dist = -100; dist <= 100; dist++) {
            fill_arena(mine, len * 3);
            fill_arena(host, len * 3);
            unsigned src = 200 + (len & 15), dst = (unsigned)((int)src + dist);
            unsigned apart = (unsigned)(dist < 0 ? -dist : dist);
            int overlapping = dist != 0 && apart < len;
            ocerz_leaf_declined = 99;
            ocerz_leaf_call(ocerz_leaf_memmove, (uint64_t)(uintptr_t)(mine + dst),
                            (uint64_t)(uintptr_t)(mine + src), len);
            expect("memmove declining", ocerz_leaf_declined, overlapping ? 1 : 0, len,
                   (unsigned)(dist + 100), 0);
            if (!overlapping)
                memmove(host + dst, host + src, len);
            expect("memmove near itself", (uint64_t)memcmp(mine, host, ARENA), 0, len,
                   (unsigned)(dist + 100), 0);
        }

    for (unsigned len = 0; len <= MOVE_MAX; len++)
        for (unsigned da = 0; da < 16; da++) {
            fill_arena(mine, len);
            fill_arena(host, len);
            uint64_t c = 0xffffff00ull | (len * 5 + da);
            uint64_t got = run(ocerz_leaf_memset, (uint64_t)(uintptr_t)(mine + 64 + da), c, len);
            memset(host + 64 + da, (int)c, len);
            expect("memset result", got, (uint64_t)(uintptr_t)(mine + 64 + da), len, da, 0);
            expect("memset bytes", (uint64_t)memcmp(mine, host, ARENA), 0, len, da, 0);
        }

    for (unsigned len = 0; len <= MOVE_MAX; len += 1 + len / 40)
        for (unsigned tail = 0; tail < 3; tail++) {
            unsigned char *d = place(ra, tail, len), *f = place(rb, 0, len);
            for (unsigned i = 0; i < len; i++)
                f[i] = (unsigned char)(i * 11 + len);
            memset(d, 0, len);
            run(ocerz_leaf_memmove, (uint64_t)(uintptr_t)d, (uint64_t)(uintptr_t)f, len);
            expect("memmove at a page end", (uint64_t)memcmp(d, f, len), 0, len, tail, 0);
            run(ocerz_leaf_memset, (uint64_t)(uintptr_t)d, 0xa5, len);
            unsigned bad = 0;
            for (unsigned i = 0; i < len; i++)
                bad += d[i] != 0xa5;
            expect("memset at a page end", bad, 0, len, tail, 0);
        }
}

int main(void)
{
    unsigned char *ra = edge_region();
    unsigned char *rb = edge_region();

    check_lengths(ra);
    check_search(ra);
    check_compare(ra, rb);
    check_moves(ra, rb);

    if (g_fail) {
        fprintf(stderr, "test_leaf: %d of %lu checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("test_leaf: %lu checks passed\n", g_checks);
    return 0;
}
