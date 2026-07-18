/*
 * tests/unit/test_chain_concurrency.c
 *
 * ATTACK C gate: LIVE-CODE PATCHING of a chained block tail while ANOTHER host
 * (guest) thread is executing that same block.
 *
 * chain_activate (src/jit.c) rewrites the one 32-bit activation B word inside an
 * already-published source block S so it branches straight into a freshly
 * compiled target T. That rewrite happens under jit_lock on the compiling
 * thread, but a DIFFERENT thread may be spinning inside S's code at the same
 * instant. The safety argument has two independent halves and this test pins
 * BOTH to observed behavior:
 *
 *   CORRECTNESS  - the store is a single aligned 32-bit write (single-copy-atomic
 *                  on arm64), and the OLD tail (b -> in-block fallback -> restore
 *                  STEP_OK -> ret to the dispatcher) and the NEW tail (b ->
 *                  target host entry) are SEMANTICALLY IDENTICAL: both continue
 *                  the guest at T's rip. So a concurrent executor that fetches
 *                  either word is correct, and it can never fetch a torn half.
 *                  Asserted: over millions of concurrent executions of S spanning
 *                  the patch, every step returns STEP_OK and lands on a VALID
 *                  successor rip (T's entry rip, or -- once chained -- the rip T
 *                  itself computes). Never a garbage rip, never a crash.
 *
 *   LIVENESS     - sys_icache_invalidate after the store is what makes the new
 *                  word observable to the OTHER core's instruction fetch (IC IVAU
 *                  is broadcast in the inner-shareable domain). Asserted: the
 *                  spinning thread EVENTUALLY observes the chained outcome (T's
 *                  computed rip), proving the patch became visible cross-core
 *                  while the thread was live in the block.
 *
 * MUTATION TEST (how this earns its place):
 *   - Delete `sys_icache_invalidate(patch_b, 4)` in chain_activate. CORRECTNESS
 *     still holds (the stale word is the old fallback tail, which is correct), so
 *     no crash -- but the spinning thread's core keeps fetching the stale word
 *     and NEVER observes the chained outcome, so the LIVENESS assertion fails
 *     (this test reports FAIL: "chain never observed by the spinning thread").
 *     That is the property the icache flush exists to provide.
 *   - Break the atomicity / correctness of the new tail (verified separately):
 *     the CORRECTNESS assertion fires instead.
 *
 * Guest layout, hand-assembled into one RW code page:
 *   A  (0x200000000):  E8 rel32   call T           <- block S, chainable CALL(imm)
 *   B  (0x200000040):  E9 rel32   jmp  DONE         <- block T, sets rip = DONE
 *   DONE(0x200000080): (never executed; a distinct sentinel rip)
 *
 * The spinning thread only ever steps rip=A, so it never compiles T itself; the
 * compiling thread is the sole writer of S's tail. Unchained, stepping S returns
 * rip=B (the CALL's target). Chained, stepping S runs S then branches into T,
 * which sets rip=DONE. {B, DONE} is therefore the exact valid-successor set.
 */
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/jit.h"
#include "ocerz/interp.h"
#include "ocerz/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>

#define CODE_BASE 0x200000000ull
#define A_RIP     (CODE_BASE + 0x00)
#define B_RIP     (CODE_BASE + 0x40)
#define DONE_RIP  (CODE_BASE + 0x80)
#define STACK_TOP (CODE_BASE + 0x8000)   /* inside the mapped page, well above code */

static OcerzVM g_vm;

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL: "); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void put_bytes(uint64_t gaddr, const uint8_t *b, size_t n)
{
    uint8_t *p = (uint8_t *)ocerz_g2h(gaddr);
    memcpy(p, b, n);
}

static atomic_int  g_spinner_ready;     /* spinner has entered its loop */
static atomic_int  g_target_compiled;   /* compiler has published T + patched S */
static atomic_int  g_stop_spin;         /* tell the spinner to finish */
static atomic_ullong g_saw_done;        /* spinner observed the chained outcome */
static atomic_ullong g_saw_b;           /* spinner observed the un-chained outcome */
static atomic_int  g_bad_rip;           /* spinner ever saw a rip outside {B,DONE} */
static atomic_int  g_bad_rc;            /* spinner ever saw a non-STEP_OK step */

/* The spinning guest thread: repeatedly execute block S (rip=A) with a fresh
 * stack each time, across the moment the compiler live-patches S's tail. */
