/*
 * Frame-pointer locals in ordered code (src/jit.c, rbp_local), read off the
 * arm64 three blocks translate to once the process has gone ordered.  A stores
 * to [rbp-8] and loads [rbp-16], a frame's locals: no acquire or release
 * access.  B loads [rbp+16], which a frame pointer reaches only for the
 * caller's arguments and a pointer in rbp for any field: ordered.  C loads
 * [rbp+rbx-24], an index: ordered.  Each block must also compute what x86
 * would.
 */
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
#define A_RIP     (CODE_BASE + 0x00)
#define B_RIP     (CODE_BASE + 0x20)
#define C_RIP     (CODE_BASE + 0x40)
#define DONE_RIP  (CODE_BASE + 0x80)
#define FRAME     (CODE_BASE + 0x7f00)
#define STACK_TOP (CODE_BASE + 0x7e00)

static int failed;

#define CHECK(c, ...) do { \
    if (!(c)) { \
        failed++; \
        fprintf(stderr, "FAIL: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

/* acquire/release accesses: LDAPUR/STLUR and their sign-extending forms, LDAR/STLR, LDAPR */
static int is_ordered(uint32_t w)
{
    return (w & 0x3F200C00u) == 0x19000000u || (w & 0x3FBFFC00u) == 0x089FFC00u ||
           (w & 0x3FFFFC00u) == 0x38BFC000u;
}

/* the ordered accesses among the words the block at rip translated to since the mark */
static int ordered_since(OcerzVM *vm, const uint32_t *from, uint64_t rip)
{
    int n = 0;
    const uint32_t *lo = NULL, *hi = NULL;
    if (!ocerz_jit_code_range(vm, &lo, &hi))
        return -1;
    for (const uint32_t *pc = from; pc < hi; pc++) {
        OcerzJitFaultInfo fi;
        if (ocerz_jit_fault_info(vm, pc, &fi) && fi.block_rip == rip)
            n += is_ordered(*pc);
    }
    return n;
}

static const uint32_t *arena_top(OcerzVM *vm)
{
    const uint32_t *lo = NULL, *hi = NULL;
    return ocerz_jit_code_range(vm, &lo, &hi) ? hi : NULL;
}

static void put_block(uint64_t rip, const uint8_t *code, size_t len)
{
    memcpy(ocerz_g2h(rip), code, len);
    uint64_t jmp_at = rip + len;
    int32_t rel = (int32_t)(DONE_RIP - (jmp_at + 5));
    uint8_t jmp[5] = { 0xe9, (uint8_t)rel, (uint8_t)(rel >> 8), (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
    memcpy(ocerz_g2h(jmp_at), jmp, sizeof jmp);
}

static int run(OcerzVM *vm, uint64_t rip)
{
    OcerzCPU *cpu = &vm->cpu;
    ocerz_st(FRAME - 8, 8, 0);
    ocerz_st(FRAME - 16, 8, 0x1616);
    ocerz_st(FRAME + 16, 8, 0x2020);
    ocerz_st(FRAME + 8, 8, 0x2424);
    cpu->rip = rip;
    cpu->gpr[OCERZ_RAX] = 0x4242;
    cpu->gpr[OCERZ_RBX] = 32;
    cpu->gpr[OCERZ_RBP] = FRAME;
    cpu->gpr[OCERZ_RSP] = STACK_TOP;
    return ocerz_jit_step(vm, cpu) == OCERZ_STEP_OK && cpu->rip == DONE_RIP;
}

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK ||
        ocerz_map_fixed(CODE_BASE, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "memory setup failed\n");
        return 2;
    }
    static const uint8_t a[] = {
        0x48, 0x89, 0x45, 0xf8,         /* mov [rbp-8], rax */
        0x48, 0x8b, 0x4d, 0xf0,         /* mov rcx, [rbp-16] */
    };
    static const uint8_t b[] = {
        0x48, 0x8b, 0x55, 0x10,         /* mov rdx, [rbp+16] */
    };
    static const uint8_t c[] = {
        0x4c, 0x8b, 0x44, 0x1d, 0xe8,   /* mov r8, [rbp+rbx-24] */
    };
    put_block(A_RIP, a, sizeof a);
    put_block(B_RIP, b, sizeof b);
    put_block(C_RIP, c, sizeof c);

    OcerzVM vm;
    ocerz_vm_init(&vm);
    vm.jit_plain_mem = 1;
    ocerz_vm_install_handlers(&vm);
    if (!vm.jit) {
        fprintf(stderr, "jit unavailable\n");
        return 2;
    }
    ocerz_jit_require_ordered(&vm);

    const uint32_t *mark = arena_top(&vm);
    CHECK(run(&vm, A_RIP), "block A did not run to its end");
    CHECK(ocerz_ld(FRAME - 8, 8) == 0x4242 && vm.cpu.gpr[OCERZ_RCX] == 0x1616, "block A computed wrong");
    int n = ordered_since(&vm, mark, A_RIP);
    CHECK(n == 0, "block A, a frame's locals, has %d ordered accesses, want none", n);

    mark = arena_top(&vm);
    CHECK(run(&vm, B_RIP) && vm.cpu.gpr[OCERZ_RDX] == 0x2020, "block B computed wrong");
    n = ordered_since(&vm, mark, B_RIP);
    CHECK(n >= 1, "block B, [rbp+16], has no ordered access");

    mark = arena_top(&vm);
    CHECK(run(&vm, C_RIP) && vm.cpu.gpr[OCERZ_R8] == 0x2424, "block C computed wrong");
    n = ordered_since(&vm, mark, C_RIP);
    CHECK(n >= 1, "block C, [rbp+rbx-24], has no ordered access");

    ocerz_jit_destroy(vm.jit);
    fprintf(stderr, "test_jit_rbp_plain: %s\n", failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
