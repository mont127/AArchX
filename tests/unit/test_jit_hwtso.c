/*
 * Hardware TSO's three memory models (src/cpu.c; src/jit.c, "memory ordering"),
 * read off the arm64 a block translates to.  One block stores, loads, adds
 * locked and exchanges with memory.  Plain (one observer): no ordered access and
 * no atomic.  Ordered with the order from the processor (OCERZ_PRETEND_HW_TSO,
 * since no thread here can get it for real): no acquire or release access, no
 * barrier, but the locked add and the exchange still LSE atomics.  Ordered after
 * a thread was refused the order (ocerz_jit_hwtso_lost): the hardware-ordered
 * block is retired and the new one orders its accesses again.  Each translation
 * must also compute the same memory.
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

typedef struct { int ordered, barriers, atomics; } Forms;

/* acquire/release accesses: LDAPUR/STLUR and their sign-extending forms, LDAR/STLR, LDAPR */
static int is_ordered(uint32_t w)
{
    return (w & 0x3F200C00u) == 0x19000000u || (w & 0x3FBFFC00u) == 0x089FFC00u ||
           (w & 0x3FFFFC00u) == 0x38BFC000u;
}
static int is_barrier(uint32_t w) { return (w & 0xFFFFF0FFu) == 0xD50330BFu; }
/* LD<op>AL and SWPAL, CASAL */
static int is_atomic(uint32_t w)
{
    return (w & 0x3FE00C00u) == 0x38E00000u || (w & 0x3FE0FC00u) == 0x08E0FC00u;
}

/* the words the block at A_RIP translated to: the arena between the two marks, kept to that block */
static Forms forms_since(OcerzVM *vm, const uint32_t *from)
{
    Forms f = { 0, 0, 0 };
    const uint32_t *lo = NULL, *hi = NULL;
    if (!ocerz_jit_code_range(vm, &lo, &hi))
        return f;
    for (const uint32_t *pc = from; pc < hi; pc++) {
        OcerzJitFaultInfo fi;
        if (!ocerz_jit_fault_info(vm, pc, &fi) || fi.block_rip != A_RIP)
            continue;
        f.ordered += is_ordered(*pc);
        f.barriers += is_barrier(*pc);
        f.atomics += is_atomic(*pc);
    }
    return f;
}

static const uint32_t *arena_top(OcerzVM *vm)
{
    const uint32_t *lo = NULL, *hi = NULL;
    return ocerz_jit_code_range(vm, &lo, &hi) ? hi : NULL;
}

/* runs A once from a known state; 1 when it left memory and registers as x86 would */
static int run_a(OcerzVM *vm, uint64_t seed)
{
    OcerzCPU *cpu = &vm->cpu;
    ocerz_st(DATA_ADDR, 8, 0);
    ocerz_st(DATA_ADDR + 8, 8, 1000);
    ocerz_st(DATA_ADDR + 16, 8, seed * 3);
    ocerz_st(DATA_ADDR + 24, 8, 77);
    cpu->rip = A_RIP;
    cpu->gpr[OCERZ_RAX] = seed;
    cpu->gpr[OCERZ_RDX] = seed + 5;
    cpu->gpr[OCERZ_RDI] = DATA_ADDR;
    cpu->gpr[OCERZ_RSP] = STACK_TOP;
    if (ocerz_jit_step(vm, cpu) != OCERZ_STEP_OK)
        return 0;
    return cpu->rip == DONE_RIP && ocerz_ld(DATA_ADDR, 8) == seed && cpu->gpr[OCERZ_RCX] == seed * 3 &&
           ocerz_ld(DATA_ADDR + 8, 8) == 1000 + seed && ocerz_ld(DATA_ADDR + 24, 8) == seed + 5 &&
           cpu->gpr[OCERZ_RDX] == 77;
}

int main(void)
{
    setenv("OCERZ_PRETEND_HW_TSO", "1", 1);
    ocerz_hwtso_enable();
    CHECK(ocerz_hwtso_on, "OCERZ_PRETEND_HW_TSO did not turn hardware TSO on");

    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK ||
        ocerz_map_fixed(CODE_BASE, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "memory setup failed\n");
        return 2;
    }
    static const uint8_t a[] = {
        0x48, 0x89, 0x07,               /* mov [rdi], rax */
        0x48, 0x8b, 0x4f, 0x10,         /* mov rcx, [rdi+16] */
        0xf0, 0x48, 0x01, 0x47, 0x08,   /* lock add [rdi+8], rax */
        0x48, 0x87, 0x57, 0x18,         /* xchg [rdi+24], rdx */
    };
    memcpy(ocerz_g2h(A_RIP), a, sizeof a);
    uint64_t jmp_at = A_RIP + sizeof a;
    int32_t rel = (int32_t)(DONE_RIP - (jmp_at + 5));
    uint8_t jmp[5] = { 0xe9, (uint8_t)rel, (uint8_t)(rel >> 8), (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
    memcpy(ocerz_g2h(jmp_at), jmp, sizeof jmp);

    OcerzVM vm;
    ocerz_vm_init(&vm);
    vm.jit_plain_mem = 1;
    ocerz_vm_install_handlers(&vm);
    if (!vm.jit) {
        fprintf(stderr, "jit unavailable\n");
        return 2;
    }

    const uint32_t *mark = arena_top(&vm);
    CHECK(run_a(&vm, 10), "plain block computed wrong");
    Forms f = forms_since(&vm, mark);
    CHECK(f.ordered == 0 && f.barriers == 0 && f.atomics == 0,
          "plain block has ordered=%d barriers=%d atomics=%d, want none", f.ordered, f.barriers, f.atomics);

    ocerz_jit_require_ordered(&vm);
    mark = arena_top(&vm);
    uint64_t before = ocerz_jit_blocks(vm.jit);
    CHECK(run_a(&vm, 20), "hardware-ordered block computed wrong");
    CHECK(ocerz_jit_blocks(vm.jit) == before + 1, "ordering did not translate A again");
    f = forms_since(&vm, mark);
    CHECK(f.ordered == 0 && f.barriers == 0, "hardware-ordered block has ordered=%d barriers=%d, want none",
          f.ordered, f.barriers);
    CHECK(f.atomics >= 2, "hardware-ordered block has %d atomics, want the locked add and the exchange", f.atomics);
    CHECK(ocerz_hwtso_on, "this thread's pretended order was taken back");

    ocerz_jit_hwtso_lost(&vm);
    CHECK(!ocerz_hwtso_on, "a refused thread left hardware TSO on");
    mark = arena_top(&vm);
    before = ocerz_jit_blocks(vm.jit);
    CHECK(run_a(&vm, 30), "software-ordered block computed wrong");
    CHECK(ocerz_jit_blocks(vm.jit) == before + 1, "losing the order did not retire the hardware-ordered block");
    f = forms_since(&vm, mark);
    CHECK(f.ordered >= 2, "software-ordered block has %d ordered accesses, want the store and the load", f.ordered);
    CHECK(f.atomics >= 2, "software-ordered block has %d atomics, want the locked add and the exchange", f.atomics);

    ocerz_jit_hwtso_lost(&vm);
    before = ocerz_jit_blocks(vm.jit);
    CHECK(run_a(&vm, 40), "software-ordered block computed wrong the second time");
    CHECK(ocerz_jit_blocks(vm.jit) == before, "losing the order twice retired the software-ordered block");

    ocerz_jit_destroy(vm.jit);
    fprintf(stderr, "test_jit_hwtso: %s\n", failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
