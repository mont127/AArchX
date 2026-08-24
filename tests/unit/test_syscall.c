/* Unit tests for the guest syscall boundary. */
#include "ocerz/vm.h"
#include "ocerz/syscall.h"
#include "ocerz/mem.h"
#include "ocerz/cpu.h"
#include "ocerz/interp.h"
#include "ocerz/sys_raw.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mig.h>
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

static void test_mmap_fixed_prot_none_reservation(void)
{
    OcerzCPU *cpu = &vm.cpu;
    const uint64_t target = 0x7ff60000ull;
    const uint64_t reserve_len = 0x80000;
    const uint64_t active = target + 0x78000;
    const uint64_t active_len = 0x8000;
    const uint64_t fault_address = target + 0x7a030;

    set_args(cpu, bsd(197), target, reserve_len, PROT_NONE,
             MAP_ANON | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == target);
    CHECK(ocerz_addr_committed(target) == 1);
    CHECK(ocerz_addr_committed(active) == 1);

    set_args(cpu, bsd(74), active, active_len, PROT_READ | PROT_WRITE,
             0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);

    ocerz_st(fault_address, 8, 0x7ffda030cafef00dull);
    CHECK(ocerz_ld(fault_address, 8) == 0x7ffda030cafef00dull);

    set_args(cpu, bsd(73), target, reserve_len, 0, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(ocerz_addr_committed(active) == 0);
    CHECK(ocerz_protect(active, active_len,
                        PROT_READ | PROT_WRITE) == OCERZ_ENOMEM);
}

static uint64_t ldt_pack(uint64_t base, uint32_t limit, uint8_t access, int big, int gran)
{
    return (limit & 0xffffULL)
         | ((base & 0xffffffULL) << 16)
         | ((uint64_t)access << 40)
         | (((uint64_t)(limit >> 16) & 0xfULL) << 48)
         | ((uint64_t)(big & 1) << 54)
         | ((uint64_t)(gran & 1) << 55)
         | (((base >> 24) & 0xffULL) << 56);
}

static void test_i386_ldt(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gbuf = scratch + 4096;
    uint64_t cs32 = ldt_pack(0, 0xfffff, 0x9b, 1, 1);
    uint64_t fs32 = ldt_pack(0x7ffdf000, 0xfff, 0x93, 1, 0);
    ocerz_st(gbuf, 8, cs32);
    ocerz_st(gbuf + 8, 8, fs32);

    set_args(cpu, machdep(5), (uint64_t)(uint32_t)-1, gbuf, 2, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    uint64_t idx = cpu->gpr[OCERZ_RAX];
    CHECK(idx >= 1);

    ocerz_st(gbuf + 16, 8, 0);
    ocerz_st(gbuf + 24, 8, 0);
    set_args(cpu, machdep(6), idx, gbuf + 16, 2, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(ocerz_ld(gbuf + 16, 8) == cs32);
    CHECK(ocerz_ld(gbuf + 24, 8) == fs32);

    set_args(cpu, machdep(5), (uint64_t)(uint32_t)-1, gbuf, (uint64_t)(uint32_t)-1, 0, 0, 0);
    r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 1);
}

static void test_mmap_fixed_outside(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t bad = ocerz_arena_hi + 0x10000;

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

static int run_raw_mach_msg2(uint64_t gmsg, uint32_t send_size,
                             uint32_t request_id, uint32_t descriptor_count,
                             mach_port_t reply_port)
{
    const uint64_t mach64_send_kobject_call = 0x0000000200000000ull;
    uint64_t bits = ocerz_ld(gmsg + 0x00, 4);
    uint64_t remote_local =
        ocerz_ld(gmsg + 0x08, 4) | ((uint64_t)reply_port << 32);
    uint64_t voucher_id =
        ocerz_ld(gmsg + 0x10, 4) | ((uint64_t)request_id << 32);
    uint64_t desc_count_rcv_name =
        descriptor_count | ((uint64_t)reply_port << 32);

    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, mach(47), gmsg,
             MACH_SEND_MSG | MACH_RCV_MSG | mach64_send_kobject_call,
             bits | ((uint64_t)send_size << 32), remote_local, voucher_id,
             desc_count_rcv_name);
    ocerz_st(cpu->gpr[OCERZ_RSP] + 8, 8, 0x400);
    ocerz_st(cpu->gpr[OCERZ_RSP] + 16, 8, MACH_MSG_TIMEOUT_NONE);
    int step = ocerz_handle_syscall(&vm, cpu);
    CHECK(step == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] == MACH_MSG_SUCCESS);
    return step == OCERZ_STEP_OK &&
           cpu->gpr[OCERZ_RAX] == MACH_MSG_SUCCESS ? 0 : -1;
}

static int run_raw_mach_msg2_vector(uint64_t gmsg, uint32_t send_size,
                                    uint32_t request_id,
                                    uint32_t descriptor_count,
                                    mach_port_t reply_port)
{
    const uint64_t mach64_msg_vector = 0x0000000100000000ull;
    const uint64_t mach64_send_mq_call = 0x0000000400000000ull;
    uint64_t gvec = scratch + 0x9000;
    uint64_t gaux = scratch + 0x9400;
    memset(ocerz_g2h(gvec), 0, 48);
    memset(ocerz_g2h(gaux), 0, 0x80);
    ocerz_st(gvec + 0x00, 8, gmsg);
    ocerz_st(gvec + 0x10, 4, send_size);
    ocerz_st(gvec + 0x14, 4, 0x400);
    ocerz_st(gvec + 0x18, 8, gaux);
    ocerz_st(gvec + 0x2c, 4, 0x80);

    uint64_t bits = ocerz_ld(gmsg + 0x00, 4);
    uint64_t remote_local =
        ocerz_ld(gmsg + 0x08, 4) | ((uint64_t)reply_port << 32);
    uint64_t voucher_id =
        ocerz_ld(gmsg + 0x10, 4) | ((uint64_t)request_id << 32);
    uint64_t desc_count_rcv_name =
        descriptor_count | ((uint64_t)reply_port << 32);

    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, mach(47), gvec,
             MACH_SEND_MSG | MACH_RCV_MSG | mach64_msg_vector |
                 mach64_send_mq_call,
             bits | (2ull << 32), remote_local, voucher_id,
             desc_count_rcv_name);
    ocerz_st(cpu->gpr[OCERZ_RSP] + 8, 8, 2);
    ocerz_st(cpu->gpr[OCERZ_RSP] + 16, 8, MACH_MSG_TIMEOUT_NONE);
    int step = ocerz_handle_syscall(&vm, cpu);
    CHECK(step == OCERZ_STEP_OK);
    CHECK(cpu->gpr[OCERZ_RAX] == MACH_MSG_SUCCESS);
    return step == OCERZ_STEP_OK &&
           cpu->gpr[OCERZ_RAX] == MACH_MSG_SUCCESS ? 0 : -1;
}

