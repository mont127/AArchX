/*
 * tests/guest/flags_deferred.c
 *
 * ADVERSARIAL flag-materialize gate. The committed flags.c differential test
 * captures RFLAGS but does it in tiny one-op call/ret helpers that fall UNDER
 * the OCERZ_PIN_MIN_INSNS (default 24) size gate, so they run the JIT's EAGER
 * flag path -- ocerz_flags_materialize() is never exercised through a captured
 * consumer there. alu.c builds big deferred blocks but folds only DATA into its
 * checksum (its produced flags are dead). So neither existing binary proves the
 * DEFERRED reconstruction is bit-exact against a PUSHF/SETcc consumer.
 *
 * This program closes that hole: each capture_* routine is a SINGLE straight-
 * line inline-asm block of >= 24 x86 instructions (no calls, no internal
 * branches) so the JIT translates it as one block, clears the size gate, DEFERS
 * every ADD/SUB/AND/OR/XOR/INC/DEC/SHL/SHR/SAR in it, and then materializes at
 * the interleaved PUSHF and SETcc consumers. The captured flags (masked to the
 * six arithmetic flags) are folded into a checksum. Any divergence between the
 * deferred JIT path and the interpreter reference changes the output, which the
 * interp-vs-jit differential flags.
 *
 * Corner cases stressed through the deferred path: shift CF/OF at count 1 and
 * larger counts; INC/DEC with CF live going in (INC/DEC preserve CF, which the
 * deferred record must snapshot); AF low-nibble carries; PF byte parity; the
 * architecturally-undefined-but-deterministic bits after logic and shifts; and
 * chains where a deferred producer is immediately followed by another producer
 * (so the record is overwritten, not materialized) before a final capture.
 */
#include "gsys.h"

#define AM 0x0cd5ULL   /* CF PF AF ZF SF OF */

/* One big single-block sweep over 64-bit ADD/SUB/logic with PUSHF captures
 * interleaved so a deferred producer is immediately materialized by the next
 * PUSHF. r15 accumulates; everything is forced into fixed registers and the
 * block has no call/branch, so the JIT compiles it as one >= 24-insn block. */
