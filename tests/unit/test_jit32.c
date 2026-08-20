/* Differential JIT-vs-interpreter test for i386 (32-bit) blocks: stage 9.
 *
 * What this proves and how
 * ------------------------
 * Every case is one straight-line 32-bit fragment ending in `jmp DONE`, run
 * TWICE from bit-identical starting state: once instruction-by-instruction
 * through ocerz_interp_step, and once through ocerz_jit_step, which compiles
 * the whole fragment into one arm64 block and runs it.  Then the entire
 * architectural state that the fragment could have touched -- all sixteen
 * GPRs including their high halves, the materialized RFLAGS, EIP, the guest
 * mode, the xmm registers, and every byte of the data, stack and 4 GB-boundary
 * windows -- is compared.  A disagreement is a codegen bug; the interpreter is
 * the oracle, and it is the same interpreter that the 16.8M-probe i386 decoder
 * gate and tests/unit/test_interp32.c already pin.
 *
 * That structure is why the cases below are chosen the way they are.  They are
 * not "does ADD work" -- ADD is width-driven and shared with 64-bit codegen.
 * They are the five places where 32-bit mode is a DIFFERENT machine:
 *
 *   1. the effective address wraps at 4 GB.  A page is mapped at 0x100500000
 *      holding a sentinel, so an address computed 64-bit-wide reads or writes
 *      a real, mapped, WRONG location instead of faulting -- the failure is a
 *      value mismatch, which is what the comparison catches, rather than a
 *      crash that could be mistaken for an unrelated fault.
 *   2. the stack slot is 4 bytes and ESP wraps at 32 bits (the ESP=0 case
 *      pushes across the 4 GB boundary and pops back over it).
 *   3. ESP itself may arrive with a dirty high half, and every stack access
 *      must ignore it.
 *   4. the return address a CALL pushes is EIP, so it wraps: the call planted
 *      at 0xfffffffb ends exactly at 2^32 and must push 0.
 *   5. a 32-bit write to a register zero-extends into the 64-bit slot -- every
 *      GPR starts with a nonzero high half, so a missed zero-extension shows
 *      up in the very first comparison.
 *
 * The last group is the other half of the claim: the opcodes stage 9 refuses
 * to compile (PUSHA/POPA, the BCD adjusts, SALC, XCHG, BTS) must still run,
 * through the interpreter callout, and must leave the same state -- including
 * when they are interleaved with compiled instructions in one block.
 */
#include "ocerz/cpu.h"
#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define M32_CODE   0x00400000ull
#define DONE       0x00404000ull
#define M32_DATA   0x00500000ull
#define M32_STACK  0x00508000ull
#define ALIAS      0x100500000ull       /* what a NON-truncated EA would hit */
#define WRAP_PAGE  0xfffff000ull        /* last page below 4G + one past it */
#define CS32       0x0fu

#define DATA_WIN   0x1000               /* compared byte for byte after each run */

static int g_failures;
static OcerzVM g_vm;
static OcerzCPU *cpu(void) { return &g_vm.cpu; }

typedef struct {
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t rflags;
    uint8_t  mode32;
    Ocerz128 xmm[4];
    uint8_t  data[DATA_WIN];
    uint8_t  stack[0x200];
    uint8_t  wrap[0x20];
} Snapshot;

/* The starting register file.  Every high half is nonzero so that a 32-bit
 * result which failed to zero-extend cannot look correct. */
static void load_start_state(uint64_t esp)
{
    ocerz_cpu_reset(cpu());
    for (int i = 0; i < 16; i++)
        cpu()->gpr[i] = (0xdead0000ull << 32) | (0x11111111ull * (unsigned)(i + 1));
    cpu()->gpr[OCERZ_RSP] = esp;
    cpu()->gpr[OCERZ_RBP] = M32_STACK - 0x40;
    cpu()->gpr[OCERZ_RBX] = M32_DATA;
    cpu()->gpr[OCERZ_RSI] = 0x10;
    cpu()->gpr[OCERZ_RDI] = 0x00000004;
    cpu()->rflags = 0x202;
    cpu()->mode32 = 1;
    cpu()->cs_sel = (uint16_t)CS32;
    cpu()->seg_sel[OCERZ_SREG_CS] = (uint16_t)CS32;
    memset(&cpu()->xmm[0], 0x5a, sizeof cpu()->xmm[0]);
    memset(&cpu()->xmm[1], 0xa5, sizeof cpu()->xmm[1]);
}