struct ool_reply_server {
    mach_port_t port;
    mach_vm_address_t payload;
    mach_msg_size_t payload_size;
    uint32_t reply_id;
    int result;
};

static void *serve_ool_reply(void *opaque)
{
    struct ool_reply_server *server = opaque;
    _Alignas(8) unsigned char request[0x100];
    memset(request, 0, sizeof request);
    mach_msg_header_t *request_header = (mach_msg_header_t *)request;
    mach_msg_return_t mr =
        mach_msg(request_header, MACH_RCV_MSG, 0, sizeof request,
                 server->port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (mr != MACH_MSG_SUCCESS) {
        server->result = (int)mr;
        return NULL;
    }

    _Alignas(8) unsigned char reply[0x2c];
    memset(reply, 0, sizeof reply);
    mach_msg_header_t *reply_header = (mach_msg_header_t *)reply;
    reply_header->msgh_bits =
        MACH_MSGH_BITS_COMPLEX |
        MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply_header->msgh_size = sizeof reply;
    reply_header->msgh_remote_port = request_header->msgh_remote_port;
    reply_header->msgh_id = (mach_msg_id_t)server->reply_id;

    mach_msg_body_t body = { .msgh_descriptor_count = 1 };
    mach_msg_ool_descriptor_t descriptor = {
        .address = (void *)(uintptr_t)server->payload,
        .deallocate = FALSE,
        .copy = MACH_MSG_VIRTUAL_COPY,
        .type = MACH_MSG_OOL_DESCRIPTOR,
        .size = server->payload_size,
    };
    memcpy(reply + 0x18, &body, sizeof body);
    memcpy(reply + 0x1c, &descriptor, sizeof descriptor);

    mr = mach_msg(reply_header, MACH_SEND_MSG, sizeof reply, 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    server->result = (int)mr;
    return NULL;
}

static void run_received_ool_case(mach_vm_size_t payload_size, int expect_copy,
                                  uint32_t request_id)
{
    const uint64_t first = 0x0123456789abcdefull;
    const uint64_t last = 0xfedcba9876543210ull;
    struct ool_reply_server server = {
        .payload_size = (mach_msg_size_t)payload_size,
        .reply_id = request_id + 100,
        .result = -1,
    };

    kern_return_t kr = mach_vm_allocate(mach_task_self(), &server.payload,
                                        payload_size, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        return;
    if (expect_copy) {
        memcpy((void *)(uintptr_t)server.payload, &first, sizeof first);
        memcpy((void *)(uintptr_t)(server.payload + payload_size - sizeof last),
               &last, sizeof last);
    }

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &server.port);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        goto out_payload;
    kr = mach_port_insert_right(mach_task_self(), server.port, server.port,
                                MACH_MSG_TYPE_MAKE_SEND);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        goto out_port;

    mach_port_t reply_port = mig_get_reply_port();
    CHECK(reply_port != MACH_PORT_NULL);
    if (reply_port == MACH_PORT_NULL)
        goto out_port;

    pthread_t thread;
    int pr = pthread_create(&thread, NULL, serve_ool_reply, &server);
    CHECK(pr == 0);
    if (pr != 0) {
        mig_put_reply_port(reply_port);
        goto out_port;
    }

    uint64_t guest_copy = 0;
    uint64_t gmsg = scratch + 0x5c00;
    memset(ocerz_g2h(gmsg), 0, 0x400);
    ocerz_st(gmsg + 0x00, 4,
             MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                            MACH_MSG_TYPE_MAKE_SEND_ONCE));
    ocerz_st(gmsg + 0x04, 4, 0x18);
    ocerz_st(gmsg + 0x08, 4, server.port);
    ocerz_st(gmsg + 0x0c, 4, reply_port);
    ocerz_st(gmsg + 0x14, 4, request_id);

    int msg_rc = run_raw_mach_msg2_vector(gmsg, 0x18, request_id, 0,
                                          reply_port);
    if (msg_rc == 0) {
        CHECK(ocerz_ld(gmsg + 0x14, 4) == server.reply_id);
        CHECK((ocerz_ld(gmsg, 4) & MACH_MSGH_BITS_COMPLEX) != 0);
        CHECK(ocerz_ld(gmsg + 0x18, 4) == 1);
        CHECK(ocerz_ld(gmsg + 0x27, 1) == MACH_MSG_OOL_DESCRIPTOR);
        guest_copy = ocerz_ld(gmsg + 0x1c, 8);
        uint32_t delivered_size = (uint32_t)ocerz_ld(gmsg + 0x28, 4);
        if (expect_copy) {
            CHECK(guest_copy >= ocerz_arena_lo && guest_copy < ocerz_arena_hi);
            CHECK(guest_copy != server.payload);
            CHECK(delivered_size == payload_size);
            CHECK(ocerz_addr_readable(guest_copy));
            CHECK(ocerz_addr_readable(guest_copy + payload_size - 1));
            if (guest_copy >= ocerz_arena_lo && guest_copy < ocerz_arena_hi &&
                ocerz_addr_readable(guest_copy) &&
                ocerz_addr_readable(guest_copy + payload_size - 1)) {
                CHECK(ocerz_ld(guest_copy, 8) == first);
                CHECK(ocerz_ld(guest_copy + payload_size - sizeof last, 8) == last);
            }
        } else {
            CHECK(guest_copy == 0);
            CHECK(delivered_size == 0);
        }
    }

    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(server.result == MACH_MSG_SUCCESS);
    mig_put_reply_port(reply_port);
    if (expect_copy && guest_copy >= ocerz_arena_lo &&
        guest_copy < ocerz_arena_hi)
        CHECK(ocerz_unmap(guest_copy, payload_size) == OCERZ_OK);

out_port:
    mach_port_destruct(mach_task_self(), server.port, -1, 0);
out_payload:
    mach_vm_deallocate(mach_task_self(), server.payload, payload_size);
}

static uint64_t task_virtual_size(void)
{
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                 (task_info_t)&info, &count);
    CHECK(kr == KERN_SUCCESS);
    return kr == KERN_SUCCESS ? info.virtual_size : 0;
}

