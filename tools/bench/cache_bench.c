#define _POSIX_C_SOURCE 200809L

#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ocerz/cache.h"

#define SAMPLE_NAMES 20000u
#define MISS_NAMES 2000u
#define REGION_CALLS 200000u

static char **names;
static size_t names_count;
static size_t names_cap;
static uint64_t rng_state = UINT64_C(0x8f3f73b5cf1c9ade);
static uint64_t hash_state = UINT64_C(14695981039346656037);

static uint32_t rng_u32(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (uint32_t)x;
}

static void hash_bytes(const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) {
        hash_state ^= p[i];
        hash_state *= UINT64_C(1099511628211);
    }
}

static void hash_u64(uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(v >> (i * 8));
        hash_bytes(&b, 1);
    }
}

static void remember_name(const char *s, size_t n)
{
    if (!n || n >= 512)
        return;
    if (names_count == names_cap) {
        size_t cap = names_cap ? names_cap * 2 : 4096;
        char **p = realloc(names, cap * sizeof(*p));
        if (!p)
            return;
        names = p;
        names_cap = cap;
    }
    char *copy = malloc(n + 1);
    if (!copy)
        return;
    memcpy(copy, s, n);
    copy[n] = '\0';
    names[names_count++] = copy;
}

static uint64_t read_uleb(const uint8_t **pp, const uint8_t *end)
{
    uint64_t v = 0;
    unsigned shift = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
        shift += 7;
    }
    *pp = p;
    return v;
}

static void walk_trie(const uint8_t *start, const uint8_t *end, const uint8_t *node,
                      char *prefix, size_t prefix_len, unsigned depth)
{
    if (depth > 256 || node < start || node >= end)
        return;
    const uint8_t *p = node;
    uint64_t term = read_uleb(&p, end);
    if (term) {
        if (term > (uint64_t)(end - p))
            return;
        remember_name(prefix, prefix_len);
        p += term;
    }
    if (p >= end)
        return;
    uint8_t children = *p++;
    for (uint8_t i = 0; i < children && p < end; i++) {
        const char *edge = (const char *)p;
        size_t left = (size_t)(end - p);
        size_t elen = strnlen(edge, left);
        if (elen == left || prefix_len + elen >= 511)
            return;
        p += elen + 1;
        uint64_t child = read_uleb(&p, end);
        size_t child_len = prefix_len + elen;
        memcpy(prefix + prefix_len, edge, elen);
        prefix[child_len] = '\0';
        walk_trie(start, end, start + child, prefix, child_len, depth + 1);
        prefix[prefix_len] = '\0';
    }
}

static int export_trie(uint64_t mh_addr, const uint8_t **start, const uint8_t **end)
{
    const uint8_t *mh = (const uint8_t *)(uintptr_t)mh_addr;
    uint32_t ncmds;
    memcpy(&ncmds, mh + 16, sizeof(ncmds));
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint64_t le_vmaddr = 0, le_fileoff = 0;
    uint32_t exp_off = 0, exp_size = 0;
    int have_le = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd, size;
        memcpy(&cmd, lc, sizeof(cmd));
        memcpy(&size, lc + 4, sizeof(size));
        if (size < sizeof(struct load_command))
            return -1;
        if (cmd == LC_SEGMENT_64) {
            struct segment_command_64 sc;
            memcpy(&sc, lc, sizeof(sc));
            if (memcmp(sc.segname, "__LINKEDIT", 10) == 0) {
                le_vmaddr = sc.vmaddr;
                le_fileoff = sc.fileoff;
                have_le = 1;
            }
        } else if (cmd == LC_DYLD_EXPORTS_TRIE) {
            memcpy(&exp_off, lc + 8, sizeof(exp_off));
            memcpy(&exp_size, lc + 12, sizeof(exp_size));
        } else if ((cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) && exp_off == 0) {
            memcpy(&exp_off, lc + 40, sizeof(exp_off));
            memcpy(&exp_size, lc + 44, sizeof(exp_size));
        }
        lc += size;
    }
    if (!have_le || !exp_off || !exp_size)
        return -1;
    uint64_t addr = le_vmaddr + ((uint64_t)exp_off - le_fileoff);
    *start = (const uint8_t *)(uintptr_t)addr;
    *end = *start + exp_size;
    return 0;
}

