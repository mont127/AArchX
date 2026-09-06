/* Guest arena allocation and reuse tests. */
#include "ocerz/mem.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define WORKER_COUNT 8
#define WORKER_ITERS 200

typedef struct {
    int index;
    int failures;
} WorkerCtx;

static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t active_allocations[WORKER_COUNT];

typedef struct {
    uint64_t address;
    int stop;
} SiblingWriter;

static void *sibling_writer(void *opaque)
{
    SiblingWriter *writer = opaque;
    uint64_t value = 0;
    while (!__atomic_load_n(&writer->stop, __ATOMIC_ACQUIRE))
        ocerz_st(writer->address, 8, ++value);
    return NULL;
}

static void *allocation_worker(void *opaque)
{
    WorkerCtx *ctx = (WorkerCtx *)opaque;
    for (int iteration = 0; iteration < WORKER_ITERS; iteration++) {
        uint64_t addr = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE,
                                           PROT_READ | PROT_WRITE);
        if (!addr) {
            ctx->failures++;
            continue;
        }

        pthread_mutex_lock(&active_lock);
        for (int i = 0; i < WORKER_COUNT; i++)
            if (i != ctx->index && active_allocations[i] == addr)
                ctx->failures++;
        active_allocations[ctx->index] = addr;
        pthread_mutex_unlock(&active_lock);

        uint64_t value = ((uint64_t)(unsigned)ctx->index << 32) |
                         (uint32_t)iteration;
        ocerz_st(addr, 8, value);
        if (ocerz_ld(addr, 8) != value)
            ctx->failures++;
        sched_yield();

        pthread_mutex_lock(&active_lock);
        active_allocations[ctx->index] = 0;
        pthread_mutex_unlock(&active_lock);
        if (ocerz_unmap(addr, OCERZ_GUEST_PAGE_SIZE) != OCERZ_OK)
            ctx->failures++;
    }
    return NULL;
}

