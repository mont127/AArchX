/*
 * tests/guest/link_spin.c
 *
 * Cross-thread-exit safety-net gate for GENERAL two-way Jcc block linking.
 *
 * interrupt_test covers the SELF-LOOP case (a single block whose Jcc chains back
 * to its own entry). This program covers the harder MULTI-BLOCK case that general
 * two-way linking creates: a hot loop whose body is a data-dependent if/else
 * DIAMOND, so the loop spans several blocks (header, diamond split, two arms,
 * merge, back-edge) linked block-to-block. Once every edge of that cluster is
 * chained, control never returns to ocerz_vm_run_cpu's header and never bumps the
 * per-dispatch insn_count -- so the ICOUNT async-stop can no longer fire and the
 * loop is breakable ONLY by the wall-clock async-stop observing cpu->interrupt on
 * a BACK-EDGE poll. That poll (on every already-compiled cross-block target) is
 * exactly what this test's mutation deletes.
 *
 * It prints one deterministic line ("start\n") and then spins forever in a tight,
 * syscall-free diamond loop that its OWN code can never exit: `stop` is volatile
 * and never written by the guest, so `!stop` is always true. `seed` is volatile
 * so clang cannot fold the branch or linearize the diamond -- the emitted loop is
 * a genuine two-way Jcc over two distinct arms with a back-edge merge. The only
 * way the process terminates is an asynchronous cross-thread stop breaking the
 * loop from outside.
 *
 * The runners launch it with OCERZ_TEST_ASYNC_STOP_MS=200 (a WALL-CLOCK stop, not
 * an instruction-count one) precisely because a fully-linked loop freezes
 * insn_count; the MS thread waits 200ms and calls ocerz_vm_request_exit(), which
 * broadcasts cpu->interrupt to every live guest CPU, and the back-edge poll breaks
 * the spin. Golden stdout is exactly "start\n"; the "spun ..." line is unreachable
 * and exists only to keep the accumulators live. Expected exit code 0.
 *
 * MUTATION TEST (how this gate earns its place): delete the GENERAL cross-block
 * back-edge interrupt poll in emit_jcc (the poll emitted before a chained arm
 * whose target is already compiled). With that poll gone, a fully-linked diamond
 * loop never observes the cross-thread stop, this test never terminates, and the
 * runner reports a timeout (exit 124) FAIL. With the poll present it passes. The
 * mutation must target the GENERAL cross-block poll, not the self-loop poll
 * (interrupt_test already covers that).
 */
#include "gsys.h"

int main(void)
{
    g_puts("start\n");

    /* Never written by the guest, so `!stop` is always true: a genuine infinite
     * loop. seed is volatile so the diamond below is a real data-dependent
     * two-way branch clang cannot fold or linearize. */
    volatile g_u64 stop = 0;
    volatile g_u64 seed = 0x9e3779b9u;
    g_u64 x = 1, y = 2;

    while (!stop) {
        g_u64 s = seed;          /* volatile load: forces the diamond split */
        if (s & 1u)
            x = x * 3u + y;      /* taken arm  -> its own forward block */
        else
            y = y * 5u + x;      /* fall arm   -> its own forward block */
        /* merge, then the loop back-edge to the volatile-stop header block */
    }

    /* Unreachable: reached only if the guest's OWN loop exited, which it cannot.
     * Keeps x/y live so the diamond body is not optimized away. */
    g_puts("spun ");
    g_putu64(x + y);
    return 0;
}
