/*
 * tests/guest/longblock.c
 *
 * Regression test for the JIT block-length-cap rip hazard. The JIT decodes a
 * basic block up to a fixed instruction cap (JIT_MAX_BLOCK_INSNS) and inlines
 * the hot ALU/MOV instructions directly, without writing cpu->rip per
 * instruction — the architectural rip is normally restored by the slow-path
 * call or the terminator that ends the block. A block that ends by running
 * OFF the cap instead of on a terminator has no such instruction at its tail,
 * so if its last (inlined) instruction does not set rip, the fall-through exit
 * resumes execution at a stale rip inside the block and silently corrupts
 * state. Real optimized crypto transforms (Digest::SHA/MD5) hit this with
 * their long unrolled rounds; this test reproduces the shape with no libc.
 *
 * The body is one enormous straight-line basic block: a dependent chain of
 * hundreds of 32-bit ADD/SUB/AND/OR/XOR/shift operations (exactly the inlined
 * fast paths) with no branch or call between them, so the translator must span
 * it across several capped blocks whose boundaries land on inlined ALU ops.
 * Two volatile seeds keep clang from folding the chain to a constant at -O2
 * while leaving the output fully deterministic (no time, pid, or pointer
 * value enters the computation), so the differential interp-vs-JIT check and
 * the checked-in golden both pin the result. A pre-fix JIT prints a different,
 * ASLR-dependent value (or loops); a correct JIT matches the interpreter.
 */
#include "gsys.h"

#define STEP(k)                          \
    a += b;                              \
    a ^= (g_u32)(0x9e3779b9u + (k));     \
    b -= a;                              \
    b &= 0x7fffffffu;                    \
    a |= (b >> 3);                       \
    a -= (g_u32)(k);                     \
    b ^= (a << 5);                       \
    a += (b | (g_u32)(k));               \
    b += (a & 0x00ffff00u);              \
    a ^= (b - (g_u32)((k) * 7u));

#define STEP4(k)  STEP(k) STEP((k) + 1) STEP((k) + 2) STEP((k) + 3)
#define STEP16(k) STEP4(k) STEP4((k) + 4) STEP4((k) + 8) STEP4((k) + 12)
#define STEP64(k) STEP16(k) STEP16((k) + 16) STEP16((k) + 32) STEP16((k) + 48)

int main(void)
{
    volatile g_u32 vs0 = 0x01234567u;
    volatile g_u32 vs1 = 0x89abcdefu;
    g_u32 a = vs0;
    g_u32 b = vs1 ^ 0xa5a5a5a5u;

    STEP64(1)
    STEP64(65)
    STEP64(129)

    g_puthex64(((g_u64)a << 32) | (g_u64)b);
    return 0;
}