static void test_received_large_ool_is_relocated(void)
{
    const uint64_t allowance = 8ull * 1024 * 1024;
    run_received_ool_case(0x4000, 1, 31900);

    uint64_t before = task_virtual_size();
    run_received_ool_case(0x1000000 + 0x4000, 1, 32000);
    uint64_t after = task_virtual_size();
    if (before && after)
        CHECK(after <= before + allowance);

    before = after;
    run_received_ool_case(0x4000000 + 0x4000, 0, 32100);
    after = task_virtual_size();
    if (before && after)
        CHECK(after <= before + allowance);
}

static int run_raw_vm_region_query(uint32_t request_id, uint64_t query,
                                   uint64_t gmsg)
{
    mach_port_t reply_port = mig_get_reply_port();
    CHECK(reply_port != MACH_PORT_NULL);
    if (reply_port == MACH_PORT_NULL)
        return -1;

    memset(ocerz_g2h(gmsg), 0, 0x400);
    ocerz_st(gmsg + 0x00, 4,
             MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                            MACH_MSG_TYPE_MAKE_SEND_ONCE));
    ocerz_st(gmsg + 0x04, 4, 0x30);
    ocerz_st(gmsg + 0x08, 4, mach_task_self());
    ocerz_st(gmsg + 0x0c, 4, reply_port);
    ocerz_st(gmsg + 0x10, 4, MACH_PORT_NULL);
    ocerz_st(gmsg + 0x14, 4, request_id);
    memcpy(ocerz_g2h(gmsg + 0x18), &NDR_record, sizeof NDR_record);
    ocerz_st(gmsg + 0x20, 8, query);
    if (request_id == 4815) {
        ocerz_st(gmsg + 0x28, 4, 0);
        ocerz_st(gmsg + 0x2c, 4, VM_REGION_SUBMAP_INFO_COUNT_64);
    } else {
        ocerz_st(gmsg + 0x28, 4, VM_REGION_BASIC_INFO_64);
        ocerz_st(gmsg + 0x2c, 4, VM_REGION_BASIC_INFO_COUNT_64);
    }

    int step = run_raw_mach_msg2(gmsg, 0x30, request_id, 0, reply_port);
    mig_put_reply_port(reply_port);
    return step;
}

static void test_mach_vm_region_mig_translation(void)
{
    uint64_t query = scratch + 0x1000;

    uint64_t recurse_msg = scratch + 0x4000;
    if (run_raw_vm_region_query(4815, query, recurse_msg) == 0) {
        CHECK(ocerz_ld(recurse_msg + 0x14, 4) == 4915);
        CHECK((ocerz_ld(recurse_msg, 4) & 0x80000000u) == 0);
        CHECK(ocerz_ld(recurse_msg + 0x20, 4) == KERN_SUCCESS);
        uint64_t address = ocerz_ld(recurse_msg + 0x24, 8);
        uint64_t size = ocerz_ld(recurse_msg + 0x2c, 8);
        CHECK(address >= ocerz_arena_lo && address < ocerz_arena_hi);
        CHECK(query >= address && query - address < size);
        CHECK((uint64_t)(uintptr_t)ocerz_g2h(address) != address);
    }

    uint64_t region_msg = scratch + 0x4400;
    if (run_raw_vm_region_query(4816, query, region_msg) == 0) {
        CHECK(ocerz_ld(region_msg + 0x14, 4) == 4916);
        CHECK((ocerz_ld(region_msg, 4) & 0x80000000u) != 0);
        CHECK(ocerz_ld(region_msg + 0x18, 4) == 1);
        CHECK(ocerz_ld(region_msg + 0x27, 1) == 0);
        uint64_t address = ocerz_ld(region_msg + 0x30, 8);
        uint64_t size = ocerz_ld(region_msg + 0x38, 8);
        CHECK(address >= ocerz_arena_lo && address < ocerz_arena_hi);
        CHECK(query >= address && query - address < size);
        CHECK((uint64_t)(uintptr_t)ocerz_g2h(address) != address);

        mach_port_t object_name =
            (mach_port_t)(uint32_t)ocerz_ld(region_msg + 0x1c, 4);
        if (object_name != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), object_name);
    }
}

struct vector_reply_server {
    mach_port_t port;
    uint64_t host_address;
    int result;
};

