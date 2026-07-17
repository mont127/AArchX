/*
 * tests/guest/ras_stress.c
 *
 * Adversarial gate for the STEP 1 return-address-stack predictor
 * (ocerz_ras_push + the emit_call_ret RET fast-return, src/jit.c).
 *
 * The predictor is a PURE HINT: on the RET side it may branch straight into a
 * cached host block ONLY when the top RAS entry's guest_rip EXACTLY equals the
 * value just popped off the guest stack. This test manufactures the precise
 * case where that guard is the ONLY thing standing between correct and wrong
 * control flow, and pins the result to real hardware (the golden is the native
 * x86 run under Rosetta), so it holds ocerz to the machine, not itself.
 *
 * THE TRAP -- a MISPREDICT with a NON-NULL top. rec() self-recurses DIRECTLY, so
 * each `rec(n-1)` is a real `call rel32` that RAS-pushes the SAME return rip
 * (the site after that call). Depth is kept UNDER 256 so RAS never saturates and
 * stays perfectly balanced -- if it saturated, dropped pushes would desync
 * ras_top down to empty and the empty-stack check, not the rip guard, would mask
 * the mispredict (measured: a depth-500 version does NOT catch the mutation).
 * After the recursive call returns, rec() makes an INDIRECT call to helper()
 * through a volatile pointer; the JIT does not inline indirect calls, so helper
 * gets NO RAS push. When helper RETURNS, the address popped off the guest stack
 * is the site after `g_h(a)`, but the RAS top still holds the PARENT frame's
 * push -- the site after `rec(n-1)`, a DIFFERENT rip whose block is compiled and
 * non-NULL. That is exactly predicted != popped, entry non-NULL: the rip guard
 * must reject it and dispatch to the g_h site. A rip-blind predictor branches
 * into the parent's continuation block and diverges (empirically: wrong checksum
 * then a SIGBUS as control runs off into garbage).
 *
 * Not closed-form-reducible: a volatile-pointer indirect call and a rare shallow
 * second self-call keep clang from linearizing rec() into a loop (a purely
 * linear self-call compiles to a loop with NO `call` at all and would test
 * nothing -- verified by disassembly; the emitted rec() has 3 real callq).
 *
 * Expected stdout: one deterministic decimal line (see golden). Exit 0.
 *
 * MUTATION TEST (how this earns its place, run during review): delete the
 * RET-side guest_rip guard in emit_call_ret -- drop the `a64_cbnz(... JTT ...)`
 * mispredict branch so the predictor trusts the top RAS entry without checking
 * it equals the popped address. With that guard gone this program printed a
 * WRONG number and then SIGBUS'd (control diverged to guest_addr 0x3); with it
 * present, ocerz JIT == ocerz interp == ocerz JIT(OCERZ_NO_RAS=1) == native.
 */
#include "gsys.h"

/* A leaf helper reached ONLY through a volatile function pointer, so every call
 * to it is an INDIRECT call: the JIT does not inline it and pushes NO RAS entry.
 * That is the crux of the trap below. */
static g_u64 helper(g_u64 x) { return (x * 3u) + 0x9e3779b9u; }
static g_u64 (*volatile g_h)(g_u64) = helper;

/* rec is called DIRECTLY (RAS-pushed). Its DIRECT self-recursion spine stays
 * under the 256-entry RAS (see header: saturation would mask the mutation) and,
 * critically, makes an INDIRECT call to helper AFTER the recursive call returns.
 * When helper returns, the address popped off the guest stack is the site right
 * after the `g_h(a)` call; but the RAS top at that instant is the entry the
 * PARENT rec frame pushed when it called THIS rec -- the site after `rec(n-1)`,
 * a DIFFERENT rip whose block is compiled and non-NULL. This is the exact
 * mispredict the RET-side rip-equality guard exists to reject: predicted !=
 * popped, entry non-NULL. A correct predictor dispatches and returns to the g_h
 * site; a rip-blind one branches into the parent's continuation block and
 * diverges. The occasional shallow second self-call keeps clang from linearizing
 * rec into a loop (a purely linear self-call compiles to a loop with no `call`
 * at all and tests nothing -- verified by disassembly). */
static g_u64 rec(unsigned n)
{
    if (n == 0)
        return 0x9e3779b9u;
    g_u64 a = rec(n - 1);                 /* deep DIRECT spine: real call rel32 */
    g_u64 b = g_h(a);                     /* INDIRECT call -> stale-top RET trap */
    g_u64 c = 0;
    if ((n & 15u) == 0)
        c = rec((n >> 4) + 1);            /* rare shallow branch: blocks loop-ization */
    return (a * 1000003u) ^ b ^ c ^ ((g_u64)n << 5);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    g_u64 acc = 0x1234567u;
    for (int i = 0; i < 200; i++) {
        acc += rec(200);                  /* direct entry; depth 200 < 256 RAS, no saturation */
        acc ^= (acc << 1) + 0x9e3779b97f4a7c15ull;
        acc = (acc >> 7) | (acc << 57);   /* rotate: keep all bits live */
    }
    g_putu64(acc);
    return 0;
}
