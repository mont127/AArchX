/*
 * A spliced callee whose rsp's register stays behind (src/jit.c,
 * lazy_rsp_scan): lz_mid is spliced into its caller with its two calls of
 * lz_leaf, and inside it the pushes, pops, calls and rets move an offset
 * instead of the register (each lz_leaf frame's rbp, a mov of rsp, goes into
 * the result).  So guest rsp has to be worked out wherever the
 * guest can look at it in the middle:
 * - plain: the region in a loop, with a push of an immediate and a high-byte
 *   add; the runner runs it again with OCERZ_NO_HIGH8_ARITH=1, which
 *   interprets the add, and with OCERZ_NO_PROMO=1, under which lz_mid's
 *   pushes and pops go as runs;
 * - fault: the same on a stack whose page below is not mapped readable, so
 *   lz_leaf's push faults; the handler (on its own stack) checks rsp, rip and
 *   the two return addresses the splice never stored, then maps the page and
 *   returns, and the call finishes;
 * - suspend: a second thread runs the region while this one suspends it and
 *   reads its registers.  Between lz_mid's load of MARK into r12 and its pop,
 *   r12's value is only on the stack, so whenever r12 reads MARK that value
 *   must lie at or above the reported rsp, as a garbage collector scanning
 *   from rsp needs.
 * Against Rosetta.
 */
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SENT 0x5a5a1234deadbeefull
#define MARK 0x77aa55cc33ee1100ull

__asm__(
    ".text\n"
    ".p2align 4\n"
    /* lz_leaf(a, b) = 3a ^ (b >> 2), with a frame whose rbp it leaves in rdx */
    ".globl _lz_leaf\n"
    "_lz_leaf:\n"
    "    push %rbp\n    mov %rsp, %rbp\n"
    "    lea (%rdi,%rdi,2), %rax\n    shr $2, %rsi\n    xor %rsi, %rax\n    mov %rbp, %rdx\n"
    "    pop %rbp\n    ret\n"
    ".p2align 4\n"
    /* lz_mid(a): small enough (24 instructions at most) to be spliced into its callers, with both its calls */
    "_lz_mid:\n"
    "    push %rbp\n    mov %rsp, %rbp\n    push %r12\n    push %rbx\n"
    "    mov %rdi, %rbx\n    lea 1(%rdi), %rsi\n"
    "    call _lz_leaf\n"
    ".globl _lz_mid_ret1\n"
    "_lz_mid_ret1:\n"
    "    mov %rax, %rcx\n"
    "    movabs $0x77aa55cc33ee1100, %r12\n"
    "    mov %rbx, %rdi\n    mov %rbx, %rsi\n"
    "    call _lz_leaf\n"
    "    add %rcx, %rax\n"
    "    add %ah, %al\n"
    "    push $0x77\n    pop %rcx\n    add %rcx, %rax\n"
    "    sub %rbp, %rdx\n    add %rdx, %rax\n"
    "    pop %rbx\n    pop %r12\n    pop %rbp\n"
    "    ret\n"
    ".p2align 4\n"
    /* lz_outer(a): r12 holds SENT across the call, as lz_mid's caller */
    ".globl _lz_outer\n"
    "_lz_outer:\n"
    "    push %rbx\n    push %r12\n    push %r12\n"
    "    movabs $0x5a5a1234deadbeef, %r12\n"
    "    call _lz_mid\n"
    "    pop %r12\n    pop %r12\n    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* lz_outer_on(stack, a): the same on another stack */
    ".globl _lz_outer_on\n"
    "_lz_outer_on:\n"
    "    push %rbx\n    push %r12\n"
    "    mov %rsp, %rbx\n    mov %rdi, %rsp\n    mov %rsi, %rdi\n"
    "    movabs $0x5a5a1234deadbeef, %r12\n"
    "    call _lz_mid\n"
    ".globl _lz_outer_on_ret\n"
    "_lz_outer_on_ret:\n"
    "    mov %rbx, %rsp\n"
    "    pop %r12\n    pop %rbx\n"
    "    ret\n");