/* Code planted in the last bytes below 4 GB has to survive the wrap-page
 * reseed that runs before every run. */
static uint8_t g_wrapcode[5];
static int g_wrapcode_set;

static void seed_memory(void)
{
    uint8_t *d = (uint8_t *)ocerz_g2h(M32_DATA);
    for (size_t i = 0; i < DATA_WIN; i++)
        d[i] = (uint8_t)(0x40 + (i * 7));
    uint8_t *s = (uint8_t *)ocerz_g2h(M32_STACK - 0x100);
    for (size_t i = 0; i < 0x200; i++)
        s[i] = (uint8_t)(0x80 + i);
    /* the alias page a non-truncated 32-bit EA would land on */
    memset(ocerz_g2h(ALIAS), 0xee, 0x1000);
    memset(ocerz_g2h(WRAP_PAGE), 0xcc, 0x2000);
    if (g_wrapcode_set)
        memcpy(ocerz_g2h(0xfffffffbull), g_wrapcode, sizeof g_wrapcode);
}

static void snap(Snapshot *s)
{
    ocerz_flags_materialize(cpu());
    memcpy(s->gpr, cpu()->gpr, sizeof s->gpr);
    s->rip = cpu()->rip;
    s->rflags = cpu()->rflags;
    s->mode32 = cpu()->mode32;
    for (int i = 0; i < 4; i++)
        s->xmm[i] = cpu()->xmm[i];
    memcpy(s->data, ocerz_g2h(M32_DATA), DATA_WIN);
    memcpy(s->stack, ocerz_g2h(M32_STACK - 0x100), sizeof s->stack);
    memcpy(s->wrap, ocerz_g2h(0xffffffe0ull), sizeof s->wrap);
}

/* Set for a case whose last instruction leaves flags the SDM calls UNDEFINED
 * (DIV/IDIV); everything else is still compared. */
static int g_skip_flags;

static int cmp_snap(const char *name, const Snapshot *a, const Snapshot *b)
{
    int bad = 0;
    static const char *rn[16] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi",
                                  "r8","r9","r10","r11","r12","r13","r14","r15" };
    for (int i = 0; i < 16; i++)
        if (a->gpr[i] != b->gpr[i]) {
            fprintf(stderr, "FAIL %s: %s interp=%#llx jit=%#llx\n", name, rn[i],
                    (unsigned long long)a->gpr[i], (unsigned long long)b->gpr[i]);
            bad = 1;
        }
    if (a->rip != b->rip) {
        fprintf(stderr, "FAIL %s: eip interp=%#llx jit=%#llx\n", name,
                (unsigned long long)a->rip, (unsigned long long)b->rip);
        bad = 1;
    }
    if (!g_skip_flags && a->rflags != b->rflags) {
        fprintf(stderr, "FAIL %s: rflags interp=%#llx jit=%#llx\n", name,
                (unsigned long long)a->rflags, (unsigned long long)b->rflags);
        bad = 1;
    }
    if (a->mode32 != b->mode32) {
        fprintf(stderr, "FAIL %s: mode32 interp=%d jit=%d\n", name, a->mode32, b->mode32);
        bad = 1;
    }
    for (int i = 0; i < 4; i++)
        if (memcmp(&a->xmm[i], &b->xmm[i], sizeof a->xmm[i])) {
            fprintf(stderr, "FAIL %s: xmm%d differs\n", name, i);
            bad = 1;
        }
    if (memcmp(a->data, b->data, DATA_WIN)) {
        for (size_t i = 0; i < DATA_WIN; i++)
            if (a->data[i] != b->data[i]) {
                fprintf(stderr, "FAIL %s: data[%#zx] interp=%#x jit=%#x\n",
                        name, i, a->data[i], b->data[i]);
                break;
            }
        bad = 1;
    }
    if (memcmp(a->stack, b->stack, sizeof a->stack)) {
        fprintf(stderr, "FAIL %s: stack window differs\n", name);
        bad = 1;
    }
    if (memcmp(a->wrap, b->wrap, sizeof a->wrap)) {
        fprintf(stderr, "FAIL %s: 4G-boundary window differs\n", name);
        bad = 1;
    }
    if (bad)
        g_failures++;
    return !bad;
}