static void *serve_vector_reply(void *opaque)
{
    struct vector_reply_server *server = opaque;
    _Alignas(8) unsigned char request[0x100];
    memset(request, 0, sizeof request);
    mach_msg_header_t *request_header = (mach_msg_header_t *)request;
    mach_msg_return_t mr =
        mach_msg(request_header, MACH_RCV_MSG, 0, sizeof request,
                 server->port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (mr != MACH_MSG_SUCCESS) {
        server->result = (int)mr;
        return NULL;
    }

    _Alignas(8) unsigned char reply[0x34];
    memset(reply, 0, sizeof reply);
    mach_msg_header_t *reply_header = (mach_msg_header_t *)reply;
    reply_header->msgh_bits =
        MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply_header->msgh_size = sizeof reply;
    reply_header->msgh_remote_port = request_header->msgh_remote_port;
    reply_header->msgh_id = 4915;
    memcpy(reply + 0x18, &NDR_record, sizeof NDR_record);
    memcpy(reply + 0x24, &server->host_address,
           sizeof server->host_address);
    uint64_t region_size = 0x4000;
    memcpy(reply + 0x2c, &region_size, sizeof region_size);
    mr = mach_msg(reply_header, MACH_SEND_MSG, sizeof reply, 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    server->result = (int)mr;
    return NULL;
}

static void test_mach_msg2_vector_in_place_reply(void)
{
    struct vector_reply_server server = {0};
    kern_return_t kr =
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &server.port);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        return;
    kr = mach_port_insert_right(mach_task_self(), server.port, server.port,
                                MACH_MSG_TYPE_MAKE_SEND);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS) {
        mach_port_destruct(mach_task_self(), server.port, -1, 0);
        return;
    }

    uint64_t query = scratch + 0x1000;
    server.host_address = (uint64_t)(uintptr_t)ocerz_g2h(query);
    pthread_t thread;
    int pr = pthread_create(&thread, NULL, serve_vector_reply, &server);
    CHECK(pr == 0);
    if (pr != 0) {
        mach_port_destruct(mach_task_self(), server.port, -1, 0);
        return;
    }

    mach_port_t reply_port = mig_get_reply_port();
    CHECK(reply_port != MACH_PORT_NULL);
    uint64_t gmsg = scratch + 0x4800;
    memset(ocerz_g2h(gmsg), 0, 0x400);
    ocerz_st(gmsg + 0x00, 4,
             MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                            MACH_MSG_TYPE_MAKE_SEND_ONCE));
    ocerz_st(gmsg + 0x04, 4, 0x18);
    ocerz_st(gmsg + 0x08, 4, server.port);
    ocerz_st(gmsg + 0x0c, 4, reply_port);
    ocerz_st(gmsg + 0x14, 4, 4815);

    if (reply_port != MACH_PORT_NULL &&
        run_raw_mach_msg2_vector(gmsg, 0x18, 4815, 0, reply_port) == 0) {
        CHECK(ocerz_ld(gmsg + 0x14, 4) == 4915);
        CHECK(ocerz_ld(gmsg + 0x24, 8) == query);
        CHECK(ocerz_ld(gmsg + 0x2c, 8) == 0x4000);
    }

    pthread_join(thread, NULL);
    CHECK(server.result == MACH_MSG_SUCCESS);
    if (reply_port != MACH_PORT_NULL)
        mig_put_reply_port(reply_port);
    mach_port_destruct(mach_task_self(), server.port, -1, 0);
}

struct thread_info_reply_server {
    mach_port_t port;
    uint64_t handle;
    uint64_t dispatch_qaddr;
    int result;
};

static void *serve_thread_info_reply(void *opaque)
{
    struct thread_info_reply_server *server = opaque;
    _Alignas(8) unsigned char request[0x100];
    memset(request, 0, sizeof request);
    mach_msg_header_t *request_header = (mach_msg_header_t *)request;
    mach_msg_return_t mr =
        mach_msg(request_header, MACH_RCV_MSG, 0, sizeof request,
                 server->port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (mr != MACH_MSG_SUCCESS) {
        server->result = (int)mr;
        return NULL;
    }

    _Alignas(8) unsigned char reply[0x40];
    memset(reply, 0, sizeof reply);
    mach_msg_header_t *reply_header = (mach_msg_header_t *)reply;
    reply_header->msgh_bits =
        MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply_header->msgh_size = sizeof reply;
    reply_header->msgh_remote_port = request_header->msgh_remote_port;
    reply_header->msgh_id = 3712;
    memcpy(reply + 0x18, &NDR_record, sizeof NDR_record);
    uint32_t count = 6;
    memcpy(reply + 0x24, &count, sizeof count);
    memcpy(reply + 0x30, &server->handle, sizeof server->handle);
    memcpy(reply + 0x38, &server->dispatch_qaddr,
           sizeof server->dispatch_qaddr);
    mr = mach_msg(reply_header, MACH_SEND_MSG, sizeof reply, 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    server->result = (int)mr;
    return NULL;
}

static void test_thread_info_rewrites_reserved_host_handle(void)
{
    const uint64_t reservation = 0x170000000ull;
    struct thread_info_reply_server server = {
        .handle = reservation + 0xe0,
        .dispatch_qaddr = reservation + 0x160,
    };
    CHECK(ocerz_map_fixed(reservation, 0x1000, PROT_NONE) == OCERZ_OK);
    CHECK(ocerz_addr_committed(server.handle) == 1);

    kern_return_t kr =
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &server.port);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        goto out_mapping;
    kr = mach_port_insert_right(mach_task_self(), server.port, server.port,
                                MACH_MSG_TYPE_MAKE_SEND);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        goto out_port;

    pthread_t thread;
    int pr = pthread_create(&thread, NULL, serve_thread_info_reply, &server);
    CHECK(pr == 0);
    if (pr != 0)
        goto out_port;

    mach_port_t reply_port = mig_get_reply_port();
    CHECK(reply_port != MACH_PORT_NULL);
    uint64_t gmsg = scratch + 0x4c00;
    memset(ocerz_g2h(gmsg), 0, 0x400);
    ocerz_st(gmsg + 0x00, 4,
             MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                            MACH_MSG_TYPE_MAKE_SEND_ONCE));
    ocerz_st(gmsg + 0x04, 4, 0x24);
    ocerz_st(gmsg + 0x08, 4, server.port);
    ocerz_st(gmsg + 0x0c, 4, reply_port);
    ocerz_st(gmsg + 0x14, 4, 3612);
    memcpy(ocerz_g2h(gmsg + 0x18), &NDR_record, sizeof NDR_record);
    ocerz_st(gmsg + 0x20, 4, THREAD_IDENTIFIER_INFO);

    uint64_t saved_gs = vm.cpu.gs_base;
    uint64_t guest_handle = scratch + 0x2000;
    vm.cpu.gs_base = guest_handle;
    if (reply_port != MACH_PORT_NULL &&
        run_raw_mach_msg2_vector(gmsg, 0x24, 3612, 0, reply_port) == 0) {
        CHECK(ocerz_ld(gmsg + 0x14, 4) == 3712);
        CHECK(ocerz_ld(gmsg + 0x30, 8) == guest_handle);
        CHECK(ocerz_ld(gmsg + 0x38, 8) == guest_handle + 0x80);
    }
    vm.cpu.gs_base = saved_gs;

    pthread_join(thread, NULL);
    CHECK(server.result == MACH_MSG_SUCCESS);
    if (reply_port != MACH_PORT_NULL)
        mig_put_reply_port(reply_port);

out_port:
    mach_port_destruct(mach_task_self(), server.port, -1, 0);
out_mapping:
    CHECK(ocerz_unmap(reservation, 0x1000) == OCERZ_OK);
}

