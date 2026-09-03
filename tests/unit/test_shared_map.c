/* Shared mappings across the guest 4K / host 16K page-size boundary. */
#include "ocerz/cpu.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
#include "ocerz/vm.h"

#include <fcntl.h>
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

static uint64_t bsd(int num)
{
    return ((uint64_t)2 << 24) | (uint64_t)num;
}

static void set_args(OcerzCPU *cpu, uint64_t rax, uint64_t rdi, uint64_t rsi,
                     uint64_t rdx, uint64_t r10, uint64_t r8, uint64_t r9)
{
    cpu->gpr[OCERZ_RAX] = rax;
    cpu->gpr[OCERZ_RDI] = rdi;
    cpu->gpr[OCERZ_RSI] = rsi;
    cpu->gpr[OCERZ_RDX] = rdx;
    cpu->gpr[OCERZ_R10] = r10;
    cpu->gpr[OCERZ_R8] = r8;
    cpu->gpr[OCERZ_R9] = r9;
    cpu->rflags &= ~(uint64_t)OCERZ_CF;
}

static int carry(const OcerzCPU *cpu)
{
    return (cpu->rflags & OCERZ_CF) != 0;
}

static int make_backing_file(void)
{
    char path[] = "/tmp/ocerz-shared-map-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, 4 * OCERZ_HOST_PAGE_SIZE) != 0) {
        close(fd);
        return -1;
    }
    unsigned char page[OCERZ_GUEST_PAGE_SIZE];
    for (unsigned i = 0; i < 16; i++) {
        memset(page, 0x10 + i, sizeof page);
        if (pwrite(fd, page, sizeof page,
                   (off_t)i * OCERZ_GUEST_PAGE_SIZE) != sizeof page) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void test_writable_4k_coherence(int fd)
{
    uint64_t first = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE,
                                        PROT_READ | PROT_WRITE);
    uint64_t alias = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE,
                                        PROT_READ | PROT_WRITE);
    CHECK(first != 0);
    CHECK(alias != 0);
    CHECK((first & (OCERZ_HOST_PAGE_SIZE - 1)) == 0);
    CHECK((alias & (OCERZ_HOST_PAGE_SIZE - 1)) == 0);
    CHECK(ocerz_map_shared_file(first, OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE, fd, 0) == OCERZ_OK);
    CHECK(ocerz_map_shared_file(alias, OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE, fd, 0) == OCERZ_OK);

    ocerz_st(first + 8, 8, 0x1122334455667788ull);
    CHECK(ocerz_ld(alias + 8, 8) == 0x1122334455667788ull);
    uint64_t value = 0;
    CHECK(pread(fd, &value, sizeof value, 8) == sizeof value);
    CHECK(value == 0x1122334455667788ull);

    value = 0x8877665544332211ull;
    CHECK(pwrite(fd, &value, sizeof value, 16) == sizeof value);
    CHECK(ocerz_ld(first + 16, 8) == value);
    CHECK(ocerz_ld(alias + 16, 8) == value);

    CHECK(ocerz_unmap(first, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    ocerz_st(alias + 24, 8, 0xfeedfacecafebeefull);
    CHECK(pread(fd, &value, sizeof value, 24) == sizeof value);
    CHECK(value == 0xfeedfacecafebeefull);
    CHECK(ocerz_unmap(alias, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
}

static void test_partial_unmap_keeps_shared_survivors(int fd)
{
    uint64_t first = ocerz_map_anywhere(OCERZ_HOST_PAGE_SIZE,
                                        PROT_READ | PROT_WRITE);
    uint64_t alias = ocerz_map_anywhere(OCERZ_HOST_PAGE_SIZE,
                                        PROT_READ | PROT_WRITE);
    CHECK(first != 0);
    CHECK(alias != 0);
    CHECK(ocerz_map_shared_file(first, OCERZ_HOST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE, fd,
                                OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_map_shared_file(alias, OCERZ_HOST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE, fd,
                                OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);

    CHECK(ocerz_unmap(first + OCERZ_GUEST_PAGE_SIZE,
                      OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_addr_committed(first) == 1);
    CHECK(ocerz_addr_committed(first + OCERZ_GUEST_PAGE_SIZE) == 0);
    CHECK(ocerz_addr_committed(first + 2 * OCERZ_GUEST_PAGE_SIZE) == 1);
    ocerz_st(first + 2 * OCERZ_GUEST_PAGE_SIZE + 32, 8,
             0x1234abcddcba4321ull);
    CHECK(ocerz_ld(alias + 2 * OCERZ_GUEST_PAGE_SIZE + 32, 8) ==
          0x1234abcddcba4321ull);

    CHECK(ocerz_unmap(first, OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);
    ocerz_st(alias + 40, 8, 0xa5a55a5af0f00f0full);
    uint64_t value = 0;
    CHECK(pread(fd, &value, sizeof value,
                OCERZ_HOST_PAGE_SIZE + 40) == sizeof value);
    CHECK(value == 0xa5a55a5af0f00f0full);
    CHECK(ocerz_unmap(alias, OCERZ_HOST_PAGE_SIZE) == OCERZ_OK);
}

static void test_private_replaces_only_shared_slice(OcerzVM *vm, int fd)
{
    uint64_t first = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE,
                                        PROT_READ | PROT_WRITE);
    uint64_t alias = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE,
                                        PROT_READ | PROT_WRITE);
    CHECK(first != 0);
    CHECK(alias != 0);
    CHECK(ocerz_map_shared_file(first, OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE, fd, 0) == OCERZ_OK);
    CHECK(ocerz_map_shared_file(alias, OCERZ_GUEST_PAGE_SIZE,
                                PROT_READ | PROT_WRITE, fd, 0) == OCERZ_OK);

    const uint64_t shared_value = 0x1020304050607080ull;
    ocerz_st(first + 32, 8, shared_value);
    CHECK(ocerz_ld(alias + 32, 8) == shared_value);

    set_args(&vm->cpu, bsd(197), first, OCERZ_GUEST_PAGE_SIZE,
             PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON,
             (uint64_t)-1, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 0);
    CHECK(vm->cpu.gpr[OCERZ_RAX] == first);
    CHECK(ocerz_ld(first + 32, 8) == 0);
    CHECK(ocerz_ld(alias + 32, 8) == shared_value);

    const uint64_t private_value = 0xfedcba9876543210ull;
    ocerz_st(first + 32, 8, private_value);
    CHECK(ocerz_ld(alias + 32, 8) == shared_value);
    uint64_t file_value = 0;
    CHECK(pread(fd, &file_value, sizeof file_value, 32) == sizeof file_value);
    CHECK(file_value == shared_value);

    const uint64_t alias_value = 0x8877665544332211ull;
    ocerz_st(alias + 32, 8, alias_value);
    CHECK(ocerz_ld(first + 32, 8) == private_value);
    CHECK(pread(fd, &file_value, sizeof file_value, 32) == sizeof file_value);
    CHECK(file_value == alias_value);

    CHECK(ocerz_unmap(first, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
    CHECK(ocerz_unmap(alias, OCERZ_GUEST_PAGE_SIZE) == OCERZ_OK);
}

static void test_readonly_copy_and_writable_rejection(OcerzVM *vm, int fd,
                                                       uint64_t base)
{
    CHECK(ocerz_map_fixed(base, OCERZ_HOST_PAGE_SIZE,
                          PROT_READ | PROT_WRITE) == OCERZ_OK);
    ocerz_st(base, 8, 0x1111111122222222ull);
    ocerz_st(base + 2 * OCERZ_GUEST_PAGE_SIZE, 8,
             0x3333333344444444ull);

    uint64_t target = base + OCERZ_GUEST_PAGE_SIZE;
    set_args(&vm->cpu, bsd(197), target, OCERZ_GUEST_PAGE_SIZE, PROT_READ,
             MAP_FIXED | MAP_SHARED, (uint64_t)fd, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 0);
    CHECK(vm->cpu.gpr[OCERZ_RAX] == target);
    CHECK(ocerz_ld(target, 1) == 0x10);
    CHECK(ocerz_ld(target + OCERZ_GUEST_PAGE_SIZE - 1, 1) == 0x10);
    CHECK(ocerz_ld(base, 8) == 0x1111111122222222ull);
    CHECK(ocerz_ld(base + 2 * OCERZ_GUEST_PAGE_SIZE, 8) ==
          0x3333333344444444ull);

    set_args(&vm->cpu, bsd(197), target, OCERZ_GUEST_PAGE_SIZE,
             PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED,
             (uint64_t)fd, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 1);
    CHECK(ocerz_ld(base, 8) == 0x1111111122222222ull);
    CHECK(ocerz_ld(base + 2 * OCERZ_GUEST_PAGE_SIZE, 8) ==
          0x3333333344444444ull);
}

static void test_readonly_subpage_allows_private_sibling(OcerzVM *vm, int fd,
                                                         uint64_t base)
{
    uint64_t before = 0x0badf00dc001d00dull;
    CHECK(pwrite(fd, &before, sizeof before, 64) == sizeof before);
    CHECK(ocerz_map_fixed(base, OCERZ_HOST_PAGE_SIZE, PROT_NONE) == OCERZ_OK);
    set_args(&vm->cpu, bsd(197), base, OCERZ_GUEST_PAGE_SIZE, PROT_READ,
             MAP_FIXED | MAP_SHARED, (uint64_t)fd, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 0);
    CHECK(ocerz_ld(base + 64, 8) == before);

    uint64_t after = 0x123456789abcdef0ull;
    CHECK(pwrite(fd, &after, sizeof after, 64) == sizeof after);
    CHECK(ocerz_ld(base + 64, 8) == before);

    uint64_t sibling = base + OCERZ_GUEST_PAGE_SIZE;
    set_args(&vm->cpu, bsd(197), sibling, OCERZ_GUEST_PAGE_SIZE,
             PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON,
             (uint64_t)-1, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 0);
    CHECK(vm->cpu.gpr[OCERZ_RAX] == sibling);
    ocerz_st(sibling, 8, 0xabcdef0123456789ull);
    CHECK(ocerz_ld(sibling, 8) == 0xabcdef0123456789ull);
    CHECK(ocerz_ld(base + 64, 8) == before);
    uint64_t file_value = 0;
    CHECK(pread(fd, &file_value, sizeof file_value,
                OCERZ_GUEST_PAGE_SIZE) == sizeof file_value);
    CHECK(file_value != 0xabcdef0123456789ull);
}

static void test_shared_anon_stays_shared(OcerzVM *vm, uint64_t base)
{
    CHECK(ocerz_map_fixed(base, OCERZ_HOST_PAGE_SIZE, PROT_NONE) == OCERZ_OK);
    set_args(&vm->cpu, bsd(197), base, OCERZ_GUEST_PAGE_SIZE,
             PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_SHARED | MAP_ANON, (uint64_t)-1, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 0);
    CHECK(vm->cpu.gpr[OCERZ_RAX] == base);

    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        ocerz_st(base, 8, 0x55aa55aa11223344ull);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(ocerz_ld(base, 8) == 0x55aa55aa11223344ull);

    /* A private 4K sibling cannot coexist with this physical 16K shared
     * backing. Failing MAP_FIXED preserves the requested shared semantics. */
    uint64_t sibling = base + OCERZ_GUEST_PAGE_SIZE;
    set_args(&vm->cpu, bsd(197), sibling, OCERZ_GUEST_PAGE_SIZE,
             PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON,
             (uint64_t)-1, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 1);
    CHECK(ocerz_ld(base, 8) == 0x55aa55aa11223344ull);
}

static void test_wine_kuser_padded_mapping(OcerzVM *vm)
{
    const uint64_t kuser = 0x7ffe0000ull;
    const uint64_t dispatcher = kuser + OCERZ_GUEST_PAGE_SIZE;
    const uint64_t initial = 0x1122334455667788ull;
    const uint64_t updated = 0x8877665544332211ull;
    const uint64_t dispatch_value = 0xcafef00d0badc0deull;
    char path[] = "/tmp/ocerz-kuser-map-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    unlink(path);

    CHECK(ftruncate(fd, OCERZ_GUEST_PAGE_SIZE) == 0);
    CHECK(pwrite(fd, &initial, sizeof initial, 64) == sizeof initial);

    set_args(&vm->cpu, bsd(197), kuser, OCERZ_GUEST_PAGE_SIZE, PROT_READ,
             MAP_FIXED | MAP_SHARED, (uint64_t)fd, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 0);
    CHECK(vm->cpu.gpr[OCERZ_RAX] == kuser);
    CHECK(ocerz_addr_prot(kuser) == PROT_READ);
    CHECK(ocerz_ld(kuser + 64, 8) == initial);

    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    CHECK(st.st_size == (off_t)OCERZ_HOST_PAGE_SIZE);

    CHECK(pwrite(fd, &updated, sizeof updated, 64) == sizeof updated);
    CHECK(ocerz_ld(kuser + 64, 8) == updated);

    set_args(&vm->cpu, bsd(197), dispatcher, OCERZ_GUEST_PAGE_SIZE,
             PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON,
             (uint64_t)-1, 0);
    CHECK(ocerz_handle_syscall(vm, &vm->cpu) == OCERZ_STEP_OK);
    CHECK(carry(&vm->cpu) == 0);
    CHECK(vm->cpu.gpr[OCERZ_RAX] == dispatcher);
    CHECK(ocerz_addr_prot(kuser) == PROT_READ);
    CHECK(ocerz_addr_prot(dispatcher) == (PROT_READ | PROT_WRITE));
    CHECK(ocerz_ld(kuser + 64, 8) == updated);

    ocerz_st(dispatcher, 8, dispatch_value);
    uint64_t file_value = 0;
    CHECK(pread(fd, &file_value, sizeof file_value,
                OCERZ_GUEST_PAGE_SIZE) == sizeof file_value);
    CHECK(file_value == dispatch_value);
    CHECK(ocerz_ld(kuser + 64, 8) == updated);

    close(fd);
}

int main(void)
{
    const uint64_t lo = 0x100000000ull;
    const uint64_t hi = lo + 64ull * 1024 * 1024;
    OcerzVM vm;

    CHECK(ocerz_mem_init(lo, hi) == OCERZ_OK);
    CHECK(ocerz_mem_init_low_shadow() == OCERZ_OK);
    CHECK(ocerz_vm_init(&vm) == OCERZ_OK);
    int fd = make_backing_file();
    CHECK(fd >= 0);
    if (fd < 0)
        return 2;

    test_writable_4k_coherence(fd);
    test_partial_unmap_keeps_shared_survivors(fd);
    test_private_replaces_only_shared_slice(&vm, fd);
    test_readonly_copy_and_writable_rejection(&vm, fd, lo + 0x10000);
    test_readonly_subpage_allows_private_sibling(&vm, fd, lo + 0x20000);
    test_shared_anon_stays_shared(&vm, lo + 0x30000);
    test_wine_kuser_padded_mapping(&vm);

    close(fd);
    printf("test_shared_map: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
