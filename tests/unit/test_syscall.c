/*
 * tests/unit/test_syscall.c
 *
 * Unit tests for the guest syscall boundary (src/syscall.c). The harness
 * brings up the same machinery the real run loop uses: ocerz_mem_init()
 * reserves the guest arena (so ocerz_g2h is valid and ocerz_map_anywhere /
 * ocerz_map_fixed work), then ocerz_vm_init() resets the CPU and links it to
 * the VM. Each test drives ocerz_handle_syscall() directly by writing the
 * syscall encoding into rax and the arguments into the architectural argument
 * registers (rdi rsi rdx r10 r8 r9), exactly as a guest SYSCALL instruction
 * would have left them — no instruction decoding is involved.
 *
 * The cardinal rule of the boundary is exercised throughout: every pointer
 * passed to a syscall is a GUEST address. Buffers are written into guest
 * memory via ocerz_g2h first, and the guest address (host address minus
 * guest_base) is what goes into the register. The handler is responsible for
 * translating it back before touching the host kernel; if it forwarded a
 * guest address blindly the kernel would fault or corrupt host state, so a
 * passing round-trip is itself proof the translation happened.
 *
 * Coverage: pipe (dual return), write/read round-trip through the pipe,
 * open/fstat64/close on /dev/null, anonymous mmap + g2h read/write + munmap,
 * MAP_FIXED at a chosen arena address, gettimeofday plausibility, getpid
 * matching the host, exit setting vm->exited/exit_code, execve with a NULL
 * path returning the kernel's EINVAL (CF set, rax=22) now that execve is a real
 * handler rather than a fatal stub, an unknown BSD number returning STEP_FATAL,
 * machdep gs_base, and a Mach task_self_trap matching mach_task_self().
 * Assertions count well past the required thirty.
 *
 * A scratch guest region is carved once with ocerz_map_anywhere and reused as
 * register-spill stack and I/O buffer space; gpr[RSP] is parked there so any
 * stack-resident extra arguments have a real backing page.
 */
#include "ocerz/vm.h"
#include "ocerz/syscall.h"
#include "ocerz/mem.h"
#include "ocerz/cpu.h"
#include "ocerz/interp.h"

#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static int tests_run;
static int tests_failed;

#define CHECK(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static OcerzVM vm;
static uint64_t scratch;

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

static uint64_t bsd(int num)
{
    return ((uint64_t)2 << 24) | (uint64_t)num;
}

static uint64_t mach(int num)
{
    return ((uint64_t)1 << 24) | (uint64_t)num;
}

static uint64_t machdep(int num)
{
    return ((uint64_t)3 << 24) | (uint64_t)num;
}

static int cf(const OcerzCPU *cpu)
{
    return (cpu->rflags & OCERZ_CF) != 0;
}

static void test_getpid(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(20), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK((pid_t)cpu->gpr[OCERZ_RAX] == getpid());
}