struct iokit_alias_reply_server {
    mach_port_t port;
    uint64_t raw_address;
    int result;
};

static void *serve_iokit_alias_reply(void *opaque)
{
    struct iokit_alias_reply_server *server = opaque;
    _Alignas(8) unsigned char request[0x100];
    memset(request, 0, sizeof request);
    mach_msg_header_t *request_header = (mach_msg_header_t *)request;
    mach_msg_return_t mr =
        mach_msg(request_header, MACH_RCV_MSG, 0, sizeof request,
                 server->port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (mr != MACH_MSG_SUCCESS) {
        server->result = (int)mr;
        return NULL;
    }

    _Alignas(8) unsigned char reply[0x28];
    memset(reply, 0, sizeof reply);
    mach_msg_header_t *reply_header = (mach_msg_header_t *)reply;
    reply_header->msgh_bits =
        MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply_header->msgh_size = sizeof reply;
    reply_header->msgh_remote_port = request_header->msgh_remote_port;
    reply_header->msgh_id = 2901;
    memcpy(reply + 0x18, &NDR_record, sizeof NDR_record);
    memcpy(reply + 0x20, &server->raw_address,
           sizeof server->raw_address);
    mr = mach_msg(reply_header, MACH_SEND_MSG, sizeof reply, 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    server->result = (int)mr;
    return NULL;
}

static void test_iokit_alias_replaces_prot_none_reservation(void)
{
    const uint64_t page_size = 0x4000;
    static const mach_vm_address_t candidates[] = {
        0x110000000ull, 0x120000000ull, 0x130000000ull,
        0x140000000ull, 0x150000000ull, 0x160000000ull,
        0x170000000ull,
    };
    mach_vm_address_t raw = 0;
    kern_return_t kr = KERN_NO_SPACE;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        raw = candidates[i];
        kr = mach_vm_allocate(mach_task_self(), &raw, page_size,
                              VM_FLAGS_FIXED);
        if (kr == KERN_SUCCESS)
            break;
    }
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        return;

    const char plist[] = "bplist00";
    const uint64_t raw_marker = 0x0123456789abcdefull;
    const uint64_t guest_marker = 0xfedcba9876543210ull;
    memcpy((void *)(uintptr_t)raw, plist, sizeof plist);
    memcpy((void *)(uintptr_t)(raw + 0x100), &raw_marker,
           sizeof raw_marker);

    CHECK(ocerz_map_fixed(raw, page_size, PROT_NONE) == OCERZ_OK);
    CHECK(ocerz_addr_committed(raw) == 1);
    CHECK(ocerz_addr_prot(raw) == PROT_NONE);

    struct iokit_alias_reply_server server = {
        .raw_address = raw,
    };
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &server.port);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        goto out_mapping;
    kr = mach_port_insert_right(mach_task_self(), server.port, server.port,
                                MACH_MSG_TYPE_MAKE_SEND);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        goto out_port;

    pthread_t thread;
    int pr = pthread_create(&thread, NULL, serve_iokit_alias_reply, &server);
    CHECK(pr == 0);
    if (pr != 0)
        goto out_port;

    mach_port_t reply_port = mig_get_reply_port();
    CHECK(reply_port != MACH_PORT_NULL);
    uint64_t gmsg = scratch + 0x5800;
    memset(ocerz_g2h(gmsg), 0, 0x400);
    ocerz_st(gmsg + 0x00, 4,
             MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                            MACH_MSG_TYPE_MAKE_SEND_ONCE));
    ocerz_st(gmsg + 0x04, 4, 0x18);
    ocerz_st(gmsg + 0x08, 4, server.port);
    ocerz_st(gmsg + 0x0c, 4, reply_port);
    ocerz_st(gmsg + 0x14, 4, 2801);

    if (reply_port != MACH_PORT_NULL &&
        run_raw_mach_msg2_vector(gmsg, 0x18, 2801, 0, reply_port) == 0) {
        CHECK(ocerz_ld(gmsg + 0x14, 4) == 2901);
        CHECK(ocerz_ld(gmsg + 0x20, 8) == raw);
        int readable = ocerz_addr_readable(raw);
        CHECK(readable);
        if (readable) {
            CHECK(memcmp(ocerz_g2h(raw), plist, sizeof plist) == 0);
            CHECK(ocerz_ld(raw + 0x100, 8) == raw_marker);
            ocerz_st(raw + 0x108, 8, guest_marker);
            uint64_t got = 0;
            memcpy(&got, (const void *)(uintptr_t)(raw + 0x108),
                   sizeof got);
            CHECK(got == guest_marker);
        }
    }

    pthread_join(thread, NULL);
    CHECK(server.result == MACH_MSG_SUCCESS);
    if (reply_port != MACH_PORT_NULL)
        mig_put_reply_port(reply_port);