/* Assemble <bytes> at `at`, then a jmp to DONE so the block terminates. */
static void plant(uint64_t at, const uint8_t *code, size_t n)
{
    uint8_t *p = (uint8_t *)ocerz_g2h(at);
    memcpy(p, code, n);
    int32_t rel = (int32_t)((uint32_t)DONE - (uint32_t)(at + n + 5));
    p[n] = 0xe9;
    p[n + 1] = (uint8_t)rel; p[n + 2] = (uint8_t)(rel >> 8);
    p[n + 3] = (uint8_t)(rel >> 16); p[n + 4] = (uint8_t)(rel >> 24);
}

static int run_interp(uint64_t at, uint64_t esp)
{
    seed_memory();
    load_start_state(esp);
    cpu()->rip = at;
    for (int i = 0; i < 64; i++) {
        if (cpu()->rip == DONE)
            return 1;
        if (ocerz_interp_step(&g_vm, cpu()) != OCERZ_STEP_OK)
            return 0;
    }
    return 0;
}

static int run_jit(uint64_t at, uint64_t esp)
{
    seed_memory();
    load_start_state(esp);
    cpu()->rip = at;
    for (int i = 0; i < 64; i++) {
        if (cpu()->rip == DONE)
            return 1;
        if (ocerz_jit_step(&g_vm, cpu()) != OCERZ_STEP_OK)
            return 0;
    }
    return 0;
}

static void run_case(const char *name, uint64_t at, uint64_t esp,
                     const uint8_t *code, size_t n)
{
    Snapshot si, sj;
    plant(at, code, n);
    ocerz_jit_invalidate_all(&g_vm);      /* recompile: the bytes just changed */
    if (!run_interp(at, esp)) {
        fprintf(stderr, "FAIL %s: interpreter did not reach DONE (eip=%#llx)\n",
                name, (unsigned long long)cpu()->rip);
        g_failures++;
        return;
    }
    snap(&si);
    uint64_t before = ocerz_jit_blocks(g_vm.jit);
    if (!run_jit(at, esp)) {
        fprintf(stderr, "FAIL %s: jit did not reach DONE (eip=%#llx)\n",
                name, (unsigned long long)cpu()->rip);
        g_failures++;
        return;
    }
    snap(&sj);
    if (ocerz_jit_blocks(g_vm.jit) == before) {
        fprintf(stderr, "FAIL %s: no block was compiled\n", name);
        g_failures++;
        return;
    }
    if (cmp_snap(name, &si, &sj))
        printf("  ok  %s\n", name);
}

#define CASE(name, at, esp, ...) do { \
    static const uint8_t _b[] = { __VA_ARGS__ }; \
    run_case(name, (at), (esp), _b, sizeof _b); \
} while (0)