static void collect_names(OcerzCache *c)
{
    char prefix[512] = { 0 };
    uint32_t count = c->images_cnt < 50 ? c->images_cnt : 50;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t mh = ocerz_cache_image_addr(c, i, NULL);
        const uint8_t *start, *end;
        if (mh && export_trie(mh, &start, &end) == 0)
            walk_trie(start, end, start, prefix, 0, 0);
    }
    for (size_t i = names_count; i > 1; i--) {
        size_t j = (size_t)rng_u32() % i;
        char *tmp = names[i - 1];
        names[i - 1] = names[j];
        names[j] = tmp;
    }
    if (names_count > SAMPLE_NAMES) {
        for (size_t i = SAMPLE_NAMES; i < names_count; i++)
            free(names[i]);
        names_count = SAMPLE_NAMES;
    }
}

static double elapsed_ns(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec);
}

static void hash_result(uint64_t value, int found)
{
    hash_u64(value);
    hash_u64((uint32_t)found);
}

static void resolve_one(OcerzCache *c, uint64_t sysmh, const char *symbol)
{
    int found = 0;
    hash_bytes(symbol, strlen(symbol) + 1);
    uint64_t value = ocerz_cache_resolve_ex(c, symbol, &found);
    hash_result(value, found);
    found = 0;
    value = ocerz_cache_resolve_in_image(c, "/usr/lib/libSystem.B.dylib", symbol, &found);
    hash_result(value, found);
    found = 0;
    value = ocerz_cache_dlsym_image(c, sysmh, symbol, &found);
    hash_result(value, found);
    found = 0;
    value = ocerz_cache_resolve_from_image(c, sysmh, symbol, &found);
    hash_result(value, found);
    found = 0;
    value = ocerz_cache_resolve_weak_ex(c, symbol, &found, NULL);
    hash_result(value, found);
}

static double run_resolve_pass(OcerzCache *c, uint64_t sysmh)
{
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t i = 0; i < names_count; i++)
        resolve_one(c, sysmh, names[i]);
    char miss[80];
    for (uint32_t i = 0; i < MISS_NAMES; i++) {
        snprintf(miss, sizeof(miss), "__ocerz_cache_bench_miss_%08x", i);
        resolve_one(c, sysmh, miss);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    return elapsed_ns(start, end) / (double)(names_count + MISS_NAMES);
}

int main(void)
{
    OcerzCache c;
    if (ocerz_cache_map(&c) != OCERZ_OK) {
        fprintf(stderr, "cache_bench: shared cache map failed\n");
        return 1;
    }
    collect_names(&c);
    uint64_t sysmh = ocerz_cache_find_alias(&c, "/usr/lib/libSystem.B.dylib");
    if (!sysmh) {
        fprintf(stderr, "cache_bench: libSystem alias not found\n");
        return 1;
    }
    double cold = run_resolve_pass(&c, sysmh);
    double warm = run_resolve_pass(&c, sysmh);
    for (uint32_t i = 0; i < c.images_cnt && i < 50; i++) {
        const char *path = NULL;
        uint64_t mh = ocerz_cache_image_addr(&c, i, &path);
        uint64_t base = 0;
        const char *name = ocerz_cache_name_for_addr(mh, &base);
        int has = ocerz_cache_has_image(&c, mh);
        uint64_t alias = path ? ocerz_cache_find_alias(&c, path) : 0;
        hash_result(mh, has);
        hash_u64(base);
        if (name)
            hash_bytes(name, strlen(name) + 1);
        if (path)
            hash_u64(alias);
    }
    static const char *const aliases[] = {
        "/usr/lib/libSystem.B.dylib",
        "/usr/lib/libz.dylib",
        "/usr/lib/libobjc.A.dylib",
        "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        uint64_t alias = ocerz_cache_find_alias(&c, aliases[i]);
        hash_u64(alias);
    }

    uint64_t state = UINT64_C(0x1020304050607081);
    uint64_t lo = c.base - (UINT64_C(1) << 30);
    uint64_t region_hash = UINT64_C(14695981039346656037);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint32_t i = 0; i < REGION_CALLS; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint64_t addr = lo + (state & ((UINT64_C(1) << 31) - 1));
        uint8_t in_cache = (uint8_t)ocerz_cache_region((uintptr_t)addr);
        region_hash ^= in_cache;
        region_hash *= UINT64_C(1099511628211);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double region = elapsed_ns(start, end) / REGION_CALLS;
    hash_u64(region_hash);
    printf("hash=%016llx names=%zu cold_ns_per_symbol=%.1f warm_ns_per_symbol=%.1f region_ns_per_call=%.2f\n",
           (unsigned long long)hash_state, names_count, cold, warm, region);
    for (size_t i = 0; i < names_count; i++)
        free(names[i]);
    free(names);
    return 0;
}
