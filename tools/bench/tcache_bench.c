#define _DARWIN_C_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ocerz/tcache.h"

#define RECORD_COUNT 50000u
#define RECORD_STRIDE 640u
#define FIND_CALLS 1000000u
#define FAST_PUT_CALLS 2000u
#define TC_BUF_BYTES_TEST (256u << 10)

static uint64_t hash_state = UINT64_C(14695981039346656037);

static uint64_t splitmix64(uint64_t x)
{
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static void hash_bytes(const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) {
        hash_state ^= p[i];
        hash_state *= UINT64_C(1099511628211);
    }
}

static void hash_record(const OcerzTcRecHead *r)
{
    hash_bytes(r + 1, r->size - sizeof(*r));
}

static size_t make_record(uint32_t i, uint8_t *dst)
{
    OcerzTcRecHead *r = (OcerzTcRecHead *)dst;
    uint64_t seed = splitmix64(UINT64_C(0x4f4345525a544341) + i);
    uint64_t address = ((uint64_t)i * UINT64_C(0x9e3779b97f4a7c15) +
                        UINT64_C(0x123456789abc)) &
                       UINT64_C(0x0000ffffffffffff);
    uint64_t mode = i % 3 == 0 ? OCERZ_TC_KEY_M32
                    : i % 3 == 1 ? OCERZ_TC_KEY_PLAIN
                                  : 0;
    size_t words = 8 + (size_t)(splitmix64(seed) % 57);
    r->magic = OCERZ_TC_REC_MAGIC;
    r->size = (uint32_t)(sizeof(*r) + words * sizeof(uint64_t));
    r->key = address | mode;
    r->sum = splitmix64(seed ^ UINT64_C(0x53554d));
    uint64_t *payload = (uint64_t *)(r + 1);
    uint64_t state = seed;
    for (size_t j = 0; j < words; j++) {
        state = splitmix64(state + j);
        payload[j] = state;
    }
    return r->size;
}

static uint8_t *make_records(void)
{
    uint8_t *records = calloc(RECORD_COUNT, RECORD_STRIDE);
    if (!records) {
        perror("calloc");
        exit(2);
    }
    for (uint32_t i = 0; i < RECORD_COUNT; i++)
        make_record(i, records + (size_t)i * RECORD_STRIDE);
    return records;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int write_records(uint8_t *records)
{
    hash_state = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < RECORD_COUNT; i++) {
        OcerzTcRecHead *r = (OcerzTcRecHead *)(records + (size_t)i * RECORD_STRIDE);
        hash_record(r);
        ocerz_tcache_put(r);
    }
    ocerz_tcache_flush();
    printf("hash=%016llx count=%u\n", (unsigned long long)hash_state, RECORD_COUNT);
    return 0;
}

static int read_records(uint8_t *records)
{
    hash_state = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < RECORD_COUNT; i++) {
        OcerzTcRecHead *expected = (OcerzTcRecHead *)(records + (size_t)i * RECORD_STRIDE);
        const OcerzTcRecHead *found = ocerz_tcache_find(expected->key);
        if (!found || found->magic != OCERZ_TC_REC_MAGIC || found->key != expected->key ||
            found->size != expected->size ||
            memcmp(found + 1, expected + 1, expected->size - sizeof(*expected)) != 0) {
            fprintf(stderr, "record mismatch at index %u key=%#llx\n",
                    i, (unsigned long long)expected->key);
            return 1;
        }
        hash_record(found);
    }
    printf("hash=%016llx count=%u\n", (unsigned long long)hash_state, RECORD_COUNT);
    return 0;
}