static g_u64 cap_block_a(g_u64 a, g_u64 b)
{
    g_u64 out;
    __asm__ volatile(
        "xor %%r15, %%r15\n\t"
        "mov %1, %%rcx\n\t"
        "mov %2, %%rdx\n\t"

        "add %%rdx, %%rcx\n\t"   /* deferred ADD */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "sub %%rdx, %%rcx\n\t"   /* deferred SUB */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "and %%rdx, %%rcx\n\t"   /* deferred LOGIC (AND) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "or %%rdx, %%rcx\n\t"    /* deferred LOGIC (OR) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "xor %%rdx, %%rcx\n\t"   /* deferred LOGIC (XOR) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "add %%rbx, %%rcx\n\t"   /* deferred ADD, immediately overwritten... */
        "sub %%rbx, %%rcx\n\t"   /* ...by deferred SUB before any capture */
        "cmp %%rdx, %%rcx\n\t"   /* deferred CMP (no writeback) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "mov %%r15, %0\n\t"
        : "=r"(out)
        : "r"(a), "r"(b)
        : "rax", "rbx", "rcx", "rdx", "r15", "cc", "memory");
    return out;
}

/* INC/DEC with CF seeded live, in one block, captured after each -- the
 * deferred INC/DEC record must snapshot the incoming CF (which itself may be a
 * deferred predecessor's CF). SHL/SHR/SAR at count 1 and larger, captured. */
static g_u64 cap_block_b(g_u64 a, g_u64 b, int seed_cf)
{
    g_u64 out;
    __asm__ volatile(
        "xor %%r15, %%r15\n\t"
        "mov %1, %%rcx\n\t"
        "mov %2, %%rdx\n\t"

        "add %%rdx, %%rdx\n\t"    /* deferred ADD sets CF from seed value */
        "test %3, %3\n\t"
        "jz 1f\n\t stc\n\t jmp 2f\n\t 1: clc\n\t 2:\n\t"
        "inc %%rcx\n\t"           /* deferred INC (must preserve the CF just set) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "dec %%rcx\n\t"           /* deferred DEC (preserve CF from INC's record) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "inc %%rcx\n\t"
        "dec %%rcx\n\t"           /* INC then DEC, only last captured */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "mov %%rdx, %%rcx\n\t"
        "shl $1, %%rcx\n\t"       /* deferred SHL count 1 (OF defined) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "shl $7, %%rcx\n\t"       /* deferred SHL count 7 */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "shr $1, %%rcx\n\t"       /* deferred SHR count 1 */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "shr $9, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "sar $1, %%rcx\n\t"       /* deferred SAR count 1 (OF=0) */
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "sar $13, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "mov %%r15, %0\n\t"
        : "=r"(out)
        : "r"(a), "r"(b), "r"((g_u64)seed_cf)
        : "rax", "rcx", "rdx", "r15", "cc", "memory");
    return out;
}

/* 32-bit deferred ops with SETcc consumers (SETcc reads flags via the slow
 * path, materializing the deferred record) in one straight-line block. */
static g_u64 cap_block_c(g_u32 a, g_u32 b)
{
    g_u64 out;
    __asm__ volatile(
        "xor %%r15, %%r15\n\t"
        "mov %k1, %%ecx\n\t"
        "mov %k2, %%edx\n\t"

        "add %%edx, %%ecx\n\t"     /* deferred 32-bit ADD */
        "seto %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "sub %%edx, %%ecx\n\t"     /* deferred 32-bit SUB */
        "setb %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"
        "and %%edx, %%ecx\n\t"     /* deferred 32-bit AND */
        "sets %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "shl $1, %%ecx\n\t"        /* deferred 32-bit SHL */
        "setc %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"
        "sar $5, %%ecx\n\t"        /* deferred 32-bit SAR */
        "sets %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "inc %%ecx\n\t"            /* deferred 32-bit INC */
        "sete %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"
        "cmp %%edx, %%ecx\n\t"     /* deferred 32-bit CMP */
        "setle %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "setg %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"

        "mov %%r15, %0\n\t"
        : "=r"(out)
        : "r"((g_u64)a), "r"((g_u64)b)
        : "rax", "rcx", "rdx", "r15", "cc", "memory");
    return out;
}

static g_u64 mix(g_u64 acc, g_u64 v)
{
    acc ^= v;
    acc = (acc << 13) | (acc >> 51);
    acc += 0x9e3779b97f4a7c15ULL;
    acc ^= acc >> 29;
    return acc;
}

static const g_u64 vals[] = {
    0, 1, 2, 0xffffffffffffffffULL, 0x8000000000000000ULL, 0x7fffffffffffffffULL,
    0x0fULL, 0x10ULL, 0xffULL, 0x100ULL, 0x0123456789abcdefULL, 0xfedcba9876543210ULL,
    0x00000000ffffffffULL, 0x0000000100000000ULL, 0x5555555555555555ULL, 0xaaaaaaaaaaaaaaaaULL,
    0x55ULL, 0xaaULL, 0x7fULL, 0x80ULL, 0xdeadbeefcafef00dULL, 0x0000000080000000ULL,
};
#define NV (sizeof(vals) / sizeof(vals[0]))

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    g_u64 acc = 0xcbf29ce484222325ULL;
    for (unsigned i = 0; i < NV; i++) {
        for (unsigned j = 0; j < NV; j++) {
            g_u64 a = vals[i], b = vals[j];
            acc = mix(acc, cap_block_a(a, b));
            acc = mix(acc, cap_block_b(a, b, (int)(j & 1)));
            acc = mix(acc, cap_block_c((g_u32)a, (g_u32)b));
        }
    }
    g_puthex64(acc);
    return 0;
}