static void test_pipe_and_io(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(42), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    uint64_t rfd = cpu->gpr[OCERZ_RAX];
    uint64_t wfd = cpu->gpr[OCERZ_RDX];
    CHECK(rfd != wfd);
    CHECK((int64_t)rfd >= 0 && (int64_t)wfd >= 0);

    const char msg[] = "ocerz!";
    uint64_t gbuf = scratch;
    memcpy(ocerz_g2h(gbuf), msg, sizeof msg);

    set_args(cpu, bsd(4), wfd, gbuf, sizeof msg - 1, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == sizeof msg - 1);

    uint64_t gread = scratch + 256;
    memset(ocerz_g2h(gread), 0, 32);
    set_args(cpu, bsd(3), rfd, gread, sizeof msg - 1, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == sizeof msg - 1);
    CHECK(memcmp(ocerz_g2h(gread), msg, sizeof msg - 1) == 0);

    set_args(cpu, bsd(6), rfd, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
    set_args(cpu, bsd(6), wfd, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
}

static void test_open_fstat_close(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const char path[] = "/dev/null";
    uint64_t gpath = scratch + 512;
    memcpy(ocerz_g2h(gpath), path, sizeof path);

    set_args(cpu, bsd(5), gpath, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    uint64_t fd = cpu->gpr[OCERZ_RAX];
    CHECK((int64_t)fd >= 0);

    uint64_t gst = scratch + 1024;
    memset(ocerz_g2h(gst), 0xaa, 256);
    set_args(cpu, bsd(339), fd, gst, 0, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);

    set_args(cpu, bsd(6), fd, 0, 0, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
}

static void test_open_bad_path(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const char path[] = "/ocerz/definitely/not/here/xyzzy";
    uint64_t gpath = scratch + 1536;
    memcpy(ocerz_g2h(gpath), path, sizeof path);
    set_args(cpu, bsd(5), gpath, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 1);
    CHECK(cpu->gpr[OCERZ_RAX] == 2);
}

static void test_mmap_anon(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t len = 0x10000;
    set_args(cpu, bsd(197), 0, len, PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_PRIVATE, (uint64_t)-1, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    uint64_t gaddr = cpu->gpr[OCERZ_RAX];
    CHECK(gaddr >= ocerz_arena_lo && gaddr < ocerz_arena_hi);

    uint64_t *hp = ocerz_g2h(gaddr);
    hp[0] = 0xdeadbeefcafef00dull;
    hp[100] = 0x0123456789abcdefull;
    CHECK(ocerz_ld(gaddr, 8) == 0xdeadbeefcafef00dull);
    CHECK(ocerz_ld(gaddr + 800, 8) == 0x0123456789abcdefull);

    set_args(cpu, bsd(73), gaddr, len, 0, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
}

static void test_mmap_shared_requires_ordered(void)
{
    OcerzCPU *cpu = &vm.cpu;
    vm.jit_plain_mem = 1;
    vm.jit_ordered_required = 0;
    set_args(cpu, bsd(197), 0, 0x4000, PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_SHARED, (uint64_t)-1, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    uint64_t gaddr = cpu->gpr[OCERZ_RAX];
    CHECK(gaddr >= ocerz_arena_lo && gaddr < ocerz_arena_hi);
    CHECK(vm.jit_ordered_required == 1);
    CHECK(vm.jit_plain_mem == 0);

    set_args(cpu, bsd(73), gaddr, 0x4000, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
}

static void test_mmap_fixed(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t len = 0x8000;
    uint64_t target = ocerz_map_anywhere(len, PROT_READ | PROT_WRITE);
    CHECK(target != 0);

    set_args(cpu, bsd(197), target, len, PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == target);

    ocerz_st(target, 8, 0x4242424242424242ull);
    CHECK(ocerz_ld(target, 8) == 0x4242424242424242ull);

    set_args(cpu, bsd(74), target, len, PROT_READ, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
}

static void test_mmap_fixed_outside(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t bad = ocerz_arena_hi + 0x10000;
    /* A FIXED anonymous mmap outside the arena is honoured iff ocerz can reserve that
     * exact host address (identity-mapped mode grabs it via mach_vm_allocate(FIXED) in
     * reserve_host_fixed). Whether `bad` is free depends on the process's memory layout,
     * so this test used to flake -- pass when `bad` happened to be taken, fail when it
     * was free. Pin the outcome by occupying `bad` on the host first, forcing the
     * "cannot place this FIXED mapping" -> ENOMEM path every time. (If `bad` was already
     * occupied the allocate simply fails and it stays occupied, which is equally fine.) */
    mach_vm_address_t occ = bad;
    kern_return_t okr = mach_vm_allocate(mach_task_self(), &occ, 0x4000, VM_FLAGS_FIXED);
    set_args(cpu, bsd(197), bad, 0x4000, PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 1);
    CHECK(cpu->gpr[OCERZ_RAX] == 12);
    if (okr == KERN_SUCCESS)
        mach_vm_deallocate(mach_task_self(), occ, 0x4000);
}

static void test_madvise(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(75), scratch, 0x1000, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
}

static void test_gettimeofday(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gtv = scratch + 2048;
    memset(ocerz_g2h(gtv), 0, 16);
    set_args(cpu, bsd(116), gtv, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    int64_t sec = (int64_t)ocerz_ld(gtv, 8);
    CHECK(sec > 1500000000);
}

static void test_getentropy(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gbuf = scratch + 2304;
    memset(ocerz_g2h(gbuf), 0, 32);
    set_args(cpu, bsd(500), gbuf, 16, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
}

static void test_writev(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(42), 0, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
    uint64_t rfd = cpu->gpr[OCERZ_RAX];
    uint64_t wfd = cpu->gpr[OCERZ_RDX];

    uint64_t gpart0 = scratch + 2560;
    uint64_t gpart1 = scratch + 2576;
    memcpy(ocerz_g2h(gpart0), "foo", 3);
    memcpy(ocerz_g2h(gpart1), "barbaz", 6);

    uint64_t giov = scratch + 2624;
    ocerz_st(giov + 0, 8, gpart0);
    ocerz_st(giov + 8, 8, 3);
    ocerz_st(giov + 16, 8, gpart1);
    ocerz_st(giov + 24, 8, 6);

    set_args(cpu, bsd(121), wfd, giov, 2, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 9);

    uint64_t gread = scratch + 2688;
    memset(ocerz_g2h(gread), 0, 16);
    set_args(cpu, bsd(3), rfd, gread, 9, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] == 9);
    CHECK(memcmp(ocerz_g2h(gread), "foobarbaz", 9) == 0);

    set_args(cpu, bsd(6), rfd, 0, 0, 0, 0, 0);
    ocerz_handle_syscall(&vm, cpu);
    set_args(cpu, bsd(6), wfd, 0, 0, 0, 0, 0);
    ocerz_handle_syscall(&vm, cpu);
}

static void test_machdep_gs_base(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t tls = 0x2222333344445555ull;
    set_args(cpu, machdep(3), tls, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gs_base == tls);
    CHECK(cpu->gpr[OCERZ_RAX] == 0x60);
}

static void test_machdep_unknown(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, machdep(7), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_FATAL);
}

static void test_mach_task_self(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, mach(28), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] == (uint64_t)mach_task_self());
}

static void test_mach_thread_self(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, mach(27), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] != 0);
}

static void test_mach_vm_allocate(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gaddr_out = scratch + 2944;
    ocerz_st(gaddr_out, 8, 0);
    set_args(cpu, mach(10), (uint64_t)mach_task_self(), gaddr_out, 0x8000, 1, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
    uint64_t got = ocerz_ld(gaddr_out, 8);
    CHECK(got >= ocerz_arena_lo && got < ocerz_arena_hi);
    ocerz_st(got, 8, 0x9999888877776666ull);
    CHECK(ocerz_ld(got, 8) == 0x9999888877776666ull);

    set_args(cpu, mach(12), (uint64_t)mach_task_self(), got, 0x8000, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
}

static void test_mach_timebase(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gtb = scratch + 3072;
    ocerz_st(gtb, 4, 0);
    ocerz_st(gtb + 4, 4, 0);
    set_args(cpu, mach(89), gtb, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
    CHECK(ocerz_ld(gtb, 4) != 0);
    CHECK(ocerz_ld(gtb + 4, 4) != 0);
}

static void test_mach_unknown(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, mach(0x123456), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_FATAL);
}

static void test_sigaction(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gact = scratch + 3200;
    uint64_t goact = scratch + 3264;
    ocerz_st(gact, 8, 0x1111222233334444ull);
    memset(ocerz_g2h(goact), 0xff, 16);
    set_args(cpu, bsd(46), 15, gact, goact, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
    CHECK(ocerz_ld(goact, 8) == 0);
    CHECK(ocerz_ld(goact + 8, 8) == 0);
}

static void test_execve_bad_args(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(59), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 1);
    CHECK(cpu->gpr[OCERZ_RAX] == 22);
}

static void test_unknown_bsd(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(9999), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_FATAL);
}

static void test_unknown_class(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, ((uint64_t)7 << 24) | 5, 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_FATAL);
}

static void test_exit(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(1), 42, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_EXIT);
    CHECK(vm.exited == 1);
    CHECK(vm.exit_code == 42);
}

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem init failed\n");
        return 2;
    }
    if (ocerz_vm_init(&vm) != OCERZ_OK) {
        fprintf(stderr, "vm init failed\n");
        return 2;
    }

    scratch = ocerz_map_anywhere(0x10000, PROT_READ | PROT_WRITE);
    if (scratch == 0) {
        fprintf(stderr, "scratch alloc failed\n");
        return 2;
    }
    vm.cpu.gpr[OCERZ_RSP] = scratch + 0x8000;

    test_getpid();
    test_pipe_and_io();
    test_open_fstat_close();
    test_open_bad_path();
    test_mmap_anon();
    test_mmap_shared_requires_ordered();
    test_mmap_fixed();
    test_mmap_fixed_outside();
    test_madvise();
    test_gettimeofday();
    test_getentropy();
    test_writev();
    test_sigaction();
    test_machdep_gs_base();
    test_machdep_unknown();
    test_mach_task_self();
    test_mach_thread_self();
    test_mach_vm_allocate();
    test_mach_timebase();
    test_mach_unknown();
    test_execve_bad_args();
    test_unknown_bsd();
    test_unknown_class();
    test_exit();

    fprintf(stderr, "test_syscall: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