static int measure_records(uint8_t *records)
{
    volatile uint64_t sink = 0;
    for (uint32_t i = 0; i < RECORD_COUNT; i++) {
        OcerzTcRecHead *r = (OcerzTcRecHead *)(records + (size_t)i * RECORD_STRIDE);
        const OcerzTcRecHead *found = ocerz_tcache_find(r->key);
        if (!found)
            return fprintf(stderr, "missing warmup hit %u\n", i), 1;
        sink += found->key;
    }
    for (uint32_t i = 0; i < 2000; i++) {
        if (ocerz_tcache_find((UINT64_C(1) << 48) | i))
            return fprintf(stderr, "unexpected warmup miss %u\n", i), 1;
    }

    uint64_t fast_storage[11] = {0};
    OcerzTcRecHead *fast_rec = (OcerzTcRecHead *)(void *)fast_storage;
    fast_rec->magic = OCERZ_TC_REC_MAGIC;
    fast_rec->size = (uint32_t)(sizeof(*fast_rec) + 8 * sizeof(uint64_t));
    fast_rec->key = UINT64_C(0x0000123456789abc);
    fast_rec->sum = UINT64_C(0xfeedfacecafebeef);
    size_t fast_bytes = (size_t)(FAST_PUT_CALLS + 1) * fast_rec->size;
    if (fast_bytes >= TC_BUF_BYTES_TEST)
        return fprintf(stderr, "fast-put sample would fill the buffer\n"), 1;
    ocerz_tcache_put(fast_rec);
    uint64_t fast_start = now_ns();
    for (uint32_t i = 0; i < FAST_PUT_CALLS; i++) {
        ocerz_tcache_put(fast_rec);
    }
    uint64_t fast_put_ns = now_ns() - fast_start;
    ocerz_tcache_flush();

    uint64_t start = now_ns();
    for (uint32_t i = 0; i < FIND_CALLS; i++) {
        uint32_t ix = i % RECORD_COUNT;
        OcerzTcRecHead *r = (OcerzTcRecHead *)(records + (size_t)ix * RECORD_STRIDE);
        const OcerzTcRecHead *found = ocerz_tcache_find(r->key);
        if (!found)
            return fprintf(stderr, "missing measured hit %u\n", ix), 1;
        sink += found->key;
    }
    uint64_t hit_ns = now_ns() - start;

    start = now_ns();
    for (uint32_t i = 0; i < FIND_CALLS; i++) {
        if (ocerz_tcache_find((UINT64_C(1) << 48) | (i % 2000)))
            return fprintf(stderr, "unexpected measured miss %u\n", i), 1;
    }
    uint64_t miss_ns = now_ns() - start;

    uint64_t put_start = now_ns();
    for (uint32_t i = 0; i < RECORD_COUNT; i++) {
        OcerzTcRecHead *r = (OcerzTcRecHead *)(records + (size_t)i * RECORD_STRIDE);
        ocerz_tcache_put(r);
    }
    uint64_t put_loop_ns = now_ns() - put_start;
    ocerz_tcache_flush();
    uint64_t put_total_ns = now_ns() - put_start;
    printf("find_hit_ns_per_call=%.2f find_miss_ns_per_call=%.2f "
           "put_fast_ns_per_call=%.2f put_loop_ns_per_call=%.2f "
           "put_total_ns_per_call=%.2f sink=%016llx\n",
           (double)hit_ns / FIND_CALLS, (double)miss_ns / FIND_CALLS,
           (double)fast_put_ns / FAST_PUT_CALLS, (double)put_loop_ns / RECORD_COUNT,
           (double)put_total_ns / RECORD_COUNT, (unsigned long long)sink);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s write|read|measure\n", argv[0]);
        return 2;
    }
    if (ocerz_tcache_mode() != OCERZ_TC_ON) {
        fprintf(stderr, "translation cache is not on\n");
        return 2;
    }
    uint8_t *records = make_records();
    int rc;
    if (!strcmp(argv[1], "write"))
        rc = write_records(records);
    else if (!strcmp(argv[1], "read"))
        rc = read_records(records);
    else if (!strcmp(argv[1], "measure"))
        rc = measure_records(records);
    else {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        rc = 2;
    }
    free(records);
    return rc;
}
