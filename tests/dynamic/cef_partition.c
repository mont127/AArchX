#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key;
    int32_t sub;
    int32_t pad;
} Rec;

typedef struct {
    Rec *pos;
    uint64_t flag;
} PartResult;

extern PartResult cef_partition(Rec *first, Rec *last);

void cef_partition_abort(void)
{
    printf("BAD abort\n");
    exit(3);
}

static int lt(const Rec *a, const Rec *b)
{
    return a->key < b->key || (a->key == b->key && a->sub < b->sub);
}

static PartResult ref_partition(Rec *begin, Rec *end, int *hit)
{
    Rec *first = begin;
    Rec pivot = *first;
    do {
        ++first;
        if (first == end) { *hit = 1; return (PartResult){ 0, 0 }; }
    } while (lt(first, &pivot));
    Rec *last = end;
    if (begin == first - 1) {
        while (first < last && !lt(--last, &pivot))
            ;
    } else {
        do {
            if (last == begin) { *hit = 1; return (PartResult){ 0, 0 }; }
            --last;
        } while (!lt(last, &pivot));
    }
    int already = first >= last;
    while (first < last) {
        Rec t = *first;
        *first = *last;
        *last = t;
        do {
            ++first;
            if (first == end) { *hit = 1; return (PartResult){ 0, 0 }; }
        } while (lt(first, &pivot));
        do {
            if (last == begin) { *hit = 1; return (PartResult){ 0, 0 }; }
            --last;
        } while (!lt(last, &pivot));
    }
    Rec *pos = first - 1;
    if (begin != pos)
        *begin = *pos;
    *pos = pivot;
    return (PartResult){ pos, (uint64_t)already };
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    static const uint64_t kranges[] = { 1, 2, 4, 16, 1000 };
    Rec a[64], b[64];
    for (long it = 0; it < iters; it++) {
        int n = 2 + (int)(rnd() % 39);
        uint64_t kr = kranges[rnd() % 5];
        for (int i = 0; i < n; i++) {
            a[i].key = (rnd() % kr) << 12;
            a[i].sub = (int32_t)(rnd() % 5) - 2;
            a[i].pad = 0;
        }
        int mx = 0;
        for (int i = 1; i < n; i++)
            if (lt(&a[mx], &a[i]))
                mx = i;
        Rec t = a[mx];
        a[mx] = a[n - 1];
        a[n - 1] = t;
        memcpy(b, a, sizeof(Rec) * (size_t)n);
        int hit = 0;
        PartResult r = ref_partition(b, b + n, &hit);
        if (hit)
            continue;
        PartResult c = cef_partition(a, a + n);
        if (c.pos - a != r.pos - b || (c.flag & 1) != r.flag ||
            memcmp(a, b, sizeof(Rec) * (size_t)n) != 0) {
            printf("BAD iter=%ld n=%d pos=%ld/%ld flag=%llu/%llu\n", it, n, (long)(c.pos - a),
                   (long)(r.pos - b), (unsigned long long)(c.flag & 1), (unsigned long long)r.flag);
            return 1;
        }
    }
    printf("OK\n");
    return 0;
}