static void *spinner_entry(void *arg)
{
    (void)arg;
    OcerzCPU cpu;
    unsigned long long iters = 0;
    atomic_store(&g_spinner_ready, 1);
    for (;;) {
        ocerz_cpu_reset(&cpu);
        cpu.rip = A_RIP;
        cpu.gpr[OCERZ_RSP] = STACK_TOP;
        int rc = ocerz_jit_step(&g_vm, &cpu);
        if (rc != OCERZ_STEP_OK)
            atomic_store(&g_bad_rc, rc ? rc : -999);
        if (cpu.rip == DONE_RIP)
            atomic_fetch_add(&g_saw_done, 1);
        else if (cpu.rip == B_RIP)
            atomic_fetch_add(&g_saw_b, 1);
        else
            atomic_store(&g_bad_rip, 1);
        iters++;
        /* Stop once the chain has been observed AND we have done a healthy run
         * past the patch, or after a hard iteration cap (so a mutant that never
         * chains still terminates and FAILS the liveness check). */
        if (atomic_load(&g_stop_spin) && iters > 200000)
            break;
        if (iters > 80000000ull)   /* hard cap ~ a few seconds */
            break;
    }
    return NULL;
}

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem_init failed\n");
        return 2;
    }
    if (ocerz_map_fixed(CODE_BASE, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "map_fixed failed\n");
        return 2;
    }

    /* S: call rel32 -> B.  rel32 = B - (A+5). */
    {
        int32_t rel = (int32_t)(B_RIP - (A_RIP + 5));
        uint8_t s[5] = { 0xE8, (uint8_t)rel, (uint8_t)(rel >> 8),
                         (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
        put_bytes(A_RIP, s, 5);
    }
    /* T: jmp rel32 -> DONE.  rel32 = DONE - (B+5). */
    {
        int32_t rel = (int32_t)(DONE_RIP - (B_RIP + 5));
        uint8_t t[5] = { 0xE9, (uint8_t)rel, (uint8_t)(rel >> 8),
                         (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
        put_bytes(B_RIP, t, 5);
    }

    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 1;
    ocerz_vm_install_handlers(&g_vm);   /* creates g_vm.jit */
    if (!g_vm.jit) {
        fprintf(stderr, "jit not created\n");
        return 2;
    }

    /* Warm S so it is published in the cache with its outgoing edge to B PENDING
     * (T is not compiled yet), on the main thread, BEFORE the spinner starts.
     * This guarantees the spinner executes an already-compiled S and that the
     * only writer of S's tail is the compiler thread below. */
    {
        OcerzCPU cpu;
        ocerz_cpu_reset(&cpu);
        cpu.rip = A_RIP;
        cpu.gpr[OCERZ_RSP] = STACK_TOP;
        int rc = ocerz_jit_step(&g_vm, &cpu);
        CHECK(rc == OCERZ_STEP_OK, "warm S rc=%d", rc);
        CHECK(cpu.rip == B_RIP, "warm S unchained rip=%#llx want B", (unsigned long long)cpu.rip);
    }

    pthread_t spin;
    pthread_create(&spin, NULL, spinner_entry, NULL);
    while (!atomic_load(&g_spinner_ready))
        ;
    /* Let the spinner get well into its loop (executing S on its own core) so the
     * patch below genuinely lands on a block that is being executed concurrently. */
    for (volatile int i = 0; i < 2000000; i++)
        ;

    /* Compile T on THIS thread. translate(T) publishes T and, via pending_drain,
     * calls chain_activate(S.tail, T.entry) -- the LIVE PATCH of S's tail while
     * the spinner is inside S. */
    {
        OcerzCPU cpu;
        ocerz_cpu_reset(&cpu);
        cpu.rip = B_RIP;
        cpu.gpr[OCERZ_RSP] = STACK_TOP;
        int rc = ocerz_jit_step(&g_vm, &cpu);
        CHECK(rc == OCERZ_STEP_OK, "compile T rc=%d", rc);
        CHECK(cpu.rip == DONE_RIP, "T rip=%#llx want DONE", (unsigned long long)cpu.rip);
        atomic_store(&g_target_compiled, 1);
    }

    /* Give the spinner time to observe the chained outcome cross-core, then stop. */
    for (volatile long i = 0; i < 20000000L; i++)
        ;
    atomic_store(&g_stop_spin, 1);
    pthread_join(spin, NULL);

    unsigned long long saw_done = atomic_load(&g_saw_done);
    unsigned long long saw_b    = atomic_load(&g_saw_b);
    CHECK(!atomic_load(&g_bad_rc), "spinner saw a non-STEP_OK step (rc=%d)", atomic_load(&g_bad_rc));
    CHECK(!atomic_load(&g_bad_rip), "spinner saw a rip outside {B,DONE} -- torn/garbage patch");
    /* Liveness: the live patch must become visible to the spinning core. This is
     * exactly what sys_icache_invalidate in chain_activate provides; removing it
     * leaves this zero and FAILS. */
    CHECK(saw_done > 0, "chain never observed by the spinning thread "
          "(saw_done=%llu saw_b=%llu) -- icache publish missing?", saw_done, saw_b);

    fprintf(stderr, "test_chain_concurrency: saw_b=%llu saw_done=%llu bad_rip=%d bad_rc=%d\n",
            saw_b, saw_done, atomic_load(&g_bad_rip), atomic_load(&g_bad_rc));
    if (g_fail) {
        fprintf(stderr, "test_chain_concurrency: %d checks FAILED\n", g_fail);
        return 1;
    }
    fprintf(stderr, "test_chain_concurrency: OK\n");
    return 0;
}
