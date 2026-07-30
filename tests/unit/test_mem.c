/* Guest arena allocation and reuse tests. */
#include "ocerz/mem.h"

#include <stdio.h>
#include <sys/mman.h>

static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

int main(void)
{
    const uint64_t lo = 0x100000000ull;
    const uint64_t hi = lo + 32ull * 1024 * 1024;
    const uint64_t chunk = 10ull * 1024 * 1024;

    CHECK(ocerz_mem_init(lo, hi) == OCERZ_OK);

    uint64_t first = ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE);
    uint64_t second = ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE);
    CHECK(first != 0);
    CHECK(second != 0);
    CHECK(second > first + chunk);
    CHECK(ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE) == 0);

    ocerz_st(first, 8, 0x123456789abcdef0ull);
    CHECK(ocerz_unmap(first, chunk) == OCERZ_OK);

    uint64_t reused = ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE);
    CHECK(reused == first);
    CHECK(ocerz_ld(reused, 8) == 0);

    CHECK(ocerz_unmap(reused, chunk) == OCERZ_OK);
    uint64_t aligned = ocerz_map_anywhere_aligned(
        2ull * 1024 * 1024, PROT_READ | PROT_WRITE, 1ull * 1024 * 1024);
    CHECK(aligned != 0);
    CHECK((aligned & ((1ull * 1024 * 1024) - 1)) == 0);

    printf("test_mem: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
