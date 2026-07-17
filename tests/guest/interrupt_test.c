/*
 * tests/guest/interrupt_test.c
 *
 * Cross-thread-exit safety-net gate for JIT block chaining.
 *
 * The hot ALU/mem/br loops the JIT is about to CHAIN are SELF-LOOPS: a Jcc back
 * to the block's own rip. Once chained, such a loop never returns to
 * ocerz_vm_run_cpu's header, so it never re-checks the process-exit / stop
 * flags. Nothing in the existing suite spins in a syscall-free loop while
 * ANOTHER thread asks it to stop, so nothing catches the hang that a missing
 * BACK-EDGE INTERRUPT POLL would introduce.
 *
 * This program is that missing case. It prints one deterministic line and then
 * spins forever in a tight, syscall-free self-loop that its OWN code can never
 * exit (the guard is volatile and never written by the guest). The only way the
 * process can terminate is for Ocerz to observe an asynchronous cross-thread
 * stop and break the loop from outside. The runner launches it with
 * OCERZ_TEST_ASYNC_STOP_ICOUNT set, which makes Ocerz spawn a separate host
 * thread that -- once this guest is past startup (so "start" is already on
 * stdout) -- calls ocerz_vm_request_exit(). That broadcasts cpu->interrupt to
 * every live guest CPU; the run loop's poll of that word breaks the spin and the
 * process exits 0.
 *
 * Golden stdout is exactly "start\n"; the "spun ..." line after the loop is
 * unreachable (the guest loop has no exit of its own) and exists only to keep
 * the accumulator live so the compiler emits a real dependent self-loop rather
 * than folding it away. Expected exit code 0.
 *
 * MUTATION TEST (how this gate earns its place): with the interrupt poll removed
 * -- run loop condition not reading cpu->interrupt AND ocerz_vm_request_exit not
 * broadcasting it, i.e. the exact state before this stage's safety net -- a
 * chained self-loop would never see the stop, this test never terminates, and
 * the runner reports a timeout (exit 124) FAIL. With the poll present it passes.
 */
#include "gsys.h"

int main(void)
{
    g_puts("start\n");

    /* Never written by the guest, so `!stop` is always true: a genuine infinite
     * self-loop. volatile forces a real load+compare+branch back-edge each
     * iteration -- the mem/br hot-loop shape -- instead of an unconditional jump
     * clang could emit for a plain for(;;). */
    volatile g_u64 stop = 0;
    g_u64 x = 0;
    while (!stop)
        x = x + 1;

    /* Unreachable: reached only if the guest's OWN loop exited, which it cannot.
     * Keeps x live so the loop body is not optimized out. */
    g_puts("spun ");
    g_putu64(x);
    return 0;
}
