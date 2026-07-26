/* The plain-to-ordered JIT retire on the first shared mapping. */
#include "ocerz/cpu.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define CODE_BASE 0x200000000ull
#define A_RIP     (CODE_BASE + 0x00)
#define B_RIP     (CODE_BASE + 0x40)
#define DONE_RIP  (CODE_BASE + 0x80)
#define DATA_ADDR (CODE_BASE + 0x1000)
#define STACK_TOP (CODE_BASE + 0x8000)

static int failed;

#define CHECK(c, ...) do { \
    if (!(c)) { \
        failed++; \
        fprintf(stderr, "FAIL: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

static void emit_rel32(uint64_t at, uint8_t opcode, uint64_t target)
{
    int32_t rel = (int32_t)(target - (at + 5));
    uint8_t bytes[5] = {
        opcode, (uint8_t)rel, (uint8_t)(rel >> 8),
        (uint8_t)(rel >> 16), (uint8_t)(rel >> 24),
    };
    memcpy(ocerz_g2h(at), bytes, sizeof bytes);
}

static int step_at(OcerzVM *vm, uint64_t rip, uint64_t value)
{
    OcerzCPU *cpu = &vm->cpu;
    cpu->rip = rip;
    cpu->gpr[OCERZ_RAX] = value;
    cpu->gpr[OCERZ_RDI] = DATA_ADDR;
    cpu->gpr[OCERZ_RSP] = STACK_TOP;
    return ocerz_jit_step(vm, cpu);
}

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK ||
        ocerz_map_fixed(CODE_BASE, 0x10000,
                        PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "memory setup failed\n");
        return 2;
    }

    const uint8_t store[] = { 0x48, 0x89, 0x07 };
    memcpy(ocerz_g2h(A_RIP), store, sizeof store);
    emit_rel32(A_RIP + sizeof store, 0xe8, B_RIP);

    const uint8_t add[] = { 0x48, 0x83, 0xc0, 0x01 };
    memcpy(ocerz_g2h(B_RIP), add, sizeof add);
    emit_rel32(B_RIP + sizeof add, 0xe9, DONE_RIP);

    OcerzVM vm;
    ocerz_vm_init(&vm);
    vm.jit_plain_mem = 1;
    ocerz_vm_install_handlers(&vm);
    if (!vm.jit) {
        fprintf(stderr, "jit unavailable\n");
        return 2;
    }

    CHECK(step_at(&vm, A_RIP, 10) == OCERZ_STEP_OK,
          "plain A did not return STEP_OK");
    CHECK(vm.cpu.rip == B_RIP, "plain A rip=%#llx",
          (unsigned long long)vm.cpu.rip);
    CHECK(step_at(&vm, B_RIP, 10) == OCERZ_STEP_OK,
          "plain B did not return STEP_OK");
    CHECK(vm.cpu.rip == DONE_RIP && vm.cpu.gpr[OCERZ_RAX] == 11,
          "plain B result rip=%#llx rax=%#llx",
          (unsigned long long)vm.cpu.rip,
          (unsigned long long)vm.cpu.gpr[OCERZ_RAX]);
    CHECK(ocerz_jit_blocks(vm.jit) == 2, "plain generation blocks=%llu",
          (unsigned long long)ocerz_jit_blocks(vm.jit));

    CHECK(step_at(&vm, A_RIP, 20) == OCERZ_STEP_OK,
          "cached plain A did not return STEP_OK");
    CHECK((vm.cpu.rip == B_RIP || vm.cpu.rip == DONE_RIP),
          "cached plain result rip=%#llx rax=%#llx",
          (unsigned long long)vm.cpu.rip,
          (unsigned long long)vm.cpu.gpr[OCERZ_RAX]);
    CHECK(ocerz_ld(DATA_ADDR, 8) == 20, "plain store value=%#llx",
          (unsigned long long)ocerz_ld(DATA_ADDR, 8));

    vm.cpu.ras_top = 1;
    vm.cpu.ras[0].guest_rip = A_RIP;
    vm.cpu.ras[0].host_entry = (void *)(uintptr_t)1;
    ocerz_jit_require_ordered(&vm);
    CHECK(vm.jit_ordered_required && !vm.jit_plain_mem,
          "ordered state was not sticky");
    CHECK(vm.cpu.ras_top == 0, "transition retained ras_top=%u",
          vm.cpu.ras_top);
    CHECK(ocerz_jit_blocks(vm.jit) == 2,
          "transition changed translation count=%llu",
          (unsigned long long)ocerz_jit_blocks(vm.jit));

    CHECK(step_at(&vm, A_RIP, 30) == OCERZ_STEP_OK,
          "ordered A did not return STEP_OK");
    CHECK(vm.cpu.rip == B_RIP, "ordered A rip=%#llx",
          (unsigned long long)vm.cpu.rip);
    CHECK(ocerz_jit_blocks(vm.jit) == 3, "ordered A blocks=%llu",
          (unsigned long long)ocerz_jit_blocks(vm.jit));
    CHECK(step_at(&vm, B_RIP, 30) == OCERZ_STEP_OK,
          "ordered B did not return STEP_OK");
    CHECK(vm.cpu.rip == DONE_RIP && vm.cpu.gpr[OCERZ_RAX] == 31,
          "ordered B result rip=%#llx rax=%#llx",
          (unsigned long long)vm.cpu.rip,
          (unsigned long long)vm.cpu.gpr[OCERZ_RAX]);
    CHECK(ocerz_jit_blocks(vm.jit) == 4, "ordered B blocks=%llu",
          (unsigned long long)ocerz_jit_blocks(vm.jit));

    ocerz_jit_require_ordered(&vm);
    CHECK(step_at(&vm, A_RIP, 40) == OCERZ_STEP_OK,
          "idempotent ordered A did not return STEP_OK");
    CHECK((vm.cpu.rip == B_RIP || vm.cpu.rip == DONE_RIP),
          "idempotent A result rip=%#llx rax=%#llx",
          (unsigned long long)vm.cpu.rip,
          (unsigned long long)vm.cpu.gpr[OCERZ_RAX]);
    if (vm.cpu.rip == B_RIP)
        CHECK(step_at(&vm, B_RIP, 40) == OCERZ_STEP_OK,
              "cached ordered B did not return STEP_OK");
    CHECK(vm.cpu.rip == DONE_RIP && vm.cpu.gpr[OCERZ_RAX] == 41,
          "idempotent ordered result rip=%#llx rax=%#llx",
          (unsigned long long)vm.cpu.rip,
          (unsigned long long)vm.cpu.gpr[OCERZ_RAX]);
    CHECK(ocerz_jit_blocks(vm.jit) == 4,
          "idempotent transition retired ordered cache (blocks=%llu)",
          (unsigned long long)ocerz_jit_blocks(vm.jit));
    CHECK(ocerz_ld(DATA_ADDR, 8) == 40, "ordered store value=%#llx",
          (unsigned long long)ocerz_ld(DATA_ADDR, 8));

    ocerz_jit_destroy(vm.jit);
    fprintf(stderr, "test_jit_order_transition: %s\n",
            failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