out_port:
    mach_port_destruct(mach_task_self(), server.port, -1, 0);
out_mapping:
    CHECK(ocerz_unmap(raw, page_size) == OCERZ_OK);
    mach_vm_deallocate(mach_task_self(), raw, page_size);
}

static int host_range_readable(uint64_t address, uint64_t size)
{
    mach_vm_address_t region = address;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    kern_return_t kr =
        mach_vm_region(mach_task_self(), &region, &region_size,
                       VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                       &count, &object_name);
    if (object_name != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object_name);
    return kr == KERN_SUCCESS && region <= address &&
           address - region <= region_size &&
           size <= region_size - (address - region) &&
           (info.protection & VM_PROT_READ) != 0;
}

static void test_mach_vm_remap_mig_relocation(void)
{
    const uint64_t page_size = 0x4000;
    const uint64_t first = 0x0123456789abcdefull;
    const uint64_t last = 0xfedcba9876543210ull;
    CHECK(ocerz_low_base != 0);

    mach_vm_address_t source = 0;
    kern_return_t kr =
        mach_vm_allocate(mach_task_self(), &source, page_size,
                         VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        return;
    memcpy((void *)(uintptr_t)source, &first, sizeof first);
    memcpy((void *)(uintptr_t)(source + page_size - sizeof last),
           &last, sizeof last);

    static const mach_vm_address_t cage_candidates[] = {
        0x10000000000ull,
        0x100000000000ull,
        0x600000000000ull,
    };
    mach_vm_address_t cage = 0;
    kr = KERN_NO_SPACE;
    for (size_t i = 0;
         i < sizeof cage_candidates / sizeof cage_candidates[0]; i++) {
        cage = cage_candidates[i];
        kr = mach_vm_allocate(mach_task_self(), &cage, page_size * 5,
                              VM_FLAGS_FIXED);
        if (kr == KERN_SUCCESS)
            break;
    }
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS) {
        mach_vm_deallocate(mach_task_self(), source, page_size);
        return;
    }
    kr = mach_vm_deallocate(mach_task_self(), cage + page_size,
                            page_size * 3);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS) {
        mach_vm_deallocate(mach_task_self(), cage, page_size * 5);
        mach_vm_deallocate(mach_task_self(), source, page_size);
        return;
    }
    mach_vm_address_t target = cage + page_size * 2;

    mach_port_t reply_port = mig_get_reply_port();
    CHECK(reply_port != MACH_PORT_NULL);
    if (reply_port == MACH_PORT_NULL) {
        mach_vm_deallocate(mach_task_self(), cage, page_size);
        mach_vm_deallocate(mach_task_self(), cage + page_size * 4, page_size);
        mach_vm_deallocate(mach_task_self(), source, page_size);
        return;
    }

    uint64_t gmsg = scratch + 0x5000;
    memset(ocerz_g2h(gmsg), 0, 0x400);
    ocerz_st(gmsg + 0x00, 4,
             MACH_MSGH_BITS_COMPLEX |
             MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                            MACH_MSG_TYPE_MAKE_SEND_ONCE));
    ocerz_st(gmsg + 0x04, 4, 0x5c);
    ocerz_st(gmsg + 0x08, 4, mach_task_self());
    ocerz_st(gmsg + 0x0c, 4, reply_port);
    ocerz_st(gmsg + 0x10, 4, MACH_PORT_NULL);
    ocerz_st(gmsg + 0x14, 4, 4813);
    ocerz_st(gmsg + 0x18, 4, 1);
    ocerz_st(gmsg + 0x1c, 4, mach_task_self());
    ocerz_st(gmsg + 0x26, 1, MACH_MSG_TYPE_COPY_SEND);
    ocerz_st(gmsg + 0x27, 1, MACH_MSG_PORT_DESCRIPTOR);
    memcpy(ocerz_g2h(gmsg + 0x28), &NDR_record, sizeof NDR_record);
    ocerz_st(gmsg + 0x30, 8, target);
    ocerz_st(gmsg + 0x38, 8, page_size);
    ocerz_st(gmsg + 0x40, 8, 0);
    ocerz_st(gmsg + 0x48, 4, VM_FLAGS_FIXED);
    ocerz_st(gmsg + 0x4c, 8, source);
    ocerz_st(gmsg + 0x54, 4, FALSE);
    ocerz_st(gmsg + 0x58, 4, VM_INHERIT_DEFAULT);

    if (run_raw_mach_msg2(gmsg, 0x5c, 4813, 1, reply_port) == 0) {
        CHECK(ocerz_ld(gmsg + 0x14, 4) == 4913);
        CHECK((ocerz_ld(gmsg, 4) & MACH_MSGH_BITS_COMPLEX) == 0);
        CHECK(ocerz_ld(gmsg + 0x20, 4) == KERN_SUCCESS);

        uint64_t guest_address = ocerz_ld(gmsg + 0x24, 8);
        uint64_t host_address =
            (uint64_t)(uintptr_t)ocerz_g2h(guest_address);
        CHECK(guest_address >= ocerz_arena_lo &&
              guest_address < ocerz_arena_hi);
        CHECK(guest_address < OCERZ_LOW_LIMIT);
        CHECK(host_address != guest_address);

        int translated_readable =
            host_range_readable(host_address, page_size);
        int raw_readable =
            host_range_readable(guest_address, page_size);
        CHECK(translated_readable);
        if (translated_readable) {
            uint64_t got_first;
            uint64_t got_last;
            memcpy(&got_first, (const void *)(uintptr_t)host_address,
                   sizeof got_first);
            memcpy(&got_last,
                   (const void *)(uintptr_t)
                       (host_address + page_size - sizeof got_last),
                   sizeof got_last);
            CHECK(got_first == first);
            CHECK(got_last == last);
        }

        int raw_has_contents = 0;
        if (raw_readable) {
            uint64_t got_first;
            uint64_t got_last;
            memcpy(&got_first, (const void *)(uintptr_t)guest_address,
                   sizeof got_first);
            memcpy(&got_last,
                   (const void *)(uintptr_t)
                       (guest_address + page_size - sizeof got_last),
                   sizeof got_last);
            raw_has_contents = got_first == first && got_last == last;
        }
        CHECK(!raw_has_contents);
        if (raw_has_contents)
            mach_vm_deallocate(mach_task_self(), guest_address, page_size);
        ocerz_unmap(guest_address, page_size);
    }

    mig_put_reply_port(reply_port);
    mach_vm_deallocate(mach_task_self(), target, page_size);
    mach_vm_deallocate(mach_task_self(), cage, page_size);
    mach_vm_deallocate(mach_task_self(), cage + page_size * 4, page_size);
    mach_vm_deallocate(mach_task_self(), source, page_size);
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