int main(void)
{
    if (ocerz_mem_init(0x10000ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem_init failed\n");
        return 2;
    }
    if (ocerz_map_fixed(M32_CODE, 0x200000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(ALIAS, 0x1000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(WRAP_PAGE, 0x2000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "map_fixed failed\n");
        return 2;
    }
    ocerz_vm_init(&g_vm);
    ocerz_vm_install_handlers(&g_vm);
    if (!g_vm.jit) {
        fprintf(stderr, "jit unavailable\n");
        return 2;
    }
    ocerz_ldt_install(CS32, 0, 0xfffff, 0xfb, /*big*/1, /*is_long*/0, /*gran*/1);
    *(uint8_t *)ocerz_g2h(DONE) = 0xf4;          /* never executed: both runs stop here */

    /* ---- 1. addressing shapes.  ebx = M32_DATA, esi = 0x10. ---- */
    CASE("mov [ebx], eax", M32_CODE, M32_STACK,
         0x89, 0x03);                                        /* mov [ebx], eax */
    CASE("mov eax, [ebx+0x24]", M32_CODE, M32_STACK,
         0x8b, 0x43, 0x24);
    CASE("mov eax, [ebx+esi*4+0x30]", M32_CODE, M32_STACK,
         0x8b, 0x44, 0xb3, 0x30);
    CASE("mov eax, [ebx+esi*8]", M32_CODE, M32_STACK,
         0x8b, 0x04, 0xf3);
    CASE("mov eax, [esi*4+M32_DATA]", M32_CODE, M32_STACK,
         0x8b, 0x04, 0xb5, 0x00, 0x00, 0x50, 0x00);
    CASE("mov eax, [M32_DATA+8] (absolute disp32)", M32_CODE, M32_STACK,
         0xa1, 0x08, 0x00, 0x50, 0x00);
    CASE("movzx/movsx from [ebx]", M32_CODE, M32_STACK,
         0x0f, 0xb6, 0x03,                                   /* movzx eax, byte [ebx] */
         0x0f, 0xbf, 0x4b, 0x02,                             /* movsx ecx, word [ebx+2] */
         0x0f, 0xbe, 0x53, 0x04);                            /* movsx edx, byte [ebx+4] */
    CASE("add/sub/cmp reg,[mem] + setcc + cmov", M32_CODE, M32_STACK,
         0x03, 0x43, 0x08,                                   /* add eax, [ebx+8] */
         0x2b, 0x4b, 0x0c,                                   /* sub ecx, [ebx+12] */
         0x3b, 0x53, 0x10,                                   /* cmp edx, [ebx+16] */
         0x0f, 0x9c, 0xc0,                                   /* setl al */
         0x0f, 0x4f, 0xf9);                                  /* cmovg edi, ecx */
    CASE("lea + shift + imul", M32_CODE, M32_STACK,
         0x8d, 0x44, 0xb3, 0x20,                             /* lea eax, [ebx+esi*4+0x20] */
         0xc1, 0xe0, 0x03,                                   /* shl eax, 3 */
         0x0f, 0xaf, 0xc6);                                  /* imul eax, esi */
    /* DIV leaves every arithmetic flag UNDEFINED, so only the quotient,
     * remainder and memory state are compared here. */
    g_skip_flags = 1;
    CASE("div / idiv", M32_CODE, M32_STACK,
         0xb8, 0x39, 0x30, 0x00, 0x00,                       /* mov eax, 0x3039 */
         0x31, 0xd2,                                         /* xor edx, edx */
         0xf7, 0xf6,                                         /* div esi */
         0x99,                                               /* cdq */
         0xf7, 0xfe);                                        /* idiv esi */
    g_skip_flags = 0;
    CASE("32-bit writes zero-extend", M32_CODE, M32_STACK,
         0xb8, 0x78, 0x56, 0x34, 0x12,                       /* mov eax, 0x12345678 */
         0x89, 0xc1,                                         /* mov ecx, eax */
         0x01, 0xc1,                                         /* add ecx, eax */
         0xf7, 0xd9,                                         /* neg ecx */
         0x0f, 0xc8);                                        /* bswap eax */

    /* ---- 2. the 4 GB effective-address wrap.
     * The fragment builds ebx = 0xfffffff0 and reads [ebx+0x00500010], whose
     * 64-bit sum is 0x100500000 -- the ALIAS page, which is mapped and holds
     * 0xee.  Truncated it is M32_DATA, which holds the seeded pattern. */
    CASE("EA wraps at 4G (load)", M32_CODE, M32_STACK,
         0xbb, 0xf0, 0xff, 0xff, 0xff,                       /* mov ebx, 0xfffffff0 */
         0x8b, 0x83, 0x10, 0x00, 0x50, 0x00);                /* mov eax, [ebx+0x500010] */
    CASE("EA wraps at 4G (store)", M32_CODE, M32_STACK,
         0xbb, 0xf0, 0xff, 0xff, 0xff,                       /* mov ebx, 0xfffffff0 */
         0xb8, 0x21, 0x43, 0x65, 0x87,                       /* mov eax, 0x87654321 */
         0x89, 0x83, 0x20, 0x00, 0x50, 0x00);                /* mov [ebx+0x500020], eax */
    CASE("EA wraps at 4G (index)", M32_CODE, M32_STACK,
         0xbb, 0xf0, 0xff, 0xff, 0xff,                       /* mov ebx, 0xfffffff0 */
         0xbe, 0x04, 0x00, 0x14, 0x00,                       /* mov esi, 0x140004 */
         0x8b, 0x04, 0xb3);                                  /* mov eax, [ebx+esi*4] */

    /* ---- 3. stack: 4-byte slots, and ESP wrapping across 4 GB ---- */
    CASE("push/pop/leave", M32_CODE, M32_STACK,
         0x53,                                               /* push ebx */
         0x68, 0x11, 0x22, 0x33, 0x44,                       /* push 0x44332211 */
         0x58,                                               /* pop eax */
         0x5a,                                               /* pop edx */
         0x89, 0xe5,                                         /* mov ebp, esp */
         0x51,                                               /* push ecx */
         0xc9);                                              /* leave */
    CASE("esp wraps at 4G", M32_CODE, 0x00000000ull,
         0x53,                                               /* push ebx  -> esp = 0xfffffffc */
         0x56,                                               /* push esi  -> esp = 0xfffffff8 */
         0x5f,                                               /* pop edi */
         0x58);                                              /* pop eax   -> esp = 0 */
    CASE("dirty high half of esp is ignored", M32_CODE, 0x0000000100507f00ull,
         0x53,                                               /* push ebx */
         0x58,                                               /* pop eax */
         0x8b, 0x0c, 0x24);                                  /* mov ecx, [esp] */
    CASE("push esp / pop esp", M32_CODE, M32_STACK,
         0x54,                                               /* push esp */
         0x5c);                                              /* pop esp */

    /* ---- 4. branches: 32-bit EIP, 4-byte return address ---- */
    CASE("jcc taken / not taken", M32_CODE, M32_STACK,
         0x31, 0xc0,                                         /* xor eax, eax */
         0x85, 0xc0,                                         /* test eax, eax */
         0x74, 0x05,                                         /* je +5 */
         0xb8, 0xff, 0xff, 0xff, 0xff,                       /* mov eax, -1 (skipped) */
         0x83, 0xc0, 0x07);                                  /* add eax, 7 */

    {
        /* near call and ret, with a 4-byte return address on the stack */
        uint8_t *cal = (uint8_t *)ocerz_g2h(M32_CODE + 0x100);
        cal[0] = 0x83; cal[1] = 0xc0; cal[2] = 0x05;         /* add eax, 5 */
        cal[3] = 0xc3;                                       /* ret */
        static const uint8_t c[] = {
            0xb8, 0x01, 0x00, 0x00, 0x00,                    /* mov eax, 1 */
            0xe8, 0xf6, 0x00, 0x00, 0x00,                    /* call M32_CODE+0x100 */
            0x83, 0xc0, 0x09,                                /* add eax, 9 */
        };
        run_case("call/ret (4-byte return address)", M32_CODE, M32_STACK, c, sizeof c);
    }
    {
        /* ret imm16 */
        uint8_t *cal = (uint8_t *)ocerz_g2h(M32_CODE + 0x100);
        cal[0] = 0xc2; cal[1] = 0x08; cal[2] = 0x00;         /* ret 8 */
        static const uint8_t c[] = {
            0x68, 0xaa, 0xbb, 0xcc, 0xdd,                    /* push 0xddccbbaa */
            0x68, 0x55, 0x66, 0x77, 0x88,                    /* push 0x88776655 */
            0xe8, 0xf1, 0x00, 0x00, 0x00,                    /* call M32_CODE+0x100 */
            0x83, 0xc0, 0x09,                                /* add eax, 9 */
        };
        run_case("ret imm16", M32_CODE, M32_STACK, c, sizeof c);
    }
    {
        /* The return address a CALL pushes is EIP, so it wraps: this call ends
         * exactly at 2^32 and must push 0, not 0x100000000. */
        int32_t rel = (int32_t)(uint32_t)(M32_CODE + 0x100);   /* target from EIP 0 */
        g_wrapcode[0] = 0xe8;
        g_wrapcode[1] = (uint8_t)rel; g_wrapcode[2] = (uint8_t)(rel >> 8);
        g_wrapcode[3] = (uint8_t)(rel >> 16); g_wrapcode[4] = (uint8_t)(rel >> 24);
        g_wrapcode_set = 1;
        uint8_t *cal = (uint8_t *)ocerz_g2h(M32_CODE + 0x100);
        cal[0] = 0x58;                                       /* pop eax */
        int32_t r2 = (int32_t)((uint32_t)DONE - (uint32_t)(M32_CODE + 0x101 + 5));
        cal[1] = 0xe9;
        cal[2] = (uint8_t)r2; cal[3] = (uint8_t)(r2 >> 8);
        cal[4] = (uint8_t)(r2 >> 16); cal[5] = (uint8_t)(r2 >> 24);
        Snapshot si, sj;
        ocerz_jit_invalidate_all(&g_vm);
        int oki = run_interp(0xfffffffbull, M32_STACK);
        snap(&si);
        int okj = run_jit(0xfffffffbull, M32_STACK);
        snap(&sj);
        if (!oki || !okj) {
            fprintf(stderr, "FAIL call at 4G-5: interp=%d jit=%d eip=%#llx\n",
                    oki, okj, (unsigned long long)cpu()->rip);
            g_failures++;
        } else if (cmp_snap("call return address wraps at 4G", &si, &sj)) {
            if (si.gpr[OCERZ_RAX] != 0) {
                fprintf(stderr, "FAIL call at 4G-5: pushed eip=%#llx (want 0)\n",
                        (unsigned long long)si.gpr[OCERZ_RAX]);
                g_failures++;
            } else {
                printf("  ok  call return address wraps at 4G (pushed 0)\n");
            }
        }
    }

    /* ---- 5. SSE through a 32-bit effective address ---- */
    CASE("sse load/store/arith via 32-bit EA", M32_CODE, M32_STACK,
         0x0f, 0x10, 0x03,                                   /* movups xmm0, [ebx] */
         0x0f, 0x10, 0x4b, 0x10,                             /* movups xmm1, [ebx+16] */
         0x66, 0x0f, 0xfe, 0xc1,                             /* paddd xmm0, xmm1 */
         0x0f, 0x11, 0x43, 0x40,                             /* movups [ebx+64], xmm0 */
         0xf3, 0x0f, 0x10, 0x53, 0x20,                       /* movss xmm2, [ebx+32] */
         0x0f, 0x28, 0xd8);                                  /* movaps xmm3, xmm0 */

    /* ---- 6. the instructions stage 9 deliberately does NOT compile still run
     * (they take the interpreter callout, and must land in the same state) ---- */
    CASE("punted i386 opcodes still execute", M32_CODE, M32_STACK,
         0x60,                                               /* pusha */
         0x61,                                               /* popa */
         0x27,                                               /* daa */
         0x37,                                               /* aaa */
         0xd6,                                               /* salc */
         0x87, 0xd8,                                         /* xchg eax, ebx */
         0x0f, 0xab, 0xc1);                                  /* bts ecx, eax */
    CASE("mixed compiled and punted", M32_CODE, M32_STACK,
         0x8b, 0x43, 0x08,                                   /* mov eax, [ebx+8]   (compiled) */
         0x60,                                               /* pusha              (punted) */
         0x01, 0xc1,                                         /* add ecx, eax       (compiled) */
         0x61,                                               /* popa               (punted) */
         0x89, 0x4b, 0x0c);                                  /* mov [ebx+12], ecx  (compiled) */

    if (g_failures) {
        fprintf(stderr, "test_jit32: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_jit32: all cases identical (interpreter vs JIT)\n");
    return 0;
}
