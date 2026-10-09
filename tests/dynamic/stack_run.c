/*
 * Runs of 64-bit pushes and pops, which src/jit.c emits with one rsp update
 * per run (emit_stack_run): odd runs, a run of sixteen and one of seventeen,
 * which goes past the longest run and splits, a pop run that loads one
 * register twice, the push rbp / mov rbp, rsp / push prologue, and a push run
 * that walks into a protected page.  Its handler unprotects the page and
 * returns, so the run restarts and has to end exactly as on x86.  Prints the
 * registers after each.  The first four lines are Rosetta's.  The fault lines
 * are x86's own result, one fault and every value in place: Rosetta on macOS
 * 27.0.1 groups pushes as well and aborts on this case ("unexpectedly need to
 * EmulateForward on a synchronous exception").
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t r[17];
static uint64_t saved_rsp;

static void print_regs(const char *what, int n)
{
    printf("%s", what);
    for (int i = 0; i < n; i++)
        printf(" %llx", (unsigned long long)r[i]);
    printf("\n");
}

static void odd_runs(void)
{
    __asm__ volatile(
        "mov $0x11, %%rax\n\t" "mov $0x22, %%rbx\n\t" "mov $0x33, %%rcx\n\t" "mov $0x44, %%rdx\n\t"
        "mov $0x55, %%rsi\n\t"
        "push %%rax\n\t" "push %%rbx\n\t" "push %%rcx\n\t"
        "push %%rdx\n\t" "push %%rsi\n\t" "push %%rax\n\t" "push %%rbx\n\t" "push %%rcx\n\t"
        "pop %%rax\n\t" "pop %%rbx\n\t" "pop %%rcx\n\t" "pop %%rdx\n\t" "pop %%rsi\n\t"
        "pop %%rdi\n\t" "pop %%r8\n\t" "pop %%r9\n\t"
        "mov %%rax, 0(%0)\n\t" "mov %%rbx, 8(%0)\n\t" "mov %%rcx, 16(%0)\n\t" "mov %%rdx, 24(%0)\n\t"
        "mov %%rsi, 32(%0)\n\t" "mov %%rdi, 40(%0)\n\t" "mov %%r8, 48(%0)\n\t" "mov %%r9, 56(%0)\n\t"
        : : "r"(r) : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "memory");
}

/* Sixteen pushes, then seventeen pops reading back one value pushed before them.  Every
   general register is in use, so the results go out through g_long, rip-relative. */
uint64_t g_long[16];

static void long_runs(void)
{
    __asm__ volatile(
        "push %%rbp\n\t"
        "mov $0x1, %%rax\n\t" "mov $0x2, %%rbx\n\t" "mov $0x3, %%rcx\n\t" "mov $0x4, %%rdx\n\t"
        "mov $0x5, %%rsi\n\t" "mov $0x6, %%rdi\n\t" "mov $0x7, %%r8\n\t" "mov $0x8, %%r9\n\t"
        "mov $0x9, %%r10\n\t" "mov $0xa, %%r11\n\t" "mov $0xb, %%r12\n\t" "mov $0xc, %%r13\n\t"
        "mov $0xd, %%r14\n\t" "mov $0xe, %%r15\n\t" "mov $0xf, %%rbp\n\t"
        "push %%r15\n\t"
        "push %%rax\n\t" "push %%rbx\n\t" "push %%rcx\n\t" "push %%rdx\n\t" "push %%rsi\n\t" "push %%rdi\n\t"
        "push %%r8\n\t" "push %%r9\n\t" "push %%r10\n\t" "push %%r11\n\t" "push %%r12\n\t" "push %%r13\n\t"
        "push %%r14\n\t" "push %%r15\n\t" "push %%rbp\n\t" "push %%rax\n\t"
        "pop %%rax\n\t" "pop %%rbx\n\t" "pop %%rcx\n\t" "pop %%rdx\n\t" "pop %%rsi\n\t" "pop %%rdi\n\t"
        "pop %%r8\n\t" "pop %%r9\n\t" "pop %%r10\n\t" "pop %%r11\n\t" "pop %%r12\n\t" "pop %%r13\n\t"
        "pop %%r14\n\t" "pop %%r15\n\t" "pop %%rbp\n\t" "pop %%r15\n\t" "pop %%r14\n\t"
        "mov %%rax, _g_long+0(%%rip)\n\t" "mov %%rbx, _g_long+8(%%rip)\n\t" "mov %%rcx, _g_long+16(%%rip)\n\t"
        "mov %%rdx, _g_long+24(%%rip)\n\t" "mov %%rsi, _g_long+32(%%rip)\n\t" "mov %%rdi, _g_long+40(%%rip)\n\t"
        "mov %%r8, _g_long+48(%%rip)\n\t" "mov %%r9, _g_long+56(%%rip)\n\t" "mov %%r10, _g_long+64(%%rip)\n\t"
        "mov %%r11, _g_long+72(%%rip)\n\t" "mov %%r12, _g_long+80(%%rip)\n\t" "mov %%r13, _g_long+88(%%rip)\n\t"
        "mov %%r14, _g_long+96(%%rip)\n\t" "mov %%r15, _g_long+104(%%rip)\n\t" "mov %%rbp, _g_long+112(%%rip)\n\t"
        "pop %%rbp\n\t"
        : : : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
              "memory");
    memcpy(r, g_long, 15 * sizeof r[0]);
}

