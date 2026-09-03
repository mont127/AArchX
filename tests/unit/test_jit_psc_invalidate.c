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

#define CODE_BASE 0x240000000ull
#define SOURCE_RIP (CODE_BASE + 0x00)
#define TARGET_RIP (CODE_BASE + 0x40)
#define DONE_RIP   (CODE_BASE + 0x80)
#define STACK_TOP  (CODE_BASE + 0x8000)

static void emit_source(void)
{
    uint8_t code[] = {
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0, /* movabs TARGET_RIP,%rcx */
        0xff, 0xe1,                         /* jmp *%rcx */
    };
    memcpy(&code[2], &(uint64_t){ TARGET_RIP }, sizeof(uint64_t));
    memcpy(ocerz_g2h(SOURCE_RIP), code, sizeof code);
}

static void emit_target(uint8_t addend)
{
    uint8_t code[] = {
        0x48, 0x83, 0xc0, addend,           /* add $addend,%rax */
        0xe9, 0, 0, 0, 0,                  /* jmp DONE_RIP */
    };
    int32_t rel = (int32_t)(DONE_RIP - (TARGET_RIP + sizeof code));
    memcpy(&code[5], &rel, sizeof rel);
    memcpy(ocerz_g2h(TARGET_RIP), code, sizeof code);
}

static int run(OcerzVM *vm, uint64_t rip, uint64_t value)
{
    vm->cpu.rip = rip;
    vm->cpu.gpr[OCERZ_RAX] = value;
    vm->cpu.gpr[OCERZ_RSP] = STACK_TOP;
    return ocerz_jit_step(vm, &vm->cpu);
}

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK ||
        ocerz_map_fixed(CODE_BASE, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "test_jit_psc_invalidate: memory setup failed\n");
        return 2;
    }

    emit_source();
    emit_target(1);

    OcerzVM vm;
    ocerz_vm_init(&vm);
    vm.jit_plain_mem = 1;
    ocerz_vm_install_handlers(&vm);
    if (!vm.jit) return 2;

    if (run(&vm, TARGET_RIP, 0) != OCERZ_STEP_OK || vm.cpu.rip != DONE_RIP ||
        run(&vm, SOURCE_RIP, 10) != OCERZ_STEP_OK || vm.cpu.rip != DONE_RIP ||
        vm.cpu.gpr[OCERZ_RAX] != 11 ||
        run(&vm, SOURCE_RIP, 20) != OCERZ_STEP_OK || vm.cpu.rip != DONE_RIP ||
        vm.cpu.gpr[OCERZ_RAX] != 21) {
        fprintf(stderr, "test_jit_psc_invalidate: initial fill failed\n");
        return 1;
    }

    ocerz_jit_invalidate_range(&vm, TARGET_RIP, 9);
    emit_target(7);
    if (run(&vm, TARGET_RIP, 0) != OCERZ_STEP_OK || vm.cpu.rip != DONE_RIP ||
        run(&vm, SOURCE_RIP, 30) != OCERZ_STEP_OK || vm.cpu.rip != DONE_RIP ||
        vm.cpu.gpr[OCERZ_RAX] != 37) {
        fprintf(stderr, "test_jit_psc_invalidate: stale body after invalidation\n");
        return 1;
    }

    ocerz_jit_destroy(vm.jit);
    fprintf(stderr, "test_jit_psc_invalidate: OK\n");
    return 0;
}