static void test_padded_shared_activation(uint64_t base)
{
    char path[] = "/tmp/ocerz-padded-map-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(ftruncate(fd, (off_t)OCERZ_GUEST_PAGE_SIZE) == 0);
    int rofd = open(path, O_RDONLY);
    CHECK(rofd >= 0);
    unlink(path);

    const uint64_t clock_a = 0x1111222233334444ull;
    const uint64_t clock_b = 0x5555666677778888ull;
    const uint64_t dirty = 0xa5a5a5a5deadbeefull;
    const uint64_t dispatch = 0xfedcba9876543210ull;
    CHECK(pwrite(fd, &clock_a, sizeof clock_a, 64) == sizeof clock_a);
    CHECK(ocerz_map_fixed(base, OCERZ_HOST_PAGE_SIZE, PROT_NONE) ==
          OCERZ_OK);
    CHECK(ocerz_map_shared_file_padded(base, OCERZ_GUEST_PAGE_SIZE,
                                       PROT_READ, fd, 0) == OCERZ_OK);

    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    CHECK((uint64_t)st.st_size >= OCERZ_HOST_PAGE_SIZE);
    CHECK(ocerz_addr_prot(base) == PROT_READ);
    CHECK(ocerz_ld(base + 64, 8) == clock_a);
    CHECK(pwrite(fd, &clock_b, sizeof clock_b, 64) == sizeof clock_b);
    CHECK(ocerz_ld(base + 64, 8) == clock_b);

    uint64_t sibling = base + OCERZ_GUEST_PAGE_SIZE;
    CHECK(pwrite(fd, &dirty, sizeof dirty,
                 (off_t)OCERZ_GUEST_PAGE_SIZE + 32) == sizeof dirty);
    CHECK(ocerz_map_fixed(sibling, OCERZ_GUEST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_OK);
    CHECK(ocerz_addr_prot(sibling) == (PROT_READ | PROT_WRITE));
    /* Activating a sibling slot must NOT zero-fill it: the slot is
     * physically the shared file every process of the prefix maps, and
     * wine's syscall-dispatcher pointer lives there.  Zeroing it for one
     * process's fresh mapping made every other process call NULL on its
     * next syscall (2026-09-06).  The file keeps what it had. */
    uint64_t value = UINT64_MAX;
    CHECK(pread(fd, &value, sizeof value,
                (off_t)OCERZ_GUEST_PAGE_SIZE + 32) == sizeof value);
    CHECK(value == dirty);
    CHECK(ocerz_ld(base + 64, 8) == clock_b);

    ocerz_st(sibling + 32, 8, dispatch);
    CHECK(pread(fd, &value, sizeof value,
                (off_t)OCERZ_GUEST_PAGE_SIZE + 32) == sizeof value);
    CHECK(value == dispatch);
    CHECK(ocerz_map_fixed(base, OCERZ_GUEST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_EUNSUP);
    CHECK(ocerz_map_fixed(sibling, OCERZ_GUEST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_EUNSUP);
    CHECK(ocerz_ld(base + 64, 8) == clock_b);
    CHECK(ocerz_ld(sibling + 32, 8) == dispatch);

    CHECK(ocerz_unmap(sibling, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK(pwrite(fd, &clock_a, sizeof clock_a, 64) == sizeof clock_a);
    CHECK(ocerz_ld(base + 64, 8) == clock_a);
    CHECK(ocerz_map_fixed(sibling, OCERZ_GUEST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_OK);
    CHECK(ocerz_ld(sibling + 32, 8) == dispatch);   /* remapped: still the shared file's bytes */
    CHECK(ocerz_ld(base + 64, 8) == clock_a);
    CHECK(ocerz_unmap(base, OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);
    CHECK((ocerz_host_region_prot(base, NULL, NULL) & 0xff) == 0);

    uint64_t rejected = base + OCERZ_HOST_PAGE_SIZE;
    CHECK(ocerz_map_fixed(rejected, OCERZ_GUEST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_OK);
    ocerz_st(rejected, 8, clock_b);
    if (rofd >= 0) {
        CHECK(ocerz_map_shared_file_padded(rejected,
                                           OCERZ_GUEST_PAGE_SIZE,
                                           PROT_READ, rofd, 0) ==
              OCERZ_EUNSUP);
        CHECK(ocerz_ld(rejected, 8) == clock_b);
    }
    CHECK(ocerz_unmap(rejected, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);

    if (rofd >= 0)
        close(rofd);
    close(fd);
}

int main(void)
{
    const uint64_t lo = 0x100000000ull;
    const uint64_t hi = lo + 32ull * 1024 * 1024;
    const uint64_t chunk = 10ull * 1024 * 1024;

    CHECK(ocerz_mem_init(lo, hi) == OCERZ_OK);
    uint64_t host_lo = (uint64_t)(uintptr_t)ocerz_g2h(lo);
    CHECK(ocerz_host_in_guest_space((const void *)(uintptr_t)host_lo));
    CHECK(!ocerz_host_in_guest_reservation(
        (const void *)(uintptr_t)(host_lo - OCERZ_GUEST_PAGE_SIZE)));

    uint64_t first = ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE);
    uint64_t second = ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE);
    CHECK(first != 0);
    CHECK(second != 0);
    CHECK(second > first + chunk);
    CHECK(ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE) == 0);

    ocerz_st(first, 8, 0x123456789abcdef0ull);
    for (uint64_t off = 0; off < chunk; off += OCERZ_GUEST_PAGE_SIZE) {
        CHECK(ocerz_unmap(first + off, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
        if (off == 0) {
            CHECK(ocerz_addr_committed(first) == 0);
            CHECK(ocerz_addr_committed(first + OCERZ_GUEST_PAGE_SIZE) == 1);
        }
    }

    uint64_t reused = ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE);
    CHECK(reused == first);
    CHECK(ocerz_ld(reused, 8) == 0);

    CHECK(ocerz_unmap(reused, chunk) == OCERZ_OK);
    uint64_t aligned = ocerz_map_anywhere_aligned(
        2ull * 1024 * 1024, PROT_READ | PROT_WRITE, 1ull * 1024 * 1024);
    CHECK(aligned != 0);
    CHECK((aligned & ((1ull * 1024 * 1024) - 1)) == 0);

    CHECK(ocerz_unmap(second, chunk) == OCERZ_OK);
    CHECK(ocerz_unmap(aligned, 2ull * 1024 * 1024) == OCERZ_OK);

    uint64_t fixed = lo + 0x10000;
    CHECK(ocerz_map_fixed(fixed, OCERZ_HOST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_OK);
    CHECK(ocerz_addr_prot(fixed) == (PROT_READ | PROT_WRITE));
    for (uint64_t off = 0; off < OCERZ_HOST_PAGE_SIZE;
         off += OCERZ_GUEST_PAGE_SIZE)
        ocerz_st(fixed + off, 8, 0x100 + off);

    CHECK(ocerz_unmap(fixed + OCERZ_GUEST_PAGE_SIZE,
                      OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_addr_committed(fixed) == 1);
    CHECK(ocerz_addr_committed(fixed + OCERZ_GUEST_PAGE_SIZE) == 0);
    CHECK(ocerz_addr_committed(fixed + 2 * OCERZ_GUEST_PAGE_SIZE) == 1);
    CHECK(ocerz_ld(fixed, 8) == 0x100);
    CHECK(ocerz_ld(fixed + 2 * OCERZ_GUEST_PAGE_SIZE, 8) ==
          0x100 + 2 * OCERZ_GUEST_PAGE_SIZE);

    uint64_t query = fixed + OCERZ_GUEST_PAGE_SIZE;
    uint64_t region_size = 0;
    unsigned region_prot = 0, region_max_prot = 0;
    CHECK(ocerz_guest_vm_region(&query, &region_size, &region_prot,
                                &region_max_prot) == 1);
    CHECK(query == fixed + 2 * OCERZ_GUEST_PAGE_SIZE);
    CHECK(region_size == 2 * OCERZ_GUEST_PAGE_SIZE);
    CHECK((region_prot & (PROT_READ | PROT_WRITE)) ==
          (PROT_READ | PROT_WRITE));

    CHECK(ocerz_map_fixed(fixed + OCERZ_GUEST_PAGE_SIZE,
                          OCERZ_GUEST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_OK);
    CHECK(ocerz_ld(fixed + OCERZ_GUEST_PAGE_SIZE, 8) == 0);
    CHECK(ocerz_ld(fixed, 8) == 0x100);
    CHECK(ocerz_ld(fixed + 2 * OCERZ_GUEST_PAGE_SIZE, 8) ==
          0x100 + 2 * OCERZ_GUEST_PAGE_SIZE);

    CHECK(ocerz_protect(fixed, OCERZ_GUEST_PAGE_SIZE, PROT_READ) == OCERZ_OK);
    CHECK(ocerz_addr_prot(fixed) == PROT_READ);
    unsigned host_prot = ocerz_host_region_prot(fixed, NULL, NULL);
    CHECK((host_prot & PROT_WRITE) != 0);
    CHECK(ocerz_protect(fixed, OCERZ_GUEST_PAGE_SIZE,
                        PROT_READ | PROT_WRITE) == OCERZ_OK);
    for (uint64_t off = 0; off < OCERZ_HOST_PAGE_SIZE;
         off += OCERZ_GUEST_PAGE_SIZE)
        CHECK(ocerz_unmap(fixed + off, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK((ocerz_host_region_prot(fixed, NULL, NULL) & 0xff) == 0);
    CHECK(ocerz_addr_prot(fixed) == -1);
    CHECK(ocerz_commit_fault_page(fixed) == 0);
    CHECK((ocerz_host_region_prot(fixed, NULL, NULL) & 0xff) == 0);

    pid_t protection_child = fork();
    CHECK(protection_child >= 0);
    if (protection_child == 0) {
        uint64_t page = lo + 0x30000;
        if (ocerz_map_fixed(page, OCERZ_HOST_PAGE_SIZE,
                            PROT_READ | PROT_WRITE) != OCERZ_OK)
            _exit(2);
        SiblingWriter writer = { .address = page };
        pthread_t thread;
        if (pthread_create(&thread, NULL, sibling_writer, &writer) != 0)
            _exit(3);
        for (int i = 0; i < 10000; i++)
            if (ocerz_map_fixed(page + OCERZ_GUEST_PAGE_SIZE,
                                OCERZ_GUEST_PAGE_SIZE,
                                PROT_NONE) != OCERZ_OK)
                _exit(4);
        __atomic_store_n(&writer.stop, 1, __ATOMIC_RELEASE);
        if (pthread_join(thread, NULL) != 0)
            _exit(5);
        _exit(0);
    }
    if (protection_child > 0) {
        int status = 0;
        CHECK(waitpid(protection_child, &status, 0) == protection_child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    test_padded_shared_activation(lo + 0x40000);

    uint64_t claim_zero = lo + 9ull * 1024 * 1024;
    CHECK(ocerz_map_fixed(claim_zero, OCERZ_HOST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_OK);
    ocerz_st(claim_zero + 2 * OCERZ_GUEST_PAGE_SIZE, 8,
             0xfeedfacecafebeefull);
    ocerz_st(claim_zero + 3 * OCERZ_GUEST_PAGE_SIZE, 8,
             0xdecafbad01234567ull);
    CHECK(ocerz_unmap(claim_zero + 3 * OCERZ_GUEST_PAGE_SIZE,
                      OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_map_claim_fixed(claim_zero + 3 * OCERZ_GUEST_PAGE_SIZE,
                                OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_OK);
    CHECK(ocerz_ld(claim_zero + 3 * OCERZ_GUEST_PAGE_SIZE, 8) == 0);
    CHECK(ocerz_ld(claim_zero + 2 * OCERZ_GUEST_PAGE_SIZE, 8) ==
          0xfeedfacecafebeefull);
    CHECK(ocerz_unmap(claim_zero, OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);

    uint64_t guarded = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE,
                                          PROT_READ | PROT_WRITE);
    CHECK(guarded != 0);
    CHECK(ocerz_map_claim_fixed(guarded, OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_ENOMEM);
    CHECK(ocerz_map_claim_fixed(guarded + OCERZ_GUEST_PAGE_SIZE,
                                OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_ENOMEM);
    CHECK(ocerz_commit_fault_page(guarded + OCERZ_GUEST_PAGE_SIZE) == 0);
    CHECK(ocerz_unmap(guarded + OCERZ_GUEST_PAGE_SIZE,
                      OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_map_claim_fixed(guarded + OCERZ_GUEST_PAGE_SIZE,
                                OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_ENOMEM);
    CHECK(ocerz_unmap(guarded, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_map_claim_fixed(guarded, OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_OK);
    CHECK(ocerz_map_claim_fixed(guarded, OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_ENOMEM);
    CHECK(ocerz_unmap(guarded, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);

    uint64_t donated = ocerz_map_donate(OCERZ_HOST_PAGE_SIZE);
    CHECK(donated != 0);
    CHECK(ocerz_addr_committed(donated) == 1);
    CHECK(ocerz_map_claim_fixed(donated, OCERZ_HOST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_ENOMEM);
    CHECK(ocerz_unmap(donated, OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_map_claim_fixed(donated, OCERZ_HOST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE) == OCERZ_OK);
    CHECK(ocerz_unmap(donated, OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);

    CHECK(ocerz_map_anywhere(0, PROT_READ | PROT_WRITE) == 0);
    CHECK(ocerz_map_anywhere(UINT64_MAX, PROT_READ | PROT_WRITE) == 0);
    CHECK(ocerz_map_anywhere_aligned(OCERZ_GUEST_PAGE_SIZE,
                                     PROT_READ | PROT_WRITE, 0x18000) == 0);

    pthread_t workers[WORKER_COUNT];
    WorkerCtx worker_ctx[WORKER_COUNT];
    for (int i = 0; i < WORKER_COUNT; i++) {
        worker_ctx[i].index = i;
        worker_ctx[i].failures = 0;
        CHECK(pthread_create(&workers[i], NULL, allocation_worker,
                             &worker_ctx[i]) == 0);
    }
    for (int i = 0; i < WORKER_COUNT; i++) {
        CHECK(pthread_join(workers[i], NULL) == 0);
        CHECK(worker_ctx[i].failures == 0);
    }

    printf("test_mem: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