/* pop rax; pop rbx; pop rax; pop rcx: the run stops before the second rax, and that pop wins. */
static void repeated_pop(void)
{
    __asm__ volatile(
        "mov $0xa1, %%rax\n\t" "mov $0xb2, %%rbx\n\t" "mov $0xc3, %%rcx\n\t" "mov $0xd4, %%rdx\n\t"
        "push %%rdx\n\t" "push %%rcx\n\t" "push %%rbx\n\t" "push %%rax\n\t"
        "xor %%eax, %%eax\n\t" "xor %%ebx, %%ebx\n\t" "xor %%ecx, %%ecx\n\t"
        "pop %%rax\n\t" "pop %%rbx\n\t" "pop %%rax\n\t" "pop %%rcx\n\t"
        "mov %%rax, 0(%0)\n\t" "mov %%rbx, 8(%0)\n\t" "mov %%rcx, 16(%0)\n\t"
        : : "r"(r) : "rax", "rbx", "rcx", "rdx", "memory");
}

__attribute__((noinline)) static uint64_t prologue(uint64_t a, uint64_t b)
{
    uint64_t out;
    __asm__ volatile(
        "push %%rbp\n\t" "mov %%rsp, %%rbp\n\t" "push %%r14\n\t" "push %%rbx\n\t" "push %%r12\n\t"
        "mov %1, %%r14\n\t" "mov %2, %%rbx\n\t" "mov $3, %%r12\n\t"
        "mov -8(%%rbp), %%rax\n\t" "add -16(%%rbp), %%rax\n\t"
        "add %%r14, %%rax\n\t" "imul %%r12, %%rbx\n\t" "add %%rbx, %%rax\n\t"
        "pop %%r12\n\t" "pop %%rbx\n\t" "pop %%r14\n\t" "pop %%rbp\n\t"
        "mov %%rax, %0"
        : "=&r"(out) : "r"(a), "r"(b) : "rax", "rbx", "r12", "r14", "memory", "cc");
    return out;
}

static char *g_lower;
static size_t g_page;
static volatile int g_faults;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    g_faults++;
    mprotect(g_lower, g_page, PROT_READ | PROT_WRITE);
}

/* rsp two slots above a protected page: the third push of the run faults. */
static void fault_run(char *top)
{
    __asm__ volatile(
        "mov %%rsp, (%1)\n\t"
        "mov %2, %%rsp\n\t"
        "mov $0x101, %%rax\n\t" "mov $0x202, %%rbx\n\t" "mov $0x303, %%rcx\n\t" "mov $0x404, %%rdx\n\t"
        "mov $0x505, %%rsi\n\t" "mov $0x606, %%rdi\n\t"
        "push %%rax\n\t" "push %%rbx\n\t" "push %%rcx\n\t" "push %%rdx\n\t" "push %%rsi\n\t" "push %%rdi\n\t"
        "pop %%r8\n\t" "pop %%r9\n\t" "pop %%r10\n\t" "pop %%r11\n\t" "pop %%rax\n\t" "pop %%rbx\n\t"
        "mov %%rsp, %%rcx\n\t"
        "mov (%1), %%rsp\n\t"
        "mov %%r8, 0(%0)\n\t" "mov %%r9, 8(%0)\n\t" "mov %%r10, 16(%0)\n\t" "mov %%r11, 24(%0)\n\t"
        "mov %%rax, 32(%0)\n\t" "mov %%rbx, 40(%0)\n\t" "sub %2, %%rcx\n\t" "mov %%rcx, 48(%0)\n\t"
        : : "r"(r), "r"(&saved_rsp), "r"(top)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
}

int main(void)
{
    odd_runs();
    print_regs("odd", 8);
    long_runs();
    print_regs("long", 15);
    repeated_pop();
    print_regs("repeat", 3);

    uint64_t acc = 0;
    for (uint64_t i = 0; i < 100000; i++) acc += prologue(i, i ^ 5);
    printf("prologue %llu\n", (unsigned long long)acc);

    /* 16 KB pages on both sides of the mismatch: Rosetta's and the host's. */
    g_page = 16384;
    char *map = mmap(NULL, 4 * g_page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (map == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    g_lower = (char *)(((uintptr_t)map + g_page - 1) & ~(uintptr_t)(g_page - 1));
    static char alt[65536];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    mprotect(g_lower, g_page, PROT_NONE);
    fault_run(g_lower + g_page + 16);
    printf("faults %d\n", g_faults);
    print_regs("fault", 7);
    return 0;
}