uint64_t lz_outer(uint64_t a);
uint64_t lz_outer_on(uint8_t *stack, uint64_t a);
extern char lz_leaf[], lz_mid_ret1[], lz_outer_on_ret[];

static uint8_t *g_top, *g_guard;
static volatile int64_t g_rsp_off = 1;
static volatile int g_rip_ok, g_ret_ok, g_outer_ok, g_faults;

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uint64_t rsp = uc->uc_mcontext->__ss.__rsp, rip = uc->uc_mcontext->__ss.__rip;
    g_faults++;
    g_rsp_off = (int64_t)(rsp - (uint64_t)(uintptr_t)g_top);
    g_rip_ok = rip == (uint64_t)(uintptr_t)lz_leaf;
    g_ret_ok = *(uint64_t *)(uintptr_t)rsp == (uint64_t)(uintptr_t)lz_mid_ret1;
    g_outer_ok = *(uint64_t *)(uintptr_t)(g_top - 8) == (uint64_t)(uintptr_t)lz_outer_on_ret;
    if (mprotect(g_guard, 16384, PROT_READ | PROT_WRITE) != 0)
        _exit(3);
}

static volatile int g_stop;
static volatile uint64_t g_worker_acc;
static mach_port_t g_worker_port;

static void *worker(void *arg)
{
    g_worker_port = mach_thread_self();
    uint64_t acc = 0;
    for (uint64_t i = 0; !g_stop; i++)
        acc = acc * 3 + lz_outer(i);
    g_worker_acc = acc;
    return arg;
}

int main(void)
{
    /* plain */
    uint64_t acc = 0;
    for (uint64_t i = 0; i < 300000; i++)
        acc = acc * 7 + lz_outer(i * 2654435761u);
    printf("plain %llx\n", (unsigned long long)acc);

    /* fault: lz_leaf's push is the first access below g_top - 40 */
    stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    uint64_t fsum = 0;
    for (int r = 0; r < 3; r++) {
        uint8_t *map = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (map == MAP_FAILED) { printf("map\n"); return 1; }
        g_guard = map;
        g_top = map + 16384 + 40;
        if (r > 0) mprotect(g_guard, 16384, PROT_NONE);
        fsum = fsum * 5 + lz_outer_on(g_top, 1000 + (uint64_t)r);
        munmap(map, 32768);
    }
    printf("fault: sum %llx faults %d rsp %+lld rip %s ret %s outer %s\n", (unsigned long long)fsum, g_faults,
           (long long)g_rsp_off, g_rip_ok ? "ok" : "bad", g_ret_ok ? "ok" : "bad", g_outer_ok ? "ok" : "bad");

    /* suspend */
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    while (!g_worker_port)
        usleep(100);
    usleep(20000);
    int windows = 0, misses = 0;
    for (int k = 0; k < 2000; k++) {
        if (thread_suspend(g_worker_port) != KERN_SUCCESS) { printf("suspend failed\n"); return 1; }
        x86_thread_state64_t st;
        mach_msg_type_number_t cnt = x86_THREAD_STATE64_COUNT;
        if (thread_get_state(g_worker_port, x86_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS &&
            st.__r12 == MARK) {
            windows++;
            int found = 0;
            for (uint64_t a = st.__rsp; a < st.__rsp + 512; a += 8)
                if (*(volatile uint64_t *)(uintptr_t)a == SENT) { found = 1; break; }
            misses += !found;
        }
        thread_resume(g_worker_port);
        if ((k & 63) == 0) usleep(50);
    }
    g_stop = 1;
    pthread_join(t, NULL);
    printf("suspend: windows %s misses %d\n", windows > 20 ? "seen" : "few", misses);
    return 0;
}