static void test_sigreturn_restores_only_ocerz_segment_bases(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gact = scratch + 0x5000;
    memset(ocerz_g2h(gact), 0, 24);
    ocerz_st(gact + 0, 8, scratch + 0x100);
    ocerz_st(gact + 8, 8, scratch + 0x200);
    set_args(cpu, bsd(46), SIGUSR1, gact, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);

    const uint64_t interrupted_gs = 0x7000012300ull;
    const uint64_t interrupted_fs = 0x7000045600ull;
    cpu->gs_base = interrupted_gs;
    cpu->fs_base = interrupted_fs;
    cpu->gpr[OCERZ_RSP] = scratch + 0x8000;
    cpu->rip = scratch + 0x300;
    CHECK(ocerz_signal_deliver(cpu, SIGUSR1, 0, 0, 0) == 1);
    uint64_t uc = cpu->gpr[OCERZ_R8];

    cpu->gs_base = 0x1700000e0ull;
    cpu->fs_base = 0x170000160ull;
    set_args(cpu, bsd(184), uc, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
    CHECK(cpu->gs_base == interrupted_gs);
    CHECK(cpu->fs_base == interrupted_fs);

    cpu->gs_base = 0x170000260ull;
    cpu->fs_base = 0x1700002e0ull;
    set_args(cpu, bsd(184), uc, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
    CHECK(cpu->gs_base == 0x170000260ull);
    CHECK(cpu->fs_base == 0x1700002e0ull);
}

static void test_nested_signal_altstack_state(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t gact = scratch + 0x5200;
    memset(ocerz_g2h(gact), 0, 24);
    ocerz_st(gact + 0, 8, scratch + 0x100);
    ocerz_st(gact + 8, 8, scratch + 0x200);
    ocerz_st(gact + 20, 4, 0x0001u);
    set_args(cpu, bsd(46), SIGUSR2, gact, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);

    uint64_t gstack = scratch + 0x5280;
    uint64_t alt = scratch + 0x9000;
    ocerz_st(gstack + 0, 8, alt);
    ocerz_st(gstack + 8, 8, 0x4000);
    ocerz_st(gstack + 16, 4, 0);
    set_args(cpu, bsd(53), gstack, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);

    cpu->gpr[OCERZ_RSP] = scratch + 0x7000;
    cpu->rip = scratch + 0x300;
    cpu->sig_on_stack = 0;
    CHECK(ocerz_signal_deliver(cpu, SIGUSR2, 0, 0, 0) == 1);
    uint64_t outer_uc = cpu->gpr[OCERZ_R8];
    CHECK(cpu->gpr[OCERZ_RSP] >= alt);
    CHECK(cpu->gpr[OCERZ_RSP] < alt + 0x4000);
    CHECK(ocerz_ld(outer_uc + 0, 4) == 0);
    CHECK(cpu->sig_on_stack == 1);

    cpu->gpr[OCERZ_RSP] = scratch + 0x6000;
    CHECK(ocerz_signal_deliver(cpu, SIGUSR2, 0, 0, 0) == 1);
    uint64_t inner_uc = cpu->gpr[OCERZ_R8];
    CHECK(cpu->gpr[OCERZ_RSP] < alt);
    CHECK(ocerz_ld(inner_uc + 0, 4) == 1);

    set_args(cpu, bsd(184), inner_uc, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
    CHECK(cpu->sig_on_stack == 1);
    set_args(cpu, bsd(184), outer_uc, 0, 0, 0, 0, 0);
    CHECK(ocerz_handle_syscall(&vm, cpu) == OCERZ_STEP_OK);
    CHECK(cpu->sig_on_stack == 0);
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

struct delayed_sem_post {
    sem_t *sem;
    volatile int ready;
};

static void *post_sem_after_delay(void *arg)
{
    struct delayed_sem_post *post = arg;
    __atomic_store_n(&post->ready, 1, __ATOMIC_RELEASE);
    usleep(50000);
    sem_post(post->sem);
    return NULL;
}

static void test_sem_wait_nocancel_blocks(void)
{
    char name[64];
    snprintf(name, sizeof(name), "/ocerz-test-sem-%d", getpid());
    sem_unlink(name);
    sem_t *sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    CHECK(sem != SEM_FAILED);
    if (sem == SEM_FAILED)
        return;

    struct delayed_sem_post post = { .sem = sem };
    pthread_t thread;
    int pr = pthread_create(&thread, NULL, post_sem_after_delay, &post);
    CHECK(pr == 0);
    if (pr != 0) {
        sem_close(sem);
        sem_unlink(name);
        return;
    }
    while (!__atomic_load_n(&post.ready, __ATOMIC_ACQUIRE))
        sched_yield();

    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(420), (uint64_t)(uintptr_t)sem, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;

    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RAX] == 0);
    CHECK(elapsed >= 20000000ull);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(sem_close(sem) == 0);
    CHECK(sem_unlink(name) == 0);
}

struct terminate_waiter {
    void *address;
    uint32_t owner;
    volatile int ready;
    uint64_t result;
    int error;
};

static void *wait_for_terminate_wake(void *arg)
{
    struct terminate_waiter *waiter = (struct terminate_waiter *)arg;
    uint64_t a[8] = {
        0x01000002ull,
        (uint64_t)(uintptr_t)waiter->address,
        waiter->owner,
        2000000,
        0, 0, 0, 0
    };

    __atomic_store_n(&waiter->ready, 1, __ATOMIC_RELEASE);
    waiter->result = ocerz_host_syscall(515, a, NULL, &waiter->error);
    return NULL;
}

static void test_bsdthread_terminate_wakes_ulock(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t join_addr = scratch + 0x3800;
    mach_port_t owner = mach_thread_self();
    struct terminate_waiter waiter = {
        .address = ocerz_g2h(join_addr),
        .owner = (uint32_t)owner,
    };
    pthread_t thread;

    ocerz_st(join_addr, 4, owner);
    int pr = pthread_create(&thread, NULL, wait_for_terminate_wake, &waiter);
    CHECK(pr == 0);
    if (pr != 0) {
        mach_port_deallocate(mach_task_self(), owner);
        return;
    }
    while (!__atomic_load_n(&waiter.ready, __ATOMIC_ACQUIRE))
        sched_yield();
    usleep(20000);

    cpu->terminated = 0;
    set_args(cpu, bsd(361), 0, 0, owner, join_addr, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->terminated == 1);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.error == 0);
    CHECK(waiter.result == 0);
    CHECK(ocerz_ld(join_addr, 4) == ((uint32_t)owner & ~3u));

    cpu->terminated = 0;
    mach_port_deallocate(mach_task_self(), owner);
}

static void test_bsdthread_terminate_signals_semaphore(void)
{
    OcerzCPU *cpu = &vm.cpu;
    semaphore_t sem = MACH_PORT_NULL;
    kern_return_t kr = semaphore_create(
        mach_task_self(), &sem, SYNC_POLICY_FIFO, 0);
    CHECK(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS)
        return;
    CHECK(((uint32_t)sem & 3u) == 3u);

    cpu->terminated = 0;
    set_args(cpu, bsd(361), 0, 0, 0, sem, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);
    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->terminated == 1);
    mach_timespec_t timeout = { 0, 0 };
    CHECK(semaphore_timedwait(sem, timeout) == KERN_SUCCESS);

    cpu->terminated = 0;
    semaphore_destroy(mach_task_self(), sem);
}

