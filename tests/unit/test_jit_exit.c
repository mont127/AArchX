/* A terminal interpreter callout must leave without reloading or re-spilling
 * pinned state.  The pre-call spill is authoritative once the syscall returns
 * OCERZ_STEP_EXIT. */
#include "ocerz/cpu.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define CODE_BASE 0x200000000ull
#define CODE_END  (CODE_BASE + 10)
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

static const uint64_t initial_flags =
    OCERZ_FLAG_FIXED1 | OCERZ_CF | OCERZ_PF | OCERZ_ZF | OCERZ_IF | OCERZ_OF;

static void prepare_cpu(OcerzVM *vm, int exit_code)
{
    OcerzCPU *cpu = &vm->cpu;
    ocerz_cpu_reset(cpu);
    for (int i = 0; i < 16; i++)
        cpu->gpr[i] = 0x1111000000000000ull + (uint64_t)i * 0x0101010101010101ull;

    cpu->rip = CODE_BASE;
    cpu->rflags = initial_flags;
    cpu->gpr[OCERZ_RAX] = (2ull << 24) | 1; /* BSD exit */
    cpu->gpr[OCERZ_RBX] = 0x1122334455667780ull;
    cpu->gpr[OCERZ_RSP] = STACK_TOP;
    cpu->gpr[OCERZ_RDI] = (uint64_t)exit_code;
    cpu->gpr[OCERZ_RCX] = 0xccccccccccccccccull;
    cpu->gpr[OCERZ_R11] = 0xbbbbbbbbbbbbbbbbull;
    cpu->xmm[0] = (Ocerz128){ .lo = 1, .hi = 2 };
    cpu->xmm[1] = (Ocerz128){ .lo = 3, .hi = 4 };
    cpu->xmm[15] = (Ocerz128){ .lo = 0x1515151515151515ull,
                               .hi = 0xf0f0f0f0f0f0f0f0ull };
}

static int block_info(OcerzVM *vm, OcerzJitFaultInfo *out)
{
    const uint32_t *lo;
    const uint32_t *hi;
    if (!ocerz_jit_code_range(vm, &lo, &hi))
        return 0;
    for (const uint32_t *pc = lo; pc < hi; pc++) {
        OcerzJitFaultInfo info;
        if (ocerz_jit_fault_info(vm, pc, &info) && info.block_rip == CODE_BASE) {
            *out = info;
            return 1;
        }
    }
    return 0;
}

static void check_result(OcerzVM *vm, int rc, int exit_code)
{
    OcerzCPU *cpu = &vm->cpu;
    CHECK(rc == OCERZ_STEP_EXIT, "step rc=%d want STEP_EXIT", rc);
    CHECK(vm->exited == 1, "vm.exited=%d", vm->exited);
    CHECK(vm->exit_code == exit_code, "exit_code=%d want %d", vm->exit_code, exit_code);
    CHECK(cpu->rip == CODE_END, "rip=%#llx want %#llx",
          (unsigned long long)cpu->rip, (unsigned long long)CODE_END);
    CHECK(cpu->cur_rip == CODE_BASE + 8, "cur_rip=%#llx want %#llx",
          (unsigned long long)cpu->cur_rip,
          (unsigned long long)(CODE_BASE + 8));
    CHECK(cpu->gpr[OCERZ_RCX] == CODE_END, "rcx=%#llx want %#llx",
          (unsigned long long)cpu->gpr[OCERZ_RCX],
          (unsigned long long)CODE_END);
    CHECK(cpu->gpr[OCERZ_R11] == (initial_flags & 0x3c7fd7ull),
          "r11=%#llx want %#llx", (unsigned long long)cpu->gpr[OCERZ_R11],
          (unsigned long long)(initial_flags & 0x3c7fd7ull));
    CHECK(cpu->gpr[OCERZ_RBX] == 0x1122334455667787ull,
          "rbx=%#llx", (unsigned long long)cpu->gpr[OCERZ_RBX]);
    CHECK(cpu->gpr[OCERZ_RSP] == STACK_TOP, "rsp=%#llx",
          (unsigned long long)cpu->gpr[OCERZ_RSP]);
    CHECK(cpu->xmm[0].lo == 4 && cpu->xmm[0].hi == 6,
          "xmm0={%#llx,%#llx}", (unsigned long long)cpu->xmm[0].lo,
          (unsigned long long)cpu->xmm[0].hi);
    CHECK(cpu->xmm[1].lo == 3 && cpu->xmm[1].hi == 4,
          "xmm1 changed to {%#llx,%#llx}",
          (unsigned long long)cpu->xmm[1].lo,
          (unsigned long long)cpu->xmm[1].hi);
    CHECK(cpu->xmm[15].lo == 0x1515151515151515ull &&
          cpu->xmm[15].hi == 0xf0f0f0f0f0f0f0f0ull,
          "untouched xmm15 changed");
}

int main(void)
{
    unsetenv("OCERZ_NO_FULLPIN");
    unsetenv("OCERZ_NO_REGFLAGS");
    unsetenv("OCERZ_NO_XMM_PIN");
    unsetenv("OCERZ_NO_XMM_GLOBAL");

    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK ||
        ocerz_map_fixed(CODE_BASE, 0x10000,
                        PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "memory setup failed\n");
        return 2;
    }

    static const uint8_t code[] = {
        0x48, 0x8d, 0x5b, 0x07, /* lea rbx, [rbx+7] */
        0x66, 0x0f, 0xd4, 0xc1, /* paddq xmm0, xmm1 */
        0x0f, 0x05,             /* syscall */
    };
    memcpy(ocerz_g2h(CODE_BASE), code, sizeof code);

    OcerzVM vm;
    ocerz_vm_init(&vm);
    ocerz_vm_install_handlers(&vm);
    if (!vm.jit) {
        fprintf(stderr, "jit unavailable\n");
        return 2;
    }

    prepare_cpu(&vm, 42);
    int rc = ocerz_jit_step(&vm, &vm.cpu);
    check_result(&vm, rc, 42);

    OcerzJitFaultInfo info;
    int have_info = block_info(&vm, &info);
    CHECK(have_info, "compiled block not found in JIT code index");
    if (have_info) {
        CHECK(info.pin_class == 3, "pin_class=%u want 3", info.pin_class);
        CHECK(info.n_pinned == 16, "n_pinned=%u want 16", info.n_pinned);
    }

    for (int i = 0; i < 256 && !failed; i++) {
        vm.exited = 0;
        vm.exit_code = 0;
        prepare_cpu(&vm, i & 0xff);
        rc = ocerz_jit_step(&vm, &vm.cpu);
        if (rc != OCERZ_STEP_EXIT || !vm.exited ||
            vm.exit_code != (i & 0xff) || vm.cpu.rip != CODE_END) {
            CHECK(0, "cached terminal iteration %d: rc=%d exited=%d code=%d rip=%#llx",
                  i, rc, vm.exited, vm.exit_code,
                  (unsigned long long)vm.cpu.rip);
        }
    }

    if (failed) {
        fprintf(stderr, "test_jit_exit: %d failure(s)\n", failed);
        return 1;
    }
    fprintf(stderr, "test_jit_exit: OK\n");
    return 0;
}