static void test_bsdthread_terminate_unmaps_stack(void)
{
    OcerzCPU *cpu = &vm.cpu;
    uint64_t freeaddr = ocerz_map_anywhere(0x8000, PROT_READ | PROT_WRITE);
    CHECK(freeaddr != 0);
    CHECK(ocerz_addr_committed(freeaddr) == 1);

    cpu->terminated = 0;
    set_args(cpu, bsd(361), freeaddr, 0x8000, 0x11223344, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);

    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->terminated == 1);
    CHECK(ocerz_addr_committed(freeaddr) == 0);
    cpu->terminated = 0;
}

static void test_fork_dual_return(void)
{
    OcerzCPU *cpu = &vm.cpu;
    set_args(cpu, bsd(2), 0, 0, 0, 0, 0, 0);
    int r = ocerz_handle_syscall(&vm, cpu);

    if (r == OCERZ_STEP_OK && cf(cpu) == 0 && cpu->gpr[OCERZ_RDX] == 1) {
        int ok = cpu->gpr[OCERZ_RAX] == 0;
        _exit(ok ? 0 : 97);
    }

    CHECK(r == OCERZ_STEP_OK);
    CHECK(cf(cpu) == 0);
    CHECK(cpu->gpr[OCERZ_RDX] == 0);
    pid_t child = (pid_t)cpu->gpr[OCERZ_RAX];
    CHECK(child > 0);
    if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }
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
    if (ocerz_mem_init(0x100000000ull, 0x700000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem init failed\n");
        return 2;
    }
    if (ocerz_mem_init_low_shadow() != OCERZ_OK) {
        fprintf(stderr, "low shadow init failed\n");
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

    test_mach_vm_remap_mig_relocation();
    test_getpid();
    test_pipe_and_io();
    test_open_fstat_close();
    test_open_bad_path();
    test_mmap_anon();
    test_mmap_shared_requires_ordered();
    test_mmap_fixed();
    test_mmap_fixed_prot_none_reservation();
    test_mmap_fixed_outside();
    test_i386_ldt();
    test_madvise();
    test_gettimeofday();
    test_getentropy();
    test_writev();
    test_sigaction();
    test_sigreturn_restores_only_ocerz_segment_bases();
    test_nested_signal_altstack_state();
    test_machdep_gs_base();
    test_machdep_unknown();
    test_mach_task_self();
    test_mach_thread_self();
    test_mach_vm_allocate();
    test_received_large_ool_is_relocated();
    test_mach_msg2_vector_in_place_reply();
    test_thread_info_rewrites_reserved_host_handle();
    test_iokit_alias_replaces_prot_none_reservation();
    test_mach_vm_region_mig_translation();
    test_mach_timebase();
    test_mach_unknown();
    test_execve_bad_args();
    test_sem_wait_nocancel_blocks();
    test_bsdthread_terminate_wakes_ulock();
    test_bsdthread_terminate_signals_semaphore();
    test_bsdthread_terminate_unmaps_stack();
    test_fork_dual_return();
    test_unknown_bsd();
    test_unknown_class();
    test_exit();

    fprintf(stderr, "test_syscall: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
